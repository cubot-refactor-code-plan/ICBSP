/**
 * @file board_cfg.hpp
 * @brief 板级实例配置（模板）：本工程用哪些外设、句柄、参数——全部在这里，库里不写死任何板级常量
 *
 * @details 用法：复制 board_cfg.hpp / board_cfg.cpp 到你的工程（例如 user/board/），按自己的 CubeMX 配置改：
 *          实例用 C++20 inline 变量定义在头文件里，不再需要 extern + .cpp 定义。
 *          - 只包含工程里真有的 CubeMX 头文件（没开 FDCAN 就没有 fdcan.h，库里的 CAN 驱动也不会编译）；
 *          - 每个外设实例对应 CubeMX 里配好的一个句柄；串口 / SPI 实例必须放进 .dma_buffer 段；
 *          - board_init() 在 system_init() 之后、system_start() 之前调用（见 app_main.cpp）。
 *          本模板就是可照抄的板级配置起点。
 */

#ifndef __BOARD_CFG_HPP__
#define __BOARD_CFG_HPP__

// 本工程 CubeMX 生成的句柄与引脚宏（按实际开的外设增删）
#include "fdcan.h" // IWYU pragma: keep（hfdcan1）
#include "main.h"  // IWYU pragma: keep（引脚宏）
#include "tim.h"   // IWYU pragma: keep（htim12）
#include "usart.h" // IWYU pragma: keep（huart1）

#include "bsp_can.hpp"
#include "bsp_pwm.hpp"
#include "bsp_uart.hpp"
#include "device_buzzer.hpp"


/* ==================== 函数声明 ==================== */

/** @brief 板级初始化：各实例 init() */
void board_init(void);


/* ==================== 实例（C++20 inline） ==================== */

/**
 * @brief CAN：句柄 + 名字（用于 RTOS 对象命名）
 * @note CubeMX 里：经典 CAN、RX FIFO0，开 FDCAN 中断 0 / 1。
 */
inline BspCanT<16, 16> bsp_can1({.hfdcan = &hfdcan1, .name = "CAN1"});

/**
 * @brief 串口：句柄 + 收发模式 + 波特率（必须与 CubeMX 一致，发送卡死判定按它估算）
 * @note 必须放进 .dma_buffer：实例里就是 DMA 收发缓冲，H7 的 DMA1 / DMA2 访问不到默认 .bss 所在的 DTCM
 *       （放错了 init() 返回 BAD_ARG）。只收不发的串口用 UartMode::RxOnly。
 *       CubeMX 里：RX DMA（Normal）+ TX DMA + 串口全局中断。
 */
inline __attribute__((section(".dma_buffer"))) BspUartT<uart_detail::DmaTransport, 128, 128> bsp_uart1(
    {.huart = &huart1, .mode = UartMode::TxRx, .baudrate = 115200U});

/**
 * @brief PWM：句柄、通道、定时器时钟 (Hz)、PSC、ARR —— 从 CubeMX 的时钟树与 tim.c 抄
 * @note 示例为 DM-MC02 的 TIM12 CH2（275 MHz、PSC 23、ARR 1999）；换板必须改成自己的数。
 */
inline BspPwm bsp_pwm_buzzer({&htim12, TIM_CHANNEL_2, 275000000UL, 23U, 1999U});

/*
 * SPI 与片选设备（CubeMX 开了 SPI 才有；board_cfg.hpp 里加 #include "spi.h"、"bsp_spi.hpp"）：
 *   总线：句柄 + 是否用 DMA + 走 DMA 的最小长度（字节）；用 DMA 时必须放进 .dma_buffer（实例里有弹跳缓冲）
 *     inline __attribute__((section(".dma_buffer"))) BspSpi bsp_spi2({&hspi2, true, 16U});
 *   片选设备：{片选端口, 片选引脚, 读标志位, 读时地址后的无效字节数}
 *     inline BspSpiDevice bmi088_acc_spi(bsp_spi2, {GYRO_ACC_CS_GPIO_Port, GYRO_ACC_CS_Pin, 0x80U, 1U});
 *     inline BspSpiDevice bmi088_gyro_spi(bsp_spi2, {GYRO_GYRO_CS_GPIO_Port, GYRO_GYRO_CS_Pin, 0x80U, 0U});
 *   board_init() 里总线先于设备：configASSERT(bsp_spi2.init() == Status::OK); ……
 */

/* ==================== 设备实例 ==================== */

///< 无源蜂鸣器：构造即可用（PWM 通道由 board_init() 启动）
inline DeviceBuzzer buzzer(bsp_pwm_buzzer);

/*
 * 板载 BMI088（需要上面的两个片选设备；board_cfg.hpp 里加 #include "device_bmi088.hpp"）：
 *     inline DeviceBmi088 imu_bmi088(bmi088_acc_spi, bmi088_gyro_spi, {DeviceBmi088::AccRange::G24, DeviceBmi088::GyroRange::DPS2000});
 *   数据由库的内部任务自动刷新（默认 auto_update = true，含 VQF 姿态解算 attitude = true），业务里用 imu_bmi088.snapshot(d) 读；
 *   board_init() 里片选设备之后：configASSERT(imu_bmi088.init() == Status::OK);
 *   数据就绪中断由宿主的 HAL_GPIO_EXTI_Callback 转调 imu_bmi088.on_acc_drdy_isr() / on_gyro_drdy_isr()。
 */

#endif // __BOARD_CFG_HPP__
