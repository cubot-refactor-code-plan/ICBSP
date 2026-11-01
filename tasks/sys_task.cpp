#include "bsp_can.hpp"  // 未开 FDCAN 时为空
#include "bsp_dwt.hpp"
#include "bsp_uart.hpp" // 未开 UART 时为空
#include "online_check.hpp"
#include "system.hpp"

#include "FreeRTOS.h" // IWYU pragma: keep
#include "task.h"
#include <stdint.h>

#include "hardware_config.h"
#include "board_instances.hpp"

#if SYS_TASK_ENABLE

// 维护周期：Online 计时、CAN 恢复、UART 巡检统一按这个节拍
constexpr uint32_t SYS_TASK_PERIOD_MS = 10U;


extern "C" void sys_task(void *argument)
{
  (void)argument;
  static_assert(configTICK_RATE_HZ == 1000U,
                "sys_task requires a 1 kHz FreeRTOS tick");

  TickType_t wake_time = xTaskGetTickCount();
  uint8_t    count     = 0U;

  for (;;)
  {
    ++count;

    // 本轮计时基准（无状态，首次结果丢弃）
    uint32_t mark = 0U;
    (void)bsp_dwt.get_delta_s(&mark);

#  if defined(HAL_FDCAN_MODULE_ENABLED) && (CAN_RX_ENABLE || CAN_TX_ENABLE)
    // CAN 硬件 Bus-Off 自恢复：按板级实例列表直接展开
#    define X(name) (void)name.service_recovery();
    BSP_CAN_INSTANCES(X)
#    undef X
#  endif

#  if defined(HAL_UART_MODULE_ENABLED) && UART_PATROL_ENABLE
    // 串口补救：卡死的发送续发 + 停摆的接收重建
#    define X(name)            \
      (void)name.tx_recover(); \
      (void)name.rx_recover();
    BSP_UART_INSTANCES(X)
#    undef X
#  endif

    // 在线情况更新
    (void)Online::update();

#  if defined(HAL_UART_MODULE_ENABLED) && defined(print_uart)
    // 每 2 s 打一次，避免打印本身占掉巡检时间
    if (count == 200U)
    {
      const double uptime_s = bsp_dwt.get_time_s();

      print_uart("[sys] time=%us cycle=%uus\r\n",
                 static_cast<unsigned>(uptime_s),
                 static_cast<unsigned>(bsp_dwt.get_delta_s(&mark) * 1000000.0));
      count = 0U;
    }
#  endif

    vTaskDelayUntil(&wake_time, pdMS_TO_TICKS(SYS_TASK_PERIOD_MS));
  }
}

#endif // SYS_TASK_ENABLE
