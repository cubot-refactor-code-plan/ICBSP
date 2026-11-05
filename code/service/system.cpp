#include "system.hpp"
#include "hardware_config.h"

#include <string.h>

#include "FreeRTOS.h" // IWYU pragma: keep
#include "bsp_dwt.hpp" // 计时基准（库内实例，本文件负责 init）
#include "status.hpp"
#include "bsp_series.hpp" // IWYU pragma: keep（HAL 模块开关 / 系列桥接）
#include "task.h"

#if defined(HAL_FDCAN_MODULE_ENABLED) && CAN_TX_ENABLE
#  include "board_instances.hpp"
#  include "can_tx_node.hpp" // CanTxNodeT::count_for()

namespace
{
bool has_tx_nodes()
{
#  define X(name)                                         \
    if (CanTxNodeT<decltype(name)>::count_for(name) > 0U) \
    {                                                     \
      return true;                                        \
    }
  BSP_CAN_INSTANCES(X)
#  undef X
  return false;
}
} // namespace
#endif


// 库任务优先级的取值检查放在库里：宿主删掉 hardware_config.h 里的检查也照样生效（评审 A2 / Q3）
static_assert(SYS_TASK_PRIORITY > tskIDLE_PRIORITY && SYS_TASK_PRIORITY < configMAX_PRIORITIES, "SYS_TASK_PRIORITY 取 1 ~ configMAX_PRIORITIES - 1");
static_assert(CAN_TASK_PRIORITY > tskIDLE_PRIORITY && CAN_TASK_PRIORITY < configMAX_PRIORITIES, "CAN_TASK_PRIORITY 取 1 ~ configMAX_PRIORITIES - 1");
static_assert(CAN_TASK_PRIORITY > SYS_TASK_PRIORITY, "CAN 收发任务要比 sys_task 优先：1 kHz 节拍内必须跑完");

/* ==================== 全局系统状态 ==================== */

/** @brief 库内唯一的 DWT 实例：本文件定义并在 `system_init()` 里 init() */
BspDwt bsp_dwt;

EventState sys_state;


/* ==================== */
/* ==================== 库入口 ==================== */


// .dma_buffer 在链接脚本里是「RAM 运行、Flash 存初值」的可加载段；CubeMX 的启动文件只拷 .data，
// 所以这里补一次拷贝。符号由 templates/linker_sections.ld 提供；宿主没加该段时按空段跳过。
extern "C"
{
  extern uint8_t __dma_buffer_start[] __attribute__((weak));
  extern uint8_t __dma_buffer_end[] __attribute__((weak));
  extern uint8_t __dma_buffer_load[] __attribute__((weak));
}

namespace
{
/**
 * @brief 把 .dma_buffer 的初值从 Flash 拷进 RAM_D1
 *
 * @note 必须早于该段里所有 C++ 全局对象的动态初始化：BspSpi 这类对象的构造函数会把配置写进
 *       对象，拷贝放在构造之后会把构造函数的结果覆盖掉。constructor(101) 排在默认
 *       .init_array（65535）之前，此时 .data / .bss 已经由启动文件处理完。
 */
void init_dma_buffer()
{
  if (__dma_buffer_start == nullptr || __dma_buffer_end == nullptr || __dma_buffer_load == nullptr)
  {
    return;
  }
  const size_t size = static_cast<size_t>(__dma_buffer_end - __dma_buffer_start);
  if (size > 0U)
  {
    memcpy(__dma_buffer_start, __dma_buffer_load, size);
  }
}

__attribute__((constructor(101))) void init_dma_buffer_early()
{
  init_dma_buffer();
}
} // namespace


extern "C" void system_init(void)
{
  // 计时基准最先起来：sys_task 用它算 10 ms 周期的耗时；宿主的 board_init() 不用再 init()
  ICBSP_CHECK(bsp_dwt.init() == Status::OK);

  // 系统状态最先建：CAN 节点注册、任务入口的 wait_running() 都依赖它
  ICBSP_CHECK(sys_state.init() == Status::OK);

  // 调度器启动前任务不会运行（CAN 收发任务还会先等运行态），所以可以早于用户的外设 init()；
  // 任务名受 configMAX_TASK_NAME_LEN（16）限制
#if SYS_TASK_ENABLE
  ICBSP_CHECK(xTaskCreate(sys_task, "sys", SYS_TASK_STACK, NULL, SYS_TASK_PRIORITY, NULL) == pdPASS);
#endif

#if defined(HAL_FDCAN_MODULE_ENABLED) && CAN_RX_ENABLE
  ICBSP_CHECK(xTaskCreate(can_rx_task, "can_rx", CAN_TASK_STACK, NULL, CAN_TASK_PRIORITY, NULL) == pdPASS);
#endif
}

extern "C" void system_start(void)
{

#if defined(HAL_FDCAN_MODULE_ENABLED) && CAN_TX_ENABLE
  // 发送节点在 board_init() 里注册完；任意一个实例有发送节点才创建 can_tx 任务
  if (has_tx_nodes())
  {
    ICBSP_CHECK(xTaskCreate(can_tx_task, "can_tx", CAN_TASK_STACK, NULL, CAN_TASK_PRIORITY, NULL) == pdPASS);
  }

/* ==================== */

#endif

  // 有初始化失败记录（各注册函数调过 sys_state.init_error）就在这里停机，fault_record 记下本行
  ICBSP_CHECK(sys_state.complete_init() == Status::OK);
}
