/**
 * @file system.hpp
 * @author ChoseB
 * @brief stm32_framework 的入口：建系统状态、创建库自带的后台任务、置运行态
 * @version 0.1
 * @date 2026-10-17
 *
 * @copyright Copyright (c) 2026
 *
 * @details 宿主工程在 freertos.c 的 MX_FREERTOS_Init()（USER CODE 段）里按顺序调用：
 *
 *            system_init();    // 1. 建系统事件组 + 创建 sys / can_rx（调度器启动前不会运行）
 *            board_init();     // 2. 用户：各外设实例 init()、设备 init()、注册 CAN 节点等
 *            system_start();   // 3. 有发送节点才建 can_tx，然后置运行位
 *
 *          任务体在 tasks/：sys / can_rx / can_tx；业务任务由用户在 user/app/task/ 里创建。
 *            - sys_task    CAN Bus-Off 恢复、每 10 ms 串口收发补救、推进 Online 离线计时；
 *            - can_rx_task 把各 CAN 收到的帧按 ID 分发给接收节点，没人要的进回退缓冲；
 *            - can_tx_task 按槽位聚合发送 CAN 控制帧（保底心跳 + 100 ms 指令有效期）；
 *              在 system_start() 里检查 CanTxNode::count()，没有发送节点时不创建。
 *          任务体在 tasks/，它们遍历 BspCan / BspUartBase 的实例登记表，不引用任何板级实例。
 *
 * @note 任务优先级与栈深等可配置项见宿主 user/board/hardware_config.h。
 */

#ifndef __SYSTEM_HPP__
#define __SYSTEM_HPP__

#ifdef __cplusplus
extern "C"
{
#endif

  /**
   * @brief 库初始化：建系统事件组、创建库任务
   * @note 必须最先调用（CAN 节点注册等要用系统事件组）；在调度器启动前调用，失败就地断言停机。
   */
  void system_init(void);

  /**
   * @brief 初始化完成：置运行位，放行所有等待运行态的任务
   * @note 有初始化失败记录（SysInitError）时断言停机，fault_record 记下本次调用位置。
   */
  void system_start(void);

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
