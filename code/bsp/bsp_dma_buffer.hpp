/**
 * @file bsp_dma_buffer.hpp
 * @brief `.dma_buffer` 段宏入口（宏本体已并入 bsp_series.hpp）与 DMA 可达性判断
 *
 * @details
 *   `ICBSP_DMA_BUFFER` 现在定义在 `bsp_series.hpp` 里，与其它系列桥接宏放在一起。
 *   本文件保留下来只为不打断现有写法：
 *
 *   inline ICBSP_DMA_BUFFER(bsp_uart1) BspUartT<uart_mode::dma, 128, 128> bsp_uart1({...});
 *   ICBSP_DMA_BUFFER(dma_rx) static uint8_t dma_rx[640];
 *
 * @note 另有 icbsp_dma_reachable()：按 DMA 控制器（DMA1/2 或 BDMA）判断缓冲能否被访问，UART / SPI 的 init() 都用它。
 * @note H7：链接脚本里 `.dma_buffer` 是可加载段（`>RAM_D1 AT> FLASH`），system.cpp 在全局构造前把初值拷进 RAM。
 *       F4：宏展开为空，不需要这个段。
 */

#ifndef __BSP_DMA_BUFFER_HPP__
#define __BSP_DMA_BUFFER_HPP__

#include "bsp_series.hpp" // IWYU pragma: keep（ICBSP_DMA_BUFFER、DMA_HandleTypeDef、IS_BDMA_CHANNEL_INSTANCE）

#include <stdint.h>

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
 * @note F4：没有 BDMA，DMA1 / DMA2 只访问不到 CCM RAM，判断见 bsp_series::dma_reachable()。
 */
inline bool icbsp_dma_reachable(const void *addr, const DMA_HandleTypeDef *hdma)
{
  if (hdma == nullptr)
  {
    return true;
  }
#if defined(BDMA)
  if (IS_BDMA_CHANNEL_INSTANCE(hdma->Instance) != 0U)
  {
    const uintptr_t a = reinterpret_cast<uintptr_t>(addr);
    return a >= 0x38000000U && a < 0x38010000U; // SRAM4
  }
#endif
  return bsp_series::dma_reachable(addr); // DMA1 / DMA2
}

#endif // __BSP_DMA_BUFFER_HPP__
