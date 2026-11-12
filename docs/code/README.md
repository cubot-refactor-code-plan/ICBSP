# 代码层索引（code/）

[← README](../../README.md) · [接入指南](../接入指南.md) · [常用接口](../常用接口.md) · [编码规范](../编码规范.md)

源码在仓库根目录的 **`code/`** 下，分五层；`service/`、`device/` 再按用途分子目录。本文是入口：先看分层图和依赖矩阵找到层，
再进该层的文档看文件表与接口。每层文档的章节和代码子目录一一对应。

![分层依赖](../img/layers.png)

> 矢量版：[layers.svg](../img/layers.svg)。三张图（分层、启动顺序、工具链）都由 `tools/draw_diagrams.py` 生成，改了代码结构就改脚本重跑。

---

## 1. 五层各管什么

| 目录 | 子目录 / 内容 | 不负责 | 文档 |
| --- | --- | --- | --- |
| `code/bsp/` | 一个外设怎么初始化、怎么收发：CAN（FDCAN）、UART、SPI、PWM、GPIO、Key、Encoder、DWT；`bsp_series.hpp`（按系列选 HAL 头）、`bsp_dma_buffer.hpp`（`.dma_buffer` 段宏） | 帧格式、设备语义 | [bsp.md](bsp.md) |
| `code/service/` | `system/`：库入口、EventState 与默认状态树、故障记录、配置检查、newlib 锁；`comm/`：CAN 收发节点；`monitor/`：在线检查；`definition/`：电机 / IMU 数据定义 | 具体设备协议 | [service.md](service.md) |
| `code/device/` | `motor/`：DJI 电机、达妙电机、直流电机、Emm_V5（含帧编解码 `emmv5_protocol`）；`sensor/`：BMI088；`remote/`：R9DS；`actuator/`：舵机、蜂鸣器 | 硬件时序细节（交给 bsp） | [device.md](device.md) |
| `code/algorithm/` | PID、四元数 PID、四元数、VQF、矩阵、RLS | 与硬件无关，不碰 HAL / RTOS | [algorithm.md](algorithm.md) |
| `code/tasks/` | 三个库任务的任务体：sys / can_rx / can_tx | 任务创建（在 `service/system/system.cpp`） | [tasks.md](tasks.md) |

包含路径只有 `code/`，一律写全子目录：`#include "bsp/bsp_uart.hpp"`、`#include "device/motor/dji_motor.hpp"`。

宿主侧的 `user/app`、`user/board`（本库的模板放在 `templates/user/`）不在库里，见 [接入指南](../接入指南.md)。

---

## 2. 依赖矩阵

行 = 谁，列 = 可以包含谁；`●` 直接 include，`○` 经 X-macro 清单反向包含宿主头文件。库内各层的规则由 `tools/check_layers.py` 检查（CI 里跑）。

|  | 宿主 board | bsp | service | device | algorithm | HAL / FreeRTOS |
| --- | --- | --- | --- | --- | --- | --- |
| **宿主 app** | ● |  | ● | ● |  | ● |
| **宿主 board** |  | ● | ● | ● |  | ● |
| **tasks** | ○ | ● | ● |  |  | ● |
| **device** |  | ● | ● |  | ● | ● |
| **service** |  | ● |  |  | ● | ● |
| **bsp** | ○ |  |  |  |  | ● |
| **algorithm** |  |  |  |  |  |  |

规则：

1. **依赖只朝下**。device 和 tasks 互不包含（tasks 不认识任何设备）；algorithm 只依赖 C++ 标准库。
2. 两个头文件各层都能包含：`service/system/status_code.hpp`（纯状态码）、`service/system/config_check.hpp`（读宿主 `hardware_config.h` 的唯一入口：缺宏 `#error`、取值 `static_assert`）。
3. 唯一的反向依赖是 `○`：`bsp/` 和 `tasks/` 包含宿主的 `board_instances.hpp`，靠 X-macro 在编译期把「板子上有哪几个实例」展开进 HAL 回调分发、巡检和 CAN 收发任务——没有运行时登记表。
   漏写清单或清单里两个实例绑同一句柄时，对应实例的 `init()` 返回 `BAD_ARG`（`board_init()` 里 `ICBSP_CHECK` 停机），不会静默失效（见 [bsp.md](bsp.md#4-实例清单x-macro)）。

---

## 3. 启动顺序

![启动顺序](../img/startup.png)

> 矢量版：[startup.svg](../img/startup.svg)

```
MX_FREERTOS_Init()            freertos.c 的 USER CODE Init 段
  └─ app_init()               user/app/app_main.cpp
       ├─ ① system_init()     sys_state、bsp_dwt，创建 sys_task / can_rx_task
       ├─ ② board_init()      外设 init、设备 init（各自注册 CAN 节点、挂到默认状态树），每个一行 ICBSP_CHECK
       ├─    创建业务任务      入口先 sys_state.wait_running()
       └─ ③ system_start()    有发送节点才建 can_tx_task，置运行位
```

顺序是硬约束：`board_init()` 里注册的 CAN 节点必须**在 `system_init()` 之后**（要用 `sys_state`）、
**在 `system_start()` 之前**（`can_tx_task` 按已注册节点决定建不建；运行期再注册会被拒绝）。

`ICBSP_CHECK(x)` 总会求值（不要写成 `configASSERT(x.init() == …)`，断言可能被定义成空）；失败时把文件 / 行号写进 `.noinit` 段的 `fault_record` 再停机，复位后在调试器里 `print fault_record`。

三个任务的周期与职责见 [tasks.md](tasks.md)。

---

## 4. 实测状态

“实测”= 在 DM-MC02（STM32H723）上按用例跑过，结果有记录；“只编译验证”= 没有硬件或没写用例，只保证 Debug / -O2 0 warning
（头文件模板由回归宿主的 `compile_probe.cpp` 显式实例化）。以阶段 3（ICBSP `phase2` 分支，2026-11-06 ~ 07）为准，用例编号见
[phase3_2026-11-06.md](../bsp_test/phase3_2026-11-06.md) 与宿主 `app/test/`。

| 层 | 模块 | 状态 | 依据 |
| --- | --- | --- | --- |
| bsp | `BspCanT`（H7 FDCAN） | 实测 | C00~C12：收发、扩展帧 / DLC、Bus-Off、无 ACK、TX-empty 动态开关（N27）、错误状态回调、Queue / FIFO、5 分钟浸泡、`CAN_RX_ENABLE = 0`（N28） |
| bsp | `BspCanT`（F4 bxCAN） | 只编译验证 | Rh-i 的 F4 移植；本回归工程（H7）不编译这一分支 |
| bsp | `BspUartT`（DMA / IT） | 实测 | U01~U21、R01~R06：收发、边界、故障注入与恢复、循环 DMA 专项、F2 短段压力、RS485 硬件 DE、IT 模式、浸泡 |
| bsp | `BspSpi` / `BspSpiDevice` | 实测 | S01~S05、P01~P06（经 BMI088） |
| bsp | `BspPwm` | 实测 | Y11（ARR 预装载、`set_freq()` 边界）；蜂鸣器开机提示音 |
| bsp | `BspDwt` | 实测 | Y08（两任务并发、跨一次 7.8 s 回绕）；没有专门跑满 10 圈 |
| bsp | `icbsp_dma_reachable()` | 实测 | Y10（DTCM / AXI / SRAM4 × DMA1 / BDMA） |
| bsp | `BspGpio`、`BspKey`、`BspEncoder` | 只编译验证 | 没有用例 |
| service | `system`、EventState、默认状态树、`Online` | 实测 | Y01~Y04、E01~E14、O01~O11、T01~T04、Y13（N13） |
| service | `fault_record` | 实测 | Y12、1.4 故意触发 BusFault |
| service | `CanRxNodeT` / `CanTxNodeT` | 实测 | C03~C11 |
| service | `config_check`、功能开关 | 编译验证 | 宿主 `tools/compile_matrix.sh` 13 种配置 |
| service | `newlib_lock` | 只编译验证 | 没有并发浮点打印的用例 |
| device | `DjiMotor` | 实测（M3508、GM6020） | M01 / M02（反馈）、M07 / M08（速度闭环；GM6020 电流模式）；M2006 没有实物 |
| device | `DmMotor` | 实测（J4340P） | M03（读寄存器、`sync_ranges()`）、M05（使能）、M09（MIT 零力矩 / 阻尼 / 位置）；J4310 只做 M04 的读寄存器与反馈；J8009P 没有实物 |
| device | `DeviceBmi088`（含 VQF 姿态） | 实测 | I01~I07、P01~P06 |
| device | `DeviceBuzzer` | 上板使用 | 开机提示音，没有专门用例 |
| device | `DeviceMotor`、`DeviceServo`、`DeviceR9dsT`、`DeviceEmmv5GroupT`（含 `emmv5_protocol`） | 只编译验证 | 没有硬件；Emm_V5 的 XOR 校验未实测 |
| algorithm | `PID`、`Quaternion`、`MatrixF32`、`Rls` | 主机单元测试 | `tests/host/`（45 条判据）；PID 另在 M07 / M08 上板 |
| algorithm | `QuaternionPID` | 只编译验证 | 没有用例 |
| algorithm | `VQF` | 实测 | BMI088 自动刷新任务里运行（I06、P 组） |
| tasks | sys / can_rx / can_tx | 实测 | 随上面各组用例运行 |

---

## 5. 建议的阅读顺序

| 想知道 | 看哪里 |
| --- | --- |
| 某个外设为什么收不到数据 | [bsp.md](bsp.md) 的「实例清单」与「常见坑」 |
| 电机不动 / 报文不对 | [device.md](device.md) 的 `motor/` 一节与「常见坑」 |
| 系统起不来、卡在 `wait_running()` | [service.md](service.md) 的 EventState 与 [启动顺序](#3-启动顺序) |
| 为什么会有 10 ms / 1 ms 这种周期 | [tasks.md](tasks.md) |
| PID 输出怪 | [algorithm.md](algorithm.md) |

新增代码前先确认「放哪一层」：只依赖 HAL 的放 `bsp`，涉及具体设备协议的放 `device` 的对应子目录，
纯数学放 `algorithm`，跨设备的调度与状态放 `service`。改完跑 `python3 tools/check_layers.py`。
