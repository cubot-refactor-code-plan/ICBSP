#include "system.hpp"
#include "hardware_config.h"
#include "board_instances.hpp"
#include "bsp_can.hpp"  // 未开 FDCAN 时为空
#include "bsp_uart.hpp" // 未开 UART 时为空

#include "FreeRTOS.h" // IWYU pragma: keep
#include "online_check.hpp"
#include "task.h"

#include <stdint.h>

#if SYS_TASK_ENABLE

volatile uint32_t sys_loop_count = 0U;       ///< 维护任务循环次数（调试观察）
volatile Status   sys_online_status = Status::NOT_INIT; ///< 最近一次 Online::update() 的结果

// 串口补救巡检的周期（本任务 1 拍 = 1 ms）：UART_PATROL_PERIOD_MS，默认 10 ms。
// tx_recover() 判“通道空闲却有数据”要连续两次巡检都看到才动手，10 ms 间隔与 main 分支实测一致；
// rx_recover() 只在 RX 三处状态不一致时才动作，正常时每次只读一次寄存器。
constexpr uint32_t UART_PATROL_PERIOD = UART_PATROL_PERIOD_MS;



extern "C" void sys_task(void *argument)
{
  (void)argument; // 任务不需要外部参数。
  static_assert(configTICK_RATE_HZ == 1000U,
                "sys_task requires a 1 kHz FreeRTOS tick");

  TickType_t wake_time = xTaskGetTickCount();
  for (;;)
  {
#if defined(HAL_FDCAN_MODULE_ENABLED) && (CAN_RX_ENABLE || CAN_TX_ENABLE)
    // CAN 硬件 Bus-Off 自恢复：按板级实例列表直接展开
#define X(name) (void)name.service_recovery();
    BSP_CAN_INSTANCES(X)
#undef X
#endif
#if defined(HAL_UART_MODULE_ENABLED) && UART_PATROL_ENABLE
    // 串口补救：卡死的发送续发 + 停摆的接收重建（ISR 只计数，恢复只能在任务侧做）
    if ((sys_loop_count % UART_PATROL_PERIOD) == 0U)
    {
#define X(name)            \
      (void)name.tx_recover(); \
      (void)name.rx_recover();
      BSP_UART_INSTANCES(X)
#undef X
    }
#endif
    // 在线情况更新
    sys_online_status = Online::update();
    sys_loop_count = sys_loop_count + 1U;
    vTaskDelayUntil(&wake_time, pdMS_TO_TICKS(1U));
  }
}

#endif // SYS_TASK_ENABLE
