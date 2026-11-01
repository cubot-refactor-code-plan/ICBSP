/**
 * @file board_cfg.cpp
 * @brief board_init()：把 board_cfg.hpp 里的实例逐个 init()
 *
 * @details 【本文件只放一件事】`board_init()`。实例本身都定义在 board_cfg.hpp 里（`inline`），
 *          这里只有函数体，好处是「改配置」和「改流程」分开：
 *          - 加外设 → 改 board_cfg.hpp + board_instances.hpp
 *          - 改初始化顺序 / 加设备 → 改本文件
 *
 *          【为什么每行都 configASSERT】
 *          `init()` 返回非 OK 说明硬件或参数不对（句柄没初始化、DMA 没配、CAN 起不来）。
 *          就地停机后 `fault_record` 会记下代码行号，`print_uart` 里直接能看出是哪一路；
 *          继续跑下去只会在半初始化的系统上产生更难查的现象。
 *
 *          【顺序有讲究】
 *            - DWT 第一个：`sys_task` 一开始就靠它算循环耗时
 *            - CAN / UART / PWM 之间没有依赖，按 board_cfg.hpp 里的定义顺序写即可
 *            - 挂在 CAN 上的设备（电机、电调）放最后：必须在 CAN `init()` 之后
 *              （它们要往 CanRxNode / CanTxNode 里注册），但在 `system_start()` 之前
 *              （`can_tx_task` 是按已注册的发送节点决定要不要建的）
 *
 *          【不要在这里做】
 *            - 不调 `HAL_Init()` / `SystemClock_Config()`：那是 CubeMX 的 `main.c` 干的
 *            - 不创建任务：库任务在 `system_init()` / `system_start()` 里创建
 *            - 不写业务逻辑：业务任务写在 `user/app/task/`
 */

#include "board_cfg.hpp"

#include "FreeRTOS.h" // IWYU pragma: keep (configASSERT)


void board_init(void)
{
  // 计时基准：sys_task 用它算 10 ms 周期内的耗时，必须最先起来
  configASSERT(bsp_dwt.init() == Status::OK);

  // 总线与外设：失败会停在对应行号，fault_record 里能看到是哪一路
  configASSERT(bsp_can1.init() == Status::OK);
  configASSERT(bsp_uart1.init() == Status::OK);
  configASSERT(bsp_pwm_buzzer.init() == Status::OK);

  // CAN 设备（电机、电调）放在这里：CAN 已 init、system_start() 还没调
  // 例：
  //   configASSERT(chassis_lf.init() == Status::OK);   // DjiMotor<Motor3508, ...>
  //   configASSERT(damiao.init() == Status::OK);       // 其它 CAN 设备
}
