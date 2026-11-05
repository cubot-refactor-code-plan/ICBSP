#include "bsp/bsp_series.hpp" // IWYU pragma: keep（HAL 模块开关 / 系列桥接）

// F4：bxCAN 的 HAL 回调（H7 的 FDCAN 回调在 bsp_fdcan.cpp）
#if defined(HAL_CAN_MODULE_ENABLED)

#include "board_instances.hpp"
#include "bsp/bsp_can.hpp"

#include "FreeRTOS.h" // IWYU pragma: keep


/* ==================== 实例清单核对（bxCAN） ==================== */

bool bsp_bxcan_is_listed(const void *self, const CAN_HandleTypeDef *hcan)
{
  bool     listed = false;
  uint32_t same   = 0U; // 清单里绑这个句柄的实例数
#define X(name)                                 \
  if (static_cast<const void *>(&name) == self) \
  {                                             \
    listed = true;                              \
  }                                             \
  if (name.handle() == hcan)                    \
  {                                             \
    same = same + 1U;                           \
  }
  ICBSP_CAN_INSTANCES(X)
#undef X
  return listed && same == 1U;
}


/* ==================== HAL 回调分发（bxCAN） ==================== */

// 按句柄和 ICBSP_CAN_INSTANCES 清单逐个比对，清单外的 bxCAN 直接忽略。
extern "C"
{
  /** @brief RX FIFO0 有新报文 */
  void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan)
  {
    BaseType_t woken = pdFALSE;
    if (false)
    {
    }
#define X(name)                         \
  else if (hcan == name.handle())       \
  {                                     \
    name.process_fifo0_isr(0U, &woken); \
  }
    ICBSP_CAN_INSTANCES(X)
#undef X
    else
    {
      icbsp_unhandled_bxcan_rx_fifo0(hcan);
    }
    portYIELD_FROM_ISR(woken);
  }

  /** @brief 发送邮箱 0 发完：续发软件缓冲里的帧 */
  void HAL_CAN_TxMailbox0CompleteCallback(CAN_HandleTypeDef *hcan)
  {
    BaseType_t woken = pdFALSE;
    if (false)
    {
    }
#define X(name)                       \
  else if (hcan == name.handle())     \
  {                                   \
    name.trigger_tx_from_isr(&woken); \
  }
    ICBSP_CAN_INSTANCES(X)
#undef X
    else
    {
      icbsp_unhandled_bxcan_tx_mailbox(hcan, 0U);
    }
    portYIELD_FROM_ISR(woken);
  }

  /** @brief 发送邮箱 1 发完：续发软件缓冲里的帧 */
  void HAL_CAN_TxMailbox1CompleteCallback(CAN_HandleTypeDef *hcan)
  {
    BaseType_t woken = pdFALSE;
    if (false)
    {
    }
#define X(name)                       \
  else if (hcan == name.handle())     \
  {                                   \
    name.trigger_tx_from_isr(&woken); \
  }
    ICBSP_CAN_INSTANCES(X)
#undef X
    else
    {
      icbsp_unhandled_bxcan_tx_mailbox(hcan, 1U);
    }
    portYIELD_FROM_ISR(woken);
  }

  /** @brief 发送邮箱 2 发完：续发软件缓冲里的帧 */
  void HAL_CAN_TxMailbox2CompleteCallback(CAN_HandleTypeDef *hcan)
  {
    BaseType_t woken = pdFALSE;
    if (false)
    {
    }
#define X(name)                       \
  else if (hcan == name.handle())     \
  {                                   \
    name.trigger_tx_from_isr(&woken); \
  }
    ICBSP_CAN_INSTANCES(X)
#undef X
    else
    {
      icbsp_unhandled_bxcan_tx_mailbox(hcan, 2U);
    }
    portYIELD_FROM_ISR(woken);
  }

  /** @brief 错误状态变化（EP / EW）：只计数 */
  void HAL_CAN_ErrorCallback(CAN_HandleTypeDef *hcan)
  {
    if (false)
    {
    }
#define X(name)                             \
  else if (hcan == name.handle())           \
  {                                         \
    name.process_error_isr(hcan->ErrorCode); \
  }
    ICBSP_CAN_INSTANCES(X)
#undef X
    else
    {
      icbsp_unhandled_bxcan_error(hcan, hcan->ErrorCode);
    }
  }

  // 清单外 bxCAN 的回调转调：默认空，宿主定义同名强函数即可
  __attribute__((weak)) void icbsp_unhandled_bxcan_rx_fifo0(CAN_HandleTypeDef *hcan)
  {
    (void)hcan;
  }
  __attribute__((weak)) void icbsp_unhandled_bxcan_tx_mailbox(CAN_HandleTypeDef *hcan, uint32_t mailbox)
  {
    (void)hcan;
    (void)mailbox;
  }
  __attribute__((weak)) void icbsp_unhandled_bxcan_error(CAN_HandleTypeDef *hcan, uint32_t error_code)
  {
    (void)hcan;
    (void)error_code;
  }
}

#endif // HAL_CAN_MODULE_ENABLED
