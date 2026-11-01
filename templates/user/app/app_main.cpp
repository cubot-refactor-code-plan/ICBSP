#include "app_main.h"

#include "board_cfg.hpp"
#include "system.hpp"

#include "FreeRTOS.h" // IWYU pragma: keep
#include "task.h"


/* ==================== 默认任务（覆盖 freertos.c 的 weak 实现） ==================== */

extern "C" void StartDefaultTask(void *argument)
{
  (void)argument;
  for (;;)
  {
    vTaskDelay(pdMS_TO_TICKS(1000)); // 空循环，保留任务
  }
}


/* ==================== 入口 ==================== */

extern "C" void app_init(void)
{
  // 1. 库入口：系统事件组 + sys / can_rx；can_tx 在 system_start() 按节点创建
  system_init();

  // 2. 板级初始化：外设实例 init()、设备 init()、注册 CAN 节点
  board_init();

  // 3. 放行：有发送节点才建 can_tx；初始化失败在这里断言停机
  system_start();
}
