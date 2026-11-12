/**
 * @file bsp_uart.hpp
 * @author Rh
 * @brief 串口驱动（DMA 与中断两种收发方式，FreeRTOS 流缓冲区）
 * @version 0.6
 * @date 2026-11-04
 *
 * @copyright Copyright (c) 2026
 *
 * @details 通用模板 BspUartT<Transport, RX_SIZE, TX_SIZE>，**只有头文件**（任意缓冲尺寸都不用补显式实例化）。
 *          流缓冲区、发送状态机、诊断计数与恢复流程共用，底层 DMA / 中断由 Transport 选择；
 *          HAL 回调按 board_instances.hpp 的 ICBSP_UART_INSTANCES 分发（bsp_uart.cpp）。
 *
 * @note 接收：
 *       - uart_mode::dma：RX DMA 工作在**循环模式**（init() 自动把 hdmarx 改成 Circular + 直接模式）。
 *         HAL 的 IDLE / 半满（HT）/ 写满（TC）事件都不停 DMA，库按“上次读到的位置 → DMA 当前写到的位置”
 *         把新字节投进流缓冲区（处理回绕）。IDLE 路径从不调用阻塞版 HAL_DMA_Abort()，没有重新挂接收的空窗。
 *         **前提：串口 / RX DMA 中断被挡住（临界区、更高优先级中断）的时间不超过收满半个 RX_SIZE 的时间**
 *         （RX_SIZE / 2 × 10 / 波特率，例如 128 字节、921600 下约 0.69 ms）。超过时 DMA 可能写满一圈、覆盖还没投递的数据，
 *         这种丢失库的诊断计数察觉不到（阶段 3 实测 U19：屏蔽 0.45 圈无损，0.75 / 1.5 圈丢帧且计数为 0）。
 *         高波特率或长临界区的场合加大 RX_SIZE。
 *       - uart_mode::it：HAL 的 ReceiveToIdle_IT，每次 IDLE / 写满投递一包后立即重新挂接收。
 *       - 接收流缓冲区容量是 2 × RX_SIZE（给“写满投递 + 紧接着的 IDLE 投递”留余量）。
 *       - 线路错误（ORE / FE / NE / PE）时 HAL 在 DMA 模式下会中止接收；sys_task 每 10 ms 调 rx_recover() 重建。
 * @note 发送：数据先写进发送流缓冲区（只有一个写者：send() 持 _tx_write_lock），再由任务（持 _tx_lock）
 *       或发送完成中断取一段交给 Transport。启动失败的那一段留在 _tx_buffer 里下次先发，中断侧从不往流缓冲区写。
 *       三把锁的取用顺序固定为 printf → write → tx，不会死锁。
 * @note 所有超时参数单位都是毫秒；传 portMAX_DELAY 表示一直等。接收是字节流（不保证整帧），
 *       发送返回 OK 只表示数据已进流缓冲区。
 * @note RS485：只支持 UART 硬件 DE（CubeMX 里 UART 选 RS485 模式）。库不提供软件 DE。
 *
 * @warning 已知限制：TX DMA 开着 DMA FIFO（CubeMX 配置）时，DMA 预读进 FIFO 的字节已被剩余计数算作“发出”；
 *          init() 还会打开 UART 硬件 FIFO（ICBSP_UART_ENABLE_FIFO）。tx_recover() 中止卡死的发送后按剩余计数续发，
 *          这两个 FIFO 里的字节会丢。只影响“卡死后恢复”这条路径，正常发送不受影响。
 */

#ifndef __BSP_UART_HPP__
#define __BSP_UART_HPP__

#include "bsp/bsp_series.hpp" // IWYU pragma: keep（HAL 模块开关 / 系列桥接）

// 宿主工程没在 CubeMX 里开 UART（HAL_UART_MODULE_ENABLED 未定义）时，本模块整体不参与编译
#ifdef HAL_UART_MODULE_ENABLED

#include "FreeRTOS.h" // IWYU pragma: keep
#include "semphr.h"   // IWYU pragma: keep（发送锁）
#include "stream_buffer.h"
#include "task.h" // IWYU pragma: keep

#include "bsp/bsp_dma_buffer.hpp" // ICBSP_DMA_BUFFER
#include "service/system/config_check.hpp" // ICBSP_UART_ENABLE_FIFO
#include "service/system/status_code.hpp"        // 统一状态码

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>


/* ==================== 内部工具 ==================== */

/** @brief 串口驱动的内部实现细节（Transport 策略与恢复用的寄存器级工具） */
namespace uart_detail
{
/**
 * @brief 毫秒 → tick；portMAX_DELAY 原样保留（表示一直等）
 * @note pdMS_TO_TICKS 用 32 位计算，pdMS_TO_TICKS(portMAX_DELAY) 会溢出成约 71.6 分钟，不再是“一直等”。
 */
inline TickType_t ms_to_ticks(uint32_t ms)
{
  return (ms == portMAX_DELAY) ? portMAX_DELAY : pdMS_TO_TICKS(ms);
}

/** @brief 串口实例对应的 NVIC 中断号；不认识的实例返回 false */
inline bool uart_irqn(const USART_TypeDef *instance, IRQn_Type *irqn)
{
#define BSP_UART_IRQN_CASE(INST)  \
  if (instance == INST)           \
  {                               \
    *irqn = INST##_IRQn;          \
    return true;                  \
  }
#if defined(USART1)
  BSP_UART_IRQN_CASE(USART1)
#endif
#if defined(USART2)
  BSP_UART_IRQN_CASE(USART2)
#endif
#if defined(USART3)
  BSP_UART_IRQN_CASE(USART3)
#endif
#if defined(UART4)
  BSP_UART_IRQN_CASE(UART4)
#endif
#if defined(UART5)
  BSP_UART_IRQN_CASE(UART5)
#endif
#if defined(USART6)
  BSP_UART_IRQN_CASE(USART6)
#endif
#if defined(UART7)
  BSP_UART_IRQN_CASE(UART7)
#endif
#if defined(UART8)
  BSP_UART_IRQN_CASE(UART8)
#endif
#if defined(UART9)
  BSP_UART_IRQN_CASE(UART9)
#endif
#if defined(USART10)
  BSP_UART_IRQN_CASE(USART10)
#endif
#if defined(LPUART1)
  BSP_UART_IRQN_CASE(LPUART1)
#endif
#undef BSP_UART_IRQN_CASE
  return false;
}

/** @brief DMA1 / DMA2 流对应的 NVIC 中断号；BDMA 或不认识的返回 false */
inline bool dma_stream_irqn(const DMA_HandleTypeDef *hdma, IRQn_Type *irqn)
{
  if (hdma == nullptr)
  {
    return false;
  }
  const void *s = hdma->Instance;
#define BSP_DMA_IRQN_CASE(D, N)            \
  if (s == D##_Stream##N)                  \
  {                                        \
    *irqn = D##_Stream##N##_IRQn;          \
    return true;                           \
  }
#if defined(DMA1_Stream0)
  BSP_DMA_IRQN_CASE(DMA1, 0)
  BSP_DMA_IRQN_CASE(DMA1, 1)
  BSP_DMA_IRQN_CASE(DMA1, 2)
  BSP_DMA_IRQN_CASE(DMA1, 3)
  BSP_DMA_IRQN_CASE(DMA1, 4)
  BSP_DMA_IRQN_CASE(DMA1, 5)
  BSP_DMA_IRQN_CASE(DMA1, 6)
  BSP_DMA_IRQN_CASE(DMA1, 7)
#endif
#if defined(DMA2_Stream0)
  BSP_DMA_IRQN_CASE(DMA2, 0)
  BSP_DMA_IRQN_CASE(DMA2, 1)
  BSP_DMA_IRQN_CASE(DMA2, 2)
  BSP_DMA_IRQN_CASE(DMA2, 3)
  BSP_DMA_IRQN_CASE(DMA2, 4)
  BSP_DMA_IRQN_CASE(DMA2, 5)
  BSP_DMA_IRQN_CASE(DMA2, 6)
  BSP_DMA_IRQN_CASE(DMA2, 7)
#endif
#undef BSP_DMA_IRQN_CASE
  return false;
}

/**
 * @brief 在作用域内屏蔽一个串口的 NVIC 中断（及其 RX / TX DMA 流的中断），离开时恢复原来的使能状态
 * @note 任务侧做「中止 → 复位 → 重新启动」时用：避免 IDLE / DMA 中断插进中间。只恢复进入前本来就开着的中断。
 */
class IrqGuard
{
public:
  explicit IrqGuard(const UART_HandleTypeDef *huart)
  {
    IRQn_Type irqn = static_cast<IRQn_Type>(0);
    if (uart_irqn(huart->Instance, &irqn))
    {
      add(irqn);
    }
    if (dma_stream_irqn(huart->hdmarx, &irqn))
    {
      add(irqn);
    }
    if (dma_stream_irqn(huart->hdmatx, &irqn))
    {
      add(irqn);
    }
  }

  ~IrqGuard()
  {
    for (uint8_t i = 0U; i < _n; ++i)
    {
      if (_was_on[i])
      {
        NVIC_EnableIRQ(_irqn[i]);
      }
    }
  }

  IrqGuard(const IrqGuard &)            = delete;
  IrqGuard &operator=(const IrqGuard &) = delete;

private:
  void add(IRQn_Type irqn)
  {
    _irqn[_n]   = irqn;
    _was_on[_n] = (NVIC_GetEnableIRQ(irqn) != 0U);
    NVIC_DisableIRQ(irqn);
    __DSB();
    __ISB();
    _n = static_cast<uint8_t>(_n + 1U);
  }

  IRQn_Type _irqn[3]   = {};
  bool      _was_on[3] = {};
  uint8_t   _n         = 0U;
};

/**
 * @brief DMA 流已经停了、HAL 句柄却不是 READY 时，把句柄状态复位（只在恢复路径里调用）
 * @return true=确实复位了一次
 * @note 实测（2026-10-16，USART10 RX）：句柄停在 HAL_DMA_STATE_ERROR 后，HAL_DMA_Start_IT() 一律返回 HAL_ERROR，
 *       HAL_DMA_Abort() 又因为状态不是 BUSY 直接返回 —— HAL 自己出不来。
 * @note 只在流的 EN 已清时复位：EN 还在说明 DMA 真在搬运，这时只请求停流，下次巡检再复位。
 */
inline bool repair_dma_handle(DMA_HandleTypeDef *hdma)
{
  if (hdma == nullptr || hdma->State == HAL_DMA_STATE_READY || IS_DMA_STREAM_INSTANCE(hdma->Instance) == 0U)
  {
    return false;
  }
  if ((reinterpret_cast<DMA_Stream_TypeDef *>(hdma->Instance)->CR & DMA_SxCR_EN) != 0U)
  {
    __HAL_DMA_DISABLE(hdma);
    return false;
  }
  hdma->ErrorCode = HAL_DMA_ERROR_NONE;
  hdma->State     = HAL_DMA_STATE_READY;
  __HAL_UNLOCK(hdma);
  return true;
}

/**
 * @brief 最多约 10 ms 等 DMA 流的 EN 清零
 * @note 不用 HAL_GetTick()：调度器启动前（board_init() 里 init()）HAL 时基中断可能被 BASEPRI 挡住、tick 不走（评审 A3）。
 *       用 CYCCNT 计时，另加循环次数上限：DWT 没开时 CYCCNT 不动，也能在有限时间内退出。
 */
inline void wait_stream_off(const DMA_Stream_TypeDef *s)
{
  const uint32_t limit = SystemCoreClock / 100U; // 10 ms 的周期数
  const uint32_t t0    = DWT->CYCCNT;
  uint32_t       spins = 0U;
  while ((s->CR & DMA_SxCR_EN) != 0U && (DWT->CYCCNT - t0) < limit && spins < limit)
  {
    spins = spins + 1U;
  }
}

/**
 * @brief 停掉 DMA 流并把 HAL 句柄复位到 READY（运行中重新 init() / deinit() 用，调度器启动前也可调用）
 * @note HAL_DMA_Abort() 超时会把句柄留在 ERROR；这里请求停流后有限时间内等 EN 清零，再复位句柄（2026-10-17 实测）。
 */
inline void stop_dma_stream(DMA_HandleTypeDef *hdma)
{
  if (hdma == nullptr || IS_DMA_STREAM_INSTANCE(hdma->Instance) == 0U)
  {
    return;
  }
  DMA_Stream_TypeDef *const s = reinterpret_cast<DMA_Stream_TypeDef *>(hdma->Instance);
  __HAL_DMA_DISABLE(hdma);
  wait_stream_off(s);
  (void)repair_dma_handle(hdma);
}

/**
 * @brief 只请求停流并有限时间内等 EN 清零，不改 HAL 句柄状态（句柄复位交给调用方计数后的 repair_dma_handle()）
 * @note HAL_UART_AbortReceive() 只在 CR3.DMAR 还置位时才停 RX DMA；DMAR 已被清（例如 ORE 时 HAL 已结束接收）时，
 *       循环模式的流会一直开着，下一次启动接收必然 BUSY。
 */
inline void disable_dma_stream(DMA_HandleTypeDef *hdma)
{
  if (hdma == nullptr || IS_DMA_STREAM_INSTANCE(hdma->Instance) == 0U)
  {
    return;
  }
  DMA_Stream_TypeDef *const s = reinterpret_cast<DMA_Stream_TypeDef *>(hdma->Instance);
  if ((s->CR & DMA_SxCR_EN) == 0U)
  {
    return;
  }
  __HAL_DMA_DISABLE(hdma);
  wait_stream_off(s);
}

/** @brief 结束任务侧中止后，把 HAL 的 RX 状态强制回到 READY（中止若因 DMA 超时提前返回，状态会停在 BUSY） */
inline void force_rx_ready(UART_HandleTypeDef *huart)
{
  ATOMIC_CLEAR_BIT(huart->Instance->CR1, USART_CR1_IDLEIE | USART_CR1_RXNEIE_RXFNEIE | USART_CR1_PEIE);
  ATOMIC_CLEAR_BIT(huart->Instance->CR3, USART_CR3_DMAR | USART_CR3_EIE | USART_CR3_RXFTIE);
  huart->RxState       = HAL_UART_STATE_READY;
  huart->ReceptionType = HAL_UART_RECEPTION_STANDARD;
}


/* ==================== */
/* ==================== Transport 策略 ==================== */

/**
 * @brief DMA 收发：RX 循环模式（init() 自动配置），TX 普通模式
 * @note 只封装 HAL / 寄存器差异，不持有状态。
 */
struct DmaTransport
{
  static constexpr bool CIRCULAR_RX = true; ///< 接收位置按 DMA 剩余计数计算，事件回调里不重新挂接收
  static constexpr bool NEEDS_DMA   = true; ///< 必须配 DMA 句柄，实例必须在 DMA 可达的内存里

  /** @brief 把 RX DMA 改成循环 + 直接模式（直接模式下剩余计数就是已写入内存的位置；DMA FIFO 会让它超前） */
  static bool prepare_rx(UART_HandleTypeDef *huart)
  {
    DMA_HandleTypeDef *d       = huart->hdmarx;
    const bool         stream  = (IS_DMA_STREAM_INSTANCE(d->Instance) != 0U);
    const bool         circ_ok = (d->Init.Mode == DMA_CIRCULAR);
    const bool         fifo_ok = !stream || (d->Init.FIFOMode == DMA_FIFOMODE_DISABLE);
    if (circ_ok && fifo_ok)
    {
      return true;
    }
    d->Init.Mode = DMA_CIRCULAR;
    if (stream)
    {
      d->Init.FIFOMode = DMA_FIFOMODE_DISABLE;
    }
    return HAL_DMA_Init(d) == HAL_OK;
  }

  static bool arm_rx(UART_HandleTypeDef *huart, uint8_t *buffer, uint16_t size)
  {
    return HAL_UARTEx_ReceiveToIdle_DMA(huart, buffer, size) == HAL_OK;
  }

  /** @brief DMA 当前写到的位置 [0, size) */
  static size_t rx_write_pos(const UART_HandleTypeDef *huart, size_t size)
  {
    const size_t left = static_cast<size_t>(__HAL_DMA_GET_COUNTER(huart->hdmarx));
    return (left == 0U || left > size) ? 0U : (size - left);
  }

  static bool start_tx(UART_HandleTypeDef *huart, uint8_t *buffer, uint16_t size)
  {
    return HAL_UART_Transmit_DMA(huart, buffer, size) == HAL_OK;
  }

  /** @brief 在途发送还剩多少字节没交给串口（DMA 剩余计数） */
  static size_t tx_remaining(const UART_HandleTypeDef *huart)
  {
    return (huart->hdmatx != nullptr) ? static_cast<size_t>(__HAL_DMA_GET_COUNTER(huart->hdmatx)) : 0U;
  }

  /**
   * @brief 中止发送并返回剩余计数（阻塞版 HAL，只能在任务上下文）
   * @note 先中止再读：中止后 DMA 停住，读到的剩余数才准（中止前读，之后又发出几个字节，续发时就会重复）。
   */
  static size_t abort_tx(UART_HandleTypeDef *huart)
  {
    (void)HAL_UART_AbortTransmit(huart);
    return tx_remaining(huart);
  }

  /** @brief 中止接收（阻塞版 HAL，只能在任务上下文），并确保 RX 流真的停了 */
  static void abort_rx(UART_HandleTypeDef *huart)
  {
    (void)HAL_UART_AbortReceive(huart);
    disable_dma_stream(huart->hdmarx);
  }

  static bool repair_rx(UART_HandleTypeDef *huart)
  {
    return repair_dma_handle(huart->hdmarx);
  }

  static bool repair_tx(UART_HandleTypeDef *huart)
  {
    return repair_dma_handle(huart->hdmatx);
  }

  /** @brief 运行中重新 init() / deinit()：确保本实例用到的流真的停了、句柄回到 READY */
  static void force_stop(UART_HandleTypeDef *huart, bool rx, bool tx)
  {
    if (rx)
    {
      stop_dma_stream(huart->hdmarx);
    }
    if (tx)
    {
      stop_dma_stream(huart->hdmatx);
    }
  }

  /** @brief RX 在收：HAL BUSY_RX + CR3.DMAR + DMA EN 三处一致 */
  static bool rx_ok(const UART_HandleTypeDef *huart)
  {
    return rx_busy(huart) && dmar(huart) && dma_en(huart);
  }

  /** @brief RX 已停：三处一致为“停” */
  static bool rx_stopped(const UART_HandleTypeDef *huart)
  {
    return !rx_busy(huart) && !dmar(huart) && !dma_en(huart);
  }

private:
  static bool rx_busy(const UART_HandleTypeDef *huart)
  {
    return huart->RxState == HAL_UART_STATE_BUSY_RX;
  }

  static bool dmar(const UART_HandleTypeDef *huart)
  {
    return (huart->Instance->CR3 & USART_CR3_DMAR) != 0U;
  }

  static bool dma_en(const UART_HandleTypeDef *huart)
  {
    if (huart->hdmarx == nullptr || IS_DMA_STREAM_INSTANCE(huart->hdmarx->Instance) == 0U)
    {
      return false;
    }
    return (reinterpret_cast<const DMA_Stream_TypeDef *>(huart->hdmarx->Instance)->CR & DMA_SxCR_EN) != 0U;
  }
};

/**
 * @brief 中断收发：没有 DMA 通道的串口用（CubeMX 只需开串口全局中断）
 * @note 接收每次 IDLE / 写满投递一包后重新挂接收；字节在 RDR / 硬件 FIFO 里等待，不会因重新挂接收而丢。
 */
struct ItTransport
{
  static constexpr bool CIRCULAR_RX = false;
  static constexpr bool NEEDS_DMA   = false;

  static bool prepare_rx(UART_HandleTypeDef *)
  {
    return true;
  }

  static bool arm_rx(UART_HandleTypeDef *huart, uint8_t *buffer, uint16_t size)
  {
    return HAL_UARTEx_ReceiveToIdle_IT(huart, buffer, size) == HAL_OK;
  }

  static size_t rx_write_pos(const UART_HandleTypeDef *, size_t)
  {
    return 0U;
  }

  static bool start_tx(UART_HandleTypeDef *huart, uint8_t *buffer, uint16_t size)
  {
    return HAL_UART_Transmit_IT(huart, buffer, size) == HAL_OK;
  }

  /** @brief 在途发送还剩多少字节没写进串口（HAL 的 TxXferCount） */
  static size_t tx_remaining(const UART_HandleTypeDef *huart)
  {
    return static_cast<size_t>(huart->TxXferCount);
  }

  /**
   * @brief 中止发送并返回剩余计数
   * @note HAL_UART_AbortTransmit() 会把 TxXferCount 清零，所以先关掉发送中断（不再往串口写），读出计数，再中止。
   */
  static size_t abort_tx(UART_HandleTypeDef *huart)
  {
    ATOMIC_CLEAR_BIT(huart->Instance->CR1, USART_CR1_TXEIE_TXFNFIE | USART_CR1_TCIE);
    ATOMIC_CLEAR_BIT(huart->Instance->CR3, USART_CR3_TXFTIE);
    const size_t left = tx_remaining(huart);
    (void)HAL_UART_AbortTransmit(huart);
    return left;
  }

  static void abort_rx(UART_HandleTypeDef *huart)
  {
    (void)HAL_UART_AbortReceive(huart);
  }

  static bool repair_rx(UART_HandleTypeDef *)
  {
    return false;
  }

  static bool repair_tx(UART_HandleTypeDef *)
  {
    return false;
  }

  static void force_stop(UART_HandleTypeDef *, bool, bool)
  {
  }

  /** @brief RX 在收：HAL BUSY_RX 且接收中断开着（FIFO 模式下 HAL 可能改用 RXFTIE） */
  static bool rx_ok(const UART_HandleTypeDef *huart)
  {
    return (huart->RxState == HAL_UART_STATE_BUSY_RX) && rx_irq_enabled(huart);
  }

  static bool rx_stopped(const UART_HandleTypeDef *huart)
  {
    return (huart->RxState != HAL_UART_STATE_BUSY_RX) && !rx_irq_enabled(huart);
  }

private:
  static bool rx_irq_enabled(const UART_HandleTypeDef *huart)
  {
    constexpr uint32_t BYTE_BITS = USART_CR1_RXNEIE_RXFNEIE | USART_CR1_IDLEIE;
    const bool         byte_irq  = (huart->Instance->CR1 & BYTE_BITS) == BYTE_BITS;
#if ICBSP_UART_HAS_FIFO
    if (huart->FifoMode == UART_FIFOMODE_ENABLE)
    {
      const bool fifo_irq = ((huart->Instance->CR3 & USART_CR3_RXFTIE) != 0U) && ((huart->Instance->CR1 & USART_CR1_IDLEIE) != 0U);
      return fifo_irq || byte_irq;
    }
#endif
    return byte_irq;
  }
};
} // namespace uart_detail


/**
 * @brief 实例是否在 board_instances.hpp 的 ICBSP_UART_INSTANCES 里，且是清单里唯一绑这个句柄的实例（实现在 bsp_uart.cpp）
 * @note HAL 回调与 sys_task 巡检只认清单里的实例：不在清单里的实例 init() 成功也收不到回调（对比评估 N13），
 *       两个实例绑同一个句柄时回调只会交给前一个。init() 用它把这两种配置错误变成 BAD_ARG。
 */
bool bsp_uart_is_listed(const void *self, const UART_HandleTypeDef *huart);

/**
 * @brief 清单外串口的回调转调（弱函数，默认什么也不做；实现在 bsp_uart.cpp）
 * @note 库占用了 HAL_UARTEx_RxEventCallback / HAL_UART_TxCpltCallback / HAL_UART_ErrorCallback，宿主不能再定义它们。
 *       宿主自己用 HAL 管的串口（不在 ICBSP_UART_INSTANCES 里）的这三个回调转到下面三个函数，宿主按需定义同名强函数即可。
 */
extern "C"
{
  void icbsp_unhandled_uart_rx_event(UART_HandleTypeDef *huart, uint16_t size);
  void icbsp_unhandled_uart_tx_cplt(UART_HandleTypeDef *huart);
  void icbsp_unhandled_uart_error(UART_HandleTypeDef *huart);
}

/** @brief BspUartT 的 Transport 短名：uart_mode::dma / uart_mode::it */
namespace uart_mode
{
using dma = uart_detail::DmaTransport;
using it  = uart_detail::ItTransport;
} // namespace uart_mode


/** @brief 串口收发方向 */
enum class UartDir : uint8_t
{
  RxOnly = 0, ///< 只接收（DMA 模式下可以不配 TX DMA）
  TxOnly,     ///< 只发送（DMA 模式下可以不配 RX DMA）
  TxRx,       ///< 收发都启用
};


/* ==================== */
/* ==================== 串口驱动 ==================== */

/**
 * @brief 串口驱动（IDLE 中断 + DMA / 中断收发 + FreeRTOS 流缓冲区）
 *
 * @tparam Transport uart_mode::dma 或 uart_mode::it
 * @tparam RX_SIZE   接收暂存区大小（字节）；接收流缓冲区是它的 2 倍
 * @tparam TX_SIZE   发送暂存区、发送流缓冲区与 printf 缓冲大小（字节），也是单次 send() 的上限
 *
 * @note 用法（实例写在 board_cfg.hpp，DMA 模式必须放进 .dma_buffer，段名按变量唯一）：
 *
 *   inline ICBSP_DMA_BUFFER(bsp_uart1) BspUartT<uart_mode::dma, 128, 128> bsp_uart1(
 *     {.huart = &huart1, .mode = UartDir::TxRx, .baudrate = 115200U});
 *   // board_instances.hpp 的 ICBSP_UART_INSTANCES 里加 X(bsp_uart1)；board_init() 里 ICBSP_CHECK(bsp_uart1.init() == Status::OK);
 *
 *   bsp_uart1.send(buf, 8);             // 写进发送流缓冲区，自动发出去
 *   bsp_uart1.printf("v=%d\r\n", 42);   // 格式化输出（10 ms 入队超时）
 *   bsp_uart1.receive(buf, 8, 10);      // 从接收流缓冲区读，最多等 10 ms
 */
template <typename Transport, size_t RX_SIZE, size_t TX_SIZE>
class BspUartT
{
  static_assert(RX_SIZE >= 2U && RX_SIZE <= 32767U, "RX_SIZE 必须在 2~32767 之间（流缓冲区是它的 2 倍）");
  static_assert(TX_SIZE > 0U && TX_SIZE <= 65535U, "TX_SIZE 必须在 1~65535 之间");

public:
  /* ==================== */
  /* ==================== 公有接口 ==================== */

  static constexpr size_t TX_CAPACITY = TX_SIZE; ///< 单次 send() 的上限（发送暂存区大小），设备层按它定自己的帧缓冲
  static constexpr size_t RX_CAPACITY = RX_SIZE; ///< 接收 DMA 缓冲大小

  /**
   * @brief 串口配置
   * @note 聚合体：可用指定初始化器 `{.huart = &huartX, .mode = ..., .baudrate = ...}`（按声明顺序）。
   */
  struct Config
  {
    UART_HandleTypeDef *huart    = nullptr;       ///< UART 句柄
    UartDir             mode     = UartDir::TxRx; ///< 收发方向
    uint32_t            baudrate = 115200U;       ///< 波特率 (bit/s)，TX 卡死判定的最短等待时间按它估算
  };

  /**
   * @brief RX 的健康状态
   * @note 只信 HAL 的 RxState 不够：底层通道可能已经停了，HAL 状态还没更新。具体检查项由 Transport 决定。
   */
  enum class RxHealth : uint8_t
  {
    OK = 0,       ///< 在收
    STOPPED,      ///< 已停（没启动过或主动停的，不算故障）
    INCONSISTENT, ///< 状态不一致，需要重建
  };

  /** @brief RX 诊断计数：只增不减，一切正常时应全为 0 */
  struct RxDiag
  {
    uint32_t arm_fail_cnt;   ///< 启动接收失败的次数（包括 init 时的第一次和运行期重启）
    uint32_t drop_bytes;     ///< 接收流缓冲区满，被丢掉的字节数
    uint32_t hw_error_cnt;   ///< HAL 错误回调里的 RX 硬件错误次数（ORE / FE / NE / PE / RTO，合计）
    uint32_t pe_cnt;         ///< 其中带奇偶校验错误（PE）的次数（F7：DMA 模式下 HAL 不中止接收，坏字节照常进流缓冲区）
    uint32_t fe_cnt;         ///< 其中带帧错误（FE）的次数：波特率不对、线上有干扰
    uint32_t ne_cnt;         ///< 其中带噪声错误（NE）的次数：共地 / 线材问题
    uint32_t ore_cnt;        ///< 其中带溢出（ORE）的次数：搬运不及时，丢了字节
    uint32_t error_cnt;      ///< rx_recover() 真正重建 RX 的次数
    uint32_t dma_repair_cnt; ///< rx_recover() 发现 RX DMA 句柄卡在非 READY（如 ERROR）并复位的次数
  };

  /** @brief TX 诊断计数：只增不减，一切正常时应全为 0 */
  struct TxDiag
  {
    uint32_t drop_bytes;        ///< 入队失败（缓冲区满或超时）丢掉的字节数
    uint32_t start_fail_cnt;    ///< 启动发送失败的次数
    uint32_t isr_skip_cnt;      ///< 中断里发现通道不空闲，跳过续传的次数
    uint32_t stall_recover_cnt; ///< 巡检发现卡死并恢复成功的次数（含下面的 idle_kick_cnt）
    uint32_t idle_kick_cnt;     ///< 其中“通道空闲、却有数据没人启动”、由巡检补发的次数（漏唤醒；正常应为 0）
    uint32_t idle_kick_uart_err; ///< 最近一次发现“空闲却有数据”时 huart->ErrorCode 的快照（定位漏唤醒的来源）
    uint32_t idle_kick_dma_err;  ///< 同上，hdmatx->ErrorCode 的快照（中断收发为 0）
    uint32_t error_cnt;         ///< TX 触发错误回调的次数（最终由 tx_recover() 恢复）
    uint32_t dma_repair_cnt;    ///< tx_recover() 发现 TX DMA 句柄卡在非 READY 并复位的次数
  };

  /**
   * @brief 构造函数（只做赋值，FreeRTOS 资源创建推迟到 init()）
   * @note 必须 constexpr：board_cfg.hpp 里的全局实例是 inline 变量，只有常量初始化才不会每个 TU 生成一份构造代码。
   */
  constexpr BspUartT(const Config &cfg) :
    _huart(cfg.huart), _rx_should_run(cfg.mode != UartDir::TxOnly), _mode(cfg.mode), _baudrate(cfg.baudrate)
  {
  }

  /** @brief 平凡析构（全局实例永不析构）；需要释放资源时显式调 deinit() */
  ~BspUartT() = default;

  // 两个对象绑同一个句柄会让 HAL 回调分不清是谁
  BspUartT(const BspUartT &)            = delete;
  BspUartT &operator=(const BspUartT &) = delete;

  /**
   * @brief 检查配置、创建流缓冲区与锁、打开 UART FIFO，然后启动接收
   *
   * @note 初始化阶段调用（system_init() 之后、system_start() 之前；调度器启动前也可以）。
   *       可以重复调用：已初始化的实例先停掉收发、释放上一次的资源再重建；
   *       此时不能有任务阻塞在本实例的 send() / receive() 上（流缓冲区会被删除重建）。
   *
   * @return OK=成功；IO_ERROR=资源创建失败或接收没能启动；
   *         BAD_ARG=句柄为空；DMA 模式下需要的 RX / TX DMA 没配，或实例不在 DMA 可达的内存里（没放进 .dma_buffer）；
   *         实例不在 ICBSP_UART_INSTANCES 清单里，或清单里有另一个实例绑了同一个句柄
   */
  Status init();

  /** @brief 停掉 RX / TX 通道并释放所有软件资源（非全局实例销毁前手动调用） */
  void deinit();

  /**
   * @brief 把数据写进发送流缓冲区，并启动发送
   *
   * @param data       要发的数据
   * @param size       字节数；超过 TX_SIZE 直接返回 BAD_ARG，调用方自己分包
   * @param timeout_ms 入队最多等多久（毫秒）；portMAX_DELAY = 一直等到全部写进去
   * @param written    返回实际写进去的字节数，不需要可以传 nullptr
   * @return OK=全部入队；TIMEOUT=只进去一部分；BAD_ARG=参数非法；IO_ERROR=没启用发送 / 没初始化
   *
   * @note 多个任务同时 send() 安全：_tx_write_lock 保证流缓冲区只有一个写者，一次写入的字节不会和别人交错。
   *       等锁与等空间共用同一个超时（最坏约 2 × timeout_ms）。
   */
  Status send(const uint8_t *data, size_t size, uint32_t timeout_ms = portMAX_DELAY, size_t *written = nullptr);

  /**
   * @brief 格式化输出，用法和 printf 一样（只能在任务里调用，超长截断为 TX_SIZE - 1 字节）
   * @note 取锁与入队都只等 10 ms：发送卡住时不把调用任务一直挂住。没全部写进去就返回 TIMEOUT，已进去的部分照常发。
   * @return OK=全部入队（空串也是 OK）；TIMEOUT=取锁超时或只进去一部分；BAD_ARG=fmt 为空或格式串错误；IO_ERROR=没启用发送 / 没初始化
   */
  Status printf(const char *fmt, ...);

  /**
   * @brief 从接收流缓冲区读数据（字节流，不保证整帧；除非流缓冲区满，否则有序、不重、不丢）
   *
   * @param buffer     收数据的地方
   * @param size       最多读多少字节
   * @param timeout_ms 最多等多久（毫秒）；portMAX_DELAY = 一直等
   * @param received   返回实际读到的字节数，不需要可以传 nullptr
   * @return OK=读到了；TIMEOUT=没有数据；BAD_ARG=参数非法；IO_ERROR=没启用接收 / 没初始化
   * @warning **单读者**：FreeRTOS 流缓冲区只允许一个读者，同一个串口只能由一个任务 receive()（F7）；多写者的 send() 才是安全的。
   */
  Status receive(uint8_t *buffer, size_t size, uint32_t timeout_ms = portMAX_DELAY, size_t *received = nullptr);

  /**
   * @brief TX 卡死后的恢复（sys_task 每 10 ms 调用；取锁不等待，只能在任务上下文）
   *
   * @note 两种情况：
   *       ① 通道空闲，但流缓冲区或发送暂存区里还有没发的数据（丢了一次发送完成中断、或启动失败）
   *          → **连续两次**巡检都看到才重新启动（send() 入队后、自己启动前被打断的一瞬间也是这样，不算卡死）；
   *       ② 通道忙，但发送没有推进（剩余计数不变且串口不在移位）达到阈值 → 中止，按剩余计数算出已发多少，
   *          只续发剩下的（不重复、不丢），之后再从流缓冲区取新数据。
   * @note 阈值 = 一整包按 Config::baudrate（为 0 时取 CubeMX 的波特率）发完所需时间的 2 倍；但只有“没推进”才计时，
   *       Config::baudrate 填错（比实际快）也不会把正常发送误判成卡死。
   * @return true=这次确实恢复了一次
   */
  bool tx_recover();

  /**
   * @brief RX 异常时的恢复（sys_task 每 10 ms 调用；只能在任务上下文）
   *
   * @note 触发条件：本该在收，但 get_rx_health() 不是 OK。一切正常时只读一次状态就返回。
   * @note 「中止 → 复位 DMA 句柄 → 重新启动」期间屏蔽本串口与其 DMA 流的中断；RX DMA 句柄卡在 ERROR 时
   *       在这里复位（计入 dma_repair_cnt）。DMA 模式下中止前先把已经收到、还没投递的字节投进流缓冲区。
   * @return true=这次确实重建了一次
   */
  bool rx_recover();

  /** @brief 底层 UART 句柄（供 HAL 回调分发） */
  UART_HandleTypeDef *get_handle() const
  {
    return _huart;
  }

  /* ==================== */
  /* ==================== ISR 入口（bsp_uart.cpp 的 HAL 回调调用） ==================== */

  /**
   * @brief 接收事件（HAL_UARTEx_RxEventCallback：IDLE / HT / TC）
   * @param size HAL 给的长度；DMA 循环模式下不用它，直接按 DMA 当前写到的位置投递
   */
  void on_idle_isr(uint16_t size, BaseType_t *pxHigherPriorityTaskWoken);

  /** @brief 发送完成（HAL_UART_TxCpltCallback）：这一段发完了，接着发下一段 */
  void start_transmission_from_isr(BaseType_t *pxHigherPriorityTaskWoken);

  /** @brief UART 错误回调：只计数，不恢复（恢复要调阻塞版 HAL，交给任务侧的 rx_recover() / tx_recover()） */
  void on_error_isr();

  /* ==================== */
  /* ==================== 查询接口 ==================== */

  /** @brief 发送流缓冲区剩余空间（字节） */
  size_t get_tx_free_space();

  /** @brief 接收流缓冲区可读字节数 */
  size_t get_rx_available_data();

  /** @brief 核对 CubeMX 写入的帧格式（只读 huart->Init） */
  bool check_format(uint32_t baudrate, uint32_t word_length, uint32_t parity, uint32_t stop_bits) const;

  /** @brief RX 是否真的在收；本该在收时返回非 OK 说明出了故障 */
  RxHealth get_rx_health() const;

  /** @brief RX 诊断计数 */
  RxDiag get_rx_diag() const;

  /** @brief TX 诊断计数 */
  TxDiag get_tx_diag() const;

  /* ==================== */

private:
  /* ==================== 成员变量 ==================== */

  static constexpr size_t   RX_STREAM_SIZE         = 2U * RX_SIZE; ///< 接收流缓冲区容量
  static constexpr uint32_t PRINTF_TIMEOUT_MS      = 10U;          ///< printf() 取锁 / 入队的等待上限
  static constexpr uint32_t TX_STALL_MIN_MS        = 10U;          ///< 发送卡死判定阈值的下限 (ms)，= sys_task 巡检周期

  UART_HandleTypeDef  *_huart;                      ///< UART 句柄
  StreamBufferHandle_t _rx_stream_buffer = nullptr; ///< 接收流缓冲区（写者：接收中断；读者：receive()）
  StreamBufferHandle_t _tx_stream_buffer = nullptr; ///< 发送流缓冲区（写者：send()；读者：持 _tx_lock 的任务侧或发送完成中断）
  SemaphoreHandle_t    _tx_write_lock    = nullptr; ///< 发送入队锁：发送流缓冲区只有一个写者
  SemaphoreHandle_t    _tx_lock          = nullptr; ///< 发送启动锁：「判忙 → 取包 → 启动」互斥
  SemaphoreHandle_t    _printf_lock      = nullptr; ///< printf 锁：保护格式化缓冲区

  bool     _rx_should_run;              ///< 是否希望 RX 在收；某次失败不会把它永久置成 false
  UartDir  _mode;                       ///< 收发方向
  uint32_t _baudrate;                   ///< 波特率 (bit/s)
  uint8_t  _rx_buffer[RX_SIZE]     = {}; ///< 接收暂存区（DMA 循环缓冲 / 中断接收缓冲）
  uint8_t  _tx_buffer[TX_SIZE]     = {}; ///< 发送暂存区
  char     _printf_buffer[TX_SIZE] = {}; ///< printf 格式化缓冲区

  volatile size_t _rx_read_pos = 0U; ///< DMA 循环模式：循环缓冲里下一个还没投递的字节位置

  // RX 诊断量：中断里累加，任务里读取
  volatile uint32_t _rx_arm_fail_cnt   = 0U;
  volatile uint32_t _rx_drop_bytes     = 0U;
  volatile uint32_t _rx_hw_error_cnt   = 0U;
  volatile uint32_t _rx_pe_cnt         = 0U; ///< RX 奇偶校验错误次数
  volatile uint32_t _rx_fe_cnt         = 0U; ///< RX 帧错误次数
  volatile uint32_t _rx_ne_cnt         = 0U; ///< RX 噪声错误次数
  volatile uint32_t _rx_ore_cnt        = 0U; ///< RX 溢出次数
  volatile uint32_t _rx_error_cnt      = 0U;
  volatile uint32_t _rx_dma_repair_cnt = 0U;

  // TX 诊断量：中断里累加，任务里读取
  volatile uint32_t _tx_drop_bytes     = 0U;
  volatile uint32_t _tx_start_fail_cnt = 0U;
  volatile uint32_t _tx_isr_skip_cnt   = 0U;
  volatile uint32_t _tx_stall_rec_cnt  = 0U;
  volatile uint32_t _tx_error_cnt      = 0U;
  volatile uint32_t _tx_dma_repair_cnt = 0U;
  volatile uint32_t _tx_idle_kick_cnt  = 0U;
  uint32_t          _tx_idle_kick_uart_err = 0U; ///< 只有 tx_recover() 写
  uint32_t          _tx_idle_kick_dma_err  = 0U; ///< 只有 tx_recover() 写

  volatile TickType_t _tx_last_activity_tick = 0U; ///< 最近一次发送有推进（启动 / 剩余计数变化 / 串口在移位）的时刻
  size_t              _tx_last_remaining     = 0U; ///< tx_recover() 上次看到的剩余计数（只有 tx_recover() 读写）

  // _tx_buffer 里还没确认发完的那一段：[_tx_chunk_off, _tx_chunk_off + _tx_chunk_len)。
  // 只在通道空闲时由任务侧（持 _tx_lock）或发送完成中断改写，两者不会同时发生。
  volatile size_t _tx_chunk_off    = 0U;    ///< 这一段在 _tx_buffer 里的起点
  volatile size_t _tx_chunk_len    = 0U;    ///< 这一段的长度；0 = 没有未发完的数据
  volatile bool   _tx_chunk_active = false; ///< true = 这一段已交给 Transport 在传；false = 启动失败、等待重发

  bool       _tx_idle_pending_seen = false; ///< 上次 tx_recover() 是否已看到「通道空闲但有数据」（只有 tx_recover() 读写）
  TickType_t _tx_idle_seen_activity = 0U;   ///< 那次看到时的 _tx_last_activity_tick：之后有过启动就不算“没人启动”（只有 tx_recover() 读写）

  /* ==================== */
  /* ==================== 内部实现 ==================== */

  bool tx_enabled() const
  {
    return _mode != UartDir::RxOnly;
  }

  bool rx_enabled() const
  {
    return _mode != UartDir::TxOnly;
  }

  /** @brief 启动接收（调用前 RxState 必须是 READY）；失败计入 arm_fail_cnt */
  bool arm_reception();

  /** @brief 中止 RX 通道并复位接收状态（只动 RX） */
  void abort_reception();

  /** @brief 停掉本实例的收发（运行中重新 init() / deinit() 用；调用方已屏蔽中断） */
  void stop_channels();

  /** @brief DMA 循环模式：把 [_rx_read_pos, DMA 当前位置) 投进接收流缓冲区（中断或已屏蔽中断的任务里调用） */
  void rx_drain(BaseType_t *pxHigherPriorityTaskWoken);

  /** @brief 把 _rx_buffer[off, off + len) 投进接收流缓冲区，放不下的计入 drop_bytes */
  void rx_push(size_t off, size_t len, BaseType_t *pxHigherPriorityTaskWoken);

  /** @brief 释放已创建的 FreeRTOS 资源（两个流缓冲区、三把锁） */
  void cleanup_resources();

  /** @brief 任务侧启动发送；wait=0 表示取不到锁就立刻返回 */
  bool start_transmission(TickType_t wait = portMAX_DELAY);

  /**
   * @brief 启动一段发送：先续发上次留下的那一段，没有才从流缓冲区取新数据
   * @param from_isr true = 在发送完成中断里调用（用 FromISR 版 API）
   * @note 调用前提：通道空闲（gState == READY），任务侧已持有 _tx_lock。
   */
  bool start_chunk(bool from_isr, BaseType_t *pxHigherPriorityTaskWoken);

  /** @brief 结算已停止的那一段：按剩余计数把起点后移、长度减去已发出的字节 */
  void settle_chunk(size_t remaining);

  /* ==================== */
};


/* ==================== 成员函数定义 ==================== */

template <typename Transport, size_t RX_SIZE, size_t TX_SIZE>
Status BspUartT<Transport, RX_SIZE, TX_SIZE>::init()
{
  if (_huart == nullptr)
  {
    return Status::BAD_ARG;
  }

  if (Transport::NEEDS_DMA)
  {
    // 没配 DMA 时 HAL_UARTEx_ReceiveToIdle_DMA() 照样返回 OK（只是不搬数据），init() 会“成功”但永远收不到（2026-10-17 实测）
    if ((rx_enabled() && _huart->hdmarx == nullptr) || (tx_enabled() && _huart->hdmatx == nullptr))
    {
      return Status::BAD_ARG;
    }
    // 收发暂存区是对象成员，DMA 直接读写：实例忘了放进 .dma_buffer（或链接脚本把它放进了 DTCM）就拒绝
    // DMA1/2 与 BDMA 能访问的内存不同，按各方向实际用的 DMA 判断（A4）
    if ((rx_enabled() && !icbsp_dma_reachable(_rx_buffer, _huart->hdmarx)) || (tx_enabled() && !icbsp_dma_reachable(_tx_buffer, _huart->hdmatx)))
    {
      return Status::BAD_ARG;
    }
  }

  // 不在清单里收不到 HAL 回调、也不被巡检；同句柄两个实例回调只交给一个：都按配置错误拒绝
  if (!bsp_uart_is_listed(this, _huart))
  {
    return Status::BAD_ARG;
  }

  // 已初始化过（运行中再次 init()）：先停掉收发再释放资源。
  // 不停的话 RX 仍是 BUSY_RX，重新启动接收必然失败，而旧资源已释放 —— 实测 init() 返回 IO_ERROR、实例从此失效。
  if (_rx_stream_buffer != nullptr || _tx_stream_buffer != nullptr)
  {
    uart_detail::IrqGuard guard(_huart);
    stop_channels();
  }
  cleanup_resources();

  _tx_idle_pending_seen = false;
  _tx_chunk_off         = 0U;
  _tx_chunk_len         = 0U;
  _tx_chunk_active      = false;
  _rx_read_pos          = 0U;

  if (rx_enabled())
  {
    _rx_stream_buffer = xStreamBufferCreate(RX_STREAM_SIZE, 1);
    if (_rx_stream_buffer == nullptr)
    {
      cleanup_resources();
      return Status::IO_ERROR;
    }
  }

  if (tx_enabled())
  {
    _tx_stream_buffer = xStreamBufferCreate(TX_SIZE, 1);
    _tx_write_lock    = xSemaphoreCreateMutex();
    _tx_lock          = xSemaphoreCreateMutex();
    _printf_lock      = xSemaphoreCreateMutex();
    if (_tx_stream_buffer == nullptr || _tx_write_lock == nullptr || _tx_lock == nullptr || _printf_lock == nullptr)
    {
      cleanup_resources();
      return Status::IO_ERROR;
    }
  }

  // 打开 UART 硬件 FIFO（16 字节）。CubeMX 默认关着，此时接收只有 RDR 一级缓冲：搬运稍晚一点下一个字节就 ORE。
  // 2026-10-17 实测：不开 FIFO 时连续流在 2 Mbps 以上几乎全丢，开了之后 4 Mbps 也基本无损。
  // 代价：tx_recover() 中止发送时会清掉 TX FIFO 里最多 16 字节（只在卡死恢复路径上）。
  // F4 没有硬件 FIFO（ICBSP_UART_HAS_FIFO = 0），也没有 HAL_UARTEx_EnableFifoMode()，整段不编译。
#if ICBSP_UART_ENABLE_FIFO && ICBSP_UART_HAS_FIFO
  (void)HAL_UARTEx_EnableFifoMode(_huart);
#endif

  if (rx_enabled())
  {
    if (!Transport::prepare_rx(_huart) || !arm_reception())
    {
      abort_reception();
      cleanup_resources();
      return Status::IO_ERROR;
    }
  }

  return Status::OK;
}

template <typename Transport, size_t RX_SIZE, size_t TX_SIZE>
void BspUartT<Transport, RX_SIZE, TX_SIZE>::deinit()
{
  if (_huart != nullptr)
  {
    _rx_should_run = false;
    uart_detail::IrqGuard guard(_huart);
    stop_channels();
  }
  cleanup_resources();
}

template <typename Transport, size_t RX_SIZE, size_t TX_SIZE>
Status BspUartT<Transport, RX_SIZE, TX_SIZE>::send(const uint8_t *data, size_t size, uint32_t timeout_ms, size_t *written)
{
  if (data == nullptr || size == 0U || size > TX_SIZE)
  {
    return Status::BAD_ARG; // 超过流缓冲区容量时 xStreamBufferSend 会一直阻塞，必须拒绝
  }
  if (!tx_enabled() || _tx_stream_buffer == nullptr || _tx_write_lock == nullptr)
  {
    return Status::IO_ERROR;
  }

  size_t bytes_written = 0U;
  if (xSemaphoreTake(_tx_write_lock, uart_detail::ms_to_ticks(timeout_ms)) == pdTRUE)
  {
    bytes_written = xStreamBufferSend(_tx_stream_buffer, data, size, uart_detail::ms_to_ticks(timeout_ms));
    (void)xSemaphoreGive(_tx_write_lock);
  }

  if (bytes_written > 0U)
  {
    (void)start_transmission();
  }

  if (bytes_written < size)
  {
    _tx_drop_bytes = _tx_drop_bytes + static_cast<uint32_t>(size - bytes_written);
  }
  if (written != nullptr)
  {
    *written = bytes_written;
  }
  return (bytes_written == size) ? Status::OK : Status::TIMEOUT;
}

template <typename Transport, size_t RX_SIZE, size_t TX_SIZE>
Status BspUartT<Transport, RX_SIZE, TX_SIZE>::printf(const char *fmt, ...)
{
  if (fmt == nullptr)
  {
    return Status::BAD_ARG;
  }
  if (!tx_enabled() || _tx_stream_buffer == nullptr || _printf_lock == nullptr)
  {
    return Status::IO_ERROR;
  }
  if (xSemaphoreTake(_printf_lock, pdMS_TO_TICKS(PRINTF_TIMEOUT_MS)) != pdTRUE)
  {
    return Status::TIMEOUT; // 别的任务正在 printf（F7：原来返回 IO_ERROR）
  }

  va_list args;
  va_start(args, fmt);
  int len = vsnprintf(_printf_buffer, sizeof(_printf_buffer), fmt, args);
  va_end(args);

  if (len <= 0)
  {
    // 0 = 空串（printf("")），没有东西要发，算成功；< 0 = 格式串错误（F7：原来两种都返回 BAD_ARG）
    (void)xSemaphoreGive(_printf_lock);
    return (len == 0) ? Status::OK : Status::BAD_ARG;
  }
  if (static_cast<size_t>(len) >= sizeof(_printf_buffer))
  {
    len = static_cast<int>(sizeof(_printf_buffer)) - 1; // vsnprintf 返回期望长度，实际已截断
  }

  const Status st = send(reinterpret_cast<const uint8_t *>(_printf_buffer), static_cast<size_t>(len), PRINTF_TIMEOUT_MS);
  (void)xSemaphoreGive(_printf_lock);
  return st;
}

template <typename Transport, size_t RX_SIZE, size_t TX_SIZE>
Status BspUartT<Transport, RX_SIZE, TX_SIZE>::receive(uint8_t *buffer, size_t size, uint32_t timeout_ms, size_t *received)
{
  if (buffer == nullptr || size == 0U)
  {
    return Status::BAD_ARG;
  }
  if (_rx_stream_buffer == nullptr)
  {
    return Status::IO_ERROR;
  }

  const size_t bytes_read = xStreamBufferReceive(_rx_stream_buffer, buffer, size, uart_detail::ms_to_ticks(timeout_ms));
  if (received != nullptr)
  {
    *received = bytes_read;
  }
  return (bytes_read > 0U) ? Status::OK : Status::TIMEOUT;
}

template <typename Transport, size_t RX_SIZE, size_t TX_SIZE>
bool BspUartT<Transport, RX_SIZE, TX_SIZE>::tx_recover()
{
  if (_huart == nullptr || _tx_stream_buffer == nullptr || _tx_lock == nullptr)
  {
    return false;
  }
  // Config::baudrate 填 0 时按 CubeMX 配的波特率估算（F7：原来直接返回，卡死恢复整个被关掉）
  const uint32_t baud = (_baudrate != 0U) ? _baudrate : _huart->Init.BaudRate;
  if (baud == 0U)
  {
    return false;
  }

  // 卡死判定阈值 (ms) = 一包发送耗时的 2 倍（向上取整）：TX_SIZE × 10 bit ÷ 波特率 × 1000 × 2，
  // 下限是巡检周期 TX_STALL_MIN_MS（Q5）：tick 只有 1 ms 分辨率，阈值 1~2 ms 时刚开始的一次传输就可能被判成卡死并中止
  const uint32_t pack_ms  = static_cast<uint32_t>((static_cast<uint64_t>(TX_SIZE) * 20000U + baud - 1U) / baud);
  const uint32_t stall_ms = (pack_ms < TX_STALL_MIN_MS) ? TX_STALL_MIN_MS : pack_ms;

  if (xStreamBufferBytesAvailable(_tx_stream_buffer) == 0U && _tx_chunk_len == 0U)
  {
    _tx_idle_pending_seen = false;
    return false; // 没有任何待发数据
  }

  // 巡检不阻塞：别的任务正在启动发送就下轮再看。恢复路径改写 _tx_buffer / 在途段，必须持锁（N31）
  if (xSemaphoreTake(_tx_lock, 0) != pdTRUE)
  {
    return false;
  }

  bool restarted = false;
  if (_huart->gState == HAL_UART_STATE_READY)
  {
    // ① 通道空闲却有数据：第一次只记下，下一次仍是这样、并且这期间没有任何一次启动，才重新启动。
    //    只看“两次都空闲且有数据”会误报：写者在“写进流缓冲区 → 拿 _tx_lock 启动”之间被本巡检抢占时也是这个样子，
    //    短段连续发送时相隔 10 ms 的两次巡检都可能落进这个窗口（阶段 3 U21 实测，数据都是写者自己随后发出的）
    if (_tx_idle_pending_seen && _tx_last_activity_tick == _tx_idle_seen_activity)
    {
      if (Transport::repair_tx(_huart))
      {
        _tx_dma_repair_cnt = _tx_dma_repair_cnt + 1U;
      }
      restarted             = start_chunk(false, nullptr);
      _tx_idle_pending_seen = false;
      if (restarted)
      {
        _tx_idle_kick_cnt = _tx_idle_kick_cnt + 1U;
      }
    }
    else
    {
      _tx_idle_pending_seen  = true;
      _tx_idle_seen_activity = _tx_last_activity_tick;
      _tx_idle_kick_uart_err = _huart->ErrorCode;
      _tx_idle_kick_dma_err  = (_huart->hdmatx != nullptr) ? _huart->hdmatx->ErrorCode : 0U;
    }
  }
  else
  {
    _tx_idle_pending_seen = false;

    // 卡死按“有没有在推进”判断：剩余计数变了，或者 UART 还在往外移位（TC 未置位），就刷新时刻。
    // 只看计数不够：一段末尾计数已是 0、UART / DMA FIFO 里的字节还在发，相邻两次巡检都落在段尾会误判（实测）。
    // 真卡死时两条都不成立：DMAT 被清 → 计数不动、移位完后 TC 置位；完成中断丢失 → 计数 0 且 TC 已置位。
    const size_t remaining = Transport::tx_remaining(_huart);
#if ICBSP_SERIES_F4
    // F4：状态寄存器是 SR，TC 位宏是 USART_SR_TC
    const bool   shifting  = (_huart->Instance->SR & USART_SR_TC) == 0U;
#else
    const bool   shifting  = (_huart->Instance->ISR & USART_ISR_TC) == 0U;
#endif
    if (remaining != _tx_last_remaining || shifting)
    {
      _tx_last_remaining     = remaining;
      _tx_last_activity_tick = xTaskGetTickCount();
    }
    else if ((xTaskGetTickCount() - _tx_last_activity_tick) >= pdMS_TO_TICKS(stall_ms))
    {
      // ② 通道忙但长时间没推进：中止（Transport 负责“先停再读剩余计数”），只续发没发出去的部分
      const size_t left = Transport::abort_tx(_huart);
      if (Transport::repair_tx(_huart))
      {
        _tx_dma_repair_cnt = _tx_dma_repair_cnt + 1U;
      }
      if (_tx_chunk_active)
      {
        settle_chunk(left);
      }
      restarted = start_chunk(false, nullptr);
    }
  }
  (void)xSemaphoreGive(_tx_lock);

  if (restarted)
  {
    _tx_stall_rec_cnt = _tx_stall_rec_cnt + 1U;
  }
  return restarted;
}

template <typename Transport, size_t RX_SIZE, size_t TX_SIZE>
bool BspUartT<Transport, RX_SIZE, TX_SIZE>::rx_recover()
{
  if (_huart == nullptr || _rx_stream_buffer == nullptr || !rx_enabled() || !_rx_should_run)
  {
    return false;
  }
  if (get_rx_health() == RxHealth::OK)
  {
    return false;
  }

  BaseType_t woken = pdFALSE;
  bool       armed = false;
  {
    uart_detail::IrqGuard guard(_huart);
    if (Transport::CIRCULAR_RX)
    {
      rx_drain(&woken); // 出错前已经收到的字节照样投递
    }
    abort_reception();
    if (Transport::repair_rx(_huart))
    {
      _rx_dma_repair_cnt = _rx_dma_repair_cnt + 1U; // 句柄卡在 ERROR：HAL 自己恢复不了，这里复位
    }
    armed = arm_reception();
  }
  if (woken != pdFALSE)
  {
    taskYIELD();
  }
  if (!armed)
  {
    return false; // 仍失败，下个周期再试（_rx_should_run 保持为 true）
  }
  _rx_error_cnt = _rx_error_cnt + 1U;
  return true;
}

template <typename Transport, size_t RX_SIZE, size_t TX_SIZE>
void BspUartT<Transport, RX_SIZE, TX_SIZE>::on_idle_isr(uint16_t size, BaseType_t *pxHigherPriorityTaskWoken)
{
  if (_huart == nullptr || !rx_enabled() || _rx_stream_buffer == nullptr)
  {
    return;
  }

  if (Transport::CIRCULAR_RX)
  {
    // 循环模式：HT / TC / IDLE 都只是“有新数据了”的通知，DMA 不停；位置以剩余计数为准，
    // 事件处理顺序（例如 IDLE 先于 HT 被响应）不影响结果
    rx_drain(pxHigherPriorityTaskWoken);
    return;
  }

  // 中断接收：HAL 已结束这次接收（RxState 回到 READY），投递后立即重新挂接收
  if (_huart->RxEventType == HAL_UART_RXEVENT_HT)
  {
    return;
  }
  if (size > RX_SIZE)
  {
    size = static_cast<uint16_t>(RX_SIZE);
  }
  rx_push(0U, size, pxHigherPriorityTaskWoken);
  (void)arm_reception(); // 失败不在中断里重试，rx_recover() 会在 10 ms 内重建
}

template <typename Transport, size_t RX_SIZE, size_t TX_SIZE>
void BspUartT<Transport, RX_SIZE, TX_SIZE>::start_transmission_from_isr(BaseType_t *pxHigherPriorityTaskWoken)
{
  if (_huart == nullptr || _tx_stream_buffer == nullptr || !tx_enabled())
  {
    return;
  }

  // 正常情况下这里通道必然空闲；不空闲说明被别的传输占了，交给 tx_recover()
  if (_huart->gState != HAL_UART_STATE_READY)
  {
    _tx_isr_skip_cnt = _tx_isr_skip_cnt + 1U;
    return;
  }

  // 本回调 = 在传的那一段已全部发完
  if (_tx_chunk_active)
  {
    _tx_chunk_len    = 0U;
    _tx_chunk_active = false;
  }
  (void)start_chunk(true, pxHigherPriorityTaskWoken);
}

template <typename Transport, size_t RX_SIZE, size_t TX_SIZE>
void BspUartT<Transport, RX_SIZE, TX_SIZE>::on_error_isr()
{
  if (_huart == nullptr)
  {
    return;
  }
  // HAL 在 DMA 出错时会停掉对应通道，gState 离开 READY → 记一次 TX 侧故障
  if (_huart->gState != HAL_UART_STATE_READY)
  {
    _tx_error_cnt = _tx_error_cnt + 1U;
  }
  // 接收侧线路错误：ORE 说明搬运不及时，FE / NE / PE 说明波特率、共地或线材有问题
  constexpr uint32_t RX_HW_ERROR_MASK = HAL_UART_ERROR_PE | HAL_UART_ERROR_NE | HAL_UART_ERROR_FE | HAL_UART_ERROR_ORE | HAL_UART_ERROR_RTO;
  const uint32_t ec = _huart->ErrorCode;
  if ((ec & RX_HW_ERROR_MASK) != 0U)
  {
    _rx_hw_error_cnt = _rx_hw_error_cnt + 1U;
  }
  // 分别计数（F7）：DMA 模式下 FE / NE / PE 不会中止接收，坏字节照常投进流缓冲区，只能靠这几个计数发现
  if ((ec & HAL_UART_ERROR_PE) != 0U)
  {
    _rx_pe_cnt = _rx_pe_cnt + 1U;
  }
  if ((ec & HAL_UART_ERROR_FE) != 0U)
  {
    _rx_fe_cnt = _rx_fe_cnt + 1U;
  }
  if ((ec & HAL_UART_ERROR_NE) != 0U)
  {
    _rx_ne_cnt = _rx_ne_cnt + 1U;
  }
  if ((ec & HAL_UART_ERROR_ORE) != 0U)
  {
    _rx_ore_cnt = _rx_ore_cnt + 1U;
  }
}

template <typename Transport, size_t RX_SIZE, size_t TX_SIZE>
size_t BspUartT<Transport, RX_SIZE, TX_SIZE>::get_tx_free_space()
{
  return (_tx_stream_buffer != nullptr) ? xStreamBufferSpacesAvailable(_tx_stream_buffer) : 0U;
}

template <typename Transport, size_t RX_SIZE, size_t TX_SIZE>
size_t BspUartT<Transport, RX_SIZE, TX_SIZE>::get_rx_available_data()
{
  return (_rx_stream_buffer != nullptr) ? xStreamBufferBytesAvailable(_rx_stream_buffer) : 0U;
}

template <typename Transport, size_t RX_SIZE, size_t TX_SIZE>
bool BspUartT<Transport, RX_SIZE, TX_SIZE>::check_format(uint32_t baudrate, uint32_t word_length, uint32_t parity, uint32_t stop_bits) const
{
  if (_huart == nullptr)
  {
    return false;
  }
  return (_huart->Init.BaudRate == baudrate) && (_huart->Init.WordLength == word_length) && (_huart->Init.Parity == parity) && (_huart->Init.StopBits == stop_bits);
}

template <typename Transport, size_t RX_SIZE, size_t TX_SIZE>
typename BspUartT<Transport, RX_SIZE, TX_SIZE>::RxHealth BspUartT<Transport, RX_SIZE, TX_SIZE>::get_rx_health() const
{
  if (_huart == nullptr || _huart->Instance == nullptr)
  {
    return RxHealth::STOPPED;
  }
  if (Transport::rx_ok(_huart))
  {
    return RxHealth::OK;
  }
  if (Transport::rx_stopped(_huart))
  {
    return RxHealth::STOPPED;
  }
  return RxHealth::INCONSISTENT;
}

template <typename Transport, size_t RX_SIZE, size_t TX_SIZE>
typename BspUartT<Transport, RX_SIZE, TX_SIZE>::RxDiag BspUartT<Transport, RX_SIZE, TX_SIZE>::get_rx_diag() const
{
  RxDiag d;
  d.arm_fail_cnt   = _rx_arm_fail_cnt;
  d.drop_bytes     = _rx_drop_bytes;
  d.hw_error_cnt   = _rx_hw_error_cnt;
  d.pe_cnt         = _rx_pe_cnt;
  d.fe_cnt         = _rx_fe_cnt;
  d.ne_cnt         = _rx_ne_cnt;
  d.ore_cnt        = _rx_ore_cnt;
  d.error_cnt      = _rx_error_cnt;
  d.dma_repair_cnt = _rx_dma_repair_cnt;
  return d;
}

template <typename Transport, size_t RX_SIZE, size_t TX_SIZE>
typename BspUartT<Transport, RX_SIZE, TX_SIZE>::TxDiag BspUartT<Transport, RX_SIZE, TX_SIZE>::get_tx_diag() const
{
  TxDiag d;
  d.drop_bytes        = _tx_drop_bytes;
  d.start_fail_cnt    = _tx_start_fail_cnt;
  d.isr_skip_cnt      = _tx_isr_skip_cnt;
  d.stall_recover_cnt = _tx_stall_rec_cnt;
  d.idle_kick_cnt      = _tx_idle_kick_cnt;
  d.idle_kick_uart_err = _tx_idle_kick_uart_err;
  d.idle_kick_dma_err  = _tx_idle_kick_dma_err;
  d.error_cnt         = _tx_error_cnt;
  d.dma_repair_cnt    = _tx_dma_repair_cnt;
  return d;
}

template <typename Transport, size_t RX_SIZE, size_t TX_SIZE>
bool BspUartT<Transport, RX_SIZE, TX_SIZE>::arm_reception()
{
  if (!rx_enabled())
  {
    return false;
  }

  // 先清掉残留的 UART 错误标志：否则 HAL 会因为“启动前已有挂起的 ORE / FE / NE”拒绝启动
#if ICBSP_SERIES_F4
  // F4 没有 ICR / UART_CLEAR_xxx：错误标志靠「先读 SR 再读 DR」清除
  __HAL_UART_CLEAR_PEFLAG(_huart);
  __HAL_UART_CLEAR_FEFLAG(_huart);
  __HAL_UART_CLEAR_NEFLAG(_huart);
  __HAL_UART_CLEAR_OREFLAG(_huart);
#else
  __HAL_UART_CLEAR_FLAG(_huart, UART_CLEAR_PEF | UART_CLEAR_FEF | UART_CLEAR_NEF | UART_CLEAR_OREF);
#endif

  if (Transport::CIRCULAR_RX)
  {
    _rx_read_pos = 0U; // DMA 从缓冲区开头写起；调用方保证此时不会有本实例的接收中断
  }
  if (!Transport::arm_rx(_huart, _rx_buffer, static_cast<uint16_t>(RX_SIZE)))
  {
    _rx_arm_fail_cnt = _rx_arm_fail_cnt + 1U;
    return false;
  }
  _rx_should_run = true;
  return true;
}

template <typename Transport, size_t RX_SIZE, size_t TX_SIZE>
void BspUartT<Transport, RX_SIZE, TX_SIZE>::abort_reception()
{
  if (!rx_enabled())
  {
    return;
  }
  Transport::abort_rx(_huart);
  ATOMIC_CLEAR_BIT(_huart->Instance->CR1, USART_CR1_IDLEIE); // 防止残留 IDLE 中断源
}

template <typename Transport, size_t RX_SIZE, size_t TX_SIZE>
void BspUartT<Transport, RX_SIZE, TX_SIZE>::stop_channels()
{
  if (rx_enabled())
  {
    abort_reception();
  }
  if (tx_enabled())
  {
    (void)Transport::abort_tx(_huart);
  }
  Transport::force_stop(_huart, rx_enabled(), tx_enabled());
  if (rx_enabled())
  {
    uart_detail::force_rx_ready(_huart);
  }
  if (tx_enabled())
  {
    _huart->gState = HAL_UART_STATE_READY;
  }
  _huart->ErrorCode = HAL_UART_ERROR_NONE;
}

template <typename Transport, size_t RX_SIZE, size_t TX_SIZE>
void BspUartT<Transport, RX_SIZE, TX_SIZE>::rx_drain(BaseType_t *pxHigherPriorityTaskWoken)
{
  // 同一实例的 IDLE（串口中断）与 HT / TC（DMA 流中断）可能分属不同中断号，任务侧恢复也会调用：整体放进临界区
  const UBaseType_t saved = taskENTER_CRITICAL_FROM_ISR();
  const size_t      pos   = Transport::rx_write_pos(_huart, RX_SIZE);
  const size_t      rd    = _rx_read_pos;
  if (pos != rd)
  {
    if (pos > rd)
    {
      rx_push(rd, pos - rd, pxHigherPriorityTaskWoken);
    }
    else
    {
      // 回绕：先投到缓冲区末尾，再从开头投到当前位置。
      // 两次事件之间最多相隔半个缓冲区（HT / TC 必然触发），所以 pos == rd 不会是“整整转了一圈”
      rx_push(rd, RX_SIZE - rd, pxHigherPriorityTaskWoken);
      rx_push(0U, pos, pxHigherPriorityTaskWoken);
    }
    _rx_read_pos = pos;
  }
  taskEXIT_CRITICAL_FROM_ISR(saved);
}

template <typename Transport, size_t RX_SIZE, size_t TX_SIZE>
void BspUartT<Transport, RX_SIZE, TX_SIZE>::rx_push(size_t off, size_t len, BaseType_t *pxHigherPriorityTaskWoken)
{
  if (len == 0U)
  {
    return;
  }
  const size_t pushed = xStreamBufferSendFromISR(_rx_stream_buffer, _rx_buffer + off, len, pxHigherPriorityTaskWoken);
  if (pushed < len)
  {
    _rx_drop_bytes = _rx_drop_bytes + static_cast<uint32_t>(len - pushed);
  }
}

template <typename Transport, size_t RX_SIZE, size_t TX_SIZE>
bool BspUartT<Transport, RX_SIZE, TX_SIZE>::start_transmission(TickType_t wait)
{
  if (_huart == nullptr || _tx_stream_buffer == nullptr || _tx_lock == nullptr)
  {
    return false;
  }
  // 「判忙 → 取包 → 启动」整体互斥：否则两个任务可能同时改写 _tx_buffer
  if (xSemaphoreTake(_tx_lock, wait) != pdTRUE)
  {
    return false;
  }
  bool started = false;
  // 必须严格用 READY 判定：ERROR / TIMEOUT 状态下 HAL 会拒绝启动
  if (_huart->gState == HAL_UART_STATE_READY)
  {
    started = start_chunk(false, nullptr);
  }
  (void)xSemaphoreGive(_tx_lock);
  return started;
}

template <typename Transport, size_t RX_SIZE, size_t TX_SIZE>
bool BspUartT<Transport, RX_SIZE, TX_SIZE>::start_chunk(bool from_isr, BaseType_t *pxHigherPriorityTaskWoken)
{
  if (_tx_chunk_active)
  {
    // 任务侧看到通道空闲、这一段却还标着“在传”：被 HAL 停掉了（出错）或完成中断丢了，只续发没发出去的部分
    settle_chunk(Transport::tx_remaining(_huart));
  }

  if (_tx_chunk_len == 0U)
  {
    const size_t n = from_isr ? xStreamBufferReceiveFromISR(_tx_stream_buffer, _tx_buffer, TX_SIZE, pxHigherPriorityTaskWoken)
                              : xStreamBufferReceive(_tx_stream_buffer, _tx_buffer, TX_SIZE, 0);
    if (n == 0U)
    {
      return false;
    }
    _tx_chunk_off = 0U;
    _tx_chunk_len = n;
  }

  // 先标记“在传”再启动（评审 F2）：短段在高波特率下可能在 HAL 返回之前就发完，
  // 完成中断看到 active 才会把这一段结掉；否则中断会把同一段再发一遍。
  _tx_chunk_active       = true;
  _tx_last_activity_tick = from_isr ? xTaskGetTickCountFromISR() : xTaskGetTickCount();
  if (!Transport::start_tx(_huart, _tx_buffer + _tx_chunk_off, static_cast<uint16_t>(_tx_chunk_len)))
  {
    // 启动失败：没有中断会来，数据留在暂存区等下次先发（不写回流缓冲区，字节顺序不变）
    _tx_chunk_active   = false;
    _tx_start_fail_cnt = _tx_start_fail_cnt + 1U;
    return false;
  }
  // 启动成功后不再改任何发送状态：完成中断可能已经结掉这一段并启动了下一段
  return true;
}

template <typename Transport, size_t RX_SIZE, size_t TX_SIZE>
void BspUartT<Transport, RX_SIZE, TX_SIZE>::settle_chunk(size_t remaining)
{
  const size_t len  = _tx_chunk_len;
  const size_t sent = (remaining <= len) ? (len - remaining) : 0U;
  _tx_chunk_off     = _tx_chunk_off + sent;
  _tx_chunk_len     = len - sent;
  _tx_chunk_active  = false;
}

template <typename Transport, size_t RX_SIZE, size_t TX_SIZE>
void BspUartT<Transport, RX_SIZE, TX_SIZE>::cleanup_resources()
{
  if (_rx_stream_buffer != nullptr)
  {
    vStreamBufferDelete(_rx_stream_buffer);
    _rx_stream_buffer = nullptr;
  }
  if (_tx_stream_buffer != nullptr)
  {
    vStreamBufferDelete(_tx_stream_buffer);
    _tx_stream_buffer = nullptr;
  }
  if (_tx_write_lock != nullptr)
  {
    vSemaphoreDelete(_tx_write_lock);
    _tx_write_lock = nullptr;
  }
  if (_tx_lock != nullptr)
  {
    vSemaphoreDelete(_tx_lock);
    _tx_lock = nullptr;
  }
  if (_printf_lock != nullptr)
  {
    vSemaphoreDelete(_printf_lock);
    _printf_lock = nullptr;
  }
}
/* ==================== */

#endif // HAL_UART_MODULE_ENABLED

#endif // __BSP_UART_HPP__
