/**
 * @file app_task.hpp
 * @brief 示例业务：两个不依赖任何外设的任务（heartbeat / monitor）
 *
 * @details 不自动创建；需要时在 app_main.cpp 里 xTaskCreate。
 */

#ifndef __APP_TASK_HPP__
#define __APP_TASK_HPP__


/* ==================== 任务入口 ==================== */

/**
 * @brief 心跳业务：每秒递增内部心跳计数
 * @param argument 未使用（NULL）
 */
void heartbeat_task(void *argument);

/**
 * @brief 监视业务：每秒记录 FreeRTOS 堆余量与任务数到内部观察量
 * @param argument 未使用（NULL）
 */
void monitor_task(void *argument);

/* ==================== */

#endif // __APP_TASK_HPP__
