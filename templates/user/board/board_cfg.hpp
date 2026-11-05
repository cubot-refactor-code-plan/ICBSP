/**
 * @file board_cfg.hpp
 * @brief 板级实例配置：本板有哪些外设实例、绑定哪些 CubeMX 句柄、参数是什么
 *
 * @details
 *
 * 【板级四个文件的分工】
 *
 *   | 文件                  | 放什么                             | 什么时候改                     |
 *   | --------------------- | ---------------------------------- | ------------------------------ |
 *   | `hardware_config.h`   | 库的全部宏（开关/优先级/队列/超时） | 调库的行为、换板               |
 *   | `board_cfg.hpp`（本文件） | 外设实例（inline 全局变量）与句柄绑定 | 加/删外设、改量程波特率等参数   |
 *   | `board_instances.hpp` | 实例名清单（X-macro）              | **每次加实例都要同步**         |
 *   | `board_cfg.cpp`       | `board_init()`：谁先 init、谁后 init | 改初始化顺序、加设备 init       |
 *
 *   业务任务与更高层的编排在 `user/app/`，本目录只管“板子上有什么、怎么起来”。
 *
 * 【初始化时序（从复位到业务跑起来）】
 *
 *   main()
 *     → HAL_Init / SystemClock_Config
 *     → MX_GPIO_Init / MX_DMA_Init / MX_TIMx_Init / MX_USARTx_UART_Init / MX_FDCANx_Init …（CubeMX 生成）
 *        这些只**配置**外设，不启动收发
 *     → MX_FREERTOS_Init()
 *         → app_init()                       ← user/app/app_main.cpp
 *             1) system_init()               ← 库：建 sys_state、创建 sys / can_rx 任务
 *             2) board_init()                ← **本目录**：各实例 init()、设备 init()、注册 CAN 节点
 *             3) system_start()              ← 库：有发送节点才建 can_tx，然后置运行位
 *     → osKernelStart()                      调度器启动，任务才开始跑
 *
 *   所以 `board_init()` 里的“顺序”只对**初始化**有意义：此时调度器还没跑，
 *   本文件里的 `init()` 都不允许阻塞等事件、也不允许依赖别的任务。
 *
 * 【三条硬规矩（踩了就很难查）】
 *
 *   1. **串口 / SPI 实例必须放 `.dma_buffer` 段，且段名按变量唯一**
 *      H7 的 DMA1/DMA2 访问不到 ITCM / DTCM，实例里的接收/发送缓冲如果是普通全局变量
 *      （被链接到 DTCM），DMA 搬过去就是脏数据或直接卡住。CMake 配置阶段有段检查会拦。
 *
 *      段名必须写成 `.dma_buffer.<变量名>`，**不能只写 `.dma_buffer`**：实例是
 *      C++17 `inline` 变量，多个 TU 各 emit 一份弱定义，段名相同则弱定义里的数据
 *      会一起进同一个输出段（符号去重了、数据没去），表现为段里多出一份无符号的
 *      副本，或者链接器 + `--gc-sections` 剔掉真数据。按变量命名后每个变量独占一个
 *      COMDAT 段，重复副本会被正常丢弃。
 *
 *   2. **`BspUartT` 只有头文件，Transport 与缓冲尺寸随便组合**
 *      DMA 模式的 RX DMA 由 `init()` 自动改成循环模式；CubeMX 里配 Normal 也可以。
 *
 *   3. **本文件每加一个实例，`board_instances.hpp` 必须同步加名字**
 *      串口实例漏写时 `init()` 返回 BAD_ARG；CAN / SPI 实例漏写目前不报错，只是 HAL 回调不会被分发
 *      ——表现是“数据收不到”，不是“编译失败”。
 *
 * 【加一个外设实例的完整步骤（以再加一路 CAN2 为例）】
 *
 *   1. CubeMX：开 FDCAN2 的 RX FIFO0 中断，确认经典 CAN 模式，重新生成
 *   2. 本文件「外设实例」分节下面加：
 *          inline BspCanT<16, 16> bsp_can2({.hfdcan = &hfdcan2, .name = "CAN2"});
 *   3. `board_instances.hpp` 的 `BSP_CAN_INSTANCES` 里加 `X(bsp_can2)`
 *   4. `board_cfg.cpp` 的 `board_init()` 里加一行 `configASSERT(bsp_can2.init() == Status::OK);`
 */

#ifndef __BOARD_CFG_HPP__
#define __BOARD_CFG_HPP__

// CubeMX 生成的句柄头文件：**按实际用到的外设增删**。
#include "bsp_series.hpp" // IWYU pragma: keep（BSP_SERIES_F4）
#include "main.h"         // IWYU pragma: keep
#include "tim.h"
#include "usart.h"

// CAN 句柄头：H7 是 FDCAN，F4 是 bxCAN。
// 没在 CubeMX 里开 CAN 时，连 can.h / fdcan.h 都不会生成，所以整段按模块开关条件化。
#if BSP_CAN_MODULE_ENABLED
#  if BSP_SERIES_F4
#    include "can.h"
#  else
#    include "fdcan.h"
#  endif
#endif

#include "bsp_can.hpp"
#include "bsp_dwt.hpp"
#include "bsp_pwm.hpp"
#include "bsp_uart.hpp"
#include "device_buzzer.hpp"

/** @brief 板级初始化：各实例 init()，实现在 board_cfg.cpp */
void board_init(void);


/* ==================== 外设实例 ==================== */
// 外设实例是 inline 全局变量：整个工程只有一份定义，任何包含本头文件的 TU 都能直接用。
//
// 为什么必须 `constexpr` 构造 + 平凡析构：inline 变量要求常量初始化，
// 否则每个包含本头文件的 .cpp 都会生成一份构造代码，还会注册 atexit。
// 需要释放资源时显式调 `deinit()`，不要写析构函数。

/**
 * CAN1：一条总线的收发（`BspCanT<收队列帧数, 发队列帧数>`）
 *
 * @note 两边的 BspCanT 接口完全一致，只有 Config 字段名不同：H7 写 `.hfdcan = &hfdcan1`，F4 写 `.hcan = &hcan1`。
 */
#if BSP_CAN_MODULE_ENABLED
#  if BSP_SERIES_F4
inline BspCanT<16, 16> bsp_can1(
  {.hcan  = &hcan1,
   .name = "CAN1"}); ///< CAN1，16 帧收 / 16 帧发
#  else
inline BspCanT<16, 16> bsp_can1(
  {.hfdcan = &hfdcan1,
   .name   = "CAN1"}); ///< CAN1，16 帧收 / 16 帧发
#  endif
#endif

/**
 * USART1：调试串口，DMA 收发，115200（`print_uart` 用的就是它）
 */
inline ICBSP_DMA_BUFFER(bsp_uart1) BspUartT<uart_mode::dma, 128, 128> bsp_uart1(
  {.huart    = &huart1,
   .mode     = UartDir::TxRx,
   .baudrate = 115200U});

#define UART_PRINT_ENABLE 1

#if UART_PRINT_ENABLE
#  define print_uart(...) bsp_uart1.printf(__VA_ARGS__)
#else
#  define print_uart(...) ((void)0)
#endif

/**
 * DWT 计时基准：**由库内部初始化**
 *
 * 实例定义在 `stm32-framework/code/service/system.cpp`，与 `system_init()` 里的 `init()` 在同一文件；
 * 这里只 `#include "bsp_dwt.hpp"` 后直接调用（如 `bsp_dwt.get_delta_s(&last)`）。
 * 不要在这里再定义实例，也不要再调 `init()`。
 */

/**
 * 蜂鸣器 PWM：TIM12_CH2
 *
 * @note `timer_clk_hz` 填的是定时器计数器时钟，必须按型号改：H7 常见 275 MHz，F4 的 APB2 定时器是 168 MHz。
 *       填错只会让频率 / 脉宽算偏，不会编译失败。
 */
inline BspPwm bsp_pwm_buzzer(
  {.htim         = &htim12,
   .channel      = TIM_CHANNEL_2,
   .timer_clk_hz = 275000000UL,
   .prescaler    = 23U,
   .period       = 1999U}); ///< 蜂鸣器 PWM：TIM12_CH2


/* ==================== */
/* ==================== 设备实例 ==================== */
// 设备层：包装外设，提供“按物理量”的接口；不拥有下面的外设实例。
// 顺序要求：设备必须在本文件里外设实例之后定义，并且在 board_init() 里外设 init() 之后 init()。

/**
 * 无源蜂鸣器：包装上面的 PWM 通道
 *
 */
inline DeviceBuzzer buzzer(bsp_pwm_buzzer);

/* ==================== */

#endif // __BOARD_CFG_HPP__
