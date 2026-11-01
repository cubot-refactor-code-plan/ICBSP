#include "stm32h7xx_hal.h" // IWYU pragma: keep（HAL 模块开关）
#include "hardware_config.h"

// 宿主工程没在 CubeMX 里开 FDCAN 或 CAN_RX_ENABLE=0 时，本文件不参与编译
#if defined(HAL_FDCAN_MODULE_ENABLED) && CAN_RX_ENABLE

#include "board_instances.hpp"
#include "can_rx_node.hpp"
#include "system.hpp"

template <typename CanT>
static void can_rx_service(CanT &can)
{
  for (uint32_t i = 0U; i < CAN_RX_DISPATCH_PER_TICK; ++i)
  {
    CanRxMsg rx = {};
    if (can.read_raw(&rx, 0U) != Status::OK)
    {
      break;
    }

    bool consumed = false;
    if ((rx.header.IdType == FDCAN_STANDARD_ID || rx.header.IdType == FDCAN_EXTENDED_ID) &&
        rx.header.RxFrameType == FDCAN_DATA_FRAME && rx.header.FDFormat == FDCAN_CLASSIC_CAN &&
        rx.header.DataLength == FDCAN_DLC_BYTES_8)
    {
      consumed = CanRxNodeT<CanT>::dispatch(can, rx);
    }

    // 没有接收节点认领的帧放回回退缓冲，仍可由 can.receive() 读取。
    if (!consumed)
    {
      (void)can.return_frame(rx);
    }
  }
}

extern "C" void can_rx_task(void *argument)
{
  (void)argument;
  sys_state.wait_running();
#define X(name) CanRxNodeT<decltype(name)>::freeze();
  BSP_CAN_INSTANCES(X)
#undef X

  static_assert(configTICK_RATE_HZ == 1000U, "can_rx_task requires a 1 kHz tick");
  TickType_t wake_time = xTaskGetTickCount();
  for (;;)
  {
    if (sys_state.running())
    {
#define X(name) can_rx_service(name);
      BSP_CAN_INSTANCES(X)
#undef X
    }
    vTaskDelayUntil(&wake_time, pdMS_TO_TICKS(1U));
  }
}

#endif // HAL_FDCAN_MODULE_ENABLED && CAN_RX_ENABLE
