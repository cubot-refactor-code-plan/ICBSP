# ICBSP

![GitHub Release](https://img.shields.io/github/v/release/cubot-refactor-code-plan/ICBSP)  [![License](https://img.shields.io/github/license/cubot-refactor-code-plan/ICBSP.svg)](https://github.com/cubot-refactor-code-plan/ICBSP/blob/main/LICENSE)  ![CopyRight](https://img.shields.io/badge/CopyRight-INVC%20&%20CUBOT-blue.svg)    
![](https://img.shields.io/badge/chip-STM32H7xx%20%7C%20STM32F4xx-blue.svg) ![](https://img.shields.io/badge/RTOS-FreeRTOS_CMSIS_V2-green.svg) ![](https://img.shields.io/badge/language-C++20-blue.svg) ![](https://img.shields.io/badge/ToolChain-CMake-blue.svg)

面向 **STM32H7 / STM32F4 + FreeRTOS（CMSIS-RTOS V2）+ CMake** 的板级库：BSP、Service、Device、Algorithm、Tasks 分层，宿主工程通过 git submodule 接入。

- 语言：**C++20**（`inline` 变量、指定初始化器）；库和宿主都按 C++20 编译
- 构建：CMake ≥ 3.22，ARM GNU Toolchain；库是 CMake **OBJECT 库** `icbsp`（用强定义覆盖 HAL / FreeRTOS 的弱符号）
- 目标：STM32H7 CubeMX（Toolchain = CMake）工程；STM32F4 的移植只编译验证过，CAN 的任务 / 节点还没接上
- 许可证：MIT，见根目录 `LICENSE`（版权人 INVC & CUBOT）；例外见 [许可证](#许可证)
- 宏前缀：`ICBSP_`（配置宏、功能开关、实例清单、系列宏），CMake 目标 `icbsp`

> **当前状态（v1.0.0 之前）**：阶段 1~3 已完成——UART / CAN 回填、评审遗留问题、新功能（EventState 默认树、DmMotor 等），
> 并在 DM-MC02（STM32H723）上做了 Debug 与 -O2 两轮全面实板回归（服务层、状态树、UART（DMA / IT）、RS485、CAN、SPI + BMI088、
> DJI / 达妙电机转动，含 5 分钟浸泡），记录见 [docs/bsp_test/phase3_2026-11-06.md](docs/bsp_test/phase3_2026-11-06.md)。
> 哪些模块实测过、哪些只编译验证，见 [docs/code/README.md 的实测状态表](docs/code/README.md#4-实测状态)。路线与待办在 [undo.md](undo.md)。

## 目录

```text
ICBSP/
├─ code/            库源码（分层见 docs/code/；包含路径只加 code/，写成 "bsp/bsp_uart.hpp"）
│  ├─ bsp/          CAN（FDCAN / bxCAN）、UART、SPI、PWM、GPIO、Key、Encoder、DWT；系列桥接、.dma_buffer 宏
│  ├─ service/
│  │  ├─ system/      库入口、Status / EventState、默认状态树、fault_record、配置检查、newlib 锁
│  │  ├─ comm/        CAN 收发节点
│  │  ├─ monitor/     Online
│  │  └─ definition/  电机 / IMU 数据定义
│  ├─ device/
│  │  ├─ motor/       DjiMotor、DmMotor、DeviceMotor、Emm_V5（含 emmv5_protocol 帧编解码）
│  │  ├─ sensor/      BMI088、WT102
│  │  ├─ remote/      R9DS
│  │  └─ actuator/    Servo、Buzzer
│  ├─ algorithm/    PID、QuaternionPID、Quaternion、VQF、Matrix、RLS
│  └─ tasks/        sys_task、can_rx_task、can_tx_task
├─ templates/       宿主接入模板（user/board、user/app、链接脚本、FreeRTOS / 中断钩子、.vscode、Flash）
├─ tools/           integrate.py（自动接入与生成）、manifest.json（生成清单）、web_ui/（图形界面）、check_layers.py、draw_diagrams.py
├─ tests/host/      algorithm/ 的主机侧单元测试（CI：.github/workflows/host-tests.yml）；接入引擎的测试在 tools/tests/
├─ docs/            接入指南、常用接口、编码规范、代码层说明、架构图、实板测试记录、器件手册
└─ CMakeLists.txt
```

## 克隆

建议将本仓库作为`submodule`克隆到目标宿主工程，以获得更新支持  
若目的仅为使用，建议浅克隆`main`分支
```bash
$ git submodule add --depth 1 git@github.com:cubot-refactor-code-plan/ICBSP.git Middlewares/Third_Party/ # 浅克隆最近一次主分支内的提交
```

若希望克隆特定分支
```bash
$ git submodule add -b <branch name> --depth <clone depth> git@github.com:cubot-refactor-code-plan/ICBSP.git <local path>
```


## 快速接入

### 直接使用python脚本
![python_version](https://img.shields.io/badge/python-3.7-blue.svg)
在宿主工程根目录：

```bash
python3 third_party/icbsp/tools/integrate.py --dry-run   # 只看将改什么
python3 third_party/icbsp/tools/integrate.py             # 执行
```

脚本可重复执行，已完成的步骤会跳过。它会检查 CubeMX 前提、给链接脚本加 `.dma_buffer` / `.noinit`、
接 `FreeRTOSConfig.h`、`freertos.c`、`stm32h7xx_it.c` 的 USER CODE 段、复制 `user/board` 与 `user/app` 模板、
改根 `CMakeLists.txt`、生成 `.vscode/*`、复制 `Flash/` 烧录脚本。

### Nodejs图形化界面
![](https://img.shields.io/badge/Node-v14.14.0-green.svg)
按 CubeMX 的 `.ioc` 生成外设实例和设备、校验并修改 `.ioc`，可以用图形界面（零依赖，不用 `npm install`）：

```bash
node third_party/icbsp/tools/web_ui/server.mjs   # 浏览器打开 http://127.0.0.1:8765/
```

参数、每步的判定条件、手动接入步骤、排错见 **[docs/接入指南.md](docs/接入指南.md)**。

## 接入要求

| 项目     | 要求                                                                                       |
| -------- | ------------------------------------------------------------------------------------------ |
| 芯片     | STM32H7；STM32F4 只编译验证                                                    |
| 工具链   | CubeMX → Project Manager → Toolchain = **CMake**；C++20                                    |
| RTOS     | FreeRTOS，**CMSIS_V2**，tick **1 kHz**，`configCHECK_FOR_STACK_OVERFLOW = 2`、`configUSE_MALLOC_FAILED_HOOK = 1` |
| HAL 时基 | 不用 SysTick；优先级没有要求（CubeMX 默认 15 即可，1.4 起实测）                              |
| D-Cache  | 保持关闭（库不做 Cache 维护）                                                               |
| FDCAN    | 经典 CAN，RX FIFO0，开 FDCAN 中断                                                          |
| UART     | RX / TX DMA + 全局中断（RX DMA 由库改成循环模式，用 IDLE / 半满 / 写满事件投递）；或中断模式 |
| SPI      | 全双工主机、软件 NSS、`MasterKeepIOState = ENABLE`                                          |
| 链接脚本 | 必须有**可加载**的 `.dma_buffer`（`>RAM_D1 AT> FLASH`）与 `.noinit`；CMake 配置阶段检查   |

库按 `HAL_FDCAN_MODULE_ENABLED` / `HAL_UART_MODULE_ENABLED` / `HAL_SPI_MODULE_ENABLED` / `HAL_TIM_MODULE_ENABLED` 条件编译：
没开的外设不会生成对应驱动和任务。每项要求的原因、CubeMX 里怎么勾、接完怎么自查，见 **[docs/接入指南.md](docs/接入指南.md)**。

## 文档

| 文档                                       | 内容                                                                |
| ------------------------------------------ | ------------------------------------------------------------------- |
| [docs/接入指南.md](docs/接入指南.md)        | 硬性要求逐条解释、CubeMX 设置、自动 / 手动接入、自查清单、常见问题  |
| [docs/code/](docs/code/)                    | 代码层索引：分层依赖图、启动顺序图、依赖矩阵、五层文档               |
| [docs/常用接口.md](docs/常用接口.md)        | UART / CAN / 电机 / PID / 系统状态的使用示例                        |
| [docs/编码规范.md](docs/编码规范.md)        | 命名、分层、注释格式、C++20 规则                                     |
| [docs/编辑器配置.md](docs/编辑器配置.md)    | `.vscode/settings.json` 各项作用；outline++ 分区正则逐段拆解        |
| [templates/README.md](templates/README.md)  | 每个模板文件复制到宿主哪里；`.vscode` 与 `Flash/` 的占位符规则     |
| [docs/bsp_test/](docs/bsp_test/)            | 实板测试记录（每份注明对应的代码版本；早期记录里的 API 名称是旧的） |
| [docs/service_test/](docs/service_test/)    | EventState / Online 的实板测试记录（2026-11-01）                     |
| [docs/sheet/](docs/sheet/README.md)         | 器件手册清单与获取方式（手册本身不随仓库分发）                       |
| [undo.md](undo.md)                          | v1.0.0 路线与待办                                                    |
| `stm32_bsp_review.md`、`stm32_framework_对比评估.md` | 评审与合并方案（代码注释里的 F / A / I / D / N / Q 编号出自这两份） |

## 配置

宿主的 `hardware_config.h`（模板 `templates/user/board/hardware_config.h`）是**必须提供的完整配置**，库里没有默认值：
任务开关、优先级、栈深、队列深度、超时都在这里。库只经 `service/system/config_check.hpp` 读取它：缺宏 `#error`、
开关依赖与取值范围 `static_assert`，HAL 模块没开时强制关掉对应开关。

```c
#define ICBSP_SYS_TASK_ENABLE       1
#define ICBSP_CAN_RX_ENABLE         1
#define ICBSP_CAN_TX_ENABLE         1
#define ICBSP_UART_PATROL_ENABLE    1
```

## 库任务

`system_init()` 创建 `sys_task`（10 ms：DWT 时间轴、CAN Bus-Off 恢复与发送兜底、UART 巡检、`Online::update()`、默认状态树巡检，
耗时统计写在 `sys_task_stats`；库本身不打印，宿主用 `system_set_report_hook()` 接打印）与 `can_rx_task`（1 ms，按 ID 分发，未认领的进回退缓冲）。

`system_start()` 检查发送节点，有节点才创建 `can_tx_task`（槽位聚合、保底心跳、指令有效期）。
`DeviceBmi088` 默认在 `init()` 里建自己的刷新任务。库任务优先级按 `configMAX_PRIORITIES` 往下取（CAN −2 > BMI088 −3 > sys −4），业务任务要更低。

## 已知限制

- BSP 没有运行时登记表，`board_instances.hpp` 必须和 `board_cfg.*` 同步。实例漏写、或清单里两个实例绑同一句柄时，`BspUartT` / `BspCanT` / `BspSpi` 的 `init()` 返回 `BAD_ARG`（`board_init()` 里 `ICBSP_CHECK` 停机）。
- 清单外的同类外设（宿主自己用 HAL 管的 UART / SPI / FDCAN）收到的 HAL 回调转到弱函数 `icbsp_unhandled_*()`，见 [docs/code/bsp.md](docs/code/bsp.md)。
- UART 循环 DMA 接收：串口 / DMA 中断被挡住的时间要短于收满半个接收缓冲的时间（128 字节、921600 下约 0.69 ms），否则可能静默丢数据。
- 库假定 D-Cache 关闭；RS485 只支持 UART 硬件 DE。
- STM32F4：只编译验证；sys / can 任务与 CAN 节点目前只在 H7（FDCAN）下编译。

## 许可证

MIT，见根目录 [`LICENSE`](LICENSE)。例外：

- `code/algorithm/vqf.*`：第三方代码 VQF，MIT，版权 Daniel Laidig（文件头的 `SPDX-FileCopyrightText` / `SPDX-License-Identifier` 行，不要删）；保持上游原样（CMake 里单独关了 `-Wshadow`）。
- `code/algorithm/matrix.*`：移植自 USTC-RoboWalker（yssickjgd/damiao_mc02_bsp）的代码，原作者许可在 RoboMaster 赛事范围内使用，**不适用本仓库的 MIT**；超出这个范围请自行征求原作者意见。文件头的出处说明要保留。
- 器件手册：版权归各厂商，**不随本仓库分发**（2026-11-12 起已从仓库及其历史中移除）；清单与官方渠道见 [docs/sheet/README.md](docs/sheet/README.md)。
