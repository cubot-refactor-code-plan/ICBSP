#include "board_cfg.hpp"

#include "FreeRTOS.h" // IWYU pragma: keep (configASSERT)


/* ==================== 初始化 ==================== */

void board_init(void)
{
  // 每条 init() 单独一行断言：失败就地停机，fault_record 记下的行号直接指出是哪一路没起来
  configASSERT(bsp_can1.init() == Status::OK);
  configASSERT(bsp_uart1.init() == Status::OK);
  configASSERT(bsp_pwm_buzzer.init() == Status::OK);

  // 电机等 CAN 设备在这里 init()（会注册 CAN 收发节点），例如：
  //   configASSERT(chassis_lf.init() == Status::OK);   // DjiMotor<Motor3508> chassis_lf(bsp_can1, 1U);
}
