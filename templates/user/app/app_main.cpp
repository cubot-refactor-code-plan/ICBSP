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

#include "board_cfg.hpp"
#include "system.hpp"
#include "app_task.hpp"

#include "FreeRTOS.h" // IWYU pragma: keep
#include "task.h"


/* ==================== 默认任务（覆盖 freertos.c 的 weak 实现） ==================== */

/**
 * @note CubeMX 默认建的 `defaultTask`（freertos.c 里另有一个 startDefaultTask 入口）。
 *       本工程不用它做业务，只保留一个空循环占位；
 *       真要停掉它，去 CubeMX 的 FREERTOS 配置里把 defaultTask 删掉。
 */
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
  // 1. 库入口：建系统事件组 sys_state、创建 sys_task 与 can_rx_task
  //    （此时调度器还没启动，任务创建后处于就绪态，不会运行）
  system_init();

  // 2. 板级初始化：外设实例 init() → 设备 init() → 挂 CAN 的设备注册节点
  //    详细顺序与失败处理见 user/board/board_cfg.cpp
  board_init();

  // 3. 业务任务（可选）：示例任务不自动创建，需要时在这里加，例如：
  //      configASSERT(xTaskCreate(heartbeat_task, "heartbeat", 256, NULL,
  //                               tskIDLE_PRIORITY + 3, NULL) == pdPASS);
  //    任务入口开头请先调 sys_state.wait_running()（原因见文件头）。
  //    优先级建议 ≤ tskIDLE_PRIORITY + 5：让库的 CAN / sys 任务（+8 / +7）先跑。


  // 4. 放行：置运行位，放行所有在 wait_running() 上等待的任务。
  //    初始化阶段有失败记录（例如 CAN 节点注册失败、外设 init() 失败）时，
  //    这里会断言停机，fault_record 记下本行。
  system_start();
}
