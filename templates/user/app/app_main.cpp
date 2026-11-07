/**
 * @file app_main.cpp
 * @brief 应用入口 `app_init()`：库初始化 → 板级初始化 → 业务任务 → 放行
 *
 * @details
 *
 * 【谁在什么时候调它】
 *   `Core/Src/freertos.c` 的 `MX_FREERTOS_Init()` 在 USER CODE 段里调 `app_init()`，
 *   而 `MX_FREERTOS_Init()` 由 `main()` 在 `osKernelStart()` **之前**调用。
 *   也就是说：本文件里的代码跑在**调度器启动前**——
 *     - 可以创建任务/队列/信号量（FreeRTOS 允许启动前创建对象）
 *     - **不能** `vTaskDelay()`、不能等信号量、不能依赖任何任务在跑
 *     - `xTaskCreate` 出来的任务不会立刻运行，要等 `osKernelStart()` 之后
 *
 * 【完整时序】
 *
 *   main()
 *     → HAL_Init / SystemClock_Config / MX_GPIO_Init / MX_DMA_Init / MX_xxx_Init …（CubeMX）
 *     → MX_FREERTOS_Init()
 *          → app_init()                       ← 本文件
 *     → osKernelStart()                       调度器开始跑，任务才有机会执行
 *
 * 【业务任务的固定写法：先等运行态】
 *   库任务和业务任务都应该在入口先 `sys_state.wait_running()`：
 *   初始化失败时它会让任务自行删除，任务就不会跑在半初始化的系统上。
 *   直接写 `for (;;)` 而不等运行态，初始化失败时会出现“外设没起来但业务在跑”的怪现象。
 */

#include "app_main.h"

#include "app_task.hpp"
#include "board_cfg.hpp"
#include "service/system/system.hpp"

#include "FreeRTOS.h" // IWYU pragma: keep
#include "task.h"


/* ==================== 默认任务（freertos.c 的 StartDefaultTask() 转调这里） ==================== */

/**
 * @note CubeMX 默认建的 `defaultTask`（freertos.c 里另有一个 startDefaultTask 入口）。
 *       本工程不用它做业务，只保留一个空循环占位；
 *       真要停掉它，去 CubeMX 的 FREERTOS 配置里把 defaultTask 删掉。
 */
extern "C" void app_default_task(void *argument)
{
  (void)argument;
  for (;;)
  {
    vTaskDelay(pdMS_TO_TICKS(1000)); // 空循环，保留任务
  }
}


/* ==================== */
/* ==================== sys_task 报告 ==================== */

/**
 * @brief sys_task 的周期报告：每 5 s 把维护耗时打到调试串口（库本身不打印，见 system_set_report_hook()）
 * @note 跑在 sys_task 的栈上；printf 的 vsnprintf 要栈，库任务栈 ICBSP_SYS_TASK_STACK 已按这个留了余量。
 */
static void sys_report(const SysTaskStats *stats)
{
  (void)print_uart("[sys] time=%lus cycle=%luus avg=%luus max=%luus\r\n",
                   static_cast<unsigned long>(xTaskGetTickCount() / configTICK_RATE_HZ),
                   static_cast<unsigned long>(stats->last_us),
                   static_cast<unsigned long>(stats->avg_us),
                   static_cast<unsigned long>(stats->max_us));
}

/* ==================== */
/* ==================== 入口 ==================== */

extern "C" void app_init(void)
{
  // 1. 库入口：建系统事件组 sys_state、创建 sys_task 与 can_rx_task
  //    （此时调度器还没启动，任务创建后处于就绪态，不会运行）
  system_init();

  // 2. 板级初始化：外设实例 init() → 设备 init() → 挂 CAN 的设备注册节点
  //    详细顺序与失败处理见 user/board/board_cfg.cpp
  board_init();

  //    sys_task 的维护耗时：库只计数（全局 sys_task_stats），要打印就在这里接回调；不要可以删掉这一行
  system_set_report_hook(sys_report, 5000U);

  // 3. 业务任务（可选）：示例任务不自动创建，需要时在这里加，例如：
  //      ICBSP_CHECK(xTaskCreate(heartbeat_task, "heartbeat", 256, NULL,
  //                               tskIDLE_PRIORITY + 3, NULL) == pdPASS);
  //    任务入口开头请先调 sys_state.wait_running()（原因见文件头）。
  //    优先级必须低于库任务（CAN / BMI088 / sys 默认是 configMAX_PRIORITIES - 2 / -3 / -4，见 hardware_config.h）：
  //    CubeMX 的 osPriorityNormal（24）、osPriorityHigh（40）都可以。
  //
  //    monitor：每 10 s 打印 heap / min / tasks / stack 四个值（实现见 task/demo_task.cpp）。
  //    默认不建；栈给 384 字（1.5 KB）而不是 256，因为里面要跑 vsnprintf。
  //  ICBSP_CHECK(xTaskCreate(monitor_task, "monitor", 384, NULL, tskIDLE_PRIORITY + 3, NULL) == pdPASS);

  // 4. 放行：置运行位，放行所有在 wait_running() 上等待的任务。
  //    初始化阶段有失败记录（例如 CAN 节点注册失败、外设 init() 失败）时，
  //    这里会断言停机，fault_record 记下本行。
  system_start();
}

/* ==================== */
