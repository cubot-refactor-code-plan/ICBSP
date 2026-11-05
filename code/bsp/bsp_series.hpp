/**
 * @file bsp_series.hpp
 * @author Rh
 * @brief 系列桥接层：库内唯一 include STM32 HAL 的地方，并把 H7 / F4 的命名差异收敛成宏
 * @version 1.0
 * @date 2026-11-05
 *
 * @copyright Copyright (c) 2026
 *
 * @details
 *   **库内所有文件都 include 本文件，不再直接 include `stm32xxxx_hal.h`。**
 *   本文件只做两件事：
 *     1. 按编译环境选一个系列，include 对应的 HAL 头
 *     2. 给 F4 补上 H7 才有、但语义等价的宏/类型，让上层代码一行都不用改
 *
 * @note H7 侧一个宏都不定义、一个类型都不新增：所有补丁都在 `#if ICBSP_SERIES_F4` 里，
 *       H7 编译出来的东西与引入本文件之前完全一致（除了多一层 include guard）。
 *
 * @note 系列判定用 `__has_include`：CubeMX 的 CMake 只把当前系列的 HAL 目录加进 include 路径，
 *       所以哪个头能找到就是哪个系列。不用芯片宏（`STM32H723xx` 之类有上百个变体，
 *       而且 `STM32H7xx` 这种系列宏是 HAL 头内部才定义的，本文件首次 include 时还不存在）。
 *
 * @warning 新增系列（G4 / F1 / L4 …）时：在这里加分支，并且逐个 BSP 文件加 `#if`。
 *          只加分支不代表能跑：F4 之所以只改 5 处，是因为它与 H7 同属 DMA-stream 架构。
 */

#ifndef __BSP_SERIES_HPP__
#define __BSP_SERIES_HPP__


/* ==================== */
/* ==================== ① HAL 引入与系列标识 ==================== */

#if __has_include("stm32h7xx_hal.h")

#  include "stm32h7xx_hal.h" // IWYU pragma: keep（HAL 模块开关）
#  define ICBSP_SERIES_H7 1
#  define ICBSP_SERIES_F4 0
#  define ICBSP_SERIES_NAME "STM32H7"

#elif __has_include("stm32f4xx_hal.h")

#  include "stm32f4xx_hal.h" // IWYU pragma: keep（HAL 模块开关）
#  define ICBSP_SERIES_H7 0
#  define ICBSP_SERIES_F4 1
#  define ICBSP_SERIES_NAME "STM32F4"

#else

#  error "bsp_series.hpp: 找不到 stm32h7xx_hal.h 或 stm32f4xx_hal.h。本库只支持 STM32H7 / STM32F4"

#endif


/* ==================== */
/* ==================== ② F4 专属补丁 ==================== */

#if ICBSP_SERIES_F4

/* ---------- UART 寄存器位名 ----------
 * F4 没有硬件 FIFO，寄存器位用的是不带 FIFO 后缀的旧名字，但**位号相同**：
 *   USART_CR1_RXNEIE_RXFNEIE (bit 5)  ←→  USART_CR1_RXNEIE  (bit 5)
 *   USART_CR1_TXEIE_TXFNFIE  (bit 7)  ←→  USART_CR1_TXEIE   (bit 7)
 * 所以直接换名字，bsp_uart.hpp 里那些 `ATOMIC_CLEAR_BIT(CR1, ...)` 一行都不用改。
 *
 * CR3 的 RXFTIE / TXFTIE 是 FIFO 阈值中断，F4 没有对应硬件，定义成 0U：
 * 参与 `| CR3` 时是无害的空操作，参与 `& CR3` 时恒为 0（“FIFO 中断没开”）。
 */
#  ifndef USART_CR1_RXNEIE_RXFNEIE
#    define USART_CR1_RXNEIE_RXFNEIE USART_CR1_RXNEIE
#  endif
#  ifndef USART_CR1_TXEIE_TXFNFIE
#    define USART_CR1_TXEIE_TXFNFIE USART_CR1_TXEIE
#  endif
#  ifndef USART_CR3_RXFTIE
#    define USART_CR3_RXFTIE 0U
#  endif
#  ifndef USART_CR3_TXFTIE
#    define USART_CR3_TXFTIE 0U
#  endif

/* F4 没有「接收超时」这个错误源（RTO 是 H7/L4 才有的 Receiver Timeout），
 * 参与错误掩码 `|` 时为 0，不影响 PE / NE / FE / ORE 任何一项。 */
#  ifndef HAL_UART_ERROR_RTO
#    define HAL_UART_ERROR_RTO 0U
#  endif

/** @brief 本系列是否有 UART 硬件 FIFO（决定 `huart->FifoMode` / `HAL_UARTEx_EnableFifoMode` 能不能用） */
#  define ICBSP_UART_HAS_FIFO 0

/* ---------- DMA 实例判定 ----------
 * bsp_uart.hpp 用它区分「DMA1 / DMA2 的流」与「BDMA 之类」。
 * F4 的 DMA 全部是 stream 架构（没有 BDMA / MDMA），所以只按“地址落在 DMA1 / DMA2 寄存器块内”判断；
 * 不逐个枚举 DMAx_Streamn：低端型号（例如 F401 的 DMA2 只有 Stream0~6）没有 Stream7，枚举会在编译期报错。
 */
#  ifndef IS_DMA_STREAM_INSTANCE
#    define IS_DMA_STREAM_INSTANCE(INSTANCE)                                  \
      (((INSTANCE) != NULL) &&                                                 \
       ((((uintptr_t)(INSTANCE) >= (uintptr_t)DMA1_BASE) &&                    \
         ((uintptr_t)(INSTANCE) < (uintptr_t)DMA1_BASE + 0x400U)) ||           \
        (((uintptr_t)(INSTANCE) >= (uintptr_t)DMA2_BASE) &&                    \
         ((uintptr_t)(INSTANCE) < (uintptr_t)DMA2_BASE + 0x400U))))
#  endif

/* ---------- CAN 开关 ----------
 * H7 用 FDCAN（HAL_FDCAN_MODULE_ENABLED），F4 用 bxCAN（HAL_CAN_MODULE_ENABLED）。
 * 上层 service / tasks / device 统一看这一个宏，不再各自判断 HAL 模块名。
 */
#  ifdef HAL_CAN_MODULE_ENABLED
#    define ICBSP_CAN_MODULE_ENABLED 1
#  else
#    define ICBSP_CAN_MODULE_ENABLED 0
#  endif

/* ---------- CAN 常量：让上层写法原样成立 ----------
 * can_rx_task / can_rx_node / dji_motor 里用的是 FDCAN 的名字，F4 上映射到 bxCAN 常量。
 * 数值核对：
 *   FDCAN_STANDARD_ID 0x0  == CAN_ID_STD     0x0
 *   FDCAN_EXTENDED_ID 0x4  == CAN_ID_EXT     0x4
 *   FDCAN_DATA_FRAME  0x0  == CAN_RTR_DATA   0x0
 *   FDCAN_DLC_BYTES_n n    == bxCAN 的 DLC 字段（也是 0~8 的字节数）
 *
 * @warning **不要**把 FDCAN_REMOTE_FRAME 映射过来：FDCAN 是 0x1，CAN_RTR_REMOTE 是 0x2，数值不同。
 *          bxCAN 后端在硬件层就把远程帧丢掉，上层只会看到 FDCAN_DATA_FRAME，这个常量在 F4 上不需要。
 */
#  define FDCAN_STANDARD_ID CAN_ID_STD
#  define FDCAN_EXTENDED_ID CAN_ID_EXT
#  define FDCAN_DATA_FRAME  CAN_RTR_DATA
#  define FDCAN_CLASSIC_CAN 0U
#  define FDCAN_DLC_BYTES_0 0U
#  define FDCAN_DLC_BYTES_1 1U
#  define FDCAN_DLC_BYTES_2 2U
#  define FDCAN_DLC_BYTES_3 3U
#  define FDCAN_DLC_BYTES_4 4U
#  define FDCAN_DLC_BYTES_5 5U
#  define FDCAN_DLC_BYTES_6 6U
#  define FDCAN_DLC_BYTES_7 7U
#  define FDCAN_DLC_BYTES_8 8U

/**
 * @brief bxCAN 后端交给上层的接收帧头（字段名与顺序照抄 H7 的 HAL 定义）
 *
 * @note 只为了让 `rx.header.Identifier` / `.IdType` / `.RxFrameType` / `.FDFormat` / `.DataLength`
 *       这些写法在 F4 上原样成立。bxCAN 的 `CAN_RxHeaderTypeDef` 字段名完全不同
 *       （`StdId` / `ExtId` / `IDE` / `RTR` / `DLC`），字段摊平在 `bsp_can.hpp` 的 bxCAN 分支里做。
 */
typedef struct
{
  uint32_t Identifier;            ///< 11 位标准 ID 或 29 位扩展 ID（由后端按 IDE 选 StdId / ExtId）
  uint32_t IdType;                ///< FDCAN_STANDARD_ID / FDCAN_EXTENDED_ID（= CAN_ID_STD / CAN_ID_EXT）
  uint32_t RxFrameType;           ///< 恒为 FDCAN_DATA_FRAME（远程帧在硬件层已丢弃）
  uint32_t DataLength;            ///< 数据长度 0~8
  uint32_t ErrorStateIndicator;   ///< F4 无此概念，恒 0
  uint32_t BitRateSwitch;         ///< F4 无 CAN FD，恒 0
  uint32_t FDFormat;              ///< 恒为 FDCAN_CLASSIC_CAN
  uint32_t RxTimestamp;           ///< bxCAN 的时间戳
  uint32_t FilterIndex;           ///< 命中的过滤器组
  uint32_t IsFilterMatchingFrame; ///< F4 无此概念，恒 0
} FDCAN_RxHeaderTypeDef;

#else /* ==================== H7 ==================== */

/** @brief 本系列是否有 UART 硬件 FIFO */
#  define ICBSP_UART_HAS_FIFO 1

#  ifdef HAL_FDCAN_MODULE_ENABLED
#    define ICBSP_CAN_MODULE_ENABLED 1
#  else
#    define ICBSP_CAN_MODULE_ENABLED 0
#  endif

#endif /* ICBSP_SERIES_F4 */


/* ==================== */
/* ==================== ③ DMA 可达性 ==================== */

#ifdef __cplusplus
namespace bsp_series
{
/**
 * @brief 地址能否被 DMA 访问
 *
 * @note H7：ITCM（0x0000_0000 起）与 DTCM（0x2000_0000 ~ 0x2400_0000）只连 CPU 与 MDMA，
 *       DMA1 / DMA2 访问会出传输错误 —— 与引入本文件之前的判断完全一致。
 * @note F4：没有 DTCM / AXI SRAM 这些概念，主 SRAM（0x2000_0000 起）全部可 DMA；
 *       唯一的例外是 CCM RAM（0x1000_0000 ~ 0x1000_FFFF，64 KB，只连 CPU）。
 *       把实例或缓冲放进 CCM 会静默出错，所以这里显式排掉。
 */
inline bool dma_reachable(const void *addr)
{
  const uintptr_t a = reinterpret_cast<uintptr_t>(addr);

#if ICBSP_SERIES_H7
  const bool itcm = a < 0x08000000U;
  const bool dtcm = a >= 0x20000000U && a < 0x24000000U;
  return !itcm && !dtcm;
#else
  const bool ccm = a >= 0x10000000U && a < 0x10010000U;
  return !ccm;
#endif
}
} // namespace bsp_series
#endif // __cplusplus


/* ==================== */
/* ==================== ④ `.dma_buffer` 段 ==================== */

/**
 * @brief 把紧跟着声明的变量放进 `.dma_buffer.<name>`；name 写变量名本身
 *
 * @note H7：DMA1 / DMA2 访问不到 DTCM，实例必须放进专门的可加载段（`>RAM_D1 AT> FLASH`）。
 *       段名必须按变量唯一（`.dma_buffer.<变量名>`）：board_cfg.hpp 里的实例是 inline 变量，
 *       每个 TU 各有一份弱定义，同名段会让重复副本也被链接进来（符号去重了、数据没去）。
 * @note F4：主 SRAM 本身就能被 DMA 访问，不需要专段 —— 宏展开为空。于是：
 *       - 链接脚本不用加 `.dma_buffer`
 *       - system.cpp 那次「全局构造前拷贝初值」因找不到 __dma_buffer_* 弱符号而自动跳过
 *       - board_cfg.hpp 里 `inline ICBSP_DMA_BUFFER(bsp_uart1) BspUartT<...>` 一处都不用改
 */
#if ICBSP_SERIES_F4
#  define ICBSP_DMA_BUFFER(name) /* F4：主 SRAM 可 DMA，不需要专用段 */
#else
#  define ICBSP_DMA_BUFFER(name) __attribute__((section(".dma_buffer." #name)))
#endif


/* ==================== */

#endif // __BSP_SERIES_HPP__
