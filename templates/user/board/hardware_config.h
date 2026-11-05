/**
 * @file hardware_config.h
 * @brief 本板的唯一配置入口：库的全部可调宏、功能开关与取值检查都集中在这里
 *
 * @details
 *
 * 【它是什么】
 *   icbsp 里**没有**任何“默认值头文件”。库的每一个可调参数都是本文件里的 `#define`，
 *   库源码经 `service/system/config_check.hpp` 包含它（宿主自己的代码可以直接包含本文件）。
 *   CMake 配置阶段会打印实际用的是哪一份：
 *
 *       -- icbsp: 使用配置 /…/user/board/hardware_config.h
 *
 *   所以“改库的行为”= 改本文件的宏，不用改库源码；换板 = 换一份本文件。
 *
 * 【四组宏怎么理解】
 *   ① 功能开关：决定“这段代码编不编译、这些任务建不建”——关掉省 Flash/RAM，但功能就没了
 *   ② 库任务：优先级与栈深——决定实时性和会不会栈溢出
 *   ③ 外设参数：队列深度、超时、缓冲区大小——决定吞吐、丢帧与阻塞时长
 *
 * 【检查在库里】
 *   缺宏、取值范围、开关之间的依赖都由库的 `code/service/system/config_check.hpp` 检查（编译期，不改行为），
 *   CubeMX 没开对应 HAL 模块时库也会把开关强制置 0——本文件只需要写 `#define`。
 *
 * 【不在这里的两项】
 *   1. 调试打印开关 `ICBSP_UART_PRINT_ENABLE` 与 `print_uart(...)` 定义在 `board_cfg.hpp`：
 *      它们要绑定具体的串口实例，跟着实例放才不会互相牵连（本文件只放“不依赖实例”的宏）。
 *   2. `BspUart::printf()` 的取锁 / 入队等待上限目前是**库内固定值**（各 10 ms，见
 *      code/bsp/bsp_uart.hpp 的 PRINTF_TIMEOUT_MS），没有做成可调宏——想要可调得先改库。
 */

#ifndef HARDWARE_CONFIG_H
#define HARDWARE_CONFIG_H

#include "FreeRTOS.h" // IWYU pragma: keep（configMAX_PRIORITIES）
#include "task.h"     // IWYU pragma: keep（configMAX_PRIORITIES）

/* ==================== ① 功能开关：关掉就不编译对应代码、也不创建对应任务 ==================== */
/*
 * 取 0 或 1。CubeMX 没开对应外设（CAN / UART）时，库会把相关开关强制置 0，这里不用改。
 * 依赖：ICBSP_UART_PATROL_ENABLE 需要 ICBSP_SYS_TASK_ENABLE；DjiMotor（以及以后的 dm_motor）需要 CAN 收发都开。
 */

/** @brief 系统维护任务 sys_task（10 ms）
 *  @note 关掉（0）会失去：CAN Bus-Off 自动恢复与发送卡死恢复、串口巡检、Online 掉线计时（is_online() 不再变化）、
 *        `sys_task_stats` 与 `system_set_report_hook()` 报告、DWT 绝对时间的回绕跟踪
 *        （此时宿主要每隔不到一圈（550 MHz 下 7.8 s）调一次 `bsp_dwt.get_cycles()`，见 bsp_dwt.hpp）。 */
#define ICBSP_SYS_TASK_ENABLE 1
/** @brief CAN 接收路径：can_rx_task（1 ms）+ CanRxNodeT
 *  @note 关掉会失去：按 ID 分发到接收节点（DjiMotor 等设备收不到反馈）；`BspCanT::receive()` 改为直接读原始接收缓冲。 */
#define ICBSP_CAN_RX_ENABLE 1
/** @brief CAN 发送路径：can_tx_task + CanTxNodeT
 *  @note 关掉会失去：共帧发送节点（多个设备拼一帧、心跳、指令过期清零）；只剩 `BspCanT::send()` 直接发。 */
#define ICBSP_CAN_TX_ENABLE 1
/** @brief sys_task 里的串口巡检
 *  @note 关掉会失去：串口接收 / 发送卡死后的自动恢复（DMA 出错、HAL 状态卡住时不再重建收发）。 */
#define ICBSP_UART_PATROL_ENABLE 1

/* ==================== */
/* ==================== 库任务（system_init() 创建） ==================== */

/** @brief 系统维护任务 sys_task 的优先级（默认 configMAX_PRIORITIES - 4，CMSIS_V2 下是 52）
 *  @note 库任务的优先级按 configMAX_PRIORITIES 往下取（评审 A2）：CAN 收发（-2）> BMI088（-3）> sys（-4），最高一档留给用户。
 *        **业务任务必须低于这三个**：CubeMX 的 osPriorityNormal（24）、osPriorityHigh（40）都可以；
 *        原来的 tskIDLE_PRIORITY + 6~8 比 osPriorityNormal 还低，按 CubeMX 习惯建的业务任务一忙，CAN 节拍、Bus-Off 恢复、
 *        串口巡检、Online 计时、IMU 刷新都会被推迟。
 *  @note sys_task 每 10 ms 只做几十微秒的维护（CAN 恢复 / 串口巡检 / Online 计时），放得高也不会占住 CPU。 */
#define ICBSP_SYS_TASK_PRIORITY (configMAX_PRIORITIES - 4)
/** @brief sys_task 栈深（字） */
#define ICBSP_SYS_TASK_STACK 256

/** @brief CAN 收 / 发任务的优先级（默认 configMAX_PRIORITIES - 2）：库任务里最高，1 kHz 节拍内必须跑完 */
#define ICBSP_CAN_TASK_PRIORITY (configMAX_PRIORITIES - 2)
/** @brief CAN 收 / 发任务栈深（字）：接收节点回调在 can_rx_task 里同步执行，回调用栈多就加大 */
#define ICBSP_CAN_TASK_STACK 512

/* ==================== */
/* ==================== CAN ==================== */

/** @brief 每路 CAN 的回退缓冲深度（帧）：没有接收节点认领的帧放这里，供 BspCanT::receive() 读取；
 *  0 = 不建回退缓冲（没人认领的帧只计入 diagnostics.rx_unclaimed，receive() 返回 NOT_SUPPORTED）。ICBSP_CAN_RX_ENABLE = 0 时不用 */
#define ICBSP_CAN_RX_RETURN_FRAMES 16
/** @brief can_rx_task 每 1 ms 每条总线最多分发的帧数（1~64） */
#define ICBSP_CAN_RX_DISPATCH_PER_TICK 16
/** @brief 接收节点总数上限（全部总线合计，≤255） */
#define ICBSP_CAN_RX_NODE_MAX 50
/** @brief 发送节点总数上限（全部总线合计，≤255） */
#define ICBSP_CAN_TX_NODE_MAX 50
/** @brief 控制指令有效期 (ms)：发送槽位超过这么久没被 fill_data() 刷新就清零（必须大于心跳） */
#define ICBSP_CAN_CMD_EXPIRE_MS 100
/** @brief 保底心跳 (ms)：发送节点至少这么久整帧发一次 */
#define ICBSP_CAN_TX_HEARTBEAT_MS 5
/** @brief 共帧槽位凑不齐时最多等多久就发 (ms)：有槽位填了新数据、其余槽位这么久还没刷新，也发出去（计入节点的 partial_sends） */
#define ICBSP_CAN_TX_GATHER_MS 1
/** @brief Bus-Off 恢复的重试间隔 (ms)，≥ 10（sys_task 每 10 ms 检查一次）；每路 CAN 的收发队列深度由 BspCanT<RX, TX> 的模板参数决定 */
#define ICBSP_CAN_BUSOFF_RETRY_MS 100

/* ==================== */
/* ==================== UART ==================== */

/** @brief BspUart::init() 打开 UART 硬件 FIFO（16 字节）；0 = 保持 CubeMX 的设置
 *  @note 默认关：开了以后 tx_recover() 中止发送会清掉 TX FIFO 里最多 16 字节（只在卡死恢复路径上）。
 *        要用就自己改成 1；F4 没有硬件 FIFO，本宏在 F4 上必须为 0（库里有 static_assert 拦）。 */
#define ICBSP_UART_ENABLE_FIFO 0

/* 注意：`BspUart::printf()` 的等待上限目前是**库内固定值**（取锁 10 ms / 入队 10 ms，
 * 见 code/bsp/bsp_uart.hpp 的 PRINTF_TIMEOUT_MS），没有做成可调宏；调试打印开关 ICBSP_UART_PRINT_ENABLE 也在
 * board_cfg.hpp（要绑串口实例）。详见文件头“不在这里的两项”。 */

/* ==================== */
/* ==================== SPI ==================== */

/** @brief 每个 BspSpi 实例的 DMA 弹跳缓冲（字节，收发各一份） */
#define ICBSP_SPI_DMA_BUFFER_SIZE 256
/** @brief BspSpi / BspSpiDevice 传输接口的默认超时 (ms) */
#define ICBSP_SPI_TRANSFER_TIMEOUT_MS 10

/* ==================== */
/* ==================== BMI088 ==================== */

/** @brief DeviceBmi088 自动刷新任务的优先级（默认 configMAX_PRIORITIES - 3）：低于 CAN 收发、高于 sys 与业务任务 */
#define ICBSP_BMI088_TASK_PRIORITY (configMAX_PRIORITIES - 3)
/** @brief DeviceBmi088 自动刷新任务栈深（字）：含 VQF 姿态解算 */
#define ICBSP_BMI088_TASK_STACK 512

/* ==================== */

#endif /* HARDWARE_CONFIG_H */
