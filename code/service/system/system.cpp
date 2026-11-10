#include "service/system/system.hpp"
#include "service/system/config_check.hpp" // 宿主配置（经检查）

#include <string.h>

#include "FreeRTOS.h" // IWYU pragma: keep
#include "bsp/bsp_dwt.hpp" // 计时基准（库内实例，本文件负责 init）
#include "service/system/state_tree.hpp" // EventState 默认树的总线层
#include "service/system/status.hpp"
#include "bsp/bsp_series.hpp" // IWYU pragma: keep（HAL 模块开关 / 系列桥接）
#include "task.h"

#if defined(HAL_FDCAN_MODULE_ENABLED) && ICBSP_CAN_TX_ENABLE
#  include "board_instances.hpp"
#  include "service/comm/can_tx_node.hpp" // CanTxNodeT::count_for()

namespace
{
bool has_tx_nodes()
{
#  define X(name)                                         \
    if (CanTxNodeT<decltype(name)>::count_for(name) > 0U) \
    {                                                     \
      return true;                                        \
    }
  ICBSP_CAN_INSTANCES(X)
#  undef X
  return false;
}
} // namespace
#endif


// 库任务优先级等取值检查在 service/system/config_check.hpp（宿主删掉 hardware_config.h 里的检查也照样生效，评审 A2 / Q3）

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
  // 外置 Flash 方案 A（QSPI_RAM）：整个程序已由 bootloader 拷进 AXI SRAM，加载地址就是运行地址
  if (size > 0U && static_cast<const void *>(__dma_buffer_load) != static_cast<const void *>(__dma_buffer_start))
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

  // 默认树的总线层（Q6）：每个清单里的 CAN / UART / SPI 实例一个节点，挂在 sys_state 下；
  // 设备在 board_init() 里 init() 时挂到所用总线的节点下
  ICBSP_CHECK(state_tree_init() == Status::OK);

  // 调度器启动前任务不会运行（CAN 收发任务还会先等运行态），所以可以早于用户的外设 init()；
  // 任务名受 configMAX_TASK_NAME_LEN（16）限制
#if ICBSP_SYS_TASK_ENABLE
  ICBSP_CHECK(xTaskCreate(sys_task, "sys", ICBSP_SYS_TASK_STACK, NULL, ICBSP_SYS_TASK_PRIORITY, NULL) == pdPASS);
#endif

#if defined(HAL_FDCAN_MODULE_ENABLED) && ICBSP_CAN_RX_ENABLE
  ICBSP_CHECK(xTaskCreate(can_rx_task, "can_rx", ICBSP_CAN_TASK_STACK, NULL, ICBSP_CAN_TASK_PRIORITY, NULL) == pdPASS);
#endif
}

extern "C" void system_start(void)
{

#if defined(HAL_FDCAN_MODULE_ENABLED) && ICBSP_CAN_TX_ENABLE
  // 发送节点在 board_init() 里注册完；任意一个实例有发送节点才创建 can_tx 任务
  if (has_tx_nodes())
  {
    ICBSP_CHECK(xTaskCreate(can_tx_task, "can_tx", ICBSP_CAN_TASK_STACK, NULL, ICBSP_CAN_TASK_PRIORITY, NULL) == pdPASS);
  }

/* ==================== */

#endif

  // 总线节点先进入运行态，并记下诊断计数的基线（之后计数再涨才算运行期错误）
  state_tree_start();

  // 有初始化失败记录（设备 / 注册函数的 init_error 一路传上来）就在这里停机，fault_record 记下本行；
  // 具体是哪条总线下的设备，查 bus_state(&实例)->get_error_code()
  ICBSP_CHECK(sys_state.complete_init() == Status::OK);
}
