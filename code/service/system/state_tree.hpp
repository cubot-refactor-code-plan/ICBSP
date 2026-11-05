/**
 * @file state_tree.hpp
 * @brief EventState 默认树：sys_state → 每个总线实例 → 挂在总线上的设备（Q6）
 *
 * @details
 *   库默认建好前两层，宿主不用写任何代码：
 *
 *     sys_state                           （system.cpp，根）
 *     ├─ bus_state(&bsp_can1)             （ICBSP_CAN_INSTANCES 里每个实例一个）
 *     │   └─ DjiMotor::state() …          （设备在自己的 init() 里挂上来）
 *     ├─ bus_state(&bsp_uart1)            （ICBSP_UART_INSTANCES）
 *     └─ bus_state(&bsp_spi2)             （ICBSP_SPI_INSTANCES）
 *
 *   - 总线节点在 `system_init()` 里建好并挂到 `sys_state`，`system_start()` 时进入运行态。
 *   - 运行期由 sys_task 每 10 ms 调 `state_tree_poll()`：比较各实例的诊断计数，哪类计数涨了就在该总线节点上
 *     记一个错误码（下表），错误随即向上传到 `sys_state`。
 *   - 设备节点（第三层）由设备自己持有：init() 时 `set_parent(*bus_state(&总线))`，掉线（Online 由在线变离线）
 *     记 `TIMEOUT`，初始化失败记 `init_error()`。
 *
 *   | 总线 | 计数涨了                                                         | 记的错误码  |
 *   | ---- | ---------------------------------------------------------------- | ----------- |
 *   | CAN  | `bus_off_events`                                                 | `IO_ERROR`  |
 *   | CAN  | `rx_dropped`、`rx_lost`、`rx_return_dropped`、`tx_dropped`、`tx_buf_full`、`tx_flushed` | `FULL` |
 *   | CAN  | `tx_stall_recover`                                               | `TIMEOUT`   |
 *   | UART | RX `hw_error_cnt`、`error_cnt`，TX `error_cnt`                   | `IO_ERROR`  |
 *   | UART | RX / TX `drop_bytes`                                             | `FULL`      |
 *   | UART | TX `stall_recover_cnt`                                           | `TIMEOUT`   |
 *   | SPI  | `error_cnt`                                                      | `IO_ERROR`  |
 *   | SPI  | `timeout_cnt`                                                    | `TIMEOUT`   |
 *   | SPI  | `lock_timeout_cnt`                                               | `BUSY`      |
 *
 * @note 传播规则（Q6 / N16）：子节点出错只置父节点的错误位，**不清 RUNNING**；`init_error()` 只在初始化期有效。
 *       错误码粘滞到 `clear_error()` / `clear_errors()`，只清本节点、不传播。查哪条总线出过什么错：
 *       `bus_state(&bsp_can1)->error_bits()`；整机有没有出过错：`sys_state.error_bits()`。
 * @note `ICBSP_SYS_TASK_ENABLE = 0` 时没有人调 `state_tree_poll()`，总线节点不会记运行期错误（设备掉线照常记）。
 */

#ifndef __SERVICE_STATE_TREE_HPP__
#define __SERVICE_STATE_TREE_HPP__

#include "service/system/status.hpp"

/**
 * @brief 取某个总线实例的 EventState 节点
 * @param bsp_instance 总线实例的地址（`&bsp_can1`、`&bsp_uart1`、`&bsp_spi2` …）
 * @return 节点指针；实例不在 ICBSP_CAN/UART/SPI_INSTANCES 清单里时返回 nullptr
 * @note `system_init()` 之后才可用（之前返回的节点还没 init，set_parent() 会返回 NOT_INIT）。
 */
EventState *bus_state(const void *bsp_instance);

/** @brief 总线节点个数（三张实例清单合计） */
unsigned bus_state_count(void);

/**
 * @brief 建总线节点并挂到 sys_state（`system_init()` 调用）
 * @return OK；FULL = 事件组创建失败（堆不够）
 */
Status state_tree_init(void);

/** @brief 总线节点进入运行态（`system_start()` 调用，先于 sys_state） */
void state_tree_start(void);

/** @brief 比较各总线实例的诊断计数，涨了就在对应节点记错误（sys_task 每 10 ms 调用） */
void state_tree_poll(void);

#endif // __SERVICE_STATE_TREE_HPP__
