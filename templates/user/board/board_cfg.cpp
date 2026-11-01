#include "board_cfg.hpp"

#include "FreeRTOS.h" // IWYU pragma: keep (configASSERT)


void board_init(void)
{
  // 失败就地停机，fault_record 的行号指出是哪一路
  configASSERT(bsp_dwt.init() == Status::OK);
  configASSERT(bsp_can1.init() == Status::OK);
  configASSERT(bsp_uart1.init() == Status::OK);
  configASSERT(bsp_pwm_buzzer.init() == Status::OK);

  // 电机等 CAN 设备也在这里 init()
}
