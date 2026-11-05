/**
 * @file board_cfg.cpp
 * @brief `board_init()`：把 board_cfg.hpp 里的实例按正确顺序逐个 init()
 *
 * @details
 *
 * 【本文件只放一件事】`board_init()` 的函数体。
 *   实例都定义在 `board_cfg.hpp`（inline 全局变量），配置和流程分开：
 *     - 加外设 / 改参数  → `board_cfg.hpp` + `board_instances.hpp`
 *     - 改顺序 / 加设备  → 本文件
 *
 * 【为什么每行都 configASSERT】
 *   `init()` 返回非 OK 说明硬件或参数不对（CubeMX 没生成句柄、DMA 没配、CAN 起不来、
 *   芯片 ID 读不到…）。就地停机后 `fault_record` 会记下**代码行号**，
 *   一眼能看出是哪一路；继续跑下去只会在半初始化的系统上产生更难查的现象。
 *   想改成“降级运行”也可以，但那样必须在 sys_state 里记错误：
 *     `if (bsp_uart4.init() != Status::OK) { (void)sys_state.error(Status::IO_ERROR); }`
 *   这样 `system_start()` 才知道有子系统挂了（它的 waiting 逻辑依赖 sys_state）。
 *
 * 【顺序有讲究（本文件的顺序不能乱）】
 *   1. **DWT 第一个**：sys_task 一开始就靠它算 10 ms 周期耗时
 *   2. CAN / UART / PWM 之间没有依赖，按 `board_cfg.hpp` 的定义顺序写即可
 *   3. **挂在 CAN 上的设备（电机、电调）放最后**：
 *      - 必须在 CAN `init()` 之后：它们要往 `CanRxNode` / `CanTxNode` 里注册
 *      - 必须在 `system_start()` 之前：`can_tx_task` 是按“有没有发送节点”决定建不建的，
 *        晚于 `system_start()` 注册的发送节点不会被 this 任务认领
 */

#include "board_cfg.hpp"
#include "fault_record.hpp" // ICBSP_CHECK

#include "FreeRTOS.h" // IWYU pragma: keep (configASSERT)


void board_init(void)
{
  // ① 计时基准 `bsp_dwt` 已由 `system_init()` 内部 init()，这里不再重复初始化。

  // ② 总线与外设：失败会停在对应行号，fault_record 里能看到是哪一路。
  ICBSP_CHECK(bsp_can1.init() == Status::OK);
  ICBSP_CHECK(bsp_uart1.init() == Status::OK);
  ICBSP_CHECK(bsp_pwm_buzzer.init() == Status::OK);

  // ③ 设备层：包装上面已 init 的外设。蜂鸣器只是包了一个 PWM 通道，没有额外 init()。
  //
  //    这里开始是“挂了 CAN 的设备”的位置，例如（示例，按实际设备打开）：
  //      ICBSP_CHECK(chassis_lf.init() == Status::OK);   // DjiMotor<Motor3508, ...>
  //      ICBSP_CHECK(servo_yaw.init() == Status::OK);    // DeviceServo
  //      ICBSP_CHECK(damiao.init() == Status::OK);      // DeviceEmmv5Group
  //
  //    它们必须在 CAN init() 之后、system_start() 之前（原因见文件头第 3 条）。
  //    设备 init() 返回 FULL 通常意味着 CAN 节点名额不够：
  //    去 hardware_config.h 加大 CAN_RX_NODE_MAX / CAN_TX_NODE_MAX。
}
