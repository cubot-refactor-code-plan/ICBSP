/**
 * @file app_main.h
 * @brief C 入口（模板）：freertos.c 的 MX_FREERTOS_Init() 只调这一个函数
 */

#ifndef __APP_MAIN_H__
#define __APP_MAIN_H__

#ifdef __cplusplus
extern "C"
{
#endif

  /** @brief 应用初始化：库入口 → 板级初始化 → 业务任务 → 放行（在调度器启动前调用） */
  void app_init(void);

  /**
   * @brief CubeMX 默认任务 defaultTask 的任务体：freertos.c 的 StartDefaultTask() 在 USER CODE 段里转调它（不返回）
   * @note 不在 C++ 里直接定义 StartDefaultTask：只有 CubeMX 把 defaultTask 设成 "As weak" 时强定义才能覆盖，
   *       默认设置下会重复定义（对比评估 N25）。integrate.py 负责把转调写进 USER CODE StartDefaultTask 段。
   */
  void app_default_task(void *argument);

#ifdef __cplusplus
}
#endif

#endif // __APP_MAIN_H__
