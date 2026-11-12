#include "bsp/bsp_series.hpp" // IWYU pragma: keep（HAL 模块开关 / 系列桥接）

// 宿主工程没在 CubeMX 里开 SPI（HAL_SPI_MODULE_ENABLED 未定义）时，本文件不参与编译
#ifdef HAL_SPI_MODULE_ENABLED

#  include "board_instances.hpp"
#  include "bsp/bsp_spi.hpp"

#  include <string.h>

#  include "task.h" // IWYU pragma: keep（xTaskGetSchedulerState）


/* ==================== 内部工具 ==================== */

namespace
{
/**
 * @brief 毫秒 → tick；portMAX_DELAY 原样保留（表示一直等）
 *
 * @note pdMS_TO_TICKS(portMAX_DELAY) 会按 32 位溢出，不再是“一直等”（与 bsp_uart.hpp 的 uart_detail::ms_to_ticks() 相同，N9）。
 */
TickType_t ms_to_ticks(uint32_t ms)
{
  return (ms == portMAX_DELAY) ? portMAX_DELAY : pdMS_TO_TICKS(ms);
}

/** @brief 调度器是否已经启动（启动前不取锁、不等信号量，一律轮询） */
bool scheduler_running()
{
  return xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED;
}

/** @brief HAL 单次传输长度上限（Size 是 uint16_t） */
constexpr size_t HAL_MAX_SEGMENT = 0xFFFFU;
} // namespace

/* ==================== */


/* ==================== HAL 回调（库独占，宿主工程不能再定义） ==================== */

extern "C"
{
  /**
   * @brief DMA 全双工传输完成（SPI EOT 中断里由 HAL 调用）
   * @note 只处理 ICBSP_SPI_INSTANCES 清单里的实例，清单外的 SPI 直接忽略。
   */
  void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
  {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (false)
    {
    }

#  define X(name)                                       \
    else if (hspi == name.get_handle())                     \
    {                                                   \
      name.on_txrx_cplt_isr(&xHigherPriorityTaskWoken); \
    }
    ICBSP_SPI_INSTANCES(X)
#  undef X
    else
    {
      icbsp_unhandled_spi_txrx_cplt(hspi);
    }

    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
  }

  /** @brief 传输错误（模式错误、溢出、DMA 错误等）：交给对应实例，等待方返回 IO_ERROR */
  void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi)
  {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (false)
    {
    }

#  define X(name)                                   \
    else if (hspi == name.get_handle())                 \
    {                                               \
      name.on_error_isr(&xHigherPriorityTaskWoken); \
    }
    ICBSP_SPI_INSTANCES(X)
#  undef X
    else
    {
      icbsp_unhandled_spi_error(hspi);
    }

    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
  }

  // 清单外 SPI 的回调转调：默认空，宿主定义同名强函数即可（对比评估 N30）
  __attribute__((weak)) void icbsp_unhandled_spi_txrx_cplt(SPI_HandleTypeDef *hspi)
  {
    (void)hspi;
  }
  __attribute__((weak)) void icbsp_unhandled_spi_error(SPI_HandleTypeDef *hspi)
  {
    (void)hspi;
  }
}

/* ==================== 实例清单核对 ==================== */

bool bsp_spi_is_listed(const void *self, const SPI_HandleTypeDef *hspi)
{
  // 实例清单为空时 X-macro 展开成空，参数用不到
  (void)self;
  (void)hspi;
  bool     listed = false;
  uint32_t same   = 0U; // 清单里绑这个句柄的实例数
#  define X(name)                                 \
    if (static_cast<const void *>(&name) == self) \
    {                                             \
      listed = true;                              \
    }                                             \
    if (name.get_handle() == hspi)                    \
    {                                             \
      same = same + 1U;                           \
    }
  ICBSP_SPI_INSTANCES(X)
#  undef X
  return listed && same == 1U;
}


/* ==================== */

/* ==================== 总线：构造与初始化 ==================== */

void BspSpi::deinit()
{
  _init = false;
  if (_hspi != nullptr && HAL_SPI_GetState(_hspi) != HAL_SPI_STATE_READY)
  {
    (void)HAL_SPI_Abort(_hspi); // 进行中的 DMA 段停下来，之后才能删完成信号量
  }
  if (_lock != nullptr)
  {
    vSemaphoreDelete(_lock);
    _lock = nullptr;
  }
  if (_done != nullptr)
  {
    vSemaphoreDelete(_done);
    _done = nullptr;
  }
}

Status BspSpi::init()
{
  // HAL 回调只认清单里的实例：漏写清单、两个实例绑同一句柄都在这里拒绝（N13）
  if (_hspi == nullptr || !bsp_spi_is_listed(this, _hspi))
  {
    return Status::BAD_ARG;
  }
  if (_use_dma)
  {
    // 没链接 DMA 时 HAL_SPI_TransmitReceive_DMA() 直接返回错误，提前拒绝，免得运行时每次大传输都失败
    if (_hspi->hdmarx == nullptr || _hspi->hdmatx == nullptr)
    {
      return Status::BAD_ARG;
    }
    // 弹跳缓冲是对象成员，DMA 直接读写：实例必须放进 .dma_buffer
    if (!icbsp_dma_reachable(_tx_bounce, _hspi->hdmatx) || !icbsp_dma_reachable(_rx_bounce, _hspi->hdmarx))
    {
      return Status::BAD_ARG;
    }
  }

  // 可重复调用：先停掉旧的（中止传输、释放资源），再重建
  deinit();
  _xfer_error       = false;
  _poll_cnt         = 0U;
  _dma_cnt          = 0U;
  _bounce_cnt       = 0U;
  _timeout_cnt      = 0U;
  _error_cnt        = 0U;
  _lock_timeout_cnt = 0U;

  _lock = xSemaphoreCreateMutex(); // 互斥量自带优先级继承
  _done = xSemaphoreCreateBinary();
  if (_lock == nullptr || _done == nullptr)
  {
    deinit();
    return Status::IO_ERROR;
  }
  _init = true;
  return Status::OK;
}

bool BspSpi::is_initialized() const
{
  return _init;
}

/* ==================== */


/* ==================== 总线：事务接口 ==================== */

Status BspSpi::lock(uint32_t timeout_ms)
{
  if (!_init)
  {
    return Status::NOT_INIT;
  }
  if (!scheduler_running())
  {
    return Status::OK; // 调度器启动前只有一个执行流
  }
  if (xSemaphoreTake(_lock, ms_to_ticks(timeout_ms)) != pdTRUE)
  {
    _lock_timeout_cnt = _lock_timeout_cnt + 1U;
    return Status::TIMEOUT;
  }
  return Status::OK;
}

void BspSpi::unlock()
{
  if (_init && scheduler_running())
  {
    xSemaphoreGive(_lock);
  }
}

/**
 * @note 片选由调用方全程保持，这里拆成的多段对器件来说是一整帧
 *       （CubeMX 里 MasterKeepIOState 必须为 ENABLE，否则段与段之间 SPE 关闭时 SCK 浮空，会出假时钟沿）。
 */
Status BspSpi::transfer_locked(const uint8_t *tx, uint8_t *rx, size_t len, uint32_t timeout_ms)
{
  if (len == 0U)
  {
    return Status::BAD_ARG;
  }
  if (!_init)
  {
    return Status::NOT_INIT;
  }

  const bool use_dma = _use_dma && len >= _dma_threshold && scheduler_running();

  // 每个方向单独决定：调用方缓冲能直接用（轮询总能用，DMA 要在 DMA 可达内存里）就直接用，
  // 否则走弹跳缓冲（tx 为空 = 弹跳缓冲填 0xFF，rx 为空 = 收进弹跳缓冲丢掉）
  const bool tx_direct = tx != nullptr && (!use_dma || icbsp_dma_reachable(tx, _hspi->hdmatx));
  const bool rx_direct = rx != nullptr && (!use_dma || icbsp_dma_reachable(rx, _hspi->hdmarx));
  // 调用方缓冲 DMA 访问不到、必须中转（只计这种，填充 / 丢弃不算）
  const bool bounced = use_dma && ((tx != nullptr && !tx_direct) || (rx != nullptr && !rx_direct));
  // 任一方向用弹跳缓冲，每段就不能超过它
  const size_t max_seg = (tx_direct && rx_direct) ? HAL_MAX_SEGMENT : ICBSP_SPI_DMA_BUFFER_SIZE;

  size_t done = 0U;
  while (done < len)
  {
    const size_t   seg    = (len - done > max_seg) ? max_seg : (len - done);
    const uint8_t *seg_tx = _tx_bounce;
    uint8_t       *seg_rx = rx_direct ? rx + done : _rx_bounce;
    if (tx_direct)
    {
      seg_tx = tx + done;
    }
    else if (tx != nullptr)
    {
      memcpy(_tx_bounce, tx + done, seg);
    }
    else
    {
      memset(_tx_bounce, 0xFF, seg);
    }

    if (bounced)
    {
      _bounce_cnt = _bounce_cnt + 1U;
    }
    const Status st = use_dma ? transfer_dma(seg_tx, seg_rx, seg, timeout_ms) : transfer_poll(seg_tx, seg_rx, seg, timeout_ms);
    if (st != Status::OK)
    {
      return st;
    }
    if (!rx_direct && rx != nullptr)
    {
      memcpy(rx + done, _rx_bounce, seg);
    }
    done += seg;
  }
  return Status::OK;
}

Status BspSpi::transfer_poll(const uint8_t *tx, uint8_t *rx, size_t len, uint32_t timeout_ms)
{
  const HAL_StatusTypeDef hs = HAL_SPI_TransmitReceive(_hspi, tx, rx, static_cast<uint16_t>(len), timeout_ms);
  if (hs == HAL_OK)
  {
    _poll_cnt = _poll_cnt + 1U;
    return Status::OK;
  }
  if (hs == HAL_TIMEOUT)
  {
    // HAL 超时返回时已关掉 SPE、句柄回到 READY，下一次可以直接用
    _timeout_cnt = _timeout_cnt + 1U;
    return Status::TIMEOUT;
  }
  _error_cnt = _error_cnt + 1U;
  return Status::IO_ERROR;
}

Status BspSpi::transfer_dma(const uint8_t *tx, uint8_t *rx, size_t len, uint32_t timeout_ms)
{
  // 清掉上一次可能残留的完成信号（例如超时后才到的完成中断）
  (void)xSemaphoreTake(_done, 0U);
  _xfer_error = false;

  if (HAL_SPI_TransmitReceive_DMA(_hspi, tx, rx, static_cast<uint16_t>(len)) != HAL_OK)
  {
    _error_cnt = _error_cnt + 1U;
    return Status::IO_ERROR;
  }

  if (xSemaphoreTake(_done, ms_to_ticks(timeout_ms)) != pdTRUE)
  {
    // 停掉 DMA 与 SPI，句柄回到 READY；中止过程中若有完成 / 错误回调进来，信号量在下一次开始前清掉
    (void)HAL_SPI_Abort(_hspi);
    _timeout_cnt = _timeout_cnt + 1U;
    return Status::TIMEOUT;
  }

  if (_xfer_error)
  {
    return Status::IO_ERROR; // error_cnt 已在错误回调里累加
  }
  _dma_cnt = _dma_cnt + 1U;
  return Status::OK;
}

/* ==================== */


/* ==================== 总线：ISR 入口与查询 ==================== */

void BspSpi::on_txrx_cplt_isr(BaseType_t *pxHigherPriorityTaskWoken)
{
  if (_done != nullptr)
  {
    xSemaphoreGiveFromISR(_done, pxHigherPriorityTaskWoken);
  }
}

void BspSpi::on_error_isr(BaseType_t *pxHigherPriorityTaskWoken)
{
  _xfer_error = true;
  _error_cnt  = _error_cnt + 1U;
  if (_done != nullptr)
  {
    xSemaphoreGiveFromISR(_done, pxHigherPriorityTaskWoken);
  }
}

BspSpi::Diag BspSpi::get_diag() const
{
  Diag d;
  d.poll_cnt         = _poll_cnt;
  d.dma_cnt          = _dma_cnt;
  d.bounce_cnt       = _bounce_cnt;
  d.timeout_cnt      = _timeout_cnt;
  d.error_cnt        = _error_cnt;
  d.lock_timeout_cnt = _lock_timeout_cnt;
  return d;
}

/* ==================== */


/* ==================== 片选设备 ==================== */

BspSpiDevice::BspSpiDevice(BspSpi &bus, const Config &cfg) : _bus(bus), _cs(BspGpio::Config(cfg.cs_port, cfg.cs_pin)), _read_flag(cfg.read_flag), _read_dummy(cfg.read_dummy)
{
  // 构造函数只做赋值
}

Status BspSpiDevice::init()
{
  if (!_bus.is_initialized())
  {
    return Status::NOT_INIT;
  }
  if (_cs.get_port() == nullptr || _cs.get_pin() == 0U)
  {
    return Status::BAD_ARG;
  }
  deselect();
  _init = true;
  return Status::OK;
}

Status BspSpiDevice::transaction(const uint8_t *head, size_t head_len, size_t skip, const uint8_t *tx, uint8_t *rx, size_t len, uint32_t timeout_ms)
{
  if (!_init)
  {
    return Status::NOT_INIT;
  }

  Status st = _bus.lock(timeout_ms);
  if (st != Status::OK)
  {
    return st;
  }

  select();
  if (head_len > 0U)
  {
    st = _bus.transfer_locked(head, nullptr, head_len, timeout_ms);
  }
  if (st == Status::OK && skip > 0U)
  {
    st = _bus.transfer_locked(nullptr, nullptr, skip, timeout_ms);
  }
  if (st == Status::OK)
  {
    st = _bus.transfer_locked(tx, rx, len, timeout_ms);
  }
  deselect();

  _bus.unlock();
  return st;
}

Status BspSpiDevice::transfer(const uint8_t *tx, uint8_t *rx, size_t len, uint32_t timeout_ms)
{
  if (len == 0U)
  {
    return Status::BAD_ARG;
  }
  return transaction(nullptr, 0U, 0U, tx, rx, len, timeout_ms);
}

Status BspSpiDevice::write_then_read(const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_len, uint32_t timeout_ms)
{
  if (tx == nullptr || tx_len == 0U || rx == nullptr || rx_len == 0U)
  {
    return Status::BAD_ARG;
  }
  return transaction(tx, tx_len, 0U, nullptr, rx, rx_len, timeout_ms);
}

Status BspSpiDevice::read_reg(uint8_t reg, uint8_t *data, size_t len, uint32_t timeout_ms)
{
  if (data == nullptr || len == 0U)
  {
    return Status::BAD_ARG;
  }
  const uint8_t addr = static_cast<uint8_t>(reg | _read_flag);
  return transaction(&addr, 1U, _read_dummy, nullptr, data, len, timeout_ms);
}

Status BspSpiDevice::write_reg(uint8_t reg, const uint8_t *data, size_t len, uint32_t timeout_ms)
{
  if (data == nullptr || len == 0U)
  {
    return Status::BAD_ARG;
  }
  const uint8_t addr = static_cast<uint8_t>(reg & static_cast<uint8_t>(~_read_flag));
  return transaction(&addr, 1U, 0U, data, nullptr, len, timeout_ms);
}

Status BspSpiDevice::read_reg8(uint8_t reg, uint8_t &value, uint32_t timeout_ms)
{
  return read_reg(reg, &value, 1U, timeout_ms);
}

Status BspSpiDevice::write_reg8(uint8_t reg, uint8_t value, uint32_t timeout_ms)
{
  return write_reg(reg, &value, 1U, timeout_ms);
}

/* ==================== */

#endif // HAL_SPI_MODULE_ENABLED
