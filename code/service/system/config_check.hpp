/**
 * @file config_check.hpp
 * @brief 库读取宿主配置的唯一入口：包含 hardware_config.h，补齐派生开关，检查缺宏、依赖和取值范围
 *
 * @details
 *   库内**不直接**包含 `hardware_config.h`，一律包含本文件（Q3）。这样：
 *     - 宿主少写了某个宏，报错直接说缺哪个、去哪里抄（库升级新增宏时同样适用）；
 *     - 范围检查在库里，宿主把模板末尾的检查删掉也照样生效；
 *     - 对应 HAL 模块没开时，功能开关在这里强制置 0，库内各处只看开关本身。
 *
 *   顺序：① 缺宏 `#error` → ② HAL 模块没开时强制关 → ③ 开关之间的依赖 → ④ 取值范围。
 *
 * @note 分层：bsp 也要读配置，所以本文件和 `status_code.hpp` 一样，是各层都能包含的公共头
 *       （`tools/check_layers.py` 的例外清单）。本文件只依赖 HAL（经 bsp_series.hpp）与 FreeRTOS 的配置头。
 */

#ifndef __SERVICE_CONFIG_CHECK_HPP__
#define __SERVICE_CONFIG_CHECK_HPP__

#include "FreeRTOS.h" // IWYU pragma: keep（configMAX_PRIORITIES）
#include "task.h"     // IWYU pragma: keep（tskIDLE_PRIORITY）

#include "bsp/bsp_series.hpp" // IWYU pragma: keep（HAL_*_MODULE_ENABLED、ICBSP_CAN_MODULE_ENABLED、ICBSP_UART_HAS_FIFO）
#include "hardware_config.h"  // IWYU pragma: export（宿主提供的唯一配置源）

/* ==================== */
/* ==================== ① 缺宏 ==================== */

#ifndef ICBSP_SYS_TASK_ENABLE
#  error "hardware_config.h 缺少 ICBSP_SYS_TASK_ENABLE（功能开关：sys_task），见 templates/user/board/hardware_config.h"
#endif
#ifndef ICBSP_CAN_RX_ENABLE
#  error "hardware_config.h 缺少 ICBSP_CAN_RX_ENABLE（功能开关：CAN 接收路径），见 templates/user/board/hardware_config.h"
#endif
#ifndef ICBSP_CAN_TX_ENABLE
#  error "hardware_config.h 缺少 ICBSP_CAN_TX_ENABLE（功能开关：CAN 发送路径），见 templates/user/board/hardware_config.h"
#endif
#ifndef ICBSP_UART_PATROL_ENABLE
#  error "hardware_config.h 缺少 ICBSP_UART_PATROL_ENABLE（功能开关：串口巡检），见 templates/user/board/hardware_config.h"
#endif
#ifndef ICBSP_SYS_TASK_PRIORITY
#  error "hardware_config.h 缺少 ICBSP_SYS_TASK_PRIORITY，见 templates/user/board/hardware_config.h"
#endif
#ifndef ICBSP_SYS_TASK_STACK
#  error "hardware_config.h 缺少 ICBSP_SYS_TASK_STACK，见 templates/user/board/hardware_config.h"
#endif
#ifndef ICBSP_CAN_TASK_PRIORITY
#  error "hardware_config.h 缺少 ICBSP_CAN_TASK_PRIORITY，见 templates/user/board/hardware_config.h"
#endif
#ifndef ICBSP_CAN_TASK_STACK
#  error "hardware_config.h 缺少 ICBSP_CAN_TASK_STACK，见 templates/user/board/hardware_config.h"
#endif
#ifndef ICBSP_CAN_RX_RETURN_FRAMES
#  error "hardware_config.h 缺少 ICBSP_CAN_RX_RETURN_FRAMES（回退缓冲深度，0 = 不建回退缓冲），见 templates/user/board/hardware_config.h"
#endif
#ifndef ICBSP_CAN_RX_DISPATCH_PER_TICK
#  error "hardware_config.h 缺少 ICBSP_CAN_RX_DISPATCH_PER_TICK，见 templates/user/board/hardware_config.h"
#endif
#ifndef ICBSP_CAN_RX_NODE_MAX
#  error "hardware_config.h 缺少 ICBSP_CAN_RX_NODE_MAX，见 templates/user/board/hardware_config.h"
#endif
#ifndef ICBSP_CAN_TX_NODE_MAX
#  error "hardware_config.h 缺少 ICBSP_CAN_TX_NODE_MAX，见 templates/user/board/hardware_config.h"
#endif
#ifndef ICBSP_CAN_CMD_EXPIRE_MS
#  error "hardware_config.h 缺少 ICBSP_CAN_CMD_EXPIRE_MS，见 templates/user/board/hardware_config.h"
#endif
#ifndef ICBSP_CAN_TX_HEARTBEAT_MS
#  error "hardware_config.h 缺少 ICBSP_CAN_TX_HEARTBEAT_MS，见 templates/user/board/hardware_config.h"
#endif
#ifndef ICBSP_CAN_TX_GATHER_MS
#  error "hardware_config.h 缺少 ICBSP_CAN_TX_GATHER_MS（共帧槽位凑不齐时最多等多久就发），见 templates/user/board/hardware_config.h"
#endif
#ifndef ICBSP_CAN_BUSOFF_RETRY_MS
#  error "hardware_config.h 缺少 ICBSP_CAN_BUSOFF_RETRY_MS，见 templates/user/board/hardware_config.h"
#endif
#ifndef ICBSP_UART_ENABLE_FIFO
#  error "hardware_config.h 缺少 ICBSP_UART_ENABLE_FIFO，见 templates/user/board/hardware_config.h"
#endif
#ifndef ICBSP_SPI_DMA_BUFFER_SIZE
#  error "hardware_config.h 缺少 ICBSP_SPI_DMA_BUFFER_SIZE，见 templates/user/board/hardware_config.h"
#endif
#ifndef ICBSP_SPI_TRANSFER_TIMEOUT_MS
#  error "hardware_config.h 缺少 ICBSP_SPI_TRANSFER_TIMEOUT_MS，见 templates/user/board/hardware_config.h"
#endif
#ifndef ICBSP_BMI088_TASK_PRIORITY
#  error "hardware_config.h 缺少 ICBSP_BMI088_TASK_PRIORITY，见 templates/user/board/hardware_config.h"
#endif
#ifndef ICBSP_BMI088_TASK_STACK
#  error "hardware_config.h 缺少 ICBSP_BMI088_TASK_STACK，见 templates/user/board/hardware_config.h"
#endif

/* ==================== */
/* ==================== ② HAL 模块没开时强制关 ==================== */

/* 取值先查一遍：后面要 #undef 重定义，宿主写的非法值（例如 2）在这之前拦下 */
#ifdef __cplusplus
static_assert(ICBSP_SYS_TASK_ENABLE == 0 || ICBSP_SYS_TASK_ENABLE == 1, "hardware_config.h: ICBSP_SYS_TASK_ENABLE 取 0 或 1");
static_assert(ICBSP_CAN_RX_ENABLE == 0 || ICBSP_CAN_RX_ENABLE == 1, "hardware_config.h: ICBSP_CAN_RX_ENABLE 取 0 或 1");
static_assert(ICBSP_CAN_TX_ENABLE == 0 || ICBSP_CAN_TX_ENABLE == 1, "hardware_config.h: ICBSP_CAN_TX_ENABLE 取 0 或 1");
static_assert(ICBSP_UART_PATROL_ENABLE == 0 || ICBSP_UART_PATROL_ENABLE == 1, "hardware_config.h: ICBSP_UART_PATROL_ENABLE 取 0 或 1");
static_assert(ICBSP_UART_ENABLE_FIFO == 0 || ICBSP_UART_ENABLE_FIFO == 1, "hardware_config.h: ICBSP_UART_ENABLE_FIFO 取 0 或 1");
#endif

/* CubeMX 里没开 CAN（H7 的 FDCAN / F4 的 bxCAN）：CAN 收发路径没有可用的外设 */
#if !ICBSP_CAN_MODULE_ENABLED
#  undef ICBSP_CAN_RX_ENABLE
#  undef ICBSP_CAN_TX_ENABLE
#  define ICBSP_CAN_RX_ENABLE 0
#  define ICBSP_CAN_TX_ENABLE 0
#endif

/* CubeMX 里没开 UART：没有可巡检的串口 */
#ifndef HAL_UART_MODULE_ENABLED
#  undef ICBSP_UART_PATROL_ENABLE
#  define ICBSP_UART_PATROL_ENABLE 0
#endif

/* ==================== */
/* ==================== ③ 开关之间的依赖 ==================== */

#ifdef __cplusplus
static_assert(ICBSP_SYS_TASK_ENABLE || !ICBSP_UART_PATROL_ENABLE,
              "hardware_config.h: 串口巡检在 sys_task 里跑，ICBSP_UART_PATROL_ENABLE = 1 需要 ICBSP_SYS_TASK_ENABLE = 1");
static_assert(ICBSP_UART_HAS_FIFO != 0 || ICBSP_UART_ENABLE_FIFO == 0,
              "hardware_config.h: 本系列（F4）没有 UART 硬件 FIFO，ICBSP_UART_ENABLE_FIFO 必须为 0");
#endif

/* ==================== */
/* ==================== ④ 取值范围 ==================== */

#ifdef __cplusplus
static_assert(ICBSP_SYS_TASK_PRIORITY > tskIDLE_PRIORITY && ICBSP_SYS_TASK_PRIORITY < configMAX_PRIORITIES,
              "hardware_config.h: ICBSP_SYS_TASK_PRIORITY 取 1 ~ configMAX_PRIORITIES - 1");
static_assert(ICBSP_CAN_TASK_PRIORITY > tskIDLE_PRIORITY && ICBSP_CAN_TASK_PRIORITY < configMAX_PRIORITIES,
              "hardware_config.h: ICBSP_CAN_TASK_PRIORITY 取 1 ~ configMAX_PRIORITIES - 1");
static_assert(ICBSP_CAN_TASK_PRIORITY > ICBSP_SYS_TASK_PRIORITY, "hardware_config.h: CAN 收发任务要比 sys_task 优先：1 kHz 节拍内必须跑完");
static_assert(ICBSP_BMI088_TASK_PRIORITY > tskIDLE_PRIORITY && ICBSP_BMI088_TASK_PRIORITY < configMAX_PRIORITIES,
              "hardware_config.h: ICBSP_BMI088_TASK_PRIORITY 取 1 ~ configMAX_PRIORITIES - 1");
static_assert(ICBSP_SYS_TASK_STACK >= 128 && ICBSP_CAN_TASK_STACK >= 128 && ICBSP_BMI088_TASK_STACK >= 128,
              "hardware_config.h: 库任务栈深至少 128 字");
static_assert(ICBSP_CAN_RX_RETURN_FRAMES >= 0, "hardware_config.h: ICBSP_CAN_RX_RETURN_FRAMES 不能为负（0 = 不建回退缓冲）");
static_assert(ICBSP_CAN_RX_DISPATCH_PER_TICK >= 1 && ICBSP_CAN_RX_DISPATCH_PER_TICK <= 64, "hardware_config.h: ICBSP_CAN_RX_DISPATCH_PER_TICK 取 1~64");
static_assert(ICBSP_CAN_RX_NODE_MAX >= 1 && ICBSP_CAN_RX_NODE_MAX <= 255, "hardware_config.h: ICBSP_CAN_RX_NODE_MAX 取 1~255（计数为 uint8_t）");
static_assert(ICBSP_CAN_TX_NODE_MAX >= 1 && ICBSP_CAN_TX_NODE_MAX <= 255, "hardware_config.h: ICBSP_CAN_TX_NODE_MAX 取 1~255（计数为 uint8_t）");
static_assert(ICBSP_CAN_TX_HEARTBEAT_MS >= 1, "hardware_config.h: ICBSP_CAN_TX_HEARTBEAT_MS 至少 1 ms");
static_assert(ICBSP_CAN_CMD_EXPIRE_MS > ICBSP_CAN_TX_HEARTBEAT_MS, "hardware_config.h: 指令有效期必须长于保底心跳");
static_assert(ICBSP_CAN_TX_GATHER_MS >= 1 && ICBSP_CAN_TX_GATHER_MS < ICBSP_CAN_TX_HEARTBEAT_MS,
              "hardware_config.h: ICBSP_CAN_TX_GATHER_MS 取 1 ~ 心跳周期 - 1");
static_assert(ICBSP_CAN_BUSOFF_RETRY_MS >= 10, "hardware_config.h: ICBSP_CAN_BUSOFF_RETRY_MS 至少 10 ms（sys_task 每 10 ms 检查一次）");
static_assert(ICBSP_SPI_DMA_BUFFER_SIZE >= 16 && ICBSP_SPI_DMA_BUFFER_SIZE <= 65535, "hardware_config.h: ICBSP_SPI_DMA_BUFFER_SIZE 取 16~65535");
static_assert(ICBSP_SPI_TRANSFER_TIMEOUT_MS >= 1, "hardware_config.h: ICBSP_SPI_TRANSFER_TIMEOUT_MS 至少 1 ms");
#endif

#endif // __SERVICE_CONFIG_CHECK_HPP__
