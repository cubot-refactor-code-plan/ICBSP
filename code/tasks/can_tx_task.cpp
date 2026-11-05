#include "hardware_config.h"
#include "bsp_series.hpp" // IWYU pragma: keep（HAL 模块开关 / 系列桥接）

// 宿主工程没在 CubeMX 里开 FDCAN 或 CAN_TX_ENABLE=0 时，本文件不参与编译
#if defined(HAL_FDCAN_MODULE_ENABLED) && CAN_TX_ENABLE

#  include "board_instances.hpp"
#  include "can_tx_node.hpp"
#  include "system.hpp"

extern "C" void can_tx_task(void *argument)
{
  (void)argument;
  sys_state.wait_running();

  const TickType_t start_tick = xTaskGetTickCount();
#  define X(name)                                          \
    do                                                     \
    {                                                      \
      using CanT = decltype(name);                         \
      CanTxNodeT<CanT>::freeze();                          \
      CanTxNodeT<CanT>::init_send_ticks(name, start_tick); \
    } while (0);
  BSP_CAN_INSTANCES(X)
#  undef X

  TickType_t wake_time = start_tick;
  for (;;)
  {
#  define X(name) CanTxNodeT<decltype(name)>::service(name);
    BSP_CAN_INSTANCES(X)
#  undef X
    vTaskDelayUntil(&wake_time, pdMS_TO_TICKS(1U));
  }
}

#endif // HAL_FDCAN_MODULE_ENABLED && CAN_TX_ENABLE
