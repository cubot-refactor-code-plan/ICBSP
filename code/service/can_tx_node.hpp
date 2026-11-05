#ifndef CAN_TX_NODE_HPP
#define CAN_TX_NODE_HPP

#include "stm32h7xx_hal.h" // IWYU pragma: keep（HAL 模块开关来自 stm32h7xx_hal_conf.h）
#include "hardware_config.h"

// 宿主工程没在 CubeMX 里开 FDCAN 或 CAN_TX_ENABLE=0 时，本模块整体不参与编译
#if defined(HAL_FDCAN_MODULE_ENABLED) && CAN_TX_ENABLE

#include "bsp_can.hpp"
#include "status.hpp"
#include "system.hpp"

#include "FreeRTOS.h" // IWYU pragma: keep
#include "semphr.h"
#include "task.h" // IWYU pragma: keep

#include <new>
#include <string.h>

#ifndef CAN_TX_GATHER_MS
#  error "hardware_config.h 缺少 CAN_TX_GATHER_MS（共帧槽位凑不齐时最多等多久就发），见 templates/user/board/hardware_config.h"
#endif

/**
 * @brief CAN 发送节点：一帧 8 字节分成若干槽位，由不同设备分别填写，聚合成一帧发送（例如 4 个 M3508 共用 0x200）
 *
 * @tparam CanT 具体的 BspCanT<RX_DEPTH, TX_DEPTH> 类型
 *
 * @note 发送时机（can_tx_task 每 1 ms 调 service()）：
 *       - **凑齐即发**：所有“有效”槽位都被重新填写过（过期的、注册了却从没填过的槽位不算，见下）；
 *       - **凑不齐时限**：有槽位填了新数据、等了 CAN_TX_GATHER_MS 还没凑齐，也发（计入 partial_sends）；
 *       - **保底心跳**：CAN_TX_HEARTBEAT_MS 内没发过就发一次当前内容（计入 heartbeat_sends）。
 *       槽位 CAN_CMD_EXPIRE_MS 没被刷新就清零并标为“过期”，过期槽位不再拖住“凑齐即发”（评审 F3）。
 * @note 发送用 send_latest()：同 ID 的上一帧还没发出去时这一帧不入队（BUSY），下个周期发最新内容；
 *       总线被更高优先级流量占满时，一个节点最多在驱动里占一帧，不会堵住别的帧。
 * @note 注册只在初始化阶段（system_start() 之前）；运行态之后 regist() 直接返回 nullptr，不记初始化错误。
 */
template <typename CanT>
class CanTxNodeT
{
public:
  /** @brief 发送统计（只增不减） */
  struct Stats
  {
    uint32_t sent;            ///< 发出的帧数
    uint32_t partial_sends;   ///< 凑不齐、按 CAN_TX_GATHER_MS 时限发出的帧数（大于 0 说明同帧设备的刷新节拍不一致）
    uint32_t heartbeat_sends; ///< 没有新数据、只靠保底心跳发出的帧数
    uint32_t expired;         ///< 槽位过期被清零的次数
  };

  /**
   * @brief 初始化阶段注册发送节点（同一 CAN、同一 ID 的节点共用一帧，按槽位区分）
   * @param can_item 所属 CAN 实例
   * @param can_id   帧 ID（标准帧 11 位 / 扩展帧 29 位）
   * @param division_ 一帧 8 字节划分的槽位数（1 / 2 / 4 / 8）
   * @param slot     本节点占用的槽位
   * @param id_type  FDCAN_STANDARD_ID / FDCAN_EXTENDED_ID
   * @return 节点；参数非法、槽位冲突、资源不足返回 nullptr 并记初始化错误；运行态之后调用返回 nullptr
   */
  static CanTxNodeT *regist(CanT &can_item, uint32_t can_id, uint8_t division_ = 1U, uint8_t slot = 0U, uint32_t id_type = FDCAN_STANDARD_ID);

  /** @brief 释放一个槽位（节点保留，供其他槽位继续用）；槽位全部释放后节点不再发送 */
  static Status unregist(CanTxNodeT *node, uint8_t slot);

  /** @brief 运行态后冻结注册（can_tx_task 启动时调用） */
  static void freeze()
  {
    _frozen = true;
  }

  /** @brief 指定 CAN 实例上已注册的发送节点数 */
  static uint32_t count_for(const CanT &can_item);

  /** @brief 发送任务启动时初始化各节点的发送时刻 */
  static void init_send_ticks(CanT &can_item, TickType_t now);

  /** @brief 发送任务每 1 ms 调用：过期清零、凑齐 / 时限 / 心跳发送 */
  static void service(CanT &can_item);

  /** @brief 写入一个槽位的数据（8 / division 字节）；返回 OK 表示已刷新 */
  Status filldata(uint8_t data[], uint8_t slot = 0);

  /** @brief 发送统计 */
  Stats get_stats() const
  {
    return _stats;
  }

private:
  CanTxMsg          txBuffer;
  uint8_t           division;
  Status            _statu;
  uint8_t           bufferRegister; ///< 已注册的槽位
  uint8_t           bufferUnsend;   ///< 上次发送之后被填写过的槽位
  bool              _registered;
  SemaphoreHandle_t _lock;
  TickType_t        last_send_tick;
  TickType_t        first_unsend_tick; ///< 本轮第一个槽位被填写的时刻（凑不齐时限从这里算）
  TickType_t        bufferFillTick[8];
  uint8_t           bufferExpired; ///< 已过期的槽位（初值全部过期：注册了却从没填过的槽位不拖住凑齐）
  Stats             _stats;
  CanT             *_can;
  CanTxNodeT       *next;

  static constexpr uint32_t CMD_EXPIRE_MS = CAN_CMD_EXPIRE_MS;

  static inline CanTxNodeT *_head   = nullptr;
  static inline uint32_t    _num    = 0U;
  static inline bool        _frozen = false;

  CanTxNodeT(CanT &can_item, uint32_t can_id, uint8_t division_, uint32_t id_type) :
    txBuffer {},
    division(division_),
    _statu(Status::NOT_INIT),
    bufferRegister(0U),
    bufferUnsend(0U),
    _registered(false),
    _lock(nullptr),
    last_send_tick(0U),
    first_unsend_tick(0U),
    bufferFillTick {},
    bufferExpired(0xFFU),
    _stats {},
    _can(&can_item),
    next(nullptr)
  {
    txBuffer.id      = can_id;
    txBuffer.id_type = id_type;
    txBuffer.dlc     = 8U;
  }

  ~CanTxNodeT()
  {
    if (_lock != nullptr)
    {
      vSemaphoreDelete(_lock);
    }
  }

  Status init();
};

template <typename CanT>
Status CanTxNodeT<CanT>::init()
{
  if (_statu == Status::OK)
  {
    return Status::BUSY;
  }
  if (_statu != Status::NOT_INIT)
  {
    return _statu;
  }
  _lock = xSemaphoreCreateMutex();
  if (_lock == nullptr)
  {
    _statu = Status::FULL;
    sys_state.init_error(_statu);
    return _statu;
  }
  next        = _head;
  _head       = this;
  _registered = true;
  _statu      = Status::OK;
  return Status::OK;
}

template <typename CanT>
CanTxNodeT<CanT> *CanTxNodeT<CanT>::regist(CanT &can_item, uint32_t can_id, uint8_t division_, uint8_t slot, uint32_t id_type)
{
  if (_frozen)
  {
    // 运行态之后：链表已被 can_tx_task 使用，不能再改。只返回失败，不记初始化错误 ——
    // init_error() 会清掉 RUNNING 位，让整个系统的 CAN 接收分发停掉（对比评估 N16）
    return nullptr;
  }
  const uint32_t id_max = (id_type == FDCAN_EXTENDED_ID) ? 0x1FFFFFFFU : 0x7FFU;
  if ((id_type != FDCAN_STANDARD_ID && id_type != FDCAN_EXTENDED_ID) || can_id > id_max || division_ == 0U || division_ > 8U ||
      (8U % division_) != 0U || slot >= division_)
  {
    sys_state.init_error(Status::BAD_ARG);
    return nullptr;
  }

  for (CanTxNodeT *node = _head; node != nullptr; node = node->next)
  {
    if (node->_can == &can_item && node->txBuffer.id == can_id && node->txBuffer.id_type == id_type)
    {
      if (node->division == division_ && (node->bufferRegister & (1U << slot)) == 0U)
      {
        node->bufferRegister |= static_cast<uint8_t>(1U << slot);
        return node;
      }
      sys_state.init_error(Status::FULL);
      return nullptr;
    }
  }

  if (_num >= CAN_TX_NODE_MAX)
  {
    sys_state.init_error(Status::FULL);
    return nullptr;
  }
  CanTxNodeT *node = new (std::nothrow) CanTxNodeT(can_item, can_id, division_, id_type);
  if (node == nullptr)
  {
    sys_state.init_error(Status::FULL);
    return nullptr;
  }
  if (node->init() != Status::OK)
  {
    delete node;
    return nullptr;
  }
  node->bufferRegister = static_cast<uint8_t>(1U << slot);
  ++_num;
  return node;
}

template <typename CanT>
Status CanTxNodeT<CanT>::unregist(CanTxNodeT *node, uint8_t slot)
{
  if (node == nullptr || slot >= node->division || !node->_registered || node->_statu != Status::OK)
  {
    return Status::BAD_ARG;
  }
  const bool running = xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED;
  if (running && xSemaphoreTake(node->_lock, portMAX_DELAY) != pdTRUE)
  {
    return Status::BUSY;
  }
  const uint8_t mask   = static_cast<uint8_t>(1U << slot);
  Status        result = Status::BAD_ARG;
  if ((node->bufferRegister & mask) != 0U)
  {
    memset(node->txBuffer.data + slot * 8U / node->division, 0, 8U / node->division);
    node->bufferRegister &= static_cast<uint8_t>(~mask);
    node->bufferUnsend &= static_cast<uint8_t>(~mask);
    result = Status::OK;
  }
  if (running)
  {
    (void)xSemaphoreGive(node->_lock);
  }
  return result;
}

template <typename CanT>
uint32_t CanTxNodeT<CanT>::count_for(const CanT &can_item)
{
  uint32_t count = 0U;
  for (CanTxNodeT *node = _head; node != nullptr; node = node->next)
  {
    if (node->_can == &can_item)
    {
      ++count;
    }
  }
  return count;
}

template <typename CanT>
void CanTxNodeT<CanT>::init_send_ticks(CanT &can_item, TickType_t now)
{
  for (CanTxNodeT *node = _head; node != nullptr; node = node->next)
  {
    if (node->_can == &can_item)
    {
      node->last_send_tick = now;
    }
  }
}

template <typename CanT>
void CanTxNodeT<CanT>::service(CanT &can_item)
{
  const TickType_t now = xTaskGetTickCount();
  for (CanTxNodeT *node = _head; node != nullptr; node = node->next)
  {
    if (node->_can != &can_item || node->bufferRegister == 0U)
    {
      continue; // 槽位全部注销的节点不再发送（N35）
    }
    if (xSemaphoreTake(node->_lock, pdMS_TO_TICKS(1U)) != pdTRUE)
    {
      continue;
    }

    for (uint8_t slot = 0U; slot < node->division; ++slot)
    {
      const uint8_t mask = static_cast<uint8_t>(1U << slot);
      if ((node->bufferRegister & mask) != 0U && (node->bufferExpired & mask) == 0U &&
          now - node->bufferFillTick[slot] >= pdMS_TO_TICKS(CMD_EXPIRE_MS))
      {
        memset(node->txBuffer.data + slot * 8U / node->division, 0, 8U / node->division);
        node->bufferExpired |= mask;
        node->bufferUnsend &= static_cast<uint8_t>(~mask);
        node->_stats.expired = node->_stats.expired + 1U;
      }
    }

    const uint8_t active  = static_cast<uint8_t>(node->bufferRegister & ~node->bufferExpired); // 在被刷新的槽位
    const bool    fresh   = (node->bufferUnsend & node->bufferRegister) != 0U;
    const bool    ready   = active != 0U && (node->bufferUnsend & active) == active;
    const bool    timeout = fresh && now - node->first_unsend_tick >= pdMS_TO_TICKS(CAN_TX_GATHER_MS);
    const bool    due     = now - node->last_send_tick >= pdMS_TO_TICKS(CAN_TX_HEARTBEAT_MS);
    if ((ready || timeout || due) &&
        can_item.send_latest(node->txBuffer.id, node->txBuffer.data, node->txBuffer.dlc, node->txBuffer.id_type) == Status::OK)
    {
      node->_stats.sent = node->_stats.sent + 1U;
      if (!ready)
      {
        if (fresh)
        {
          node->_stats.partial_sends = node->_stats.partial_sends + 1U;
        }
        else
        {
          node->_stats.heartbeat_sends = node->_stats.heartbeat_sends + 1U;
        }
      }
      node->bufferUnsend   = 0U;
      node->last_send_tick = xTaskGetTickCount();
    }
    (void)xSemaphoreGive(node->_lock);
  }
}

template <typename CanT>
Status CanTxNodeT<CanT>::filldata(uint8_t data[], uint8_t slot)
{
  if (_statu != Status::OK || !_registered)
  {
    return _statu;
  }
  if (data == nullptr || slot >= division)
  {
    return Status::BAD_ARG;
  }
  if (xSemaphoreTake(_lock, pdMS_TO_TICKS(3U)) != pdTRUE)
  {
    return Status::BUSY;
  }
  if ((bufferRegister & (1U << slot)) == 0U)
  {
    (void)xSemaphoreGive(_lock);
    return Status::BAD_ARG;
  }
  const TickType_t now = xTaskGetTickCount();
  memcpy(txBuffer.data + slot * 8U / division, data, 8U / division);
  if (bufferUnsend == 0U)
  {
    first_unsend_tick = now; // 本轮第一个被填写的槽位
  }
  bufferUnsend |= static_cast<uint8_t>(1U << slot);
  bufferFillTick[slot] = now;
  bufferExpired &= static_cast<uint8_t>(~(1U << slot));
  (void)xSemaphoreGive(_lock);
  return Status::OK;
}

#endif // HAL_FDCAN_MODULE_ENABLED && CAN_TX_ENABLE

#endif // CAN_TX_NODE_HPP
