#include "bsp/bsp_series.hpp" // IWYU pragma: keep（HAL 模块开关 / 系列桥接）

// 宿主工程没在 CubeMX 里开 UART（HAL_UART_MODULE_ENABLED 未定义）时，本文件不参与编译
#ifdef HAL_UART_MODULE_ENABLED

#include "board_instances.hpp"
#include "bsp/bsp_uart.hpp"

#include "FreeRTOS.h" // IWYU pragma: keep


/* ==================== 实例清单核对 ==================== */

bool bsp_uart_is_listed(const void *self, const UART_HandleTypeDef *huart)
{
  // 实例清单为空时 X-macro 展开成空，参数用不到
  (void)self;
  (void)huart;
  bool     listed = false;
  uint32_t same   = 0U; // 清单里绑这个句柄的实例数
#define X(name)                                                    \
  if (static_cast<const void *>(&name) == self)                    \
  {                                                                \
    listed = true;                                                 \
  }                                                                \
  if (name.get_handle() == huart)                                      \
  {                                                                \
    same = same + 1U;                                              \
  }
  ICBSP_UART_INSTANCES(X)
#undef X
  return listed && same == 1U;
}


/* ==================== */
/* ==================== HAL 回调分发 ==================== */

// BspUartT 只有头文件，这里只放 HAL 回调：HAL 的回调没有实例参数，按 huart 指针和 ICBSP_UART_INSTANCES 清单逐个比对。
// 清单外的串口（宿主自己用 HAL 管的）直接忽略。
extern "C"
{
  /** @brief 接收事件（IDLE / HT / TC） */
  void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
  {
    BaseType_t woken = pdFALSE;
    if (false)
    {
    }
#define X(name)                     \
  else if (huart == name.get_handle())  \
  {                                 \
    name.on_idle_isr(Size, &woken); \
  }
    ICBSP_UART_INSTANCES(X)
#undef X
    else
    {
      icbsp_unhandled_uart_rx_event(huart, Size);
    }
    portYIELD_FROM_ISR(woken);
  }

  /** @brief 发送完成：接着发下一段 */
  void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
  {
    BaseType_t woken = pdFALSE;
    if (false)
    {
    }
#define X(name)                               \
  else if (huart == name.get_handle())            \
  {                                           \
    name.start_transmission_from_isr(&woken); \
  }
    ICBSP_UART_INSTANCES(X)
#undef X
    else
    {
      icbsp_unhandled_uart_tx_cplt(huart);
    }
    portYIELD_FROM_ISR(woken);
  }

  /** @brief 错误回调：只计数，恢复由 sys_task 的 rx_recover() / tx_recover() 完成 */
  void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
  {
    if (false)
    {
    }
#define X(name)                    \
  else if (huart == name.get_handle()) \
  {                                \
    name.on_error_isr();           \
  }
    ICBSP_UART_INSTANCES(X)
#undef X
    else
    {
      icbsp_unhandled_uart_error(huart);
    }
  }

  // 清单外串口的回调转调：默认空，宿主定义同名强函数即可（对比评估 N30）
  __attribute__((weak)) void icbsp_unhandled_uart_rx_event(UART_HandleTypeDef *huart, uint16_t size)
  {
    (void)huart;
    (void)size;
  }
  __attribute__((weak)) void icbsp_unhandled_uart_tx_cplt(UART_HandleTypeDef *huart)
  {
    (void)huart;
  }
  __attribute__((weak)) void icbsp_unhandled_uart_error(UART_HandleTypeDef *huart)
  {
    (void)huart;
  }
}
/* ==================== */

#endif // HAL_UART_MODULE_ENABLED
