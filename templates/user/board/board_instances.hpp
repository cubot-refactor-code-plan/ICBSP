/**
 * @file board_instances.hpp
 * @brief 板级实例清单：库用 X-macro 按这里的名字展开，不再维护运行时登记表
 *
 * @details 【为什么需要这个文件】
 *          BSP 层没有运行时登记表、没有基类、没有虚函数。HAL 回调
 *          （`HAL_FDCAN_RxFifo0Callback`、`HAL_UARTEx_RxEventCallback` 等）、
 *          `sys_task` 的串口巡检、`can_rx` / `can_tx` 都必须知道「板子上究竟有哪几个实例」，
 *          这件事由这组 X-macro 在编译期展开完成：
 *
 *              BSP_CAN_INSTANCES(X)   →   X(bsp_can1) X(bsp_can2)
 *              展开后就是「对每个实例依次做同一件事」，中断里也没有间接跳转。
 *
 *          好处：零运行时开销；代价：本文件和 board_cfg.hpp 必须手工保持一致。
 *
 *          【只写名字，不写类型】
 *          类型由 `decltype` 从 board_cfg.hpp 推出来，所以改 `BspCanT` 的模板参数不用动这里。
 *
 *          【加一个实例的完整步骤】
 *            1. board_cfg.hpp 里定义：`inline BspCanT<16, 16> bsp_can2({.hfdcan = &hfdcan2, .name = "CAN2"});`
 *            2. 本文件对应宏里加一项：`X(bsp_can2)`（续行用反斜杠，最后一项后面不加逗号）
 *            3. board_cfg.cpp 的 `board_init()` 里加：`configASSERT(bsp_can2.init() == Status::OK);`
 *            4. CubeMX 里把对应中断打开（FDCAN2 的 RX FIFO0）
 *
 *          【忘了同步会怎样】不报错、不告警：那个实例的回调不会被分发，`sys_task` 也不巡检它
 *          ——表现是「数据收不到」而不是「编译失败」。加实例时一定要两处一起改。
 */

#ifndef __BOARD_INSTANCES_HPP__
#define __BOARD_INSTANCES_HPP__

#include "board_cfg.hpp"

/**
 * @brief CAN 实例列表
 *
 * 每个名字必须已在 board_cfg.hpp 里定义；顺序无所谓，但建议和板子上 CAN1/CAN2 的顺序一致，
 * 这样 `sys_task` 打印和在线检查里的序号对得上。
 *
 * 例：两路 CAN
 *     #define BSP_CAN_INSTANCES(X) \
 *       X(bsp_can1)                \
 *       X(bsp_can2)
 */
#define BSP_CAN_INSTANCES(X) \
  X(bsp_can1)

/**
 * @brief UART 实例列表
 *
 * `sys_task` 的串口补救巡检按此展开；调试串口 `print_uart` 用的实例也建议列进来。
 * 只收不发的设备同样要列（巡检看的是「有没有断流」，与方向无关）。
 */
#define BSP_UART_INSTANCES(X) \
  X(bsp_uart1)

/**
 * @brief SPI 实例列表
 *
 * 当前没有 SPI 实例：宏体写成空注释，展开后什么都不产生。
 * 打开 SPI 时（board_cfg.hpp 里取消注释后）改成：
 *     #define BSP_SPI_INSTANCES(X) X(bsp_spi2)
 */
#define BSP_SPI_INSTANCES(X) /* 无 */

#endif // __BOARD_INSTANCES_HPP__
