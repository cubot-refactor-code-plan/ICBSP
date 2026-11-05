/**
 * @file bsp_dma_buffer.hpp
 * @brief 把变量放进 .dma_buffer 段的宏（H7 的 DMA1 / DMA2 访问不到 DTCM，DMA 收发的实例必须放这里）
 *
 * @note 段名必须按变量唯一（`.dma_buffer.<变量名>`）：board_cfg.hpp 里的实例是 inline 变量，每个 TU 各有一份弱定义，
 *       同名段会让重复的副本也被链接进来（符号去重了、数据没去）。宏按变量名拼出段名，不用手写：
 *
 *   inline ICBSP_DMA_BUFFER(bsp_uart1) BspUartT<uart_mode::dma, 128, 128> bsp_uart1({...});
 *   ICBSP_DMA_BUFFER(dma_rx) static uint8_t dma_rx[640];
 *
 * @note 另有 icbsp_dma_reachable()：按 DMA 控制器（DMA1/2 或 BDMA）判断缓冲能否被访问，UART / SPI 的 init() 都用它。
 * @note 链接脚本里 `.dma_buffer` 是可加载段（`>RAM_D1 AT> FLASH`），system.cpp 在全局构造前把初值拷进 RAM。
 */

#ifndef __BSP_DMA_BUFFER_HPP__
#define __BSP_DMA_BUFFER_HPP__

/** @brief 把紧跟着声明的变量放进 `.dma_buffer.<name>`；name 写变量名本身 */
#define ICBSP_DMA_BUFFER(name) __attribute__((section(".dma_buffer." #name)))

#include <stdint.h>

#include "stm32h7xx_hal.h" // IWYU pragma: keep（DMA_HandleTypeDef、IS_BDMA_CHANNEL_INSTANCE）

/**
 * @brief 地址能否被 hdma 所属的 DMA 控制器访问（UART / SPI 共用，评审 A4）
 *
 * @param addr 缓冲地址
 * @param hdma 要用的 DMA 句柄；nullptr = 这个方向不用 DMA，总是 true
 *
 * @note STM32H7 的总线矩阵：
 *       - DMA1 / DMA2（D2 域）：访问不到 ITCM（0x0000_0000 起）和 DTCM（0x2000_0000 ~ 0x2400_0000）；任务栈、默认 .bss 都在 DTCM。
 *         AXI SRAM（`.dma_buffer`）、SRAM1~4、Flash 都可以。
 *       - BDMA（D3 域，SPI6 / LPUART1 / I2C4 等用它）：只能访问 SRAM4（0x3800_0000 起 64 KB）。`.dma_buffer` 在 AXI SRAM，
 *         这些外设的实例要另放进 SRAM4 的段，否则 init() 拒绝。
 *       - MDMA 不经这里（库没有用 MDMA 的外设）。
 */
inline bool icbsp_dma_reachable(const void *addr, const DMA_HandleTypeDef *hdma)
{
  if (hdma == nullptr)
  {
    return true;
  }
  const uintptr_t a = reinterpret_cast<uintptr_t>(addr);
#if defined(BDMA)
  if (IS_BDMA_CHANNEL_INSTANCE(hdma->Instance) != 0U)
  {
    return a >= 0x38000000U && a < 0x38010000U; // SRAM4
  }
#endif
  const bool itcm = a < 0x08000000U;
  const bool dtcm = a >= 0x20000000U && a < 0x24000000U;
  return !itcm && !dtcm;
}

#endif // __BSP_DMA_BUFFER_HPP__
