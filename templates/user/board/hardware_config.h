/**
 * @file hardware_config.h
 * @brief 本板的唯一配置入口：库的全部可调宏、功能开关与取值检查都集中在这里
 *
 * @details
 *
 * 【它是什么】
 *   stm32_framework 里**没有**任何“默认值头文件”。库的每一个可调参数都是本文件里的 `#define`，
 *   库源码通过 CMake 加的包含路径直接 `#include "hardware_config.h"`。
 *   CMake 配置阶段会打印实际用的是哪一份：
 *
 *       -- stm32_framework: 使用配置 /…/user/board/hardware_config.h
 *
 *   所以“改库的行为”= 改本文件的宏，不用改库源码；换板 = 换一份本文件。
 *
 * 【四组宏怎么理解】
 *   ① 功能开关：决定“这段代码编不编译、这些任务建不建”——关掉省 Flash/RAM，但功能就没了
 *   ② 库任务：优先级与栈深——决定实时性和会不会栈溢出
 *   ③ 外设参数：队列深度、超时、缓冲区大小——决定吞吐、丢帧与阻塞时长
 *   ④ 取值检查：只做编译期断言，不改任何行为
 */

#ifndef HARDWARE_CONFIG_H
#define HARDWARE_CONFIG_H

#include "FreeRTOS.h"      // IWYU pragma: keep（configMAX_PRIORITIES）
#include "stm32h7xx_hal.h" // IWYU pragma: keep（HAL_*_MODULE_ENABLED）
#include "task.h"          // IWYU pragma: keep（tskIDLE_PRIORITY）

/* ==================== ① 功能开关：关掉就不编译对应代码、也不创建对应任务 ==================== */

/** @brief 系统维护任务 sys_task：CAN Bus-Off 恢复、串口补救、Online 计时 */
#define SYS_TASK_ENABLE 1
/** @brief CAN 接收路径：can_rx_task + CanRxNode */
#define CAN_RX_ENABLE 1
/** @brief CAN 发送路径：can_tx_task + CanTxNode */
#define CAN_TX_ENABLE 1
/** @brief sys_task 里的串口补救巡检 */
#define UART_PATROL_ENABLE 1

/* HAL 没开对应模块时强制关闭，避免任务引用不存在的类型（例如换板时 CubeMX 里没开 FDCAN） */
#ifndef HAL_FDCAN_MODULE_ENABLED
#  undef CAN_RX_ENABLE
#  undef CAN_TX_ENABLE
#  define CAN_RX_ENABLE 0
#  define CAN_TX_ENABLE 0
#endif

#ifndef HAL_UART_MODULE_ENABLED
#  undef UART_PATROL_ENABLE
#  define UART_PATROL_ENABLE 0
#endif

/* ==================== */
/* ==================== 库任务（system_init() 创建） ==================== */

/** @brief 系统维护任务 sys_task 的优先级：高于业务任务，低于 CAN 收发任务 */
#define SYS_TASK_PRIORITY (tskIDLE_PRIORITY + 7)
/** @brief sys_task 栈深（字） */
#define SYS_TASK_STACK 256

/** @brief CAN 收 / 发任务的优先级：最高，1 kHz 节拍内必须跑完 */
#define CAN_TASK_PRIORITY (tskIDLE_PRIORITY + 8)
/** @brief CAN 收 / 发任务栈深（字）：接收节点回调在 can_rx_task 里同步执行，回调用栈多就加大 */
#define CAN_TASK_STACK 512

/* ==================== */
/* ==================== CAN ==================== */

/** @brief 每路 CAN 的接收队列深度（帧）：ISR 写入，can_rx_task 每毫秒取走 */
#define CAN_RX_QUEUE_FRAMES 16
/** @brief 每路 CAN 的回退队列深度（帧）：没有接收节点认领的帧，供 BspCan::receive() 读取 */
#define CAN_RX_RETURN_FRAMES 16
/** @brief can_rx_task 每 1 ms 每条总线最多分发的帧数（1~64） */
#define CAN_RX_DISPATCH_PER_TICK 16
/** @brief 每路 CAN 的发送队列深度（帧） */
#define CAN_TX_QUEUE_FRAMES 8
/** @brief 接收节点总数上限（全部总线合计，≤255） */
#define CAN_RX_NODE_MAX 50
/** @brief 发送节点总数上限（全部总线合计，≤255） */
#define CAN_TX_NODE_MAX 50
/** @brief 控制指令有效期 (ms)：发送槽位超过这么久没被 FillData() 刷新就清零（必须大于心跳） */
#define CAN_CMD_EXPIRE_MS 100
/** @brief 保底心跳 (ms)：发送节点至少这么久整帧发一次 */
#define CAN_TX_HEARTBEAT_MS 5
/** @brief Bus-Off 恢复的重试间隔 (ms) */
#define CAN_BUSOFF_RETRY_MS 100

/* ==================== */
/* ==================== UART ==================== */

/** @brief BspUart::init() 打开 UART 硬件 FIFO（16 字节）；0 = 保持 CubeMX 的设置 */
#define UART_ENABLE_FIFO 1
/** @brief BspUart::printf() 取格式化缓冲锁、入队的等待上限 (ms) */
#define UART_PRINTF_TIMEOUT_MS 10

/**
 * @brief 调试打印宏 print_uart() 的总开关（宏本身也定义在本文件，见下）
 *
 * 【取值】1 = 正常调 `bsp_uart1.printf()`；0 = 展开成 `((void)0)`，调用处保持原样但**完全静默**
 * 【为什么需要】发布固件、或这路串口改作业务通信用时，把调试输出一次性关掉，
 *   不用去每个调用点加 `#if`；同时也避免 printf 在高速任务里阻塞
 * 【代价】关掉后参数**不会被求值**，也**不再返回 Status**——调用处只能当语句用，
 *   不要写 `if (print_uart(...) != Status::OK)` 或在参数里放有副作用的表达式
 * 【Flash】省多少取决于有几个调用点：本工程 Debug 下实测 110 780 → 109 220 B（约 1.5 KB），
 *   但 `BspUartT::printf` 本身还是编进库的（要给其它实例用），所以不要把这一项当减肥手段
 */
#define UART_PRINT_ENABLE 1

/**
 * @brief 调试打印宏：`print_uart("x=%d\r\n", x)`
 *
 * - 开（`UART_PRINT_ENABLE = 1`）：展开成 `bsp_uart1.printf(...)`，
 *   返回 Status（OK / TIMEOUT 半行 / BAD_ARG）
 * - 关（`= 0`）：展开成 `((void)0)`，调用处不用改，但**参数不求值、不打印、也不返回 Status**
 * - 能用的范围：凡是能看到 `bsp_uart1` 的地方（即包含 board_cfg.hpp 的文件）；
 *   换调试串口只改 board_cfg.hpp 里那个实例名，这里不用动
 * - 内部有 printf 锁，多任务同时打印不会互相覆盖；
 *   单行超过该实例的 TX_SIZE（默认 128 字节）会被截断
 */
#if UART_PRINT_ENABLE
#  define print_uart(...) bsp_uart1.printf(__VA_ARGS__)
#else
#  define print_uart(...) ((void)0)
#endif

/* ==================== */
/* ==================== SPI ==================== */

/** @brief 每个 BspSpi 实例的 DMA 弹跳缓冲（字节，收发各一份） */
#define SPI_DMA_BUFFER_SIZE 256
/** @brief BspSpi / BspSpiDevice 传输接口的默认超时 (ms) */
#define SPI_TRANSFER_TIMEOUT_MS 10

/* ==================== */
/* ==================== BMI088 ==================== */

/** @brief DeviceBmi088 自动刷新任务的优先级：低于库的 sys / CAN 任务，高于一般业务任务 */
#define BMI088_TASK_PRIORITY (tskIDLE_PRIORITY + 6)
/** @brief DeviceBmi088 自动刷新任务栈深（字）：含 VQF 姿态解算 */
#define BMI088_TASK_STACK 512

/* ==================== */
/* ==================== 取值检查 ==================== */

#ifdef __cplusplus
static_assert(SYS_TASK_ENABLE == 0 || SYS_TASK_ENABLE == 1, "hardware_config.h: SYS_TASK_ENABLE 取 0 或 1");
static_assert(CAN_RX_ENABLE == 0 || CAN_RX_ENABLE == 1, "hardware_config.h: CAN_RX_ENABLE 取 0 或 1");
static_assert(CAN_TX_ENABLE == 0 || CAN_TX_ENABLE == 1, "hardware_config.h: CAN_TX_ENABLE 取 0 或 1");
static_assert(UART_PATROL_ENABLE == 0 || UART_PATROL_ENABLE == 1, "hardware_config.h: UART_PATROL_ENABLE 取 0 或 1");
static_assert(SYS_TASK_PRIORITY < configMAX_PRIORITIES, "hardware_config.h: SYS_TASK_PRIORITY 必须小于 configMAX_PRIORITIES");
static_assert(CAN_TASK_PRIORITY < configMAX_PRIORITIES, "hardware_config.h: CAN_TASK_PRIORITY 必须小于 configMAX_PRIORITIES");
static_assert(SYS_TASK_STACK >= 128 && CAN_TASK_STACK >= 128, "hardware_config.h: 库任务栈深至少 128 字");
static_assert(CAN_RX_QUEUE_FRAMES >= 1 && CAN_RX_RETURN_FRAMES >= 1 && CAN_TX_QUEUE_FRAMES >= 1, "hardware_config.h: CAN 队列深度至少 1 帧");
static_assert(CAN_RX_DISPATCH_PER_TICK >= 1 && CAN_RX_DISPATCH_PER_TICK <= 64, "hardware_config.h: CAN_RX_DISPATCH_PER_TICK 取 1~64");
static_assert(CAN_RX_NODE_MAX >= 1 && CAN_RX_NODE_MAX <= 255, "hardware_config.h: CAN_RX_NODE_MAX 取 1~255（计数为 uint8_t）");
static_assert(CAN_TX_NODE_MAX >= 1 && CAN_TX_NODE_MAX <= 255, "hardware_config.h: CAN_TX_NODE_MAX 取 1~255（计数为 uint8_t）");
static_assert(CAN_TX_HEARTBEAT_MS >= 1, "hardware_config.h: CAN_TX_HEARTBEAT_MS 至少 1 ms");
static_assert(CAN_CMD_EXPIRE_MS > CAN_TX_HEARTBEAT_MS, "hardware_config.h: 指令有效期必须长于保底心跳");
static_assert(CAN_BUSOFF_RETRY_MS >= 1, "hardware_config.h: CAN_BUSOFF_RETRY_MS 至少 1 ms");
static_assert(UART_ENABLE_FIFO == 0 || UART_ENABLE_FIFO == 1, "hardware_config.h: UART_ENABLE_FIFO 取 0 或 1");
static_assert(UART_PRINTF_TIMEOUT_MS >= 1, "hardware_config.h: UART_PRINTF_TIMEOUT_MS 至少 1 ms");
static_assert(UART_PRINT_ENABLE == 0 || UART_PRINT_ENABLE == 1, "hardware_config.h: UART_PRINT_ENABLE 取 0 或 1");
static_assert(SPI_DMA_BUFFER_SIZE >= 16 && SPI_DMA_BUFFER_SIZE <= 65535, "hardware_config.h: SPI_DMA_BUFFER_SIZE 取 16~65535");
static_assert(SPI_TRANSFER_TIMEOUT_MS >= 1, "hardware_config.h: SPI_TRANSFER_TIMEOUT_MS 至少 1 ms");
static_assert(BMI088_TASK_STACK >= 128, "hardware_config.h: BMI088_TASK_STACK 至少 128 字");
#endif

/* ==================== */

#endif /* HARDWARE_CONFIG_H */
