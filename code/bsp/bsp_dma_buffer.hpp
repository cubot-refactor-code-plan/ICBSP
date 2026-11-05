/**
 * @file bsp_dma_buffer.hpp
 * @brief `.dma_buffer` 段宏入口（宏本体已并入 bsp_series.hpp）
 *
 * @details
 *   `ICBSP_DMA_BUFFER` 现在定义在 `bsp_series.hpp` 里，与其它系列桥接宏放在一起。
 *   本文件保留下来只为不打断现有写法：
 *
 *   inline ICBSP_DMA_BUFFER(bsp_uart1) BspUartT<uart_mode::dma, 128, 128> bsp_uart1({...});
 *   ICBSP_DMA_BUFFER(dma_rx) static uint8_t dma_rx[640];
 */

#ifndef __BSP_DMA_BUFFER_HPP__
#define __BSP_DMA_BUFFER_HPP__

#include "bsp_series.hpp" // IWYU pragma: keep（ICBSP_DMA_BUFFER）

#endif // __BSP_DMA_BUFFER_HPP__
