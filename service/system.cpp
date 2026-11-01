#include "system.hpp"
#include "hardware_config.h"

#include "FreeRTOS.h" // IWYU pragma: keep
#include "status.hpp"
#include "stm32h7xx_hal.h" // IWYU pragma: keep（HAL 模块开关）
#include "task.h"

#if defined(HAL_FDCAN_MODULE_ENABLED) && CAN_TX_ENABLE
#  include "can_tx_node.hpp" // CanTxNode::count()
#endif


// ---------------- 库入口 ----------------


extern "C" void system_init(void)
{
  // 系统事件组最先建：CAN 节点注册、SysFlagWaitRunning() 都依赖它
  configASSERT(SysFlagInit() == Status::OK);

  // 调度器启动前任务不会运行（CAN 收发任务还会先等运行态），所以可以早于用户的外设 init()；
  // 任务名受 configMAX_TASK_NAME_LEN（16）限制
#if SYS_TASK_ENABLE
  configASSERT(xTaskCreate(sys_task, "sys", SYS_TASK_STACK, NULL, SYS_TASK_PRIORITY, NULL) == pdPASS);
#endif
#if defined(HAL_FDCAN_MODULE_ENABLED) && CAN_RX_ENABLE
  configASSERT(xTaskCreate(can_rx_task, "can_rx", CAN_TASK_STACK, NULL, CAN_TASK_PRIORITY, NULL) == pdPASS);
#endif
}

extern "C" void system_start(void)
{
#if defined(HAL_FDCAN_MODULE_ENABLED) && CAN_TX_ENABLE
  // 发送节点在 board_init() 里注册完；没有节点就不创建 can_tx 任务
  if (CanTxNode::count() > 0U)
  {
    configASSERT(xTaskCreate(can_tx_task, "can_tx", CAN_TASK_STACK, NULL, CAN_TASK_PRIORITY, NULL) == pdPASS);
  }
#endif

  // 有初始化失败记录（各注册函数调过 SysInitError）就在这里停机，fault_record 记下本行
  configASSERT(SysCompleteInit() == Status::OK);
}


// ----------------
