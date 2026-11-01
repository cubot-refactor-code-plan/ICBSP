/**
 * @file board_cfg.hpp
 * @brief 板级实例配置：外设、句柄、参数
 *
 * @note 只包含本工程 CubeMX 实际生成的句柄头文件；串口 / SPI 实例必须放 .dma_buffer。
 *       board_init() 在 system_init() 之后、system_start() 之前调用。
 */

#ifndef __BOARD_CFG_HPP__
#define __BOARD_CFG_HPP__

#include "fdcan.h"
#include "main.h"
#include "tim.h"
#include "usart.h"

#include "bsp_can.hpp"
#include "bsp_pwm.hpp"
#include "bsp_uart.hpp"
#include "device_buzzer.hpp"


void board_init(void);


/* ==================== 实例（C++20 inline） ==================== */

inline BspCanT<16, 16> bsp_can1({.hfdcan = &hfdcan1, .name = "CAN1"}); ///< CAN1

/// USART1，DMA 收发；必须放 .dma_buffer
inline __attribute__((section(".dma_buffer"))) BspUartT<uart_mode::dma, 128, 128> bsp_uart1(
    {.huart = &huart1, .mode = UartDir::TxRx, .baudrate = 115200U});

inline BspPwm bsp_pwm_buzzer({&htim12, TIM_CHANNEL_2, 275000000UL, 23U, 1999U}); ///< 蜂鸣器 PWM

// SPI / BMI088 按需取消注释；SPI 实例也要放 .dma_buffer
// inline __attribute__((section(".dma_buffer"))) BspSpi bsp_spi2({&hspi2, true, 16U});
// inline BspSpiDevice bmi088_acc_spi(bsp_spi2, {GYRO_ACC_CS_GPIO_Port, GYRO_ACC_CS_Pin, 0x80U, 1U});
// inline BspSpiDevice bmi088_gyro_spi(bsp_spi2, {GYRO_GYRO_CS_GPIO_Port, GYRO_GYRO_CS_Pin, 0x80U, 0U});
// inline DeviceBmi088 imu_bmi088(bmi088_acc_spi, bmi088_gyro_spi, {DeviceBmi088::AccRange::G24, DeviceBmi088::GyroRange::DPS2000});

/// 无源蜂鸣器
inline DeviceBuzzer buzzer(bsp_pwm_buzzer);

#endif // __BOARD_CFG_HPP__
