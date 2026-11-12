/**
 * @file newlib_lock.cpp
 * @brief newlib 堆的线程锁（评审 A9）
 *
 * @details newlib(-nano) 的 malloc / free 在进出时调 __malloc_lock / __malloc_unlock，默认实现什么也不做。
 *          库里会走 newlib 堆的地方：`BspUartT::printf()` 的 `%f`（_dtoa_r → Balloc → malloc）、`new (std::nothrow)`
 *          （CAN 节点、VQF，都在初始化期）。两个任务同时对**不同串口**用浮点 printf 时，就会并发进 malloc、破坏堆。
 *          这里用挂起调度器实现（malloc 很短，中断照常响应）；中断里不能 malloc，这里也不处理中断上下文。
 *
 * @note 定义成弱符号：宿主已经有自己的锁（例如 CubeMX FreeRTOS 的 newlib 锁策略生成的 newlib_lock_glue.c）时，宿主的为准。
 * @note 库用到两个堆：FreeRTOS 堆（configTOTAL_HEAP_SIZE，xPortGetFreeHeapSize() 统计的就是它）和 newlib 堆（_sbrk，链接脚本里
 *       .bss 之后到栈之间），后者 xPortGetFreeHeapSize() 统计不到。
 */

#include "FreeRTOS.h" // IWYU pragma: keep
#include "task.h"

#include "bsp/bsp_series.hpp" // IWYU pragma: keep（__get_IPSR）

struct _reent;

namespace
{
/** @brief 任务上下文且调度器在跑：只有这时需要（也只有这时能）挂起调度器 */
bool need_lock()
{
  return __get_IPSR() == 0U && xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED;
}
} // namespace

extern "C"
{
  __attribute__((weak)) void __malloc_lock(struct _reent *reent)
  {
    (void)reent;
    if (need_lock())
    {
      vTaskSuspendAll(); // 可嵌套：malloc 里再 malloc 也平衡
    }
  }

  __attribute__((weak)) void __malloc_unlock(struct _reent *reent)
  {
    (void)reent;
    if (need_lock())
    {
      (void)xTaskResumeAll();
    }
  }
}
