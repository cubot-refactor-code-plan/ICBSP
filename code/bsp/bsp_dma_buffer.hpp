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
 * @note 链接脚本里 `.dma_buffer` 是可加载段（`>RAM_D1 AT> FLASH`），system.cpp 在全局构造前把初值拷进 RAM。
 */

#ifndef __BSP_DMA_BUFFER_HPP__
#define __BSP_DMA_BUFFER_HPP__

/** @brief 把紧跟着声明的变量放进 `.dma_buffer.<name>`；name 写变量名本身 */
#define ICBSP_DMA_BUFFER(name) __attribute__((section(".dma_buffer." #name)))

#endif // __BSP_DMA_BUFFER_HPP__
