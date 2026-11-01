# stm32-framework

面向 **STM32H7 + FreeRTOS（CMSIS-RTOS V2）+ CMake** 的板级库：BSP、Service、Device、Protocol、Algorithm 分层，宿主工程通过 git submodule 接入。

- 语言：**C++20**（`inline` 变量、指定初始化器）
- 构建：CMake ≥ 3.22，ARM GNU Toolchain
- 目标：任意 STM32H7 CubeMX（CMake）工程
- 许可证：MIT，见根目录 `LICENSE`

> 当前状态：`User/` 的 BSP / Service / Device / Protocol（Emm_V5）/ Algorithm（PID）已合并进库。
> BSP 层不再有运行时登记表、基类、虚函数；实例由宿主的 `board_instances.hpp` 清单驱动。
> `dm_mc02` 已通过 Debug / Release 编译（0 warning），实板验证尚未完成。

---

## 1. 目录

```text
stm32-framework/
├─ bsp/          CAN、UART、SPI、PWM、GPIO、Key、Encoder、DWT
├─ service/      Status / EventState、Online、CAN 收发节点、fault_record、system
├─ tasks/        sys_task、can_rx_task、can_tx_task
├─ device/       DjiMotor、DeviceMotor、Servo、R9DS、Emm_V5、BMI088、Buzzer
├─ protocol/     emmv5_protocol（帧编解码）
├─ algorithm/    PID、QuaternionPID、VQF、Quaternion、Matrix、RLS
├─ templates/    宿主接入模板（board、app、链接脚本、FreeRTOS 钩子）
├─ tools/        integrate.py：自动接入 CubeMX 工程
├─ docs/         编码规范、器件手册、历史测试记录
└─ CMakeLists.txt
```

---

## 2. 接入要求

| 项目 | 要求 |
| --- | --- |
| 芯片 | STM32H7 系列 |
| 工具链 | CubeMX → Project Manager → Toolchain = **CMake** |
| RTOS | FreeRTOS，**CMSIS_V2**，tick **1 kHz** |
| HAL 时基 | 不用 SysTick；时基中断优先级要高于 UART / CAN 中断 |
| FDCAN | 经典 CAN，RX FIFO0，开 FDCAN 中断 |
| UART | RX DMA + TX DMA + 全局中断；只用中断模式时可只开 UART 中断 |
| SPI | 全双工主机、软件 NSS、`MasterKeepIOState = ENABLE` |
| 链接脚本 | 必须有可加载的 `.dma_buffer` 与 `.noinit` |

库按 `HAL_FDCAN_MODULE_ENABLED` / `HAL_UART_MODULE_ENABLED` / `HAL_SPI_MODULE_ENABLED` 等宏条件编译：没开的外设不会生成对应驱动和任务。

---

## 3. 自动接入

在宿主工程根目录：

```bash
python3 third_party/stm32-framework/tools/integrate.py --dry-run   # 只看将改什么
python3 third_party/stm32-framework/tools/integrate.py             # 执行
```

`integrate.py` 会：

- 检查 CubeMX 前提
- 给链接脚本加 `.dma_buffer` / `.noinit`
- 配置 `FreeRTOSConfig.h` 与 `freertos.c` 的 USER CODE 段
- 复制 `templates/user/board`、`templates/user/app` 到宿主
- 在根 `CMakeLists.txt` 接入库、设置 C++20、收集 `user/**/*.cpp`
- 配置 clangd 的 compile_commands 目录

脚本可重复执行，已完成的步骤会跳过。`board_cfg` 里的句柄、波特率、PWM 参数仍要按实际 CubeMX 工程核对。

---

## 4. 手动接入

### 4.1 CMake

```cmake
add_subdirectory(third_party/stm32-framework)
target_include_directories(stm32_framework PRIVATE user/board)
target_link_libraries(${CMAKE_PROJECT_NAME} stm32_framework)
target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_20)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE user/board user/app user/module)
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE -u _printf_float)

file(GLOB_RECURSE USER_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/user/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/user/*.cpp)
target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${USER_SOURCES})
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE user)
```

`stm32_framework` 是 **OBJECT 库**：HAL / FreeRTOS 的弱符号回调必须直接进入最终链接，不能做成静态库。

### 4.2 `user/board`

从 `templates/user/board/` 复制：

| 文件 | 作用 |
| --- | --- |
| `hardware_config.h` | 唯一配置源：任务开关、优先级、栈深、队列深度、超时 |
| `board_cfg.hpp` | `inline` 外设实例、句柄、参数 |
| `board_cfg.cpp` | 只放 `board_init()` |
| `board_instances.hpp` | `BSP_CAN_INSTANCES` / `BSP_UART_INSTANCES` / `BSP_SPI_INSTANCES` 实例名清单 |

`board_cfg.hpp` 示例：

```cpp
inline BspCanT<16, 16> bsp_can1(
    {.hfdcan = &hfdcan1,
     .name = "CAN1"});

inline __attribute__((section(".dma_buffer"))) BspUartT<uart_mode::dma, 128, 128> bsp_uart1(
    {.huart = &huart1,
     .mode = UartDir::TxRx,
     .baudrate = 115200U});

inline BspPwm bsp_pwm_buzzer(
    {.htim = &htim12,
     .channel = TIM_CHANNEL_2,
     .timer_clk_hz = 275000000UL,
     .prescaler = 23U,
     .period = 1999U});
```

`board_instances.hpp` 必须同步：

```cpp
#define BSP_CAN_INSTANCES(X) X(bsp_can1)
#define BSP_UART_INSTANCES(X) X(bsp_uart1)
#define BSP_SPI_INSTANCES(X) /* 无 SPI 实例 */
```

HAL 回调、`sys_task` 的巡检、`can_rx` / `can_tx` 都按这份清单展开；新增实例只改这里和 `board_cfg.*`。

### 4.3 链接脚本

`.dma_buffer` 必须**可加载**：

```ld
.dma_buffer :
{
  . = ALIGN(32);
  __dma_buffer_start = .;
  *(.dma_buffer)
  *(.dma_buffer*)
  . = ALIGN(32);
  __dma_buffer_end = .;
} >RAM_D1 AT> FLASH
__dma_buffer_load = LOADADDR(.dma_buffer);

.noinit (NOLOAD) :
{
  . = ALIGN(4);
  *(.noinit)
  *(.noinit*)
  . = ALIGN(4);
} >RAM_D1
```

框架在全局构造之前把 `.dma_buffer` 的初值从 Flash 拷进 RAM_D1。写成 `NOLOAD` 会让 `BspUartT` 的句柄 / 波特率、`BspSpi` 的句柄 / DMA 配置上电后变成随机值。CMake 配置阶段会检查：缺段、`NOLOAD`、缺 `AT>` 都直接报错。

### 4.4 入口

`freertos.c` 的 `MX_FREERTOS_Init()` 里调用：

```cpp
system_init();   // 系统状态 + sys / can_rx 任务
board_init();    // 外设实例 init、设备 init、注册 CAN 节点
system_start();  // 有发送节点才建 can_tx，然后置运行位
```

业务任务入口先等运行态：

```cpp
sys_state.wait_running();
```

初始化失败时 `wait_running()` 会删除当前任务，不会跑在半初始化系统上。

---

## 5. 配置

### 5.1 `hardware_config.h`

位置：宿主 `user/board/hardware_config.h`；模板：`templates/user/board/hardware_config.h`。

主要开关：

```c
#define SYS_TASK_ENABLE       1
#define CAN_RX_ENABLE         1
#define CAN_TX_ENABLE         1
#define UART_PATROL_ENABLE    1
```

`HAL_FDCAN_MODULE_ENABLED` / `HAL_UART_MODULE_ENABLED` 未定义时，对应开关会自动置 0。任务优先级、栈深、CAN / UART 队列深度、超时都在这里。

### 5.2 C++20

库目标固定 `CXX_STANDARD 20`。宿主应用目标也要启用：

```cmake
target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_20)
```

---

## 6. 常用接口

### UART

```cpp
BspUartT<uart_mode::dma, 128, 128>   // DMA 收发
BspUartT<uart_mode::it, 128, 32>     // 中断收发
```

收发方向：

```cpp
UartDir::RxOnly
UartDir::TxOnly
UartDir::TxRx
```

DMA 模式的实例必须放 `.dma_buffer`。

### CAN

```cpp
BspCanT<16, 16> bsp_can1(
    {.hfdcan = &hfdcan1,
     .name = "CAN1"});
```

`CanRxNodeT<CanT>` / `CanTxNodeT<CanT>` 负责按 ID 分发和槽位聚合，一般由 `DjiMotor` 等设备内部使用。

### DJI 电机

```cpp
DjiMotor<Motor3508, decltype(bsp_can1)> chassis_lf(bsp_can1, 1U);
configASSERT(chassis_lf.init() == Status::OK);
```

`DjiMotor` 按单个电机创建；`dji_motor_group` 没有并入库。

### 直流电机 + PID 波形

`DeviceMotor` 不绑定串口。`pid_frame()` 只负责组 JustFloat 帧：

```cpp
DeviceMotor motor({.pwm_a = &bsp_pwm1, .pwm_b = &bsp_pwm2, .encoder = &bsp_encoder1, ...});
configASSERT(motor.init() == Status::OK);

uint8_t frame[16];
const size_t n = motor.pid_frame(
    frame,
    DeviceMotor::PID_TARGET | DeviceMotor::PID_OUTPUT);

bsp_uart1.send(frame, n, 0);   // App 层自己发
```

可选字段：

```cpp
DeviceMotor::PID_TARGET
DeviceMotor::PID_FEEDBACK
DeviceMotor::PID_OUTPUT
DeviceMotor::PID_ALL
```

帧格式：`float... + 00 00 80 7F`，返回实际字节数。

### 姿态 PID

```cpp
#include "quaternion_pid.hpp"

QuaternionPID attitude(cfg);
Vector3 omega_ref = attitude.calc(q_ref, q, gyro);
```

标量 PID 仍然只包含 `pid.hpp`。

### 系统状态

```cpp
extern EventState sys_state;

sys_state.wait_running();
sys_state.error(Status::TIMEOUT);       // 运行期错误，向上传播
sys_state.init_error(Status::IO_ERROR); // 初始化失败
```

`set_parent()` 可把子系统挂到父状态上，错误只向上传播，不聚合 running。

### Emm_V5

`protocol/emmv5_protocol.hpp` 只做帧编解码；串口收发由设备层 `DeviceEmmv5GroupT` 或 App 负责。

---

## 7. 库任务

`system_init()` 创建：

- `sys_task`：CAN Bus-Off 恢复、UART 巡检、`Online::update()`
- `can_rx_task`：按 ID 分发 CAN 帧，未认领的进回退缓冲

`system_start()` 检查发送节点，有节点才创建：

- `can_tx_task`：槽位聚合、保底心跳、指令有效期

CAN 接收和发送保持两个独立任务，不合并。

---

## 8. 已知限制

- BSP 没有运行时登记表，`board_instances.hpp` 必须和 `board_cfg.*` 同步；漏写实例不会报错，但对应中断不会分发。
- `BspUartT` 的显式实例化在 `bsp_uart.cpp`；新增 Transport / 缓冲大小时要补一行实例化。
- `DeviceMotor` 不再持有串口，`pid_frame()` 只组帧，发送由 App 做。
- `dji_motor_group`、`corexy_protocol`、`Module` 没有并入库。
- GXS_V1 宿主迁移、全新 CubeMX 工程从零接入、实板验证尚未完成。
- `docs/bsp_test/` 是历史测试记录，里面的旧 API 名称不代表当前接口。

---

## 9. 许可证

MIT，见根目录 [`LICENSE`](LICENSE)。
