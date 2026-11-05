# ICBSP

**ICBSP = INVC & CUBOT Board Support Package**，由 INVC 与 CUBOT 共同维护（前身 stm32-framework / STM32_BSP）。

![GitHub Release](https://img.shields.io/github/v/release/cubot-refactor-code-plan/ICBSP)[![License](https://img.shields.io/github/license/cubot-refactor-code-plan/ICBSP.svg)](https://github.com/cubot-refactor-code-plan/ICBSP/blob/main/LICENSE)
![](https://img.shields.io/badge/chip-STM32H7xx%20%7C%20STM32F4xx-blue.svg) ![](https://img.shields.io/badge/RTOS-FreeRTOS_CMSIS_V2-green.svg) ![](https://img.shields.io/badge/language-C++20-blue.svg) ![](https://img.shields.io/badge/ToolChain-CMake-blue.svg) 

面向 **STM32H7 / STM32F4 + FreeRTOS（CMSIS-RTOS V2）+ CMake** 的板级库：BSP、Service、Device、Algorithm 分层，宿主工程通过 git submodule 接入。

- 语言：**C++20**（`inline` 变量、指定初始化器）
- 构建：CMake ≥ 3.22，ARM GNU Toolchain
- 目标：任意 STM32H7 CubeMX（CMake）工程
- 许可证：MIT，见根目录 `LICENSE`（版权人 INVC & CUBOT）
- 宏前缀：`ICBSP_`（配置宏、功能开关、实例清单），CMake 目标 `icbsp`

> 当前状态：`User/` 的 BSP / Service / Device / Protocol（Emm_V5）/ Algorithm（PID）已合并进库。
> BSP 层没有运行时登记表、基类、虚函数；实例由宿主的 `board_instances.hpp` 清单驱动。
> `dm_mc02` 已通过 Debug / Release 编译（0 warning），实板验证尚未完成。

## 目录

```text
ICBSP/
├─ code/         库源码（分层见 docs/code/；包含路径只加 code/，写成 "bsp/bsp_uart.hpp"）
│  ├─ bsp/       CAN、UART、SPI、PWM、GPIO、Key、Encoder、DWT
│  ├─ service/
│  │  ├─ system/      system、Status / EventState、fault_record
│  │  ├─ comm/        CAN 收发节点
│  │  ├─ monitor/     Online
│  │  └─ definition/  电机 / IMU 数据定义
│  ├─ device/
│  │  ├─ motor/       DjiMotor、DeviceMotor、Emm_V5（含 emmv5_protocol 帧编解码）
│  │  ├─ sensor/      BMI088
│  │  ├─ remote/      R9DS
│  │  └─ actuator/    Servo、Buzzer
│  ├─ algorithm/ PID、QuaternionPID、VQF、Quaternion、Matrix、RLS
│  └─ tasks/     sys_task、can_rx_task、can_tx_task
├─ templates/    宿主接入模板（board、app、链接脚本、FreeRTOS 钩子、.vscode、Flash）
├─ tools/        integrate.py（自动接入）、draw_diagrams.py（画架构图）
├─ docs/         接入指南、常用接口、编辑器配置、代码层说明、架构图、器件手册
└─ CMakeLists.txt
```

## 快速接入

在宿主工程根目录：

```bash
python3 third_party/icbsp/tools/integrate.py --dry-run   # 只看将改什么
python3 third_party/icbsp/tools/integrate.py             # 执行
```

脚本可重复执行，已完成的步骤会跳过。它会检查 CubeMX 前提、给链接脚本加 `.dma_buffer` / `.noinit`、
接 `FreeRTOSConfig.h` 与 `freertos.c` 的 USER CODE 段、复制 `user/board` 与 `user/app` 模板、
改根 `CMakeLists.txt`、生成 `.vscode/*`、复制 `Flash/` 烧录脚本。

参数、每步的判定条件、排错见 **[docs/接入指南.md](docs/接入指南.md)**。

## 接入要求

| 项目     | 要求                                                     |
| -------- | -------------------------------------------------------- |
| 芯片     | STM32H7 系列                                             |
| 工具链   | CubeMX → Project Manager → Toolchain = **CMake** |
| RTOS     | FreeRTOS，**CMSIS_V2**，tick **1 kHz**       |
| HAL 时基 | 不用 SysTick；优先级没有要求（CubeMX 默认 15 即可，1.4 起实测） |
| FDCAN    | 经典 CAN，RX FIFO0，开 FDCAN 中断                        |
| UART     | RX/TX DMA + 全局中断（驱动用 IDLE 断帧）                 |
| SPI      | 全双工主机、软件 NSS、`MasterKeepIOState = ENABLE`     |
| 链接脚本 | 必须有**可加载**的 `.dma_buffer` 与 `.noinit`  |

库按 `HAL_FDCAN_MODULE_ENABLED` / `HAL_UART_MODULE_ENABLED` / `HAL_SPI_MODULE_ENABLED` 等宏条件编译：
没开的外设不会生成对应驱动和任务。每项要求的原因、CubeMX 里怎么勾、接完怎么自查，
见 **[docs/接入指南.md](docs/接入指南.md)**。

## 文档

| 文档                                      | 内容                                                               |
| ----------------------------------------- | ------------------------------------------------------------------ |
| [docs/接入指南.md](docs/接入指南.md)       | 硬性要求逐条解释、CubeMX 设置、自动/手动接入、自查清单、常见问题   |
| [docs/code/](docs/code/)                   | 代码层索引：分层依赖图、启动顺序图、依赖矩阵、六层文档             |
| [docs/常用接口.md](docs/常用接口.md)       | UART / CAN / 电机 / PID / 系统状态的使用示例                       |
| [docs/编辑器配置.md](docs/编辑器配置.md)   | `.vscode/settings.json` 各项作用；outline++ 分区正则逐段拆解       |
| [docs/编码规范.md](docs/编码规范.md)       | 命名、分层、注释格式                                               |
| [templates/README.md](templates/README.md) | 每个模板文件复制到宿主哪里；`.vscode` 与 `Flash/` 的占位符规则 |
| [docs/bsp_test/](docs/bsp_test/)           | BSP 历史测试记录（旧 API 名称不代表当前接口）                      |

## 配置

`user/board/hardware_config.h` 是**唯一配置源**：任务开关、优先级、栈深、队列深度、超时都在这里，
HAL 模块没开时对应开关会自动置 0。模板见 `templates/user/board/hardware_config.h`。

```c
#define ICBSP_SYS_TASK_ENABLE       1
#define ICBSP_CAN_RX_ENABLE         1
#define ICBSP_CAN_TX_ENABLE         1
#define ICBSP_UART_PATROL_ENABLE    1
```

## 库任务

`system_init()` 创建 `sys_task`（10 ms：DWT 计时、CAN Bus-Off 恢复、UART 巡检、`Online::update()`，
每 2 s 打印一次循环耗时）与 `can_rx_task`（按 ID 分发，未认领的进回退缓冲）。

`system_start()` 检查发送节点，有节点才创建 `can_tx_task`（槽位聚合、保底心跳、指令有效期）。
CAN 接收和发送保持两个独立任务，不合并。

## 已知限制

- BSP 没有运行时登记表，`board_instances.hpp` 必须和 `board_cfg.*` 同步。实例漏写、或清单里两个实例绑同一句柄时，`BspUartT` / `BspCanT` / `BspSpi` 的 `init()` 返回 `BAD_ARG`（`board_init()` 里断言停机）。
- 清单外的同类外设（宿主自己用 HAL 管的 UART / SPI / FDCAN）收到的 HAL 回调转到弱函数 `icbsp_unhandled_*()`，见 `docs/code/bsp.md`。
- `BspUartT` 只有头文件，任意 Transport / 缓冲尺寸组合都不用在库里补显式实例化。

## 许可证

MIT，见根目录 [`LICENSE`](LICENSE)。
