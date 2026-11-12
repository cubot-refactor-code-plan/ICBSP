/**
 * @file system.hpp
 * @author ChoseB
 * @brief icbsp 的入口：建系统状态、创建库自带的后台任务、置运行态
 * @version 0.1
 * @date 2026-10-17
 *
 * @copyright Copyright (c) 2026
 *
 * @details 宿主工程在 freertos.c 的 MX_FREERTOS_Init()（USER CODE 段）里按顺序调用：
 *
 *            system_init();    // 1. 建系统 EventState + 创建 sys / can_rx（调度器启动前不会运行）
 *            board_init();     // 2. 用户：各外设实例 init()、设备 init()、注册 CAN 节点等
 *            system_start();   // 3. 有发送节点才建 can_tx，然后置运行位
 *
 *          任务体在 tasks/：sys / can_rx / can_tx；业务任务由用户在 user/app/task/ 里创建。
 *            - sys_task    CAN Bus-Off 恢复、每 10 ms 串口收发补救、推进 Online 离线计时；
 *            - can_rx_task 把各 CAN 收到的帧按 ID 分发给接收节点，没人要的进回退缓冲；
 *            - can_tx_task 按槽位聚合发送 CAN 控制帧（保底心跳 + 100 ms 指令有效期）；
 *              在 system_start() 里检查板级实例的发送节点，没有发送节点时不创建。
 *          任务体在 tasks/，通过 board_instances.hpp 的 X-macro 展开，不维护实例登记表。
 *
 * @note 任务优先级与栈深等可配置项见宿主 user/board/hardware_config.h。
 */

#ifndef __SYSTEM_HPP__
#define __SYSTEM_HPP__

#include <stdint.h>

#ifdef __cplusplus
#include "service/system/status.hpp"
#include "service/system/fault_record.hpp" // IWYU pragma: keep（ICBSP_CHECK）

/** @brief 整个系统的状态（定义在 service/system.cpp） */
extern EventState sys_state;
#endif

#ifdef __cplusplus
extern "C"
{
#endif

  /**
   * @brief 库初始化：建系统 EventState、创建库任务
   * @note 必须最先调用（CAN 节点注册等要用系统状态）；在调度器启动前调用，失败就地断言停机。
   */
  void system_init(void);

  /**
   * @brief 初始化完成：置运行位，放行所有等待运行态的任务
   * @note 有初始化失败记录（sys_state.init_error）时断言停机，fault_record 记下本次调用位置。
   */
  void system_start(void);

  /**
   * @brief sys_task 的循环耗时统计（库内计数，不依赖宿主的打印宏，对比评估 N14 / Q5）
   * @note 单位 µs，由 DWT 计时；只有 sys_task 写。调试器里可以直接读全局变量 sys_task_stats。
   */
  typedef struct
  {
    uint32_t cycles;  ///< 已运行的维护周期数（每 10 ms 一次）
    uint32_t last_us; ///< 最近一个周期的维护耗时
    uint32_t max_us;  ///< 启动以来的最大维护耗时
    uint32_t avg_us;  ///< 最近 1 s（100 个周期）的平均维护耗时，每 1 s 更新一次
  } SysTaskStats;

  /** @brief sys_task 的循环耗时统计（定义在 tasks/sys_task.cpp；ICBSP_SYS_TASK_ENABLE = 0 时恒为 0） */
  extern volatile SysTaskStats sys_task_stats;

  /**
   * @brief 周期报告回调：在 sys_task 里调用，参数是当前统计的快照
   * @note 跑在 sys_task 的栈上（ICBSP_SYS_TASK_STACK）；回调里打印要留够 vsnprintf 的栈，不要阻塞太久（会推迟 CAN 恢复和串口巡检）。
   */
  typedef void (*SystemReportHook)(const SysTaskStats *stats);

  /**
   * @brief 设置周期报告回调（默认没有，库自己不打印任何东西）
   * @param hook      回调；nullptr = 取消
   * @param period_ms 调用周期，按 10 ms 取整，最小 10 ms
   * @note 任何时候都可以调用（例如 system_init() 之后，在 board_init() 里把它接到调试串口的 printf）。
   */
  void system_set_report_hook(SystemReportHook hook, uint32_t period_ms);

  /** @brief 系统维护任务（由 system_init() 创建，业务代码不要直接调用） */
  void sys_task(void *argument);

  /** @brief CAN 接收分发任务（由 system_init() 创建） */
  void can_rx_task(void *argument);

  /** @brief CAN 发送任务（由 system_init() 创建） */
  void can_tx_task(void *argument);

#ifdef __cplusplus
}
#endif

#endif // __SYSTEM_HPP__
