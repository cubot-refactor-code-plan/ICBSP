#include "bsp/bsp_can.hpp" // 未开 FDCAN 时为空
#include "bsp/bsp_dwt.hpp"
#include "bsp/bsp_uart.hpp" // 未开 UART 时为空
#include "service/monitor/online_check.hpp"
#include "service/system/system.hpp"

#include "FreeRTOS.h" // IWYU pragma: keep
#include "task.h"
#include <stdint.h>

#include "board_instances.hpp"
#include "hardware_config.h"

// 维护周期：Online 计时、CAN 恢复、UART 巡检统一按这个节拍
constexpr uint32_t SYS_TASK_PERIOD_MS = 10U;

static_assert(ICBSP_CAN_BUSOFF_RETRY_MS >= SYS_TASK_PERIOD_MS, "hardware_config.h: ICBSP_CAN_BUSOFF_RETRY_MS 不能短于 sys_task 的 10 ms 节拍");

extern "C"
{
  volatile SysTaskStats sys_task_stats = {};
}

namespace
{
// 报告回调与周期：任务侧随时可改，sys_task 每周期读一次（两个字分开读，最多错一个周期，无害）
SystemReportHook volatile report_hook          = nullptr;
volatile uint32_t         report_period_cycles = 0U;
} // namespace

extern "C" void system_set_report_hook(SystemReportHook hook, uint32_t period_ms)
{
  const uint32_t cycles = period_ms / SYS_TASK_PERIOD_MS;
  report_period_cycles  = (cycles == 0U) ? 1U : cycles;
  report_hook           = hook;
}

#if ICBSP_SYS_TASK_ENABLE


extern "C" void sys_task(void *argument)
{
  (void)argument;
  static_assert(configTICK_RATE_HZ == 1000U,
                "sys_task requires a 1 kHz FreeRTOS tick");

  TickType_t wake_time   = xTaskGetTickCount();
  uint32_t   report_cnt  = 0U; // 距上次报告的周期数
  uint32_t   window_cnt  = 0U; // 平均值窗口内的周期数
  uint64_t   window_sum  = 0U; // 平均值窗口内的耗时和 (µs)
  constexpr uint32_t AVG_WINDOW = 1000U / SYS_TASK_PERIOD_MS;

  vTaskDelayUntil(&wake_time, pdMS_TO_TICKS(1000));
  for (;;)
  {
    // 本轮计时基准（无状态，首次结果丢弃）
    uint32_t mark = 0U;
    (void)bsp_dwt.get_delta_s(&mark);

    // DWT 绝对时间的回绕跟踪：两次调用要短于一圈（550 MHz 下 7.8 s），这里每 10 ms 推进一次（C11）
    (void)bsp_dwt.get_cycles();

#  if defined(HAL_FDCAN_MODULE_ENABLED) && (ICBSP_CAN_RX_ENABLE || ICBSP_CAN_TX_ENABLE)
    // CAN：Bus-Off 自恢复 + 发送兜底（软件缓冲有帧、TX-FIFO-EMPTY 中断却关着时补发，N27）
#    define X(name)                   \
      (void)name.service_recovery(); \
      (void)name.tx_recover();
    ICBSP_CAN_INSTANCES(X)
#    undef X
#  endif

#  if defined(HAL_UART_MODULE_ENABLED) && ICBSP_UART_PATROL_ENABLE
    // 串口补救：卡死的发送续发 + 停摆的接收重建
#    define X(name)            \
      (void)name.tx_recover(); \
      (void)name.rx_recover();
    ICBSP_UART_INSTANCES(X)
#    undef X
#  endif

    // 在线情况更新
    (void)Online::update();

    // 耗时统计（不含报告回调本身）
    const uint32_t cost_us  = static_cast<uint32_t>(bsp_dwt.get_delta_s(&mark) * 1000000.0);
    sys_task_stats.cycles  = sys_task_stats.cycles + 1U;
    sys_task_stats.last_us = cost_us;
    if (cost_us > sys_task_stats.max_us)
    {
      sys_task_stats.max_us = cost_us;
    }
    window_sum = window_sum + cost_us;
    window_cnt = window_cnt + 1U;
    if (window_cnt >= AVG_WINDOW)
    {
      sys_task_stats.avg_us = static_cast<uint32_t>(window_sum / window_cnt);
      window_sum            = 0U;
      window_cnt            = 0U;
    }

    // 周期报告：宿主用 system_set_report_hook() 接上（例如打印到调试串口），库本身不打印
    const SystemReportHook hook = report_hook;
    report_cnt                  = report_cnt + 1U;
    if (hook != nullptr && report_cnt >= report_period_cycles)
    {
      report_cnt               = 0U;
      const SysTaskStats snap = {sys_task_stats.cycles, sys_task_stats.last_us, sys_task_stats.max_us, sys_task_stats.avg_us};
      hook(&snap);
    }

    vTaskDelayUntil(&wake_time, pdMS_TO_TICKS(SYS_TASK_PERIOD_MS));
  }
}

#endif // ICBSP_SYS_TASK_ENABLE
