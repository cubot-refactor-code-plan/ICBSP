#include "bsp/bsp_series.hpp" // IWYU pragma: keep（HAL 模块开关 / 系列桥接）

// FDCAN 的 HAL 回调（H7）；F4 的 bxCAN 回调在 bsp_can.cpp
#if defined(HAL_FDCAN_MODULE_ENABLED)

#include "board_instances.hpp"
#include "bsp/bsp_can.hpp"

#include "FreeRTOS.h" // IWYU pragma: keep


/* ==================== 实例清单核对 ==================== */

bool bsp_can_is_listed(const void *self, const FDCAN_HandleTypeDef *hfdcan)
{
  // 实例清单为空时 X-macro 展开成空，参数用不到
  (void)self;
  (void)hfdcan;
  bool     listed = false;
  uint32_t same   = 0U; // 清单里绑这个句柄的实例数
#define X(name)                                 \
  if (static_cast<const void *>(&name) == self) \
  {                                             \
    listed = true;                              \
  }                                             \
  if (name.get_handle() == hfdcan)                  \
  {                                             \
    same = same + 1U;                           \
  }
  ICBSP_CAN_INSTANCES(X)
#undef X
  return listed && same == 1U;
}


/* ==================== */
/* ==================== HAL 回调分发 ==================== */

// BspCanT 只有头文件，这里只放 HAL 回调：按句柄和 ICBSP_CAN_INSTANCES 清单逐个比对，清单外的 FDCAN 直接忽略。
extern "C"
{
  /**
   * @brief RX FIFO0 回调（RF0N / RF0L）
   * @note HAL 调用前已把 RxFifo0ITs 里的标志清掉，这里是这些事件唯一的处理机会。
   */
  void HAL_FDCAN_RxFifo0Callback(FDCAN_HandleTypeDef *hfdcan, uint32_t RxFifo0ITs)
  {
    BaseType_t woken = pdFALSE;
    if (false)
    {
    }
#define X(name)                                  \
  else if (hfdcan == name.get_handle())              \
  {                                              \
    name.process_fifo0_isr(RxFifo0ITs, &woken);  \
  }
    ICBSP_CAN_INSTANCES(X)
#undef X
    else
    {
      icbsp_unhandled_fdcan_rx_fifo0(hfdcan, RxFifo0ITs);
    }
    portYIELD_FROM_ISR(woken);
  }

  /** @brief TX FIFO 变空：续发软件缓冲里的帧 */
  void HAL_FDCAN_TxFifoEmptyCallback(FDCAN_HandleTypeDef *hfdcan)
  {
    BaseType_t woken = pdFALSE;
    if (false)
    {
    }
#define X(name)                            \
  else if (hfdcan == name.get_handle())        \
  {                                        \
    name.trigger_tx_from_isr(&woken);      \
  }
    ICBSP_CAN_INSTANCES(X)
#undef X
    else
    {
      icbsp_unhandled_fdcan_tx_fifo_empty(hfdcan);
    }
    portYIELD_FROM_ISR(woken);
  }

  /** @brief 错误状态变化（EP / EW）：只计数 */
  void HAL_FDCAN_ErrorStatusCallback(FDCAN_HandleTypeDef *hfdcan, uint32_t ErrorStatusITs)
  {
    if (false)
    {
    }
#define X(name)                             \
  else if (hfdcan == name.get_handle())         \
  {                                         \
    name.process_error_isr(ErrorStatusITs); \
  }
    ICBSP_CAN_INSTANCES(X)
#undef X
    else
    {
      icbsp_unhandled_fdcan_error_status(hfdcan, ErrorStatusITs);
    }
  }

  // 清单外 FDCAN 的回调转调：默认空，宿主定义同名强函数即可（对比评估 N30）
  __attribute__((weak)) void icbsp_unhandled_fdcan_rx_fifo0(FDCAN_HandleTypeDef *hfdcan, uint32_t its)
  {
    (void)hfdcan;
    (void)its;
  }
  __attribute__((weak)) void icbsp_unhandled_fdcan_tx_fifo_empty(FDCAN_HandleTypeDef *hfdcan)
  {
    (void)hfdcan;
  }
  __attribute__((weak)) void icbsp_unhandled_fdcan_error_status(FDCAN_HandleTypeDef *hfdcan, uint32_t its)
  {
    (void)hfdcan;
    (void)its;
  }
}
/* ==================== */

#endif // HAL_FDCAN_MODULE_ENABLED
