#include "service/system/config_check.hpp" // 宿主配置（经检查）
#include "bsp/bsp_series.hpp" // IWYU pragma: keep（HAL 模块开关 / 系列桥接）

// 宿主工程没在 CubeMX 里开 FDCAN 或 ICBSP_CAN_RX_ENABLE=0 时，本文件不参与编译
#if defined(HAL_FDCAN_MODULE_ENABLED) && ICBSP_CAN_RX_ENABLE

#  include "board_instances.hpp"
#  include "service/comm/can_rx_node.hpp"
#  include "service/system/system.hpp"

template <typename CanT>
static void can_rx_service(CanT &can)
{
  for (uint32_t i = 0U; i < ICBSP_CAN_RX_DISPATCH_PER_TICK; ++i)
  {
    CanRxMsg rx = {};
    if (can.read_raw(&rx, 0U) != Status::OK)
    {
      break;
    }

    bool consumed = false;
    // 远程帧由硬件拒收、DLC > 8 的帧在中断里已丢弃；长度是否符合由各节点的 dlc_mask 决定（评审 I2）
    if ((rx.header.IdType == FDCAN_STANDARD_ID || rx.header.IdType == FDCAN_EXTENDED_ID) && rx.header.RxFrameType == FDCAN_DATA_FRAME && rx.header.FDFormat == FDCAN_CLASSIC_CAN)
    {
      consumed = CanRxNodeT<CanT>::dispatch(can, rx);
    }

    // 没有接收节点认领的帧：计入 rx_unclaimed；有回退缓冲（ICBSP_CAN_RX_RETURN_FRAMES > 0）就放进去，供 can.receive() 读取
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
#  define X(name) CanRxNodeT<decltype(name)>::freeze();
  ICBSP_CAN_INSTANCES(X)
#  undef X

  static_assert(configTICK_RATE_HZ == 1000U, "can_rx_task requires a 1 kHz tick");
  TickType_t wake_time = xTaskGetTickCount();
  for (;;)
  {
    if (sys_state.is_running())
    {
#  define X(name) can_rx_service(name);
      ICBSP_CAN_INSTANCES(X)
#  undef X
    }
    vTaskDelayUntil(&wake_time, pdMS_TO_TICKS(1U));
  }
}

#endif // HAL_FDCAN_MODULE_ENABLED && ICBSP_CAN_RX_ENABLE
