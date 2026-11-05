/**
 * @file board_instances.hpp
 * @brief 板级实例清单：库用 X-macro 按这里的名字展开，**没有**运行时登记表
 *
 * @details
 *
 * 【为什么需要这个文件】
 *   BSP 层没有基类、没有虚函数、也没有“实例数组”，但下面这些地方必须知道
 *   “板子上到底有哪几个实例”：
 *
 *     | 宏                   | 谁展开它                                    | 用来做什么                       |
 *     | -------------------- | ------------------------------------------- | -------------------------------- |
 *     | `BSP_CAN_INSTANCES`  | `code/bsp/bsp_can.cpp`                      | FDCAN 的 HAL 回调按句柄找到实例   |
 *     |                      | `code/service/system.cpp`                   | 判断有没有发送节点、要不要建 can_tx |
 *     |                      | `code/tasks/can_rx_task.cpp`                | 每条总线轮询取帧并分发           |
 *     |                      | `code/tasks/can_tx_task.cpp`                | 每条总线下发聚合帧               |
 *     |                      | `code/tasks/sys_task.cpp`                   | 每条总线的 Bus-Off 恢复          |
 *     | `BSP_UART_INSTANCES` | `code/bsp/bsp_uart.cpp`                     | UART 的 HAL 回调按句柄找到实例   |
 *     |                      | `code/tasks/sys_task.cpp`                   | 串口收发补救巡检                 |
 *     | `BSP_SPI_INSTANCES`  | `code/bsp/bsp_spi.cpp`                      | SPI 的 HAL 回调按句柄找到实例    |
 *
 *   做法是编译期展开：`BSP_CAN_INSTANCES(X)` 展开成 `X(bsp_can1) X(bsp_can2)`，
 *   调用方把 `X` 定义成一段代码，于是“对每个实例做同一件事”，中断里也没有间接跳转。
 *
 *   **好处**：零运行时开销、不占 RAM；**代价**：本文件与 `board_cfg.hpp` 必须手工保持一致。
 *
 * 【只写名字，不写类型】
 *   类型由 `decltype` 从 `board_cfg.hpp` 推出来，所以改 `BspCanT` 的模板参数不用动这里。
 *
 * 【忘了同步会怎样】（最容易踩的坑）
 *   不报错、不告警：
 *     - CAN / UART / SPI 实例漏写 → HAL 回调里遍历不到它，**数据收不到**
 *     - UART 实例漏写 → sys_task 不巡检它，卡死后不自愈
 *   表现都是“运行期没反应”，不是“编译失败”。加实例时一定两处一起改。
 *
 * 【加实例的三步（以 CAN2 为例）】
 *   1. `board_cfg.hpp`：`inline BspCanT<16, 16> bsp_can2({.hfdcan = &hfdcan2, .name = "CAN2"});`
 *   2. 本文件：`BSP_CAN_INSTANCES(X)` 里加一行 `X(bsp_can2)`（续行用反斜杠，**最后一项后面不加逗号**）
 *   3. `board_cfg.cpp`：`configASSERT(bsp_can2.init() == Status::OK);`
 */

#ifndef __BOARD_INSTANCES_HPP__
#define __BOARD_INSTANCES_HPP__

#include "board_cfg.hpp"// IWYU pragma: keep

/**
 * @brief CAN 实例列表
 *
 * 每个名字必须已在 `board_cfg.hpp` 里定义。
 *
 * 两路 CAN 的写法：
 *     #define BSP_CAN_INSTANCES(X) \
 *       X(bsp_can1)                \
 *       X(bsp_can2)
 *
 * 注意：**这里列了几个，就要求每个都已 init()**——sys_task 的 Bus-Off 恢复和
 * can_rx_task 的分发都会直接访问它们。
 */
#if BSP_CAN_MODULE_ENABLED
#  define BSP_CAN_INSTANCES(X) \
    X(bsp_can1)
#else
/* 没在 CubeMX 里开 CAN（H7 的 FDCAN / F4 的 bxCAN）：没有实例，展开为空 */
#  define BSP_CAN_INSTANCES(X) /* 无 */
#endif

/**
 * @brief UART 实例列表
 *
 * 用途：① HAL 的 UART 回调按句柄找到对应实例；② sys_task 每 10 ms 巡检它们的收发健康度。
 */
#define BSP_UART_INSTANCES(X) \
  X(bsp_uart1)

/**
 * @brief SPI 实例列表
 *
 * 本工程没有 SPI 实例，所以宏体写成空注释，展开后什么都不产生。
 * 打开 SPI 时改成：
 *     #define BSP_SPI_INSTANCES(X) 
 *       X(bsp_spi2)
 *
 */
#define BSP_SPI_INSTANCES(X) /* 无 */

#endif // __BOARD_INSTANCES_HPP__
