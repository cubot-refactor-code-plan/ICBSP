/**
 * @file bsp_can.hpp
 * @author Rh
 * @brief CAN 驱动入口：H7 转给 bsp_fdcan.hpp（FDCAN），F4 在本文件里实现（bxCAN）
 * @version 0.8
 * @date 2026-11-05
 *
 * @copyright Copyright (c) 2026
 *
 * @details
 *   上层（device / service / tasks / board_cfg）只 include 本文件，不用关心系列：
 *     - H7（HAL_FDCAN_MODULE_ENABLED）：实现全部在 bsp_fdcan.hpp，HAL 回调在 bsp_fdcan.cpp
 *     - F4（HAL_CAN_MODULE_ENABLED）：实现在下面的 `#elif` 分支里，HAL 回调在 bsp_can.cpp
 *
 *   两个分支的公开接口（BspCanT / CanRxMsg / CanTxMsg / CAN_DLC_MASK_*）逐一对齐，
 *   上层代码一行都不用改；只有 Config 的句柄字段名不同：H7 是 `hfdcan`，F4 是 `hcan`。
 */

#ifndef __BSP_CAN_HPP__
#define __BSP_CAN_HPP__

#include "bsp/bsp_series.hpp" // IWYU pragma: keep（系列桥接）

#if defined(HAL_FDCAN_MODULE_ENABLED)

#  include "bsp/bsp_fdcan.hpp"

#elif defined(HAL_CAN_MODULE_ENABLED)

/* ==================== */
/* ==================== F4：bxCAN 后端 ==================== */

/*
 * 接口、语义、诊断计数与 bsp_fdcan.hpp 的 FDCAN 版一致，差异只有底层：
 *
 *   概念         FDCAN（H7）                      bxCAN（F4）
 *   发送队列     TX FIFO + 队列（AddToTxFifoQ）    3 个发送邮箱（HAL_CAN_AddTxMessage）
 *   续发中断     FDCAN_IT_TX_FIFO_EMPTY            CAN_IT_TX_MAILBOX_EMPTY
 *   覆盖最新帧   GetLatestTxFifoQRequestBuffer     无硬件对应，send_latest() 退化为 send()
 *   接收         RX FIFO0 + 中断                   RX FIFO0 + 中断
 *   过滤         StdFiltersNbr + 全局滤波           过滤器组 0（32 位掩码，全通）
 *   Bus-Off      PSR.BO / CCCR.INIT                ESR.BOFF / MCR.INRQ
 *
 * CubeMX 侧硬性要求：
 *   - AutoBusOff = DISABLE（恢复统一由 service_recovery() 管）
 *   - AutoRetransmission = ENABLE
 *   - 打开 CAN1 RX FIFO 0 中断
 *   - 过滤器组 0 归本驱动独占
 */

#include "FreeRTOS.h" // IWYU pragma: keep
#include "message_buffer.h"
#include "task.h" // IWYU pragma: keep

#include "hardware_config.h" // ICBSP_CAN_RX_ENABLE、ICBSP_CAN_RX_RETURN_FRAMES、ICBSP_CAN_BUSOFF_RETRY_MS
#include "service/system/status_code.hpp"

#include <string.h>

#ifndef ICBSP_CAN_RX_RETURN_FRAMES
#  error "hardware_config.h 缺少 ICBSP_CAN_RX_RETURN_FRAMES（回退缓冲深度，0 = 不建回退缓冲），见 templates/user/board/hardware_config.h"
#endif
#ifndef ICBSP_CAN_BUSOFF_RETRY_MS
#  error "hardware_config.h 缺少 ICBSP_CAN_BUSOFF_RETRY_MS，见 templates/user/board/hardware_config.h"
#endif


/* ==================== 帧结构（与 bsp_fdcan.hpp 的 FDCAN 版逐字相同） ==================== */

/** @brief 接收到的一帧：帧头 + 最多 8 字节数据（DLC 之后的字节为 0） */
typedef struct
{
  FDCAN_RxHeaderTypeDef header; ///< 字段与 H7 的 HAL 定义一致，见 bsp_series.hpp
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
 */
bool bsp_bxcan_is_listed(const void *self, const CAN_HandleTypeDef *hcan);

/**
 * @brief 清单外 bxCAN 的回调转调（弱函数，默认什么也不做；实现在 bsp_can.cpp）
 * @note 本驱动占用了 HAL_CAN_RxFifo0MsgPendingCallback / HAL_CAN_TxMailbox0..2CompleteCallback /
 *       HAL_CAN_ErrorCallback，宿主不能再定义它们。
 */
extern "C"
{
  void icbsp_unhandled_bxcan_rx_fifo0(CAN_HandleTypeDef *hcan);
  void icbsp_unhandled_bxcan_tx_mailbox(CAN_HandleTypeDef *hcan, uint32_t mailbox);
  void icbsp_unhandled_bxcan_error(CAN_HandleTypeDef *hcan, uint32_t error_code);
}

/** @brief CAN 驱动的内部工具（与 bsp_fdcan.hpp 的 FDCAN 版同名同义） */
namespace can_detail
{
/** @brief 毫秒 → tick；portMAX_DELAY 原样保留（pdMS_TO_TICKS(portMAX_DELAY) 会溢出） */
inline TickType_t ms_to_ticks(uint32_t ms)
{
  return (ms == portMAX_DELAY) ? portMAX_DELAY : pdMS_TO_TICKS(ms);
}

/** @brief bxCAN 的 DLC 字段本身就是 0~8 的字节数；> 8 返回 0xFF（不支持） */
inline uint32_t dlc_to_len(uint32_t dlc_code)
{
  return (dlc_code <= 8U) ? dlc_code : 0xFFU;
}
} // namespace can_detail


/* ==================== 驱动 ==================== */

/**
 * @brief CAN 驱动模板（bxCAN：收发 + 恢复 + 诊断）
 *
 * @tparam RX_DEPTH 原始接收缓冲深度（帧）：FIFO0 中断写入，can_rx_task（或 receive()）读出
 * @tparam TX_DEPTH 软件发送缓冲深度（帧）
 *
 * @note 用法与 FDCAN 版完全相同，只有 Config 的字段名是 `hcan`：
 *
 *   inline BspCanT<16, 16> bsp_can1({.hcan = &hcan1, .name = "CAN1"});
 */
template <size_t RX_DEPTH, size_t TX_DEPTH>
class BspCanT
{
  static_assert(RX_DEPTH >= 1U && TX_DEPTH >= 1U, "CAN 收发缓冲深度至少 1 帧");

public:
  /* ==================== 配置 ==================== */

  /** @brief 配置（聚合体：`{.hcan = &hcanX, .name = "CANx"}`） */
  struct Config
  {
    CAN_HandleTypeDef *hcan = nullptr; ///< bxCAN 句柄（CubeMX 配成 Normal 模式、RX FIFO0）
    const char        *name = "CAN";   ///< 实例名称（调试用）
  };

  /** @brief 运行诊断计数：只增不减；除 bus_off_events / err_* 外，正常应恒为 0 */
  struct Diagnostics
  {
    // 总线错误
    volatile uint32_t bus_off_events     = 0;     ///< 进入 Bus-Off 的次数
    volatile uint32_t err_passive        = 0;     ///< 进入错误被动的次数
    volatile uint32_t err_warning        = 0;     ///< 进入错误警告的次数
    volatile uint32_t recovery_attempts  = 0;     ///< service_recovery() 清 INRQ 的次数
    volatile uint32_t recovery_successes = 0;     ///< 恢复成功次数
    volatile uint32_t recovery_max_ticks = 0;     ///< 单次恢复最长耗时（ticks）
    volatile bool     recovering         = false; ///< 当前是否在恢复流程中
    // 接收
    volatile uint32_t rx_dropped        = 0; ///< 原始接收缓冲满而丢弃的帧数
    volatile uint32_t rx_lost           = 0; ///< 硬件 RX FIFO0 溢出次数
    volatile uint32_t rx_len_drop       = 0; ///< 远程帧 / DLC > 8 而丢弃的帧数
    volatile uint32_t rx_unclaimed      = 0; ///< 没有接收节点认领的帧数
    volatile uint32_t rx_return_dropped = 0; ///< 没人认领、回退缓冲又满而丢弃的帧数
    // 发送
    volatile uint32_t tx_dropped       = 0; ///< 写硬件邮箱失败而丢弃的帧数
    volatile uint32_t tx_buf_full      = 0; ///< 软件发送缓冲满、send() 返回 FULL 的次数
    volatile uint32_t tx_stall_recover = 0; ///< TX 完成中断关着、由 tx_recover() 补发的次数
    volatile uint32_t tx_it_fail       = 0; ///< 开关 TX 完成中断时 HAL 拒绝的次数
    volatile uint32_t tx_flushed       = 0; ///< 进入 Bus-Off 时清掉的软件发送缓冲帧数
  };

  /* ==================== */
  /* ==================== 公有接口 ==================== */

  /** @brief 只绑定句柄（constexpr：board_cfg.hpp 的 inline 全局实例要常量初始化） */
  constexpr BspCanT(const Config &cfg) : _hcan(cfg.hcan), _name(cfg.name)
  {
  }

  /** @brief 平凡析构（全局实例永不析构）；需要释放资源时显式调 deinit() */
  ~BspCanT() = default;

  BspCanT(const BspCanT &)            = delete;
  BspCanT &operator=(const BspCanT &) = delete;

  /** @brief 停外设、关通知、清软件缓冲，再按 CubeMX 配置重新启动（可重复调用，失败回滚） */
  Status init();

  /** @brief 停外设、关通知并释放软件资源 */
  void deinit();

  /** @brief 构造时是否给了句柄（不代表 init() 成功） */
  bool is_ready() const
  {
    return _hcan != nullptr;
  }

  /** @brief 底层句柄（供 HAL 回调分发） */
  CAN_HandleTypeDef *handle() const
  {
    return _hcan;
  }

  /** @brief 发送一帧（语义同 FDCAN 版） */
  Status send(uint32_t id, const uint8_t *data, uint8_t dlc = 8U, uint32_t id_type = FDCAN_STANDARD_ID);

  /**
   * @brief 周期帧用的发送：同一 ID 的上一帧还在驱动里（软件缓冲 / 硬件邮箱）等着时，这一帧不入队
   * @return 同 send()；另外 BUSY 也表示“同 ID 的上一帧还没发出去”
   * @note 语义与 bsp_fdcan.hpp 的 FDCAN 版完全一致：bxCAN 本来就没有“覆盖”能力，
   *       FDCAN 版也只是在这种情况下返回 BUSY，而不是覆写队列。
   */
  Status send_latest(uint32_t id, const uint8_t *data, uint8_t dlc = 8U, uint32_t id_type = FDCAN_STANDARD_ID);

  /** @brief 取一帧（语义同 FDCAN 版） */
  Status receive(CanRxMsg *msg, uint32_t timeout_ms = portMAX_DELAY);

  /** @brief can_rx_task 专用：从原始接收缓冲取一帧 */
  Status read_raw(CanRxMsg *msg, uint32_t timeout_ms = 0U);

  /** @brief can_rx_task 专用：记一帧“没人认领”，有回退缓冲就放进去 */
  bool return_frame(const CanRxMsg &msg);

  /** @brief 发送兜底（sys_task 每 10 ms 调用） */
  bool tx_recover();

  /** @brief Bus-Off 恢复（sys_task 每 10 ms 调用，非阻塞） */
  Status service_recovery();

  /* ==================== */
  /* ==================== ISR 入口（bsp_can.cpp 的 HAL 回调调用） ==================== */

  /** @brief RX FIFO0 有新帧：排空硬件 FIFO */
  void process_fifo0_isr(uint32_t its, BaseType_t *pxHigherPriorityTaskWoken);

  /** @brief 某个发送邮箱发完了：把软件缓冲里的帧填进空邮箱 */
  void trigger_tx_from_isr(BaseType_t *pxHigherPriorityTaskWoken);

  /** @brief 错误状态变化（EP / EW）：只计“进入”的次数。its = hcan->ErrorCode */
  void process_error_isr(uint32_t its);

  /* ==================== */

  Diagnostics diagnostics; ///< 运行诊断（调试观察用，不要改写）

private:
  /* ==================== 成员 ==================== */

  /** @brief 回退缓冲是否存在（有 can_rx_task 且深度非 0） */
  static constexpr bool HAS_RETURN = (ICBSP_CAN_RX_ENABLE != 0) && (ICBSP_CAN_RX_RETURN_FRAMES > 0);

  /** @brief 发送邮箱个数（bxCAN 固定 3 个） */
  static constexpr uint32_t MAILBOXES = 3U;

  CAN_HandleTypeDef    *_hcan;
  const char           *_name;
  MessageBufferHandle_t _tx_message_buffer  = nullptr; ///< 写者：send()（临界区）；读者：临界区里的任务侧或发送完成中断
  MessageBufferHandle_t _rx_message_buffer  = nullptr; ///< 写者：FIFO0 中断；读者：can_rx_task（或 ICBSP_CAN_RX_ENABLE = 0 时的 receive()）
  MessageBufferHandle_t _rx_return_buffer   = nullptr; ///< 写者：can_rx_task；读者：receive()
  TickType_t            _recovery_started   = 0U;
  TickType_t            _recovery_attempted = 0U;
  bool                  _err_passive_last   = false; ///< 上次进错误中断时的 EP 状态（HAL 的 ErrorCode 不清零，只能自己做边沿）
  bool                  _err_warning_last   = false; ///< 上次进错误中断时的 EW 状态

  // bxCAN 多个邮箱同时有帧时按 ID 优先级仲裁，同 ID 的多帧按邮箱编号发而不是按入队顺序。
  // 所以记下每个邮箱里的帧 ID；下一帧的 ID 已经有一帧在邮箱里等着时，先暂存，等那一帧发完再写。
  uint32_t _tx_slot_key[MAILBOXES] = {}; ///< 邮箱 i 里的帧：ID | 扩展帧标志（只在 TSR.TMEi = 0 时有效）
  CanTxMsg _tx_staged              = {}; ///< 从软件缓冲取出、还没写进硬件的一帧
  bool     _tx_staged_valid        = false; ///< _tx_staged 是否有效

  /** @brief 软件侧（发送缓冲 + 暂存位）每个 ID 有几帧在等：n = 0 的表项是空的 */
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
    // bxCAN 没有 HAL_FDCAN_STATE_BUSY：HAL_CAN_Start() 之后状态是 LISTENING
    return _hcan->State == HAL_CAN_STATE_LISTENING && !diagnostics.recovering && (_hcan->Instance->ESR & CAN_ESR_BOFF) == 0U &&
           (_hcan->Instance->MCR & CAN_MCR_INRQ) == 0U;
  }

  /** @brief 三个发送邮箱里至少有一个是空的 */
  bool hw_has_room() const
  {
    return (_hcan->Instance->TSR & (CAN_TSR_TME0 | CAN_TSR_TME1 | CAN_TSR_TME2)) != 0U;
  }

  /** @brief 把软件缓冲里的帧搬进所有空邮箱，再按“软件缓冲是否还有帧”开关发送完成中断 */
  uint32_t pump_tx(bool from_isr, BaseType_t *pxHigherPriorityTaskWoken);

  /** @brief send() / send_latest() 的共同实现 */
  Status send_impl(uint32_t id, const uint8_t *data, uint8_t dlc, uint32_t id_type, bool latest_only);

  /** @brief 帧在 _tx_slot_key 里的键 */
  static uint32_t tx_key(const CanTxMsg &msg)
  {
    return msg.id | ((msg.id_type == FDCAN_EXTENDED_ID) ? 0x80000000U : 0U);
  }

  /** @brief 硬件邮箱里是否已有同 ID 的帧在等（bxCAN 按 ID 仲裁，同 ID 可能乱序） */
  bool tx_same_id_pending(uint32_t key) const
  {
    return hw_has_key(key);
  }

  /** @brief 软件侧这个 ID 的计数加 / 减 1（临界区或发送完成中断里调用） */
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

  /** @brief 这个 ID 是否有帧在驱动里等（软件侧或硬件邮箱） */
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

  /** @brief 硬件邮箱里是否有这个 ID 的帧在等发送 */
  bool hw_has_key(uint32_t key) const
  {
    const uint32_t tsr = _hcan->Instance->TSR;
    for (uint32_t i = 0U; i < MAILBOXES; ++i)
    {
      // TMEi = 1 表示邮箱 i 空；为 0 表示里面还有帧（正在发或排队）
      if ((tsr & (CAN_TSR_TME0 << i)) == 0U && _tx_slot_key[i] == key)
      {
        return true;
      }
    }
    return false;
  }

  /** @brief 软件侧还有没写进硬件的帧（发送缓冲或暂存位） */
  bool tx_has_pending() const
  {
    return _tx_staged_valid || xMessageBufferIsEmpty(_tx_message_buffer) == pdFALSE;
  }

  /** @brief 开关发送完成中断（状态没变就不动寄存器） */
  void set_tx_empty_it(bool enable);

  /** @brief 清空软件发送缓冲并取消邮箱里未发出的帧（临界区里调用），返回清掉的帧数 */
  uint32_t flush_tx();

  /** @brief 停外设并摘除本驱动用过的全部通知（幂等） */
  void reset_hardware();

  /** @brief 过滤器组 0（全通）+ 启动 + 打开 RX / 错误通知 */
  Status configure_hardware();

  /** @brief 释放消息缓冲 */
  void cleanup_resources();

  /* ==================== */
};


/* ==================== 成员函数定义 ==================== */

template <size_t RX_DEPTH, size_t TX_DEPTH>
Status BspCanT<RX_DEPTH, TX_DEPTH>::init()
{
  if (_hcan == nullptr || !bsp_bxcan_is_listed(this, _hcan))
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
  _err_passive_last   = false;
  _err_warning_last   = false;
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
  if (_hcan == nullptr || _tx_message_buffer == nullptr)
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

  // 整段在临界区里：发送缓冲只有一个写者，任务侧取帧也不会和发送完成中断交错
  taskENTER_CRITICAL();
  if (!bus_usable() || (latest_only && key_pending(tx_key(msg))))
  {
    taskEXIT_CRITICAL();
    return Status::BUSY;
  }
  size_t sent = xMessageBufferSend(_tx_message_buffer, &msg, sizeof(msg), 0);
  if (sent != sizeof(msg))
  {
    // 软件缓冲满：先往硬件邮箱搬一次（万一丢过唤醒），再试一次
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
  MessageBufferHandle_t buf = _rx_return_buffer; // 只读回退缓冲：原始缓冲只归 can_rx_task
#else
  MessageBufferHandle_t buf = _rx_message_buffer; // 没有 can_rx_task：直接读原始缓冲
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
  if (_hcan == nullptr || _tx_message_buffer == nullptr)
  {
    return false;
  }
  taskENTER_CRITICAL();
  const bool     it_was_off = (_hcan->Instance->IER & CAN_IER_TMEIE) == 0U;
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
  if (_hcan == nullptr || _tx_message_buffer == nullptr || _hcan->State != HAL_CAN_STATE_LISTENING)
  {
    return Status::NOT_INIT;
  }

  const TickType_t now   = xTaskGetTickCount();
  const TickType_t retry = pdMS_TO_TICKS(ICBSP_CAN_BUSOFF_RETRY_MS);
  taskENTER_CRITICAL();
  const bool bus_off = (_hcan->Instance->ESR & CAN_ESR_BOFF) != 0U;
  if (bus_off && !diagnostics.recovering)
  {
    diagnostics.recovering     = true;
    diagnostics.bus_off_events = diagnostics.bus_off_events + 1U; // 只在这里计一次
    _recovery_started          = now;
    _recovery_attempted        = now - retry; // 首次立即尝试
    // 软件发送缓冲里的都是 Bus-Off 之前的旧指令：清掉，同时取消邮箱里没发出去的
    diagnostics.tx_flushed = diagnostics.tx_flushed + flush_tx();
    (void)HAL_CAN_AbortTxRequest(_hcan, CAN_TX_MAILBOX0 | CAN_TX_MAILBOX1 | CAN_TX_MAILBOX2);
  }
  if (diagnostics.recovering)
  {
    // 只在硬件再次置 INRQ 时启动恢复，避免反复打断 128 × 11 位的恢复序列
    if ((_hcan->Instance->MCR & CAN_MCR_INRQ) != 0U && now - _recovery_attempted >= retry)
    {
      (void)HAL_CAN_AbortTxRequest(_hcan, CAN_TX_MAILBOX0 | CAN_TX_MAILBOX1 | CAN_TX_MAILBOX2);
      CLEAR_BIT(_hcan->Instance->MCR, CAN_MCR_INRQ);
      _recovery_attempted           = now;
      diagnostics.recovery_attempts = diagnostics.recovery_attempts + 1U;
    }
    // 旧发送请求彻底取消（三个邮箱都空）后才允许新帧入队
    const bool boxes_free = (_hcan->Instance->TSR & (CAN_TSR_TME0 | CAN_TSR_TME1 | CAN_TSR_TME2)) == (CAN_TSR_TME0 | CAN_TSR_TME1 | CAN_TSR_TME2);
    if (!bus_off && (_hcan->Instance->MCR & CAN_MCR_INRQ) == 0U && boxes_free)
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
      (void)HAL_CAN_AbortTxRequest(_hcan, CAN_TX_MAILBOX0 | CAN_TX_MAILBOX1 | CAN_TX_MAILBOX2); // 没取消完的继续请求取消，不忙等
    }
  }
  const bool recovering = diagnostics.recovering;
  taskEXIT_CRITICAL();
  return recovering ? Status::BUSY : Status::OK;
}

template <size_t RX_DEPTH, size_t TX_DEPTH>
void BspCanT<RX_DEPTH, TX_DEPTH>::process_fifo0_isr(uint32_t its, BaseType_t *pxHigherPriorityTaskWoken)
{
  (void)its; // bxCAN 的 HAL 回调没有事件位参数
  if (_hcan == nullptr || _rx_message_buffer == nullptr)
  {
    return;
  }

  // RX FIFO0 溢出：读 RF0R 不会自动清 FOVR0，必须写 1 清除。
  // 放在排空之前做，保证“丢过帧”这件事先被记下来。
  if ((_hcan->Instance->RF0R & CAN_RF0R_FOVR0) != 0U)
  {
    diagnostics.rx_lost   = diagnostics.rx_lost + 1U;
    _hcan->Instance->RF0R = CAN_RF0R_FOVR0;
  }

  for (uint32_t k = 0U; k < RX_DEPTH && HAL_CAN_GetRxFifoFillLevel(_hcan, CAN_RX_FIFO0) > 0U; ++k)
  {
    CAN_RxHeaderTypeDef hdr;
    uint8_t             raw[8] = {};
    if (HAL_CAN_GetRxMessage(_hcan, CAN_RX_FIFO0, &hdr, raw) != HAL_OK)
    {
      break;
    }

    CanRxMsg msg = {};
    // 远程帧由驱动丢掉（CubeMX 的 filter 不能单独拒收远程帧）
    if (hdr.RTR != CAN_RTR_DATA)
    {
      diagnostics.rx_len_drop = diagnostics.rx_len_drop + 1U;
      continue;
    }
    const uint32_t len = can_detail::dlc_to_len(hdr.DLC);
    if (len > 8U)
    {
      diagnostics.rx_len_drop = diagnostics.rx_len_drop + 1U;
      continue;
    }

    // bxCAN 把标准帧 / 扩展帧拆在两个字段里，按 IDE 取一个填进统一的 Identifier
    msg.header.Identifier            = (hdr.IDE == CAN_ID_STD) ? hdr.StdId : hdr.ExtId;
    msg.header.IdType                = hdr.IDE; // CAN_ID_STD / CAN_ID_EXT 与 FDCAN_STANDARD_ID / FDCAN_EXTENDED_ID 同值
    msg.header.RxFrameType           = FDCAN_DATA_FRAME;
    msg.header.DataLength            = len;
    msg.header.ErrorStateIndicator   = 0U;
    msg.header.BitRateSwitch         = 0U;
    msg.header.FDFormat              = FDCAN_CLASSIC_CAN;
    msg.header.RxTimestamp           = hdr.Timestamp;
    msg.header.FilterIndex           = hdr.FilterMatchIndex;
    msg.header.IsFilterMatchingFrame = 0U;
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
  if (_hcan == nullptr || _tx_message_buffer == nullptr)
  {
    return;
  }
  (void)pump_tx(true, pxHigherPriorityTaskWoken);
}

template <size_t RX_DEPTH, size_t TX_DEPTH>
void BspCanT<RX_DEPTH, TX_DEPTH>::process_error_isr(uint32_t its)
{
  (void)its; // 用 ESR 做边沿判断，不看 ErrorCode（HAL 只 |=，不清零）
  if (_hcan == nullptr)
  {
    return;
  }
  // its = hcan->ErrorCode。F4 HAL 只做 |=、从不自动清零，所以不能拿它当“本次事件”：
  // 用 ESR 的当前状态做边沿判断，只在“没进 → 进了”时计数，与 FDCAN 版的“只计进入”一致。
  const uint32_t esr     = _hcan->Instance->ESR;
  const bool     passive = (esr & CAN_ESR_EPVF) != 0U;
  const bool     warning = (esr & CAN_ESR_EWGF) != 0U;
  if (passive && !_err_passive_last)
  {
    diagnostics.err_passive = diagnostics.err_passive + 1U;
  }
  if (warning && !_err_warning_last)
  {
    diagnostics.err_warning = diagnostics.err_warning + 1U;
  }
  _err_passive_last = passive;
  _err_warning_last = warning;
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
      break; // 同 ID 的上一帧还没发完，这一帧先留在暂存位（发送完成中断 / tx_recover() 会再来）
    }

    CAN_TxHeaderTypeDef hdr = {};
    hdr.StdId              = (msg.id_type == FDCAN_EXTENDED_ID) ? 0U : msg.id;
    hdr.ExtId              = (msg.id_type == FDCAN_EXTENDED_ID) ? msg.id : 0U;
    hdr.IDE                = (msg.id_type == FDCAN_EXTENDED_ID) ? CAN_ID_EXT : CAN_ID_STD;
    hdr.RTR                = CAN_RTR_DATA;
    hdr.DLC                = msg.dlc;
    hdr.TransmitGlobalTime = DISABLE;

    uint32_t                mailbox = 0U;
    const HAL_StatusTypeDef st      = HAL_CAN_AddTxMessage(_hcan, &hdr, _tx_staged.data, &mailbox);
    _tx_staged_valid                = false;
    sw_pend_adjust(key, false); // 离开软件侧（写进邮箱后由 hw_has_key() 接着跟踪）
    if (st != HAL_OK)
    {
      // 上面已确认有空邮箱，失败只可能是外设状态变了：丢弃计数，不写回
      diagnostics.tx_dropped = diagnostics.tx_dropped + 1U;
      break;
    }
    if (mailbox < MAILBOXES)
    {
      _tx_slot_key[mailbox] = key;
    }
    moved = moved + 1U;
  }
  set_tx_empty_it(tx_has_pending());
  return moved;
}

template <size_t RX_DEPTH, size_t TX_DEPTH>
void BspCanT<RX_DEPTH, TX_DEPTH>::set_tx_empty_it(bool enable)
{
  const bool on = (_hcan->Instance->IER & CAN_IER_TMEIE) != 0U;
  if (on == enable)
  {
    return;
  }
  const HAL_StatusTypeDef st = enable ? HAL_CAN_ActivateNotification(_hcan, CAN_IT_TX_MAILBOX_EMPTY)
                                      : HAL_CAN_DeactivateNotification(_hcan, CAN_IT_TX_MAILBOX_EMPTY);
  if (st != HAL_OK)
  {
    diagnostics.tx_it_fail = diagnostics.tx_it_fail + 1U; // tx_recover() 会在 10 ms 内补
  }
}

template <size_t RX_DEPTH, size_t TX_DEPTH>
uint32_t BspCanT<RX_DEPTH, TX_DEPTH>::flush_tx()
{
  // 逐帧读空而不是 xMessageBufferReset()：有任务阻塞在缓冲上时 Reset 会失败且没有提示
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
  for (uint32_t i = 0U; i < MAILBOXES; ++i)
  {
    _tx_slot_key[i] = 0U;
  }
  set_tx_empty_it(false);
  return n;
}

template <size_t RX_DEPTH, size_t TX_DEPTH>
void BspCanT<RX_DEPTH, TX_DEPTH>::reset_hardware()
{
  if (_hcan == nullptr)
  {
    return;
  }
  (void)HAL_CAN_DeactivateNotification(_hcan, CAN_IT_RX_FIFO0_MSG_PENDING | CAN_IT_TX_MAILBOX_EMPTY | CAN_IT_ERROR_PASSIVE |
                                                 CAN_IT_ERROR_WARNING | CAN_IT_BUSOFF);
  (void)HAL_CAN_Stop(_hcan); // 没启动过时返回 HAL_ERROR，属正常
}

template <size_t RX_DEPTH, size_t TX_DEPTH>
Status BspCanT<RX_DEPTH, TX_DEPTH>::configure_hardware()
{
  // 过滤器组 0：32 位掩码模式，ID 与掩码全 0 = 全部接收，进 FIFO0。过滤器组 0 归本驱动独占。
  // SlaveStartFilterBank 只在双 CAN 型号（CAN2）上有意义：CAN2 的过滤器组从 14 开始。
  CAN_FilterTypeDef f = {};
  f.FilterBank           = 0U;
  f.FilterMode           = CAN_FILTERMODE_IDMASK;
  f.FilterScale          = CAN_FILTERSCALE_32BIT;
  f.FilterIdHigh         = 0U;
  f.FilterIdLow          = 0U;
  f.FilterMaskIdHigh     = 0U;
  f.FilterMaskIdLow      = 0U;
  f.FilterFIFOAssignment = CAN_FILTER_FIFO0;
  f.FilterActivation     = ENABLE;
  f.SlaveStartFilterBank = 14U;
  if (HAL_CAN_ConfigFilter(_hcan, &f) != HAL_OK)
  {
    return Status::IO_ERROR;
  }
  if (HAL_CAN_Start(_hcan) != HAL_OK)
  {
    return Status::IO_ERROR;
  }
  if (HAL_CAN_ActivateNotification(_hcan, CAN_IT_RX_FIFO0_MSG_PENDING) != HAL_OK)
  {
    return Status::IO_ERROR;
  }
  if (HAL_CAN_ActivateNotification(_hcan, CAN_IT_ERROR_PASSIVE | CAN_IT_ERROR_WARNING | CAN_IT_BUSOFF) != HAL_OK)
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

#endif // HAL_CAN_MODULE_ENABLED

#endif // __BSP_CAN_HPP__
