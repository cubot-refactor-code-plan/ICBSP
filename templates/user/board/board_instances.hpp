/**
 * @file board_instances.hpp
 * @brief 板级实例列表：库任务用 X-macro 按这里的实例展开，不再维护运行时登记表
 *
 * @details 只写实例名，类型由 decltype 推导；换板只改本文件与 board_cfg.*。
 */

#ifndef __BOARD_INSTANCES_HPP__
#define __BOARD_INSTANCES_HPP__

#include "board_cfg.hpp"

/** @brief CAN 实例列表（每项必须已在 board_cfg.hpp 中声明） */
#define BSP_CAN_INSTANCES(X) \
  X(bsp_can1)

/** @brief UART 实例列表（sys_task 的串口巡检按此展开） */
#define BSP_UART_INSTANCES(X) \
  X(bsp_uart1)

/** @brief SPI 实例列表（当前没有 SPI 实例） */
#define BSP_SPI_INSTANCES(X) /* 无 */

#endif // __BOARD_INSTANCES_HPP__
