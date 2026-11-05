/**
 * @file bsp_fdcan.hpp
 * @author Rh
 * @brief 经典 CAN（FDCAN 外设的 CAN 2.0 模式）收发驱动：标准帧 / 扩展帧，DLC 0~8（H7）
 * @version 0.7
 * @date 2026-11-04
 *
 * @copyright Copyright (c) 2026
 *
 * @details 本层只做三件事：硬件收发、总线恢复、诊断计数。**只有头文件**（任意 `<RX_DEPTH, TX_DEPTH>` 都能用）；
 *          HAL 回调按 board_instances.hpp 的 ICBSP_CAN_INSTANCES 分发（bsp_can.cpp）。
 *
 * @note 发送：send() 非阻塞，帧进软件发送缓冲后立即尝试写进硬件 TX FIFO；硬件满时由 TX-FIFO-EMPTY 中断续发
 *       （该中断只在软件缓冲有帧时打开）。sys_task 每 10 ms 调 tx_recover() 兜底。
 *       “软件缓冲 → 硬件 FIFO”只发生在临界区（任务侧）或 FDCAN 中断里，发送缓冲只有一个写者、读者之间不会交错。
 *       send() 用任务级临界区，**不能在中断里调用**。
 * @note 接收：FIFO0 中断把帧（只拷 DLC 个字节，其余为 0）投进原始接收缓冲。
 *       - ICBSP_CAN_RX_ENABLE = 1：can_rx_task 每 1 ms 取走并分发给接收节点，没有节点认领的帧进回退缓冲，
 *         receive() 只读回退缓冲（深度 ICBSP_CAN_RX_RETURN_FRAMES，0 = 不建回退缓冲，没人认领的帧只计数）；
 *       - ICBSP_CAN_RX_ENABLE = 0：没有 can_rx_task，receive() 直接读原始接收缓冲。
 *       同一条总线的 receive() **只允许一个任务调用**。
 * @note 远程帧由硬件拒收；DLC > 8 的帧（FD 帧或经典帧 DLC 9~15）丢弃并计入 rx_len_drop。
 * @note Bus-Off：sys_task 每 10 ms 调 service_recovery()，按 ICBSP_CAN_BUSOFF_RETRY_MS 限流清 CCCR.INIT，
 *       进入 Bus-Off 时取消硬件里未发出的旧帧并清空软件发送缓冲（避免恢复瞬间发出过期指令）。
 */

#ifndef __BSP_FDCAN_HPP__
#define __BSP_FDCAN_HPP__

#include "bsp/bsp_series.hpp" // IWYU pragma: keep（HAL 模块开关 / 系列桥接）

// 宿主工程没在 CubeMX 里开 FDCAN（HAL_FDCAN_MODULE_ENABLED 未定义）时，本模块整体不参与编译
#ifdef HAL_FDCAN_MODULE_ENABLED

#include "FreeRTOS.h" // IWYU pragma: keep
#include "message_buffer.h"
#include "task.h" // IWYU pragma: keep

#include "service/system/config_check.hpp" // ICBSP_CAN_RX_ENABLE、ICBSP_CAN_RX_RETURN_FRAMES、ICBSP_CAN_BUSOFF_RETRY_MS
#include "service/system/status_code.hpp"

#include <stddef.h>
#include <stdint.h>
#include <string.h>



/* ==================== 帧结构 ==================== */

/** @brief 接收到的一帧：HAL 帧头 + 最多 8 字节数据（DLC 之后的字节为 0） */
typedef struct
{
  FDCAN_RxHeaderTypeDef header;
  uint8_t               data[8];
} CanRxMsg;

/** @brief 待发送的一帧 */
typedef struct
{
  uint32_t id;      ///< 11 位标准 ID 或 29 位扩展 ID
  uint32_t id_type; ///< FDCAN_STANDARD_ID / FDCAN_EXTENDED_ID
  uint8_t  dlc;     ///< 数据长度 0~8
  uint8_t  data[8]; ///< 数据
} CanTxMsg;

/** @brief 接收节点可接受的 DLC 位图：第 n 位 = 接受 DLC 为 n 的帧 */
constexpr uint16_t CAN_DLC_MASK_8   = 1U << 8;  ///< 只接受 8 字节帧（默认）
constexpr uint16_t CAN_DLC_MASK_ANY = 0x01FFU;  ///< 接受 0~8 任意长度

/**
 * @brief 实例是否在 board_instances.hpp 的 ICBSP_CAN_INSTANCES 里，且是清单里唯一绑这个句柄的实例（实现在 bsp_can.cpp）
 * @note HAL 回调与 can_rx / can_tx / sys_task 只认清单里的实例；init() 用它把“漏写清单 / 两个实例绑同一句柄”变成 BAD_ARG。
 */
bool bsp_can_is_listed(const void *self, const FDCAN_HandleTypeDef *hfdcan);

/**
 * @brief 清单外 FDCAN 的回调转调（弱函数，默认什么也不做；实现在 bsp_can.cpp）
 * @note 库占用了 HAL_FDCAN_RxFifo0Callback / HAL_FDCAN_TxFifoEmptyCallback / HAL_FDCAN_ErrorStatusCallback，宿主不能再定义它们。
 *       不在 ICBSP_CAN_INSTANCES 里的 FDCAN 的这三个回调转到下面三个函数，宿主按需定义同名强函数即可。
 */
extern "C"
{
  void icbsp_unhandled_fdcan_rx_fifo0(FDCAN_HandleTypeDef *hfdcan, uint32_t its);
  void icbsp_unhandled_fdcan_tx_fifo_empty(FDCAN_HandleTypeDef *hfdcan);
  void icbsp_unhandled_fdcan_error_status(FDCAN_HandleTypeDef *hfdcan, uint32_t its);
}

/** @brief CAN 驱动的内部工具 */
namespace can_detail
{
/** @brief 毫秒 → tick；portMAX_DELAY 原样保留（pdMS_TO_TICKS(portMAX_DELAY) 会溢出） */
inline TickType_t ms_to_ticks(uint32_t ms)
{
  return (ms == portMAX_DELAY) ? portMAX_DELAY : pdMS_TO_TICKS(ms);
}

/** @brief 经典 CAN 的 DLC 编码 → 字节数；H7 HAL 里 FDCAN_DLC_BYTES_0~8 就是 0~8，> 8 返回 0xFF（不支持） */
inline uint32_t dlc_to_len(uint32_t dlc_code)
{
  return (dlc_code <= FDCAN_DLC_BYTES_8) ? dlc_code : 0xFFU;
}
} // namespace can_detail


/* ==================== 驱动 ==================== */

/**
 * @brief CAN 驱动模板（纯硬件：收发 + 恢复 + 诊断）
 *
 * @tparam RX_DEPTH 原始接收缓冲深度（帧）：FIFO0 中断写入，can_rx_task（或 receive()）读出
 * @tparam TX_DEPTH 软件发送缓冲深度（帧）
 *
 * @note 用法（实例写在 board_cfg.hpp）：
 *
 *   inline BspCanT<16, 16> bsp_can1({.hfdcan = &hfdcan1, .name = "CAN1"});
 *   // board_instances.hpp 的 ICBSP_CAN_INSTANCES 里加 X(bsp_can1)；board_init() 里 ICBSP_CHECK(bsp_can1.init() == Status::OK);
 *
 *   uint8_t d[8] = {...};
 *   bsp_can1.send(0x200, d);                                   // 标准帧 8 字节
 *   bsp_can1.send(0x1ABCDE, d, 4U, FDCAN_EXTENDED_ID);         // 扩展帧 4 字节
 */
template <size_t RX_DEPTH, size_t TX_DEPTH>
class BspCanT
{
  static_assert(RX_DEPTH >= 1U && TX_DEPTH >= 1U, "CAN 收发缓冲深度至少 1 帧");

public:
  /* ==================== 配置 ==================== */

  /** @brief 配置（聚合体：`{.hfdcan = &hfdcanX, .name = "CANx"}`） */
  struct Config
  {
    FDCAN_HandleTypeDef *hfdcan = nullptr; ///< FDCAN 句柄（CubeMX 配成经典 CAN、RX FIFO0）
    const char          *name   = "CAN";   ///< 实例名称（调试用）
  };

  /** @brief 运行诊断计数：只增不减；除 bus_off_events / err_* 外，正常应恒为 0 */
  struct Diagnostics
  {
    // 总线错误
    volatile uint32_t bus_off_events     = 0;     ///< 进入 Bus-Off 的次数（service_recovery() 发现时计一次）
    volatile uint32_t err_passive        = 0;     ///< 进入错误被动的次数
    volatile uint32_t err_warning        = 0;     ///< 进入错误警告的次数
    volatile uint32_t recovery_attempts  = 0;     ///< service_recovery() 清 INIT 的次数
    volatile uint32_t recovery_successes = 0;     ///< 恢复成功次数
    volatile uint32_t recovery_max_ticks = 0;     ///< 单次恢复最长耗时（ticks）
    volatile bool     recovering         = false; ///< 当前是否在恢复流程中
    // 接收
    volatile uint32_t rx_dropped        = 0; ///< 原始接收缓冲满而丢弃的帧数
    volatile uint32_t rx_lost           = 0; ///< 硬件 RX FIFO0 溢出（RF0L）的次数
    volatile uint32_t rx_len_drop       = 0; ///< DLC > 8 而丢弃的帧数
    volatile uint32_t rx_unclaimed      = 0; ///< 没有接收节点认领的帧数（ICBSP_CAN_RX_ENABLE = 1 时由 can_rx_task 计）
    volatile uint32_t rx_return_dropped = 0; ///< 没人认领、回退缓冲又满而丢弃的帧数
    // 发送
    volatile uint32_t tx_dropped       = 0; ///< 写入硬件 TX FIFO 失败而丢弃的帧数
    volatile uint32_t tx_buf_full      = 0; ///< 软件发送缓冲满、send() 返回 FULL 的次数
    volatile uint32_t tx_stall_recover = 0; ///< 软件缓冲有帧、TX-FIFO-EMPTY 中断却关着，由 tx_recover() 补发的次数
    volatile uint32_t tx_it_fail       = 0; ///< 开关 TX-FIFO-EMPTY 中断时 HAL 拒绝的次数
    volatile uint32_t tx_flushed       = 0; ///< 进入 Bus-Off 时清掉的软件发送缓冲帧数
  };

  /* ==================== */
  /* ==================== 公有接口 ==================== */

  /** @brief 只绑定句柄（constexpr：board_cfg.hpp 的 inline 全局实例要常量初始化） */
  constexpr BspCanT(const Config &cfg) : _hfdcan(cfg.hfdcan), _name(cfg.name)
  {
  }

  /** @brief 平凡析构（全局实例永不析构）；需要释放资源时显式调 deinit() */
  ~BspCanT() = default;

  BspCanT(const BspCanT &)            = delete;
  BspCanT &operator=(const BspCanT &) = delete;

  /**
   * @brief 停外设、关通知、清软件缓冲，再按 CubeMX 配置重新启动（可重复调用，失败回滚）
   * @return OK；BAD_ARG=句柄为空、实例不在 ICBSP_CAN_INSTANCES 里或清单里另有实例绑了同一句柄；
   *         IO_ERROR=资源创建或硬件启动失败
   * @note 初始化阶段调用（system_init() 之后、system_start() 之前）；运行中重新调用时不能有任务阻塞在本实例的 receive() 上。
   */
  Status init();

  /** @brief 停外设、关通知并释放软件资源 */
  void deinit();

  /** @brief 构造时是否给了句柄（不代表 init() 成功） */
  bool is_ready() const
  {
    return _hfdcan != nullptr;
  }

  /** @brief 底层句柄（供 HAL 回调分发） */
  FDCAN_HandleTypeDef *get_handle() const
  {
    return _hfdcan;
  }

  /**
   * @brief 发送一帧（非阻塞；只能在任务上下文或调度器启动前调用）
   * @param id      标准帧 0x000~0x7FF / 扩展帧 0~0x1FFFFFFF
   * @param data    数据；dlc 为 0 时可以为 nullptr
   * @param dlc     数据长度 0~8
   * @param id_type FDCAN_STANDARD_ID / FDCAN_EXTENDED_ID
   * @return OK=已进发送缓冲；FULL=缓冲满（这一帧没发）；BUSY=总线不可用（Bus-Off / 恢复中 / 外设没启动）；
   *         BAD_ARG=参数非法；NOT_INIT=未初始化
   */
  Status send(uint32_t id, const uint8_t *data, uint8_t dlc = 8U, uint32_t id_type = FDCAN_STANDARD_ID);

  /**
   * @brief 周期帧用的发送：同一 ID 的上一帧还在驱动里（软件缓冲 / 硬件缓冲）等着时，这一帧不入队
   * @return 同 send()；另外 BUSY 也表示“同 ID 的上一帧还没发出去”——调用方下个周期再发最新值即可
   * @note 发送节点（CanTxNodeT）用它：总线被更高优先级流量占满时，一个周期 ID 最多占一个位置，
   *       不会把软件缓冲和硬件 FIFO 塞满、挡住别的帧（10-17 C06c / C06d）。TX 队列模式下更高优先级的帧照样能先发；
   *       FIFO 模式按入队顺序发，排在前面的这一帧发不出去时后面的帧也只能等（FIFO 模式的固有限制）。
   */
  Status send_latest(uint32_t id, const uint8_t *data, uint8_t dlc = 8U, uint32_t id_type = FDCAN_STANDARD_ID);

  /**
   * @brief 取一帧：ICBSP_CAN_RX_ENABLE = 1 时取没人认领的帧（回退缓冲），= 0 时取所有收到的帧
   * @param timeout_ms 0=不等待，portMAX_DELAY=一直等
   * @return OK；TIMEOUT=没有数据；BAD_ARG；NOT_INIT；NOT_SUPPORTED=回退缓冲被关掉（ICBSP_CAN_RX_RETURN_FRAMES = 0）
   */
  Status receive(CanRxMsg *msg, uint32_t timeout_ms = portMAX_DELAY);

  /** @brief can_rx_task 专用：从原始接收缓冲取一帧 */
  Status read_raw(CanRxMsg *msg, uint32_t timeout_ms = 0U);

  /** @brief can_rx_task 专用：记一帧“没人认领”，有回退缓冲就放进去；返回是否放进去了 */
  bool return_frame(const CanRxMsg &msg);

  /**
   * @brief 发送兜底（sys_task 每 10 ms 调用）：软件缓冲有帧、硬件 FIFO 有空位时搬过去
   * @return true=本次搬了帧且 TX-FIFO-EMPTY 中断原本是关着的（丢过一次唤醒）
   */
  bool tx_recover();

  /**
   * @brief Bus-Off 恢复（sys_task 每 10 ms 调用，非阻塞）
   * @return OK=总线可用；BUSY=恢复中；NOT_INIT=未初始化
   */
  Status service_recovery();

  /* ==================== */
  /* ==================== ISR 入口（bsp_can.cpp 的 HAL 回调调用） ==================== */

  /** @brief RX FIFO0 事件：排空硬件 FIFO（RF0L 计数） */
  void process_fifo0_isr(uint32_t its, BaseType_t *pxHigherPriorityTaskWoken);

  /** @brief TX FIFO 变空：把软件缓冲里的帧填进所有空位 */
  void trigger_tx_from_isr(BaseType_t *pxHigherPriorityTaskWoken);

  /** @brief 错误状态变化（EP / EW）：只计“进入”的次数 */
  void process_error_isr(uint32_t its);

  /* ==================== */

  Diagnostics diagnostics; ///< 运行诊断（调试观察用，不要改写）

private:
  /* ==================== 成员 ==================== */

  /** @brief 回退缓冲是否存在（有 can_rx_task 且深度非 0） */
  static constexpr bool HAS_RETURN = (ICBSP_CAN_RX_ENABLE != 0) && (ICBSP_CAN_RX_RETURN_FRAMES > 0);

  FDCAN_HandleTypeDef  *_hfdcan;
  const char           *_name;
  MessageBufferHandle_t _tx_message_buffer  = nullptr; ///< 写者：send()（临界区）；读者：临界区里的任务侧或 FDCAN 中断
  MessageBufferHandle_t _rx_message_buffer  = nullptr; ///< 写者：FIFO0 中断；读者：can_rx_task（或 ICBSP_CAN_RX_ENABLE = 0 时的 receive()）
  MessageBufferHandle_t _rx_return_buffer   = nullptr; ///< 写者：can_rx_task；读者：receive()
  TickType_t            _recovery_started   = 0U;
  TickType_t            _recovery_attempted = 0U;

  // TX 队列模式（TXBC.TFQM = 1）下保证同 ID 不乱序：硬件按 ID 优先级发，同 ID 的多个缓冲按缓冲编号发而不是按入队顺序。
  // 所以记下每个硬件缓冲里的帧 ID；下一帧的 ID 已经有一帧在硬件里等着时，先把它暂存在 _tx_staged，等那一帧发完再写。
  uint32_t _tx_slot_key[32] = {};    ///< 硬件 TX 缓冲 i 里的帧：ID | 扩展帧标志（只在 TXBRP 第 i 位为 1 时有效）
  CanTxMsg _tx_staged       = {};    ///< 从软件缓冲取出、还没写进硬件的一帧
  bool     _tx_staged_valid = false; ///< _tx_staged 是否有效

  /** @brief 软件侧（发送缓冲 + 暂存位）每个 ID 有几帧在等：n = 0 的表项是空的。表满时多出来的 ID 不统计（send_latest() 退化为 send()） */
  struct PendCount
  {
    uint32_t key;
    uint16_t n;
  };
  static constexpr uint32_t PEND_SLOTS = 16U;
  PendCount                 _sw_pend[PEND_SLOTS] = {};

  /* ==================== 内部实现 ==================== */

  /** @brief 外设在跑、不在 Bus-Off、不在恢复中 */
  bool bus_usable() const
  {
    return _hfdcan->State == HAL_FDCAN_STATE_BUSY && !diagnostics.recovering && (_hfdcan->Instance->PSR & FDCAN_PSR_BO) == 0U &&
           (_hfdcan->Instance->CCCR & FDCAN_CCCR_INIT) == 0U;
  }

  /**
   * @brief 硬件 TX FIFO / 队列还有空位
   * @note 看 TXFQS.TFQF（满标志），FIFO 与 Queue 两种模式都有效；TFFL 在 Queue 模式下恒为 0（2026-10-17 实测一帧都发不出）。
   */
  bool hw_has_room() const
  {
    return (_hfdcan->Instance->TXFQS & FDCAN_TXFQS_TFQF) == 0U;
  }

  /**
   * @brief 把软件缓冲里的帧搬进硬件 FIFO 的所有空位，再按“软件缓冲是否还有帧”开关 TX-FIFO-EMPTY 中断
   * @param from_isr true = FDCAN 中断里调用；false = 调用方已在临界区里
   * @return 搬了几帧
   */
  uint32_t pump_tx(bool from_isr, BaseType_t *pxHigherPriorityTaskWoken);

  /** @brief send() / send_latest() 的共同实现 */
  Status send_impl(uint32_t id, const uint8_t *data, uint8_t dlc, uint32_t id_type, bool latest_only);

  /** @brief 帧在 _tx_slot_key 里的键 */
  static uint32_t tx_key(const CanTxMsg &msg)
  {
    return msg.id | ((msg.id_type == FDCAN_EXTENDED_ID) ? 0x80000000U : 0U);
  }

  /** @brief 队列模式下，硬件里是否已有同 ID 的帧在等（FIFO 模式恒为 false：硬件按入队顺序发，不会乱序） */
  bool tx_same_id_pending(uint32_t key) const
  {
    return (_hfdcan->Instance->TXBC & FDCAN_TXBC_TFQM) != 0U && hw_has_key(key);
  }

  /** @brief 软件侧这个 ID 的计数加 / 减 1（临界区或 FDCAN 中断里调用） */
  void sw_pend_adjust(uint32_t key, bool inc)
  {
    PendCount *free_slot = nullptr;
    for (uint32_t i = 0U; i < PEND_SLOTS; ++i)
    {
      if (_sw_pend[i].n != 0U && _sw_pend[i].key == key)
      {
        _sw_pend[i].n = static_cast<uint16_t>(inc ? _sw_pend[i].n + 1U : _sw_pend[i].n - 1U);
        return;
      }
      if (free_slot == nullptr && _sw_pend[i].n == 0U)
      {
        free_slot = &_sw_pend[i];
      }
    }
    if (inc && free_slot != nullptr)
    {
      free_slot->key = key;
      free_slot->n   = 1U;
    }
  }

  /** @brief 这个 ID 是否有帧在驱动里等（软件侧或硬件缓冲） */
  bool key_pending(uint32_t key) const
  {
    for (uint32_t i = 0U; i < PEND_SLOTS; ++i)
    {
      if (_sw_pend[i].n != 0U && _sw_pend[i].key == key)
      {
        return true;
      }
    }
    return hw_has_key(key);
  }

  /** @brief 硬件 TX 缓冲里是否有这个 ID 的帧在等发送 */
  bool hw_has_key(uint32_t key) const
  {
    uint32_t pend = _hfdcan->Instance->TXBRP;
    while (pend != 0U)
    {
      const uint32_t i = static_cast<uint32_t>(__builtin_ctz(pend));
      if (_tx_slot_key[i] == key)
      {
        return true;
      }
      pend &= pend - 1U;
    }
    return false;
  }

  /** @brief 软件侧还有没写进硬件的帧（发送缓冲或暂存位） */
  bool tx_has_pending() const
  {
    return _tx_staged_valid || xMessageBufferIsEmpty(_tx_message_buffer) == pdFALSE;
  }

  /** @brief 开关 TX-FIFO-EMPTY 中断（状态没变就不动寄存器） */
  void set_tx_empty_it(bool enable);

  /** @brief 清空软件发送缓冲（临界区里调用），返回清掉的帧数 */
  uint32_t flush_tx();

  /** @brief 停外设并摘除本驱动用过的全部通知（幂等） */
  void reset_hardware();

  /** @brief 滤波器 + 全局滤波（拒收远程帧）+ 启动 + 打开 RX / 错误状态通知 */
  Status configure_hardware();

  /** @brief 释放消息缓冲 */
  void cleanup_resources();

  /* ==================== */
};


/* ==================== 成员函数定义 ==================== */

template <size_t RX_DEPTH, size_t TX_DEPTH>
Status BspCanT<RX_DEPTH, TX_DEPTH>::init()
{
  if (_hfdcan == nullptr || !bsp_can_is_listed(this, _hfdcan))
  {
    return Status::BAD_ARG;
  }

  // 可重复调用：先停外设、关通知，再释放旧缓冲（运行中重新 init() 时 ISR 不会再碰旧指针）
  reset_hardware();
  cleanup_resources();
  diagnostics         = Diagnostics {};
  _recovery_started   = 0U;
  _recovery_attempted = 0U;
  _tx_staged_valid    = false;
  for (uint32_t i = 0U; i < PEND_SLOTS; ++i)
  {
    _sw_pend[i].n = 0U;
  }

  _rx_message_buffer = xMessageBufferCreate((sizeof(CanRxMsg) + 4U) * RX_DEPTH);
  _tx_message_buffer = xMessageBufferCreate((sizeof(CanTxMsg) + 4U) * TX_DEPTH);
  if (HAS_RETURN)
  {
    _rx_return_buffer = xMessageBufferCreate((sizeof(CanRxMsg) + 4U) * static_cast<size_t>(ICBSP_CAN_RX_RETURN_FRAMES > 0 ? ICBSP_CAN_RX_RETURN_FRAMES : 1));
  }
  if (_rx_message_buffer == nullptr || _tx_message_buffer == nullptr || (HAS_RETURN && _rx_return_buffer == nullptr))
  {
    cleanup_resources();
    return Status::IO_ERROR;
  }

  if (configure_hardware() != Status::OK)
  {
    reset_hardware();
    cleanup_resources();
    return Status::IO_ERROR;
  }
  return Status::OK;
}

template <size_t RX_DEPTH, size_t TX_DEPTH>
void BspCanT<RX_DEPTH, TX_DEPTH>::deinit()
{
  reset_hardware();
  cleanup_resources();
}

template <size_t RX_DEPTH, size_t TX_DEPTH>
Status BspCanT<RX_DEPTH, TX_DEPTH>::send(uint32_t id, const uint8_t *data, uint8_t dlc, uint32_t id_type)
{
  return send_impl(id, data, dlc, id_type, false);
}

template <size_t RX_DEPTH, size_t TX_DEPTH>
Status BspCanT<RX_DEPTH, TX_DEPTH>::send_latest(uint32_t id, const uint8_t *data, uint8_t dlc, uint32_t id_type)
{
  return send_impl(id, data, dlc, id_type, true);
}

template <size_t RX_DEPTH, size_t TX_DEPTH>
Status BspCanT<RX_DEPTH, TX_DEPTH>::send_impl(uint32_t id, const uint8_t *data, uint8_t dlc, uint32_t id_type, bool latest_only)
{
  const uint32_t id_max = (id_type == FDCAN_EXTENDED_ID) ? 0x1FFFFFFFU : 0x7FFU;
  if ((id_type != FDCAN_STANDARD_ID && id_type != FDCAN_EXTENDED_ID) || id > id_max || dlc > 8U || (data == nullptr && dlc > 0U))
  {
    return Status::BAD_ARG;
  }
  if (_hfdcan == nullptr || _tx_message_buffer == nullptr)
  {
    return Status::NOT_INIT;
  }

  CanTxMsg msg = {};
  msg.id       = id;
  msg.id_type  = id_type;
  msg.dlc      = dlc;
  if (dlc > 0U)
  {
    memcpy(msg.data, data, dlc);
  }

  // 整段在临界区里：发送缓冲只有一个写者（多任务 send() 不会交错），任务侧取帧也不会和 FDCAN 中断交错（N20 / N29）
  taskENTER_CRITICAL();
  if (!bus_usable() || (latest_only && key_pending(tx_key(msg))))
  {
    taskEXIT_CRITICAL();
    return Status::BUSY;
  }
  size_t sent = xMessageBufferSend(_tx_message_buffer, &msg, sizeof(msg), 0);
  if (sent != sizeof(msg))
  {
    // 软件缓冲满：先往硬件 FIFO 搬一次（万一丢过唤醒），再试一次（N27）
    (void)pump_tx(false, nullptr);
    sent = xMessageBufferSend(_tx_message_buffer, &msg, sizeof(msg), 0);
  }
  if (sent == sizeof(msg))
  {
    sw_pend_adjust(tx_key(msg), true);
  }
  (void)pump_tx(false, nullptr);
  taskEXIT_CRITICAL();

  if (sent != sizeof(msg))
  {
    diagnostics.tx_buf_full = diagnostics.tx_buf_full + 1U;
    return Status::FULL;
  }
  return Status::OK;
}

template <size_t RX_DEPTH, size_t TX_DEPTH>
Status BspCanT<RX_DEPTH, TX_DEPTH>::receive(CanRxMsg *msg, uint32_t timeout_ms)
{
  if (msg == nullptr)
  {
    return Status::BAD_ARG;
  }
#if ICBSP_CAN_RX_ENABLE
  if (!HAS_RETURN)
  {
    return Status::NOT_SUPPORTED; // ICBSP_CAN_RX_RETURN_FRAMES = 0：没人认领的帧不保留
  }
  MessageBufferHandle_t buf = _rx_return_buffer; // 只读回退缓冲：原始缓冲只归 can_rx_task（N21）
#else
  MessageBufferHandle_t buf = _rx_message_buffer; // 没有 can_rx_task：直接读原始缓冲（N28）
#endif
  if (buf == nullptr)
  {
    return Status::NOT_INIT;
  }
  const size_t n = xMessageBufferReceive(buf, msg, sizeof(CanRxMsg), can_detail::ms_to_ticks(timeout_ms));
  return (n > 0U) ? Status::OK : Status::TIMEOUT;
}

template <size_t RX_DEPTH, size_t TX_DEPTH>
Status BspCanT<RX_DEPTH, TX_DEPTH>::read_raw(CanRxMsg *msg, uint32_t timeout_ms)
{
  if (msg == nullptr)
  {
    return Status::BAD_ARG;
  }
  if (_rx_message_buffer == nullptr)
  {
    return Status::NOT_INIT;
  }
  const size_t n = xMessageBufferReceive(_rx_message_buffer, msg, sizeof(CanRxMsg), can_detail::ms_to_ticks(timeout_ms));
  return (n > 0U) ? Status::OK : Status::TIMEOUT;
}

template <size_t RX_DEPTH, size_t TX_DEPTH>
bool BspCanT<RX_DEPTH, TX_DEPTH>::return_frame(const CanRxMsg &msg)
{
  diagnostics.rx_unclaimed = diagnostics.rx_unclaimed + 1U;
  if (_rx_return_buffer == nullptr)
  {
    return false; // 没有回退缓冲（ICBSP_CAN_RX_RETURN_FRAMES = 0）：只计数
  }
  if (xMessageBufferSend(_rx_return_buffer, &msg, sizeof(msg), 0) != sizeof(msg))
  {
    diagnostics.rx_return_dropped = diagnostics.rx_return_dropped + 1U;
    return false;
  }
  return true;
}

template <size_t RX_DEPTH, size_t TX_DEPTH>
bool BspCanT<RX_DEPTH, TX_DEPTH>::tx_recover()
{
  if (_hfdcan == nullptr || _tx_message_buffer == nullptr)
  {
    return false;
  }
  taskENTER_CRITICAL();
  const bool     it_was_off = (_hfdcan->Instance->IE & FDCAN_IE_TFEE) == 0U;
  const bool     pending    = tx_has_pending();
  const uint32_t moved      = pump_tx(false, nullptr);
  taskEXIT_CRITICAL();

  if (pending && it_was_off && moved > 0U)
  {
    diagnostics.tx_stall_recover = diagnostics.tx_stall_recover + 1U;
    return true;
  }
  return false;
}

template <size_t RX_DEPTH, size_t TX_DEPTH>
Status BspCanT<RX_DEPTH, TX_DEPTH>::service_recovery()
{
  if (_hfdcan == nullptr || _tx_message_buffer == nullptr || _hfdcan->State != HAL_FDCAN_STATE_BUSY)
  {
    return Status::NOT_INIT;
  }

  const TickType_t now   = xTaskGetTickCount();
  const TickType_t retry = pdMS_TO_TICKS(ICBSP_CAN_BUSOFF_RETRY_MS);
  taskENTER_CRITICAL();
  const bool bus_off = (_hfdcan->Instance->PSR & FDCAN_PSR_BO) != 0U;
  if (bus_off && !diagnostics.recovering)
  {
    diagnostics.recovering     = true;
    diagnostics.bus_off_events = diagnostics.bus_off_events + 1U; // 只在这里计一次（N22）
    _recovery_started          = now;
    _recovery_attempted        = now - retry; // 首次立即尝试
    // 软件发送缓冲里的都是 Bus-Off 之前的旧指令：清掉，同时取消硬件里没发出去的
    diagnostics.tx_flushed   = diagnostics.tx_flushed + flush_tx();
    _hfdcan->Instance->TXBCR = _hfdcan->Instance->TXBRP;
  }
  if (diagnostics.recovering)
  {
    // 只在硬件再次置 INIT 时启动恢复，避免反复打断 129 × 11 位的恢复序列
    if ((_hfdcan->Instance->CCCR & FDCAN_CCCR_INIT) != 0U && now - _recovery_attempted >= retry)
    {
      _hfdcan->Instance->TXBCR = _hfdcan->Instance->TXBRP;
      CLEAR_BIT(_hfdcan->Instance->CCCR, FDCAN_CCCR_INIT);
      _recovery_attempted           = now;
      diagnostics.recovery_attempts = diagnostics.recovery_attempts + 1U;
    }
    // 旧发送请求彻底取消后才允许新帧入队
    if (!bus_off && (_hfdcan->Instance->CCCR & FDCAN_CCCR_INIT) == 0U && _hfdcan->Instance->TXBRP == 0U)
    {
      diagnostics.recovering         = false;
      diagnostics.recovery_successes = diagnostics.recovery_successes + 1U;
      const TickType_t elapsed       = now - _recovery_started;
      if (elapsed > diagnostics.recovery_max_ticks)
      {
        diagnostics.recovery_max_ticks = elapsed;
      }
    }
    else if (now - _recovery_attempted >= retry)
    {
      _hfdcan->Instance->TXBCR = _hfdcan->Instance->TXBRP; // 没取消完的继续请求取消，不忙等
    }
  }
  const bool recovering = diagnostics.recovering;
  taskEXIT_CRITICAL();
  return recovering ? Status::BUSY : Status::OK;
}

template <size_t RX_DEPTH, size_t TX_DEPTH>
void BspCanT<RX_DEPTH, TX_DEPTH>::process_fifo0_isr(uint32_t its, BaseType_t *pxHigherPriorityTaskWoken)
{
  if (_hfdcan == nullptr || _rx_message_buffer == nullptr)
  {
    return;
  }
  if ((its & FDCAN_IT_RX_FIFO0_MESSAGE_LOST) != 0U)
  {
    diagnostics.rx_lost = diagnostics.rx_lost + 1U;
  }
  if ((its & FDCAN_IT_RX_FIFO0_NEW_MESSAGE) == 0U)
  {
    return;
  }

  // HAL_FDCAN_GetRxMessage 按 DLC 查表拷贝（DLC 9~15 → 12~64 字节）：先读进 64 字节缓冲，避免写穿 CanRxMsg::data
  uint8_t        raw[64];
  const uint32_t drain_max = _hfdcan->Init.RxFifo0ElmtsNbr;
  for (uint32_t k = 0U; k < drain_max && HAL_FDCAN_GetRxFifoFillLevel(_hfdcan, FDCAN_RX_FIFO0) > 0U; ++k)
  {
    CanRxMsg msg = {};
    if (HAL_FDCAN_GetRxMessage(_hfdcan, FDCAN_RX_FIFO0, &msg.header, raw) != HAL_OK)
    {
      break;
    }
    const uint32_t len = can_detail::dlc_to_len(msg.header.DataLength);
    if (len > 8U || msg.header.FDFormat != FDCAN_CLASSIC_CAN)
    {
      diagnostics.rx_len_drop = diagnostics.rx_len_drop + 1U;
      continue;
    }
    // 只拷 DLC 个字节：raw 是栈上的缓冲，后面是旧数据（2026-10-17 实测，N19）；msg 已清零
    memcpy(msg.data, raw, len);
    if (xMessageBufferSendFromISR(_rx_message_buffer, &msg, sizeof(msg), pxHigherPriorityTaskWoken) != sizeof(msg))
    {
      diagnostics.rx_dropped = diagnostics.rx_dropped + 1U;
    }
  }
}

template <size_t RX_DEPTH, size_t TX_DEPTH>
void BspCanT<RX_DEPTH, TX_DEPTH>::trigger_tx_from_isr(BaseType_t *pxHigherPriorityTaskWoken)
{
  if (_hfdcan == nullptr || _tx_message_buffer == nullptr)
  {
    return;
  }
  (void)pump_tx(true, pxHigherPriorityTaskWoken);
}

template <size_t RX_DEPTH, size_t TX_DEPTH>
void BspCanT<RX_DEPTH, TX_DEPTH>::process_error_isr(uint32_t its)
{
  if (_hfdcan == nullptr)
  {
    return;
  }
  // IR.EP / IR.EW 是“状态变化”标志（进、出各置一次）：只在 PSR 显示已进入时计数
  const uint32_t psr = _hfdcan->Instance->PSR;
  if ((its & FDCAN_IT_ERROR_PASSIVE) != 0U && (psr & FDCAN_PSR_EP) != 0U)
  {
    diagnostics.err_passive = diagnostics.err_passive + 1U;
  }
  if ((its & FDCAN_IT_ERROR_WARNING) != 0U && (psr & FDCAN_PSR_EW) != 0U)
  {
    diagnostics.err_warning = diagnostics.err_warning + 1U;
  }
}

template <size_t RX_DEPTH, size_t TX_DEPTH>
uint32_t BspCanT<RX_DEPTH, TX_DEPTH>::pump_tx(bool from_isr, BaseType_t *pxHigherPriorityTaskWoken)
{
  uint32_t moved = 0U;
  while (bus_usable() && hw_has_room())
  {
    if (!_tx_staged_valid)
    {
      const size_t n = from_isr ? xMessageBufferReceiveFromISR(_tx_message_buffer, &_tx_staged, sizeof(_tx_staged), pxHigherPriorityTaskWoken)
                                : xMessageBufferReceive(_tx_message_buffer, &_tx_staged, sizeof(_tx_staged), 0);
      if (n != sizeof(_tx_staged))
      {
        break;
      }
      _tx_staged_valid = true;
    }
    const CanTxMsg &msg = _tx_staged;
    const uint32_t  key = tx_key(msg);
    if (tx_same_id_pending(key))
    {
      break; // 队列模式：同 ID 的上一帧还没发完，这一帧先留在暂存位（TX-FIFO-EMPTY / tx_recover() 会再来）
    }
    FDCAN_TxHeaderTypeDef hdr = {};
    hdr.Identifier            = msg.id;
    hdr.IdType                = msg.id_type;
    hdr.TxFrameType           = FDCAN_DATA_FRAME;
    hdr.DataLength            = msg.dlc; // H7 HAL：FDCAN_DLC_BYTES_0~8 = 0~8
    hdr.ErrorStateIndicator   = FDCAN_ESI_ACTIVE;
    hdr.BitRateSwitch         = FDCAN_BRS_OFF;
    hdr.FDFormat              = FDCAN_CLASSIC_CAN;
    hdr.TxEventFifoControl    = FDCAN_NO_TX_EVENTS;
    hdr.MessageMarker         = 0U;
    const HAL_StatusTypeDef st = HAL_FDCAN_AddMessageToTxFifoQ(_hfdcan, &hdr, _tx_staged.data);
    _tx_staged_valid           = false;
    sw_pend_adjust(key, false); // 离开软件侧（写进硬件后由 hw_has_key() 接着跟踪）
    if (st != HAL_OK)
    {
      // 上面已确认有空位，失败只可能是外设状态变了：丢弃计数，不写回（写回会让中断成为发送缓冲的第二个写者）
      diagnostics.tx_dropped = diagnostics.tx_dropped + 1U;
      break;
    }
    const uint32_t put = HAL_FDCAN_GetLatestTxFifoQRequestBuffer(_hfdcan); // 刚写入的缓冲（位图）
    if (put != 0U)
    {
      _tx_slot_key[static_cast<uint32_t>(__builtin_ctz(put))] = key;
    }
    moved = moved + 1U;
  }
  set_tx_empty_it(tx_has_pending());
  return moved;
}

template <size_t RX_DEPTH, size_t TX_DEPTH>
void BspCanT<RX_DEPTH, TX_DEPTH>::set_tx_empty_it(bool enable)
{
  const bool on = (_hfdcan->Instance->IE & FDCAN_IE_TFEE) != 0U;
  if (on == enable)
  {
    return;
  }
  // IR.TFE 在中断关着时也会置位；打开时若硬件 FIFO 已空，会马上进一次中断，不会丢唤醒
  const HAL_StatusTypeDef st = enable ? HAL_FDCAN_ActivateNotification(_hfdcan, FDCAN_IT_TX_FIFO_EMPTY, 0U)
                                      : HAL_FDCAN_DeactivateNotification(_hfdcan, FDCAN_IT_TX_FIFO_EMPTY);
  if (st != HAL_OK)
  {
    diagnostics.tx_it_fail = diagnostics.tx_it_fail + 1U; // tx_recover() 会在 10 ms 内补
  }
}

template <size_t RX_DEPTH, size_t TX_DEPTH>
uint32_t BspCanT<RX_DEPTH, TX_DEPTH>::flush_tx()
{
  // 逐帧读空而不是 xMessageBufferReset()：有任务阻塞在缓冲上时 Reset 会失败且没有提示（N35）
  uint32_t n = 0U;
  if (_tx_staged_valid)
  {
    _tx_staged_valid = false;
    n                = 1U;
  }
  CanTxMsg msg;
  while (n <= TX_DEPTH + 1U && xMessageBufferReceive(_tx_message_buffer, &msg, sizeof(msg), 0) == sizeof(msg))
  {
    n = n + 1U;
  }
  for (uint32_t i = 0U; i < PEND_SLOTS; ++i)
  {
    _sw_pend[i].n = 0U;
  }
  set_tx_empty_it(false);
  return n;
}

template <size_t RX_DEPTH, size_t TX_DEPTH>
void BspCanT<RX_DEPTH, TX_DEPTH>::reset_hardware()
{
  if (_hfdcan == nullptr)
  {
    return;
  }
  (void)HAL_FDCAN_DeactivateNotification(_hfdcan, FDCAN_IT_RX_FIFO0_NEW_MESSAGE | FDCAN_IT_RX_FIFO0_MESSAGE_LOST | FDCAN_IT_TX_FIFO_EMPTY |
                                                      FDCAN_IT_ERROR_PASSIVE | FDCAN_IT_ERROR_WARNING);
  (void)HAL_FDCAN_Stop(_hfdcan); // 没启动过时返回 HAL_ERROR，属正常
}

template <size_t RX_DEPTH, size_t TX_DEPTH>
Status BspCanT<RX_DEPTH, TX_DEPTH>::configure_hardware()
{
  // 标准帧滤波器只在 CubeMX 给了 StdFiltersNbr 时才写：StdFiltersNbr = 0 时 FilterIndex 0 会写到 RX FIFO0 的位置（F9）。
  // 全局滤波已经把不匹配的标准 / 扩展数据帧都收进 FIFO0，这个范围滤波器只是显式写出“全收”。
  if (_hfdcan->Init.StdFiltersNbr > 0U)
  {
    FDCAN_FilterTypeDef f = {};
    f.IdType              = FDCAN_STANDARD_ID;
    f.FilterIndex         = 0U;
    f.FilterType          = FDCAN_FILTER_RANGE;
    f.FilterConfig        = FDCAN_FILTER_TO_RXFIFO0;
    f.FilterID1           = 0x000U;
    f.FilterID2           = 0x7FFU;
    if (HAL_FDCAN_ConfigFilter(_hfdcan, &f) != HAL_OK)
    {
      return Status::IO_ERROR;
    }
  }
  // 远程帧由硬件拒收：HAL 的 FDCAN_FILTER_REMOTE 是“远程帧按数据帧一样过滤”（即接收），拒收要用 REJECT_REMOTE（N17）
  if (HAL_FDCAN_ConfigGlobalFilter(_hfdcan, FDCAN_ACCEPT_IN_RX_FIFO0, FDCAN_ACCEPT_IN_RX_FIFO0, FDCAN_REJECT_REMOTE, FDCAN_REJECT_REMOTE) != HAL_OK)
  {
    return Status::IO_ERROR;
  }
  if (HAL_FDCAN_Start(_hfdcan) != HAL_OK)
  {
    return Status::IO_ERROR;
  }
  if (HAL_FDCAN_ActivateNotification(_hfdcan, FDCAN_IT_RX_FIFO0_NEW_MESSAGE | FDCAN_IT_RX_FIFO0_MESSAGE_LOST, 0U) != HAL_OK)
  {
    return Status::IO_ERROR;
  }
  if (HAL_FDCAN_ActivateNotification(_hfdcan, FDCAN_IT_ERROR_PASSIVE | FDCAN_IT_ERROR_WARNING, 0U) != HAL_OK)
  {
    return Status::IO_ERROR;
  }
  return Status::OK;
}

template <size_t RX_DEPTH, size_t TX_DEPTH>
void BspCanT<RX_DEPTH, TX_DEPTH>::cleanup_resources()
{
  if (_rx_message_buffer != nullptr)
  {
    vMessageBufferDelete(_rx_message_buffer);
    _rx_message_buffer = nullptr;
  }
  if (_rx_return_buffer != nullptr)
  {
    vMessageBufferDelete(_rx_return_buffer);
    _rx_return_buffer = nullptr;
  }
  if (_tx_message_buffer != nullptr)
  {
    vMessageBufferDelete(_tx_message_buffer);
    _tx_message_buffer = nullptr;
  }
}

#endif // HAL_FDCAN_MODULE_ENABLED

#endif // __BSP_FDCAN_HPP__
