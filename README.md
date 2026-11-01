# stm32-framework：可作为 git submodule 接入的 STM32H7 板级库
![GitHub Release](https://img.shields.io/github/v/release/cubot-refactor-code-plan/stm32-framework)[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
![](https://img.shields.io/badge/chip-STM32H7xx-blue.svg) ![](https://img.shields.io/badge/RTOS-FreeRTOS_CMSIS_V2-green.svg) ![](https://img.shields.io/badge/language-C/C++-blue.svg) ![](https://img.shields.io/badge/ToolChain-CMake_v3.22.1-blue.svg) 

> 分支 `main`。目标：从 DM-MC02 整车工程里提炼出一个库，**任何 STM32H7 CubeMX 工程**都能把它作为 git submodule 接入。
>
> 期望的使用方式：**CubeMX 配置一个新工程 → 接入本 submodule → 修改少量配置 → 编写业务代码。**
>
> 当前进度：阶段 A~D 已完成；`User/` 的 BSP / Service / Device / Protocol（Emm_V5）与 PID 接口已合并进库，运行时登记表、基类、虚函数与 `friend` 已去掉，实例统一由宿主 `board_instances.hpp` 的 X-macro 清单驱动。本仓库只保留库、模板和文档，不内置示例工程；剩阶段 E：用一个全新 CubeMX 工程按第 1 节从零接入验证。

---

## 1. 用法

### 1.1 CubeMX 新建工程时的要求

| 项目 | 要求 | 原因 |
| --- | --- | --- |
| 芯片 | STM32H7 系列 | 驱动用到 FDCAN、DMA Stream、AXI SRAM（RAM_D1） |
| 工具链 | Project Manager → Toolchain = **CMake** | 库以 CMake 子目录方式接入，依赖 CubeMX 生成的 `stm32cubemx` INTERFACE 目标 |
| RTOS | FreeRTOS，**CMSIS_V2** 接口；tick **1 kHz** | 代码里有 `static_assert(configTICK_RATE_HZ == 1000)` |
| FreeRTOS 钩子 | `configCHECK_FOR_STACK_OVERFLOW = 2`、`configUSE_MALLOC_FAILED_HOOK = 1` | 钩子里调库的 `fault_on_stack_overflow()` / `fault_on_malloc_failed()` 记录现场 |
| HAL 时基 | 不用 SysTick（选一个空闲定时器），**该定时器中断优先级要高于串口 / CAN 中断**（数值小于 5，如 4；CubeMX 默认 15） | SysTick 给 FreeRTOS；HAL 在串口中断里调用的 `HAL_DMA_Abort()` 靠 `HAL_GetTick()` 超时，时基优先级低时一旦命中 HAL 的读-改-写竞争就整机卡死（见 3.6） |
| FDCAN | 经典 CAN、RX FIFO0；开中断 0 / 1 | `BspCanT` 用 FIFO0 收、TX FIFO 发、TX-FIFO-EMPTY 中断续发 |
| UART | 需要的串口开 **RX DMA + TX DMA + 全局中断**（只收的串口可不配 TX DMA）；RS485 用 CubeMX 的硬件 DE（`HAL_RS485Ex_Init()`） | `BspUartT` 用 IDLE + DMA 收、DMA 发；不再提供软件 DE |
| TIM PWM | 需要的通道配成 PWM，记下定时器时钟、PSC、ARR | `BspPwm` 的参数全部由用户配置传入 |
| SPI | 全双工主机、软件 NSS、8 bit；**`MasterKeepIOState = ENABLE`**；用 DMA 时开 RX DMA + TX DMA（Normal、Byte）+ SPI 全局中断（优先级 ≥ 5）；片选配成推挽输出、初始高电平并起 Label | `BspSpi` 在一次片选内分几段传输（地址、无效字节、数据），段间 SPE 关闭，KeepIOState 关着时 SCK 浮空会多出时钟沿；完成靠 SPI EOT 中断。只轮询（`use_dma = false`）时可以不配 DMA / 中断 |
| 不用的外设 | 不开即可 | 库按 `HAL_FDCAN_MODULE_ENABLED` / `HAL_UART_MODULE_ENABLED` / `HAL_TIM_MODULE_ENABLED` / `HAL_SPI_MODULE_ENABLED` 条件编译：没开 FDCAN 就没有 CAN 驱动、节点、`DjiMotor` 与 CAN 任务，没开 UART / TIM / SPI 同理（没开 SPI 也就没有 `DeviceBmi088`） |
| GPIO | 按键、片选等按需配置并起 Label | `BspGpio` / `BspKey` 只封装电平读写 |

### 1.2 接入 submodule

```bash
git submodule add git@github.com:cubot-refactor-code-plan/stm32-framework.git Middlewares/Third_Party/stm32-framework # SSH格式
```

**自动接入**：`tools/integrate.py` 按本节与 1.3 完成宿主工程的适配——检查 1.1 的 CubeMX 前提，改链接脚本、`FreeRTOSConfig.h` / `freertos.c` 的 USER CODE 段、根 `CMakeLists.txt`，复制 `templates/user/board`、`templates/user/app`、`templates/user/app/task` 与 `hardware_config.h` 到 `user/board`，建 `user/board`、`user/app/task`、`user/module`，并配置 clangd（见本节末条）。`app_init()` 只调 `system_init()` / `board_init()` / `system_start()`；`StartDefaultTask()` 在 `user/app/app_main.cpp` 空循环。所有片段从 `templates/` 读取；每步先判断是否已完成，可重复运行；只改内容仍是 CubeMX 默认的 USER CODE 段，已有自定义内容就报错并指出要手动合并的位置；有任何错误就不写任何文件；保持各文件原行尾。

```bash
python3 third_party/stm32-framework/tools/integrate.py --dry-run   # 在宿主工程根目录：只检查并打印 diff
python3 third_party/stm32-framework/tools/integrate.py             # 执行；--help 看 --board-dir / --app-dir / --user-dir / --linker-script / --ram-region 等选项
```

复制来的 `board_cfg.*` 是 DM-MC02 的句柄与参数：脚本对照本工程 CubeMX 生成的代码，提示不存在的句柄、波特率与 PSC / ARR 不一致，但定时器时钟与具体用哪些外设仍需手动改。2026-10-17 实测：在接入前的全新 CubeMX 工程上运行，结果与按下文手动接入一致（只有链接脚本与 `freertos.c` 里的注释措辞不同），编译 0 warning；再次运行全部跳过。

手动接入的步骤如下。在 CubeMX 生成的根 `CMakeLists.txt` 末尾接入库，并加上自己的应用源文件：

```cmake
add_subdirectory(third_party/stm32-framework)
target_include_directories(stm32_framework PRIVATE user/board)  # 库自身也要看到 hardware_config.h
target_link_libraries(${CMAKE_PROJECT_NAME} stm32_framework)
target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_20)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE user/board user/app user/module)
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE -u _printf_float)  # BspUartT::printf 打印浮点需要（nano.specs）

# 业务代码：user/ 下的 .c / .cpp 自动收集，新增业务文件不用改 CMake
file(GLOB_RECURSE USER_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/user/*.c ${CMAKE_CURRENT_SOURCE_DIR}/user/*.cpp)
target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${USER_SOURCES})
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE user)
```

- 库按 **C++20** 编译，并链接 `stm32cubemx` 取 HAL / FreeRTOS 的头文件与宏，所以宿主工程不需要再配 include 路径；应用目标也要启用 C++20（`target_compile_features(... cxx_std_20)`，`integrate.py` 会加）。
- `stm32_framework` 是 **OBJECT 库**：库里有覆盖 HAL / FreeRTOS 弱符号的强定义（`HAL_*Callback`、钩子调用的 `fault_on_*`），做成静态库时这些成员不会被链接器拉进来，会静默失效。
- 仓库根不能单独配置（会报错提示）；必须在 `add_subdirectory(cmake/stm32cubemx)` 之后接入。
- 库只包含 `stm32h7xx_hal.h`，不包含 CubeMX 按外设生成的 `main.h` / `fdcan.h` / `usart.h` / `tim.h`，也不引用任何板级实例；句柄与引脚宏由用户的板级配置自己包含。
- 库目录不在宿主工程之下时，`add_subdirectory` 的第二个参数用来指定 binary 目录。
- **clangd**：CubeMX 的 CMake 预设把 `compile_commands.json` 放在 `build/Debug/`，clangd 默认只在祖先目录及其 `build/` 下找，找不到就按主机 x86 参数解析，宿主代码报 `fdcan.h` 找不到、`BspCanT` / `hfdcan1` 未定义。解决：宿主工程 `.vscode/settings.json` 用命令行参数指定数据库，并让 clangd 向交叉编译器查询系统头文件路径（先 `cmake --preset Debug` 一次）：

  ```json
  "clangd.arguments": [
      "--compile-commands-dir=${workspaceFolder}/build/Debug",
      "--query-driver=/opt/arm-none-eabi/bin/arm-none-eabi-*"   // 改成本机工具链路径
  ]
  ```

  2026-10-17 在全新 CubeMX 宿主工程上实测（clangd 22.1.6 `--check`）：宿主 `Core/`、`user/` 与库源文件均 0 诊断。

### 1.3 修改的配置（模板与片段都在 `templates/`，索引见 `templates/README.md`）

1. **链接脚本**加 `.dma_buffer` 与 `.noinit` 两段（`templates/linker_sections.ld`，放在 `SECTIONS` 里 `.data` 之前）：H7 的 DMA1 / DMA2 访问不到 DTCM，而 CubeMX 默认 `.bss` 在 DTCM，所以串口 / SPI 实例要放进 AXI SRAM（0x24000000 起；H723 的链接脚本叫 RAM_D1，H743 / H750 叫 RAM，`integrate.py` 按地址识别）；`fault_record` 要跨复位保留。
   - `.dma_buffer` 必须是**可加载段**（`>RAM_D1 AT> FLASH` + `__dma_buffer_load = LOADADDR(.dma_buffer)`）：实例里的句柄 / 波特率等是 constexpr 初值，框架在全局构造前把它们从 Flash 拷进 RAM_D1。写成 `NOLOAD` 时上电后配置是随机值，串口直接起不来。
   - CubeMX 新建工程的链接脚本没有这两段。库的 CMake 在配置阶段检查 `CMAKE_EXE_LINKER_FLAGS` 里 `-T` 指定的链接脚本，缺段、`NOLOAD` 或没有 `AT>` 都直接报错；链接脚本登记为配置依赖，改了下次构建自动重查。
   - CubeMX 6.18.1 重新生成时**保留**已有链接脚本的内容（2026-10-17 实测：两段与手改的 `MEMORY` 都在，只改了几行行尾）；其他版本未验证，靠上面的检查兜底。
   - 串口实例忘了放进 `.dma_buffer`（落在 DTCM / ITCM）时 `BspUartT::init()` 返回 `BAD_ARG`，`configASSERT` 停机并记下行号——不检查的话系统照常运行、串口静默收不到数据（实测）。

2. **`FreeRTOSConfig.h` 的 USER CODE 1 段**把 `configASSERT` 接到故障记录（`templates/FreeRTOSConfig_user_code.h`）：

   ```c
   void fault_on_assert(const char *file, int line);
   void fault_on_stack_overflow(const char *task_name);
   void fault_on_malloc_failed(void);
   #define configASSERT( x ) if ((x) == 0) { fault_on_assert(__FILE__, __LINE__); }
   ```

3. **`freertos.c`**（`templates/freertos_user_code.c`）：USER CODE 4 / 5 的钩子分别调 `fault_on_stack_overflow(pcTaskName)`、`fault_on_malloc_failed()`；`MX_FREERTOS_Init()` 的 USER CODE Init 段调 `app_init()`。

4. **板级实例配置**（`templates/user/board/board_cfg.hpp` / `board_cfg.cpp`；实例写在 `.hpp` 的 inline 变量里，`.cpp` 只保留 `board_init()`）：

   ```cpp
   #include "fdcan.h"   // 本工程 CubeMX 生成的句柄（库不包含这些头文件）
   #include "tim.h"
   #include "usart.h"
   #include "bsp_can.hpp"
   #include "bsp_pwm.hpp"
   #include "bsp_uart.hpp"

   // 哪些外设、用哪个句柄、波特率 / 定时器参数——全部由用户决定，库里不写死任何板级常量。
   // 实例用 C++20 inline 变量定义在 board_cfg.hpp（不再需要 extern + .cpp 定义）。
   inline BspCanT<16, 16> bsp_can1({.hfdcan = &hfdcan1, .name = "CAN1"});
   inline __attribute__((section(".dma_buffer")))
   BspUartT<uart_mode::dma, 128, 128> bsp_uart1(
       {.huart = &huart1, .mode = UartDir::TxRx, .baudrate = 115200U});
   inline BspPwm bsp_pwm_buzzer({&htim12, TIM_CHANNEL_2, 275000000UL, 23U, 1999U});

   void board_init(void)
   {
     // HAL 回调与库任务按 board_instances.hpp 的清单比对句柄，不再有运行时登记表
     configASSERT(bsp_can1.init() == Status::OK);       // 每条 init() 单独一行断言，失败时记录里的行号直接指出是哪一路
     configASSERT(bsp_uart1.init() == Status::OK);
     configASSERT(bsp_pwm_buzzer.init() == Status::OK);
   }
   ```

   新增 / 删除 CAN、UART、SPI 实例时，还要同步 `user/board/board_instances.hpp` 的 `BSP_*_INSTANCES` 清单；库的 HAL 回调和任务只按清单展开，清单里没有的实例收不到中断。

5. **初始化入口**（`templates/user/app/app_main.cpp`；库入口声明在 `service/system.hpp`，C 文件也能调用）：

   ```cpp
   extern "C" void StartDefaultTask(void *argument)  // 覆盖 freertos.c 的 weak 实现
   {
     (void)argument;
     for (;;) { vTaskDelay(pdMS_TO_TICKS(1000)); }   // 空循环，保留任务
   }

   extern "C" void app_init(void)   // freertos.c 的 MX_FREERTOS_Init() 里调用（调度器启动前）
   {
     system_init();   // 建系统事件组与 sys / can_rx
     board_init();    // 外设实例 init()、设备 init()、注册 CAN 节点
     system_start();  // 有发送节点才建 can_tx，然后置运行位
   }
   ```

   业务代码放 `user/app/task/`：任务入口第一句 `sys_state.wait_running()`，需要时在 `StartDefaultTask()` 里 `xTaskCreate`；`app_task.hpp` 声明的 `heartbeat` / `monitor` 只是模板，不自动创建。

   库任务都在 `tasks/`，1 kHz：`sys_task`（CAN Bus-Off 恢复、串口巡检、Online），`can_rx_task`（按 ID 分发），`can_tx_task`（槽位聚合、保底心跳、指令有效期；`system_start()` 按 `CanTxNode::count()` 决定是否创建）。

6. **必须：`hardware_config.h`**（`templates/hardware_config.h` 复制到 `user/board/`）。所有可调宏都在这里直接写成实际值，不再有默认值头文件；顶部已包含 `stm32h7xx_hal.h` / `FreeRTOS.h`，库和用户代码都包含同一份文件。越界在编译时报 `static_assert`。

   功能开关：`SYS_TASK_ENABLE`、`CAN_RX_ENABLE`、`CAN_TX_ENABLE`、`UART_PATROL_ENABLE`。`HAL_FDCAN_MODULE_ENABLED` 未定义时自动把 CAN 收 / 发开关置 0，`HAL_UART_MODULE_ENABLED` 未定义时自动关闭串口巡检，不需要再改库代码。宿主需要给 `stm32_framework` 加 `user/board` 包含路径（`integrate.py` 会自动加）。

   | 宏 | 默认 | 含义 |
   | --- | --- | --- |
   | `SYS_TASK_PRIORITY` / `_STACK` | `tskIDLE_PRIORITY + 7` / 256 字 | 系统维护任务 |
   | `CAN_TASK_PRIORITY` / `_STACK` | `tskIDLE_PRIORITY + 8` / 512 字 | CAN 收 / 发任务（接收节点回调在 CAN 接收任务里执行） |
   | `CAN_RX_QUEUE_FRAMES` / `_RX_RETURN_FRAMES` / `_TX_QUEUE_FRAMES` | 16 / 16 / 8 | 每路 CAN 的接收、回退（无人认领的帧）、发送队列深度（接收两项 2026-10-17 由 8 提到 16：1 Mbps 满载会溢出） |
   | `CAN_RX_DISPATCH_PER_TICK` | 16 | CAN 接收任务每 1 ms 每路最多分发的帧数（原写死 8，低于 1 Mbps 满载帧率） |
   | `CAN_RX_NODE_MAX` / `_TX_NODE_MAX` | 50 / 50 | 收 / 发节点总数上限（≤ 255） |
   | `CAN_CMD_EXPIRE_MS` | 100 | 指令有效期：槽位这么久没 `FillData()` 就清零（必须大于心跳） |
   | `CAN_TX_HEARTBEAT_MS` | 5 | 保底心跳：发送节点至少这么久整帧发一次 |
   | `CAN_BUSOFF_RETRY_MS` | 100 | Bus-Off 恢复重试间隔 |
   | `UART_PATROL_PERIOD_MS` | 10 | 串口补救巡检周期 |
   | `UART_PRINTF_TIMEOUT_MS` | 10 | `BspUartT::printf()` 取锁 / 入队等待上限 |

   在线超时按对象在构造时给出，不是全局配置。

### 1.4 编写业务代码

```cpp
DjiMotor<Motor3508, decltype(bsp_can1)> chassis_lf(bsp_can1, 1U); // board_init() 里 init()：注册收发节点

extern "C" void chassis_task(void *)
{
  sys_state.wait_running();                     // 等系统进入运行态；初始化失败时任务自行退出
  for (;;)
  {
    const MotorData &d = chassis_lf.data();     // 反馈由 CAN 接收任务按 ID 分发、解包
    (void)chassis_lf.FillData(1000);            // 只写发送槽位，CAN 发送任务统一发出；100 ms 不刷新自动清零
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}
```

---

## 2. 库包含什么（当前状态）

依赖方向：`App → Module → Device → Bsp`，`Service` / `Algorithm` 贯穿各层；下层不包含上层头文件。

| 层 | 模块 | 功能 | 实机状态 |
| --- | --- | --- | --- |
| Bsp | `bsp_can` | FDCAN 收发（MessageBuffer）、DLC > 8 帧丢弃计数、Bus-Off 非阻塞恢复 | DLC 过滤：内部回环实测；Bus-Off 恢复：2026-09-30 实测 |
| Bsp | `bsp_uart` | IDLE + DMA 收、多任务安全发（写锁 + printf 锁）、发送卡死续发、接收停摆重建 | 串口跳线实测（2026-10-16） |
| Bsp | `bsp_pwm` | 通用 PWM：占空比 / 脉宽 / 频率，参数全由配置传入 | 蜂鸣器通道实测频率 |
| Bsp | `bsp_spi` | SPI 主机：总线锁 + 片选设备，寄存器读写约定（读标志位、读时无效字节），短传输轮询、长传输 DMA，DTCM 缓冲经弹跳缓冲中转并分段 | BMI088 实测（2026-10-28，3.7） |
| Bsp | `bsp_key` / `bsp_gpio` / `bsp_encoder` / `bsp_dwt` | 轮询消抖按键 / GPIO 电平 / 编码器 / 内核 CYCCNT 计时 | 按键、编码器实测 |
| Service | `status` | 统一状态码、`EventState`（可多实例、父子错误向上传播）、等待运行态 | 实测 |
| Service | `fault_record` | 断言 / 栈溢出 / 分配失败现场记录，`.noinit` 跨复位保留 | 实测 |
| Service | `can_rx_node` / `can_tx_node` | CAN 按 ID 分发接收、槽位聚合发送、100 ms 指令有效期 | 回环实测 |
| Service | `online_check`、`motor_definition`、`imu_definition` | 在线计时、电机通用数据、IMU 通用数据 | 编译通过（`imu_definition` 经 BMI088 实测） |
| Device | `dji_motor` | M3508 / M2006 / GM6020，每电机一个对象 | C620 + M3508 实测（2026-09-01 / 10-01，当时版本；本轮改动后未接电机复测） |
| Device | `device_motor` / `device_servo` / `device_r9ds` / `device_emmv5_group` | 直流电机（PWM + 编码器 + 速度环）、舵机、R9DS、Emm_V5 步进组 | 编译通过 |
| Device | `device_buzzer` | 无源蜂鸣器（音调 / 音量 / 时长） | 实测 |
| Device | `device_bmi088` | BMI088 六轴 IMU（SPI）：上电配置与回读校验、读数换算到 `ImuData`；默认由内部任务按陀螺仪数据就绪自动刷新（默认 1 kHz），并用 VQF 解算姿态（四元数 / 欧拉角）；`snapshot()` 取同一帧；饱和标志 / 计数；不驱动加热电阻 | DM-MC02 板载 BMI088 实测（2026-10-28，3.7）；VQF 姿态只编译验证 |
| Algorithm | `pid` | 位置式 PID（`PID::get()` 取目标/反馈/原始输出） | 已测 |
| Algorithm | `vqf` | [VQF开源页面](https://github.com/dlaidig/vqf), MIT License |  |
| Algorithm | `matrix` | 定长单精度矩阵 `MatrixF32<ROWS, COLS>`：四则运算、转置、求逆（奇异时返回 false，相对阈值）、向量模长 / 归一化 / 点乘 / 叉乘、轴角 / Z-Y-X 欧拉角旋转矩阵。移植自 [yssickjgd/damiao_mc02_bsp](https://github.com/yssickjgd/damiao_mc02_bsp)（USTC-RoboWalker，经作者授权），**不适用本库的 MIT 许可证**；改动见文件头 | 主机侧与原作逐项对照（5000 组随机输入）、修复项与误用编译报错检查 |
| Algorithm | `rls` | 带遗忘因子的递归最小二乘 `Rls<X_DIM, Y_DIM>`（多输出共用回归向量）：参数检查、tr(P) 上限防协方差爆炸、非有限样本丢弃、P 对称化与失正定重置，均计数 | 主机侧与双精度参考实现对照、静态辨识 / 参数突变跟踪 / 零激励 10 万步 / 单方向激励 / 100 万步长跑 |
| Algorithm | `quaternion` | 姿态四元数运算：乘法 / 共轭、旋转向量、姿态误差（`error_to()`，无奇异点）、角速度积分、slerp、云台指向角（`pointing()`）、Z-Y-X 欧拉角、角度折回 | 主机侧数值自测（与参考公式、VQF 输出对照）, 暂未实测 |
| Service | `system` | 库入口 `system_init()` / `system_start()`；任务体在 `tasks/`（`sys` / `can_rx` / `can_tx`） | 内部回环 + 串口跳线回归 |

已按 main 移除（2026-10-16）：TinyUSB / `bsp_usb`、QSPI 外置 Flash 与 XIP、DataPack / `protocol_uart`、LCD 与菜单、达妙电机空声明、ADC1 与 CMSIS-DSP、`bsp_buzzer`（换成 `bsp_pwm` + `device_buzzer`）。2026-10-31 合并 `User/`：BSP / Service / Device / Emm_V5 编解码与 PID 接口进库；`dji_motor_group` 与 `corexy_protocol` 未并入库。

---

## 3. 设计

### 3.1 目录结构

```txt
stm32-framework/                    仓库根 = 库
├─ CMakeLists.txt             stm32_framework（OBJECT 库）；链接 stm32cubemx（INTERFACE）取 HAL / FreeRTOS
├─ bsp/                       bsp_can、bsp_uart、bsp_spi、bsp_pwm、bsp_key、bsp_gpio、bsp_encoder、bsp_dwt
├─ service/                   system（库入口）、status、fault_record、can_rx_node / can_tx_node、online_check、motor_definition、imu_definition
├─ tasks/                     库任务：sys_task、can_rx_task、can_tx_task（system_init/system_start 创建）
├─ device/                    dji_motor、device_buzzer、device_bmi088、device_motor、device_servo、device_r9ds、device_emmv5_group
├─ protocol/                  emmv5_protocol（Emm_V5 帧编解码）
├─ algorithm/                 pid、vqf（姿态解算）、quaternion、matrix、rls
├─ templates/                 接入用：hardware_config.h、user/board/board_cfg.* / board_instances.hpp、user/app/app_main.*、user/app/app_task.hpp、user/app/task/demo_task.cpp、FreeRTOSConfig.h / freertos.c USER CODE 片段、链接脚本片段
├─ tools/                     integrate.py：按 templates/ 自动接入宿主工程（1.2）
├─ docs/                      编码规范、器件手册、bsp_test/（UART / RS485 / CAN 实机测试记录 3.6，SPI / BMI088 3.7）

```



### 3.2 约束与限制

- 只支持 STM32H7（FDCAN、DMA Stream、RAM_D1）。
- 假定 **D-Cache 关闭**；开启时 `.dma_buffer` 要配成 MPU 非缓存区。
- 需要 FreeRTOS + CMSIS-RTOS v2；C++20；tick 1 kHz。
- `BspUartT` 在 `init()` 里打开 UART 硬件 FIFO（2 Mbps 以上连续接收必需）。卡死恢复中止发送时，DMA FIFO 预读的字节与 UART TX FIFO 里的字节会丢（只在卡死恢复路径上）。
- `BspUartT` 的接收流缓冲区是模板参数的 2 倍（堆上），`Config::baudrate` 只用来估算卡死阈值，填错不再导致误判。
- CAN：经典帧、8 字节数据帧；远程帧由硬件过滤器拒收，DLC > 8 的帧丢弃计数。TX 支持 CubeMX 的 FIFO 与 Queue 两种模式；
  总线被更高优先级的流量占满时，本节点排在硬件 TX 里的低优先级帧（例如发送节点的周期帧）会一直发不出去并占住 TX 缓冲，
  后来的高优先级帧也进不去（实测见 `docs/bsp_test/can1_can2.md` C06c/C06d）。
- `BspSpi`：同一总线上的设备共用 CubeMX 的 CPOL / CPHA / 速率；一次片选内分段传输，CubeMX 里 `MasterKeepIOState` 必须为 ENABLE；
  总线锁（互斥量）不按先来后到，和高优先级任务共用总线的低优先级任务应自己传更长的 `timeout_ms`（默认 `SPI_TRANSFER_TIMEOUT_MS` = 10 ms，实测争用下最低优先级者等过 9.3 ms 后超时）。
  传输接口只能在任务里调用；用 DMA 的实例放进 `.dma_buffer`，DTCM 里的缓冲经弹跳缓冲中转（按 `SPI_DMA_BUFFER_SIZE` 分段）。
- `DeviceBmi088`：默认加速度计 ±24 g（±6 g 在剧烈晃动下实测饱和）、陀螺仪 ±2000 °/s（芯片上限，剧烈晃动实测峰值 1934 °/s）；饱和帧由 `Bmi088RawData::saturated` 与
  `acc_saturation_count()` / `gyro_saturation_count()` 给出。不驱动板上的加热电阻（DM-MC02 上是 PB1 / TIM3_CH4，误开容易损坏硬件），温控属于上层。
  数据就绪中断由宿主的 `HAL_GPIO_EXTI_Callback` 转调（库不占用它）。默认 `Config::auto_update = true`：`init()` 建内部任务 "imu"（`BMI088_TASK_PRIORITY` / `BMI088_TASK_STACK`），
  每次陀螺仪数据就绪刷新一帧（没有中断时每 10 ms）；默认陀螺仪 1000 Hz（`HZ1000_BW116`），约占 CPU 7.5%~14%，改 2000 Hz 翻倍；自动刷新的对象必须存活到系统停止，临时对象用 `auto_update = false`。
  默认 `Config::attitude = true`：内部任务每帧用 VQF（`algorithm/vqf.*`，dlaidig/vqf，MIT，见 `LICENSES/MIT.txt`，版权在文件头 SPDX 行）做六轴姿态解算，写 `ImuData` 的四元数 / 欧拉角（Z-Y-X，芯片本体坐标，yaw 零点任意、无磁力计会慢漂）；
  按陀螺仪 ODR 周期积分，所以只在数据就绪中断唤醒的帧上解算（没转调 EXTI 时姿态不更新），漏帧按最新一帧补积分并计入 `attitude_skip_count()`；手动模式不解算。任务栈默认 512 字。
- HAL V1.11.6 `HAL_DMA_Abort()` 读-改-写竞争（3.6）：HAL 时基优先级须高于串口中断；即使这样，-O2 构建下偶有 5 ms 中断内等待与少量丢数，根治（接收改 DMA 循环模式）待做。
- 运行期可恢复的失败返回 `Status`，初始化失败一律 `configASSERT` 停机并写 `fault_record`（见 `docs/编码规范.md` 第 8 节）。

### 3.3 实机测试（2026-10-17）

在宿主工程 `MC02_with_submodule`（DM-MC02，全新 CubeMX 工程按第 1 节接入）上对 UART（U7 ⇄ U10）、RS485（U2 ⇄ U3，硬件 DE）、CAN（CAN1 ⇄ CAN2，无终端电阻）
做了常规、边界、满载、故障注入、浸泡与物理插拔测试，Debug（-O0）与 -O2 各跑。完整记录（用例 / 目的 / 方案 / 现象 / 结论、原始数据、改动清单）在 **`docs/bsp_test/`**（宿主工程同名目录的副本）。

结果：

- UART：921600 双向满负荷无损（约 90 KB/s）；连续流 921600~4 Mbps 无损；帧错误 / 溢出 / DMA 停流 / 发送卡死注入后自恢复；拔插接收线自恢复。
- RS485：收发器发送时不回波；请求-应答往返 4.5 ms（115200）；总线冲突、拔插 A 线后自恢复；4 Mbps 连续流无损。
- CAN：1 Mbps 无终端电阻双向无错；双向满载无丢帧；20 轮真实 Bus-Off 全部自恢复（最长 102 ms）；对端离线只到 error-passive；拔插 CAN_H 自恢复；-O2 浸泡 5 min 双向各 30 万帧无丢失。

据此修复的库问题（提交 `d32a996`，详见 `docs/bsp_test/changes.md` 第 3 节）：

| 模块 | 问题 | 修复 |
| --- | --- | --- |
| `BspUartT` | 运行中再 `init()` 返回 IO_ERROR、实例失效 | 先停收发、强制停 DMA 流并复位句柄，再重建 |
| `BspUartT` | 绑到没配 DMA 的串口时 `init()` 返回 OK、永远收不到 | 返回 BAD_ARG |
| `BspUartT` | UART 硬件 FIFO 关着，2 Mbps 以上连续接收几乎全丢（ORE） | `init()` 打开 FIFO（`UART_ENABLE_FIFO`，默认 1） |
| `BspUartT` | 接收流缓冲区与 DMA 缓冲一样大，写满投递后紧接的帧尾被丢（921600 下确定性丢帧） | 接收流缓冲区 2 倍 |
| `BspUartT` | HAL 在 IDLE 时先读计数再停 DMA，中间到达的字节不计入，静默丢 1 字节 | 停流后按剩余计数重算长度 |
| `BspUartT` | 卡死判定依赖 `Config::baudrate`，填错时误判中止，开 FIFO 后还丢数据 | 改为看 DMA 计数是否在动、UART 是否仍在移位 |
| `BspUartT` | `uart_irqn()` 缺 USART2 / USART6 | 补上 |
| `BspCanT` | DLC < 8 的帧 DLC 之后带出栈上旧数据 | 按 DLC 拷贝 |
| `BspCanT` | 远程帧进入应用（HAL 的 `FDCAN_FILTER_REMOTE` 含义是接收） | 硬件拒收 |
| `BspCanT` | 回退缓冲满丢帧无计数；接收队列与每 tick 分发（写死 8）低于 1 Mbps 满载帧率 | 新增 `rx_return_dropped`；默认 16，新增 `CAN_RX_DISPATCH_PER_TICK` |
| `BspCanT` | TX 配成 Queue 模式时一帧都发不出（TFFL 在队列模式恒为 0） | 改用 TFQF 判断 |

仍未解决：

- HAL V1.11.6 `HAL_DMA_Abort()` 先清 EN、紧接着读-改-写 CR 时可能把 EN 写回，流重新使能后 HAL 死等；在串口 IDLE 中断里且 HAL 时基优先级低于串口时整机卡死（-O2 必现）。
  宿主工程已把时基优先级改为 4 缓解（见 1.1），-O2 浸泡 16 min 不再卡死，但命中时仍有 5 ms 等待与少量丢数（0.04%）。根治：接收改 DMA 循环模式（待做）。
- 总线被更高优先级流量占满时，本节点的周期帧占满 TX 缓冲，高优先级帧也进不去（设计层面，未改）。

### 3.4 SPI / BMI088 实机测试（2026-10-27 ~ 10-28）

在宿主工程 `MC02_with_submodule` 上测试新增的 `bsp_spi`（`BspSpi` / `BspSpiDevice`）与 `device_bmi088`（输出 `imu_definition` 的 `ImuData`），板载 BMI088 挂在 SPI2 上
（RX / TX DMA、SPI 中断、`MasterKeepIOState = ENABLE`）。完整记录在 **`docs/bsp_test/spi_bmi088.md`**，改动清单在 `docs/bsp_test/changes.md` 第 6、7 节。全程未打开加热电阻。

结果：

- Debug 自动用例 20 / 20：错误路径、芯片 ID、读时无效字节、配置回读、量程换算、数据就绪中断 / 任务通知、轮询与 DMA、零拷贝与弹跳缓冲分段、DMA 超时注入恢复（100 次）、反复 `init()`；
  2 kHz 读取 10 min 120 万帧不漏帧；4 个优先级不同的任务同时访问两颗芯片 10 min 0 错误、0 数据不一致。
- -O2：基本用例全部通过；4 任务争用浸泡 30 min（360 万帧）0 错误、0 不合理帧。
- -O2 轮中出现过的加速度“不合理帧”经碰板对照确认是板子被碰造成的（不碰的 10 min 为 0，碰板 5 min 为 3869 帧）；剧烈晃动下 SPI 0 错误、不漏帧。

据此的库改动：

| 编号 | 问题 | 修复 |
| --- | --- | --- |
| L-SPI1 | `transfer_locked()` 只要 tx 或 rx 为空就两边都走弹跳缓冲，`read_reg()` 永远不能零拷贝 | 两个方向各自判断能否直接用；`bounce_cnt` 只计调用方缓冲 DMA 访问不到的段 |
| L-BMI1 | ±6 g 在剧烈晃动下饱和（实测 25 帧被截断），±24 g 在 16.6 g 冲击下 0 饱和 | `DeviceBmi088::Config` 默认 ±24 g；新增饱和标志与计数 |
| L-SPI2 | 低优先级任务在总线争用下可能等锁超过默认 10 ms | 默认值不变，文档写明由低优先级使用者传更长的 `timeout_ms`；`DeviceBmi088::update()` 增加 `timeout_ms` 参数（只编译验证） |
| L-BMI2 | `DeviceBmi088` 是拉取式，正常固件里没人调 `update()`，数据一直是 0 | 默认由内部任务自动刷新（参照 `DjiMotor` 由库任务刷新），`snapshot()`、`update_count()` / `update_fail_count()`；实测 1 s 刷新 2013 次 = 中断数、0 失败 |

未覆盖：±24 g 默认配置下的全量自动用例（只在量程用例与晃动对比里跑过）、D-Cache 开启、SPI 时钟高于 4.3 MHz、多路 SPI。

---

## 4. 开发约定

- 编码规范：`docs/编码规范.md`（命名、注释、`Config` 聚合 + 指定初始化、分层、断言与初始化失败）。格式由 `.clang-format` 固定，手动执行。
- C 调 C++ 只通过 `app_main.h` 转接一次（`main.c` / `freertos.c` → `app_init()` / 任务入口），HAL 与 FreeRTOS 回调用 `extern "C"`；库入口 `system.hpp` 本身是 `extern "C"`，C 文件也能直接调用。
- 间接包含的头文件行尾加 `// IWYU pragma: keep`，避免 clangd 误报。
- 很多文件是 CRLF 行尾：修改时保持原行尾，提交前用 `git diff --numstat` 与 `git diff --ignore-cr-at-eol --numstat` 对比，确认没有整文件的行尾改动。
- CubeMX 重新生成后看一眼 `git status`：6.18.1 会改写链接脚本几行的行尾（内容不变，`git checkout` 还原即可），并拷回 `Drivers/CMSIS/DSP/`（已忽略）；链接脚本缺段时 CMake 配置阶段会直接报错。
