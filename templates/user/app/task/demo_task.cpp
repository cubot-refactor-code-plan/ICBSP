#include "app_task.hpp"

#include "system.hpp"

#include "FreeRTOS.h" // IWYU pragma: keep
#include "task.h"


/* ==================== 调试观察量（仅本文件可见） ==================== */

/** @brief 运行监视结果（GDB：print user_monitor） */
struct UserMonitor
{
  uint32_t heap_free;       ///< 当前 FreeRTOS 堆剩余（字节）
  uint32_t heap_min_free;   ///< 开机以来 FreeRTOS 堆最低剩余（字节）：接近 0 就该加 configTOTAL_HEAP_SIZE
  uint32_t task_num;        ///< 当前任务数
  uint32_t self_stack_free; ///< 本任务栈最低剩余（字）；INCLUDE_uxTaskGetStackHighWaterMark = 0 时恒为 0
};

static volatile uint32_t    user_alive_count = 0U; ///< 心跳计数：每秒加一（GDB：print user_alive_count）
static volatile UserMonitor user_monitor     = {}; ///< 运行监视结果，每秒刷新


/* ==================== 任务 ==================== */

/**
 * @note 任务入口先等运行态：初始化失败时 sys_state.wait_running() 让任务自行退出，不会跑在半初始化的系统上。
 */
void heartbeat_task(void *argument)
{
  (void)argument;
  sys_state.wait_running();
  TickType_t wake = xTaskGetTickCount();
  for (;;)
  {
    user_alive_count = user_alive_count + 1U;
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(1000U));
  }
}

void monitor_task(void *argument)
{
  (void)argument;
  sys_state.wait_running();
  TickType_t wake = xTaskGetTickCount();
  for (;;)
  {
    user_monitor.heap_free       = static_cast<uint32_t>(xPortGetFreeHeapSize());
    user_monitor.heap_min_free   = static_cast<uint32_t>(xPortGetMinimumEverFreeHeapSize());
    user_monitor.task_num        = static_cast<uint32_t>(uxTaskGetNumberOfTasks());
#if INCLUDE_uxTaskGetStackHighWaterMark
    user_monitor.self_stack_free = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(NULL));
#endif
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(1000U));
  }
}
