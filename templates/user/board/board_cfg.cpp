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
 * 【为什么每行都 ICBSP_CHECK】
 *   `init()` 返回非 OK 说明硬件或参数不对（CubeMX 没生成句柄、DMA 没配、实例没写进清单、CAN 起不来、
 *   芯片 ID 读不到…）。`ICBSP_CHECK(x.init() == Status::OK)` 总会求值（不要写进 configASSERT：断言可能被定义成空），
 *   失败就地停机，`fault_record` 会记下**代码行号**，
 *   一眼能看出是哪一路；继续跑下去只会在半初始化的系统上产生更难查的现象。
 *   想改成“降级运行”也可以，但那样必须在 sys_state 里记错误：
 *     `if (bsp_uart4.init() != Status::OK) { (void)sys_state.error(Status::IO_ERROR); }`
 *   这样 `system_start()` 才知道有子系统挂了（它的 waiting 逻辑依赖 sys_state）。
 *
 * 【顺序有讲究（本文件的顺序不能乱）】
 *   1. 计时基准 `bsp_dwt` 由库在 `system_init()` 里初始化，宿主不用管
 *   2. 总线与外设（CAN / UART / SPI / PWM）之间没有依赖，按 `board_cfg.hpp` 的定义顺序写即可
 *   3. **设备放在它用的总线之后**：
 *      - CAN 设备（DjiMotor、DmMotor）要往 `CanRxNodeT` / `CanTxNodeT` 里注册，并挂到总线的状态节点下
 *      - 串口设备（R9DS、Emm_V5 组）、SPI 设备（BMI088）、PWM 设备（舵机、蜂鸣器）同理，放在各自的外设之后
 *   4. 全部在 `system_start()` 之前：`can_tx_task` 按“有没有发送节点”决定建不建，运行态之后再注册节点会被拒绝
 */

#include "board_cfg.hpp"
#include "service/system/fault_record.hpp" // ICBSP_CHECK

#include "FreeRTOS.h" // IWYU pragma: keep (configASSERT)


/* ==================== 设备实例（构造不是 constexpr，定义在这里，board_cfg.hpp 里 extern） ==================== */
// <ICBSP-GEN devices>
// </ICBSP-GEN devices da39a3ee>

void board_init(void)
{
  // ① 计时基准 `bsp_dwt` 已由 `system_init()` 内部 init()，这里不再重复初始化。

  // ② 总线与外设、生成的设备：失败会停在对应行号，fault_record 里能看到是哪一路。
  // <ICBSP-GEN init>
#if ICBSP_CAN_MODULE_ENABLED
  ICBSP_CHECK(bsp_can1.init() == Status::OK);
#endif
  ICBSP_CHECK(bsp_uart1.init() == Status::OK);
  ICBSP_CHECK(bsp_pwm_buzzer.init() == Status::OK);
  // </ICBSP-GEN init e74165f3>

  // ③ 手写的设备（不由 integrate.py 生成的）写在这里，生成区之外：包装上面已 init 的外设。蜂鸣器只是包了一个 PWM 通道，没有额外 init()。
  //
  //    例如（示例，按实际设备打开）：
  //      ICBSP_CHECK(chassis_lf.init() == Status::OK);   // DjiMotor<DjiMotorType::M3508, ...>（CAN）
  //      ICBSP_CHECK(yaw_motor.init() == Status::OK);    // DmMotor<...>（CAN，达妙）
  //      ICBSP_CHECK(servo_yaw.init() == Status::OK);    // DeviceServo（PWM 舵机）
  //      ICBSP_CHECK(stepper.init() == Status::OK);      // DeviceEmmv5GroupT<...>（串口，张大头 Emm_V5 步进）
  //
  //    它们必须在所用总线的 init() 之后、system_start() 之前（原因见文件头第 3、4 条）。
  //    CAN 设备 init() 返回 FULL 通常意味着节点名额不够：
  //    去 hardware_config.h 加大 ICBSP_CAN_RX_NODE_MAX / ICBSP_CAN_TX_NODE_MAX。
}

/* ==================== 中断转调（生成的设备用到 EXTI 时在这里生成 HAL_GPIO_EXTI_Callback；宿主别处就不要再定义它） ==================== */
// <ICBSP-GEN callbacks>
// </ICBSP-GEN callbacks da39a3ee>
