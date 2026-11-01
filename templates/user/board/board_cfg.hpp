/**
 * @file board_cfg.hpp
 * @brief 板级实例配置：外设、句柄、参数
 *
 * @note 只包含本工程 CubeMX 实际生成的句柄头文件；串口 / SPI 实例必须放 .dma_buffer。
 *       board_init() 在 system_init() 之后、system_start() 之前调用。
 *       新增 BspUartT 的 Transport / 缓冲大小时，要在 bsp_uart.cpp 补对应显式实例化。
 */

#ifndef __BOARD_CFG_HPP__
#define __BOARD_CFG_HPP__

// CubeMX 句柄（按实际外设增删）
#include "fdcan.h"
#include "main.h"
#include "tim.h"
#include "usart.h"

#include "bsp_can.hpp"
#include "bsp_dwt.hpp"
#include "bsp_pwm.hpp"
#include "bsp_uart.hpp"
#include "device_buzzer.hpp"


/** @brief 板级初始化：各实例 init() */
void board_init(void);


/* ==================== 外设实例 ==================== */

inline BspCanT<16, 16> bsp_can1(
    {.hfdcan = &hfdcan1,
     .name = "CAN1"}); ///< CAN1，16 帧收 / 16 帧发

/// USART1：DMA 收发，115200；必须放 .dma_buffer
inline __attribute__((section(".dma_buffer"))) BspUartT<uart_mode::dma, 128, 128> bsp_uart1(
    {.huart = &huart1, .mode = UartDir::TxRx, .baudrate = 115200U});

/// 所有包含本头文件的文件都可调用：print_uart("x=%d", x)
#define print_uart(...) bsp_uart1.printf(__VA_ARGS__)

inline BspDwt bsp_dwt {};

inline BspPwm bsp_pwm_buzzer(
    {.htim = &htim12,
     .channel = TIM_CHANNEL_2,
     .timer_clk_hz = 275000000UL,
     .prescaler = 23U,
     .period = 1999U}); ///< 蜂鸣器 PWM：TIM12_CH2

// SPI / 片选：按需打开；SPI 实例也要放 .dma_buffer
// inline __attribute__((section(".dma_buffer"))) BspSpi bsp_spi2(
//     {.hspi = &hspi2,
//      .use_dma = true,
//      .dma_threshold = 16U});
// inline BspSpiDevice bmi088_acc_spi(
//     bsp_spi2,
//     {.cs_port = GYRO_ACC_CS_GPIO_Port,
//      .cs_pin = GYRO_ACC_CS_Pin,
//      .read_flag = 0x80U,
//      .read_dummy = 1U});
// inline BspSpiDevice bmi088_gyro_spi(
//     bsp_spi2,
//     {.cs_port = GYRO_GYRO_CS_GPIO_Port,
//      .cs_pin = GYRO_GYRO_CS_Pin,
//      .read_flag = 0x80U,
//      .read_dummy = 0U});


/* ==================== 设备实例 ==================== */

inline DeviceBuzzer buzzer(bsp_pwm_buzzer); ///< 无源蜂鸣器，PWM 由 board_init() 启动

// BMI088：SPI 片选就绪后打开
// inline DeviceBmi088 imu_bmi088(
//     bmi088_acc_spi,
//     bmi088_gyro_spi,
//     {.acc_range = DeviceBmi088::AccRange::G24,
//      .gyro_range = DeviceBmi088::GyroRange::DPS2000});

#endif // __BOARD_CFG_HPP__
