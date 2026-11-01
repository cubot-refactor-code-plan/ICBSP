/**
 * @file board_cfg.hpp
 * @brief 板级实例配置：外设实例、句柄、参数
 *
 * @details 本文件是**模板**：`integrate.py` 只在宿主 `user/board/` 下没有同名文件时才复制过来，
 *          之后它就完全归宿主所有，脚本不会再改（只会核对参数并给 [注意]）。
 *
 *          【要改的三处】
 *            1. 句柄：模板用的是 DM-MC02 的 `hfdcan1` / `huart1` / `htim12`，换成自己 CubeMX 生成的
 *            2. 参数：CAN 队列长度、串口波特率、PWM 的 timer_clk_hz / prescaler / period
 *            3. 没开的外设：整段删掉，连顶部对应的 `#include` 一起删
 *               （库按 `HAL_FDCAN_MODULE_ENABLED` / `HAL_UART_MODULE_ENABLED` / `HAL_SPI_MODULE_ENABLED`
 *                条件编译，没有实例不影响其它部分）
 *
 *          【脚本会核对什么】`integrate.py` 会把这些 `&句柄` 与 `Core/Src/*.c` 里的
 *          `xxx_HandleTypeDef` 比对，并把串口波特率、PWM 的 prescaler / period 与 CubeMX 的值对比，
 *          对不上给 [注意]。但 `timer_clk_hz` 是时钟树给的，脚本算不出来，必须自己看时钟树。
 *
 *          【三条硬规矩】
 *            - 串口 / SPI 实例必须放 `.dma_buffer`：DMA1/DMA2 访问不到 ITCM / DTCM，
 *              链接脚本检查（CMake 配置阶段）会拦下来
 *            - 改动 `BspUartT` 的 Transport（`dma` / `it`）或缓冲大小时，
 *              要去 `bsp/bsp_uart.cpp` 补一行对应的显式实例化，否则链接报未定义
 *            - 这里每加一个实例，`board_instances.hpp` 必须同步加名字，否则中断不会分发
 *
 *          【调用顺序】`board_init()` 在 `system_init()` 之后、`system_start()` 之前调用。
 */

#ifndef __BOARD_CFG_HPP__
#define __BOARD_CFG_HPP__

// CubeMX 生成的句柄头文件：按实际用到的外设增删。
// 名字对不上时编译直接报「hfdcan1 未定义」，比链接期才发现好。
#include "fdcan.h"
#include "main.h"
#include "tim.h"
#include "usart.h"

#include "bsp_can.hpp"
#include "bsp_dwt.hpp"
#include "bsp_pwm.hpp"
#include "bsp_uart.hpp"
#include "device_buzzer.hpp"


/** @brief 板级初始化：各实例 init()，实现在 board_cfg.cpp */
void board_init(void);


/* ==================== 外设实例 ==================== */

/**
 * CAN1
 *   - 模板参数 `BspCanT<收队列帧数, 发队列帧数>`：ISR 写、can_rx_task 每毫秒取走，
 *     16 帧够 8 个电机一轮；挂满一条总线（比如 20 个 3508 + 电调）就提到 32/48
 *   - `.hfdcan` 必须是 CubeMX 的经典 CAN 模式 + RX FIFO0 中断
 *   - `.name` 只在打印里出现，随意但别重名
 */
inline BspCanT<16, 16> bsp_can1(
    {.hfdcan = &hfdcan1,
     .name = "CAN1"}); ///< CAN1，16 帧收 / 16 帧发

/**
 * USART1：DMA 收发，115200
 *   - 两个模板参数是 RX / TX 环形缓冲字节数，128 字节能放几帧自定义协议
 *   - 换成中断模式：`BspUartT<uart_mode::it, 128, 32>`，CubeMX 里只开 UART 全局中断即可
 *   - 收发方向：`UartDir::RxOnly` / `TxOnly` / `TxRx`，只收的从机会话不占用 TX DMA
 *   - 必须放 .dma_buffer（见文件头「三条硬规矩」）
 *   - 波特率要和 CubeMX 里 USART1 的 BaudRate 一致，否则脚本给 [注意]、通讯也会乱码
 */
inline __attribute__((section(".dma_buffer"))) BspUartT<uart_mode::dma, 128, 128> bsp_uart1(
    {.huart = &huart1, .mode = UartDir::TxRx, .baudrate = 115200U});

/**
 * 调试打印：任何包含本头文件的文件都能用
 *
 *     print_uart("can rx %d frames\r\n", count);
 *
 * 就是 `bsp_uart1.printf` 的宏，换了调试串口只改上一行实例即可。
 * 打印浮点要在宿主 CMake 里加 `target_link_options(... -u _printf_float)`。
 */
#define print_uart(...) bsp_uart1.printf(__VA_ARGS__)

/**
 * DWT 计时：给 sys_task 算循环耗时、给业务做微秒级计时。
 * 必须在 board_init() 里 init()，且要放在第一个（sys_task 一开始就要用）。
 */
inline BspDwt bsp_dwt {};

/**
 * 蜂鸣器 PWM：TIM12_CH2
 *   - 频率 = timer_clk_hz / (prescaler + 1) / (period + 1)
 *     模板值：275 MHz / 24 / 2000 ≈ 5.7 kHz
 *   - timer_clk_hz 填 CubeMX 时钟树里 TIM12 的输入时钟（APB1 定时器时钟），填错音调就偏
 *   - prescaler / period 要和 CubeMX 的 TIM12 参数一致（脚本会对比这两项）
 */
inline BspPwm bsp_pwm_buzzer(
    {.htim = &htim12,
     .channel = TIM_CHANNEL_2,
     .timer_clk_hz = 275000000UL,
     .prescaler = 23U,
     .period = 1999U}); ///< 蜂鸣器 PWM：TIM12_CH2

// ---------------- 以下按需打开 ----------------
//
// SPI 与片选：CubeMX 里 SPI 要「全双工主机 + 软件 NSS + MasterKeepIOState = ENABLE」，
// 片选是普通 GPIO 输出。BspSpi 用 DMA 时同样要放 .dma_buffer。
//
//   inline __attribute__((section(".dma_buffer"))) BspSpi bsp_spi2(
//       {.hspi = &hspi2,
//        .use_dma = true,        // DMA 传输，短帧走阻塞阈值以下的路径
//        .dma_threshold = 16U}); // 小于这个字节数用轮询，省一次 DMA 开销
//
// 每个片选一个 BspSpiDevice：
//   .cs_port / .cs_pin  片选 GPIO，CubeMX 的 main.h 里有 XXX_CS_Pin 定义
//   .read_flag          读命令要或上的标志位（BMI088 是 0x80）
//   .read_dummy         读之前要先发的空字节数（BMI088 陀螺仪是 1，加速度计是 0）
//
//   inline BspSpiDevice bmi088_acc_spi(
//       bsp_spi2,
//       {.cs_port = GYRO_ACC_CS_GPIO_Port,
//        .cs_pin = GYRO_ACC_CS_Pin,
//        .read_flag = 0x80U,
//        .read_dummy = 1U});
// ----------------


/* ==================== 设备实例 ==================== */

/**
 * 无源蜂鸣器：包装了一个 PWM 通道，`buzzer.beep(freq, ms)` 之类由设备层提供。
 * PWM 在 board_init() 里启动，不在这里。
 */
inline DeviceBuzzer buzzer(bsp_pwm_buzzer);

// ---------------- BMI088（SPI 片选就绪后再打开） ----------------
//
//   inline DeviceBmi088 imu_bmi088(
//       bmi088_acc_spi,
//       bmi088_gyro_spi,
//       {.acc_range = DeviceBmi088::AccRange::G24,
//        .gyro_range = DeviceBmi088::GyroRange::DPS2000});

#endif // __BOARD_CFG_HPP__
