#ifndef CAN_TX_NODE_HPP
#define CAN_TX_NODE_HPP

#include "stm32h7xx_hal.h" // IWYU pragma: keep（HAL 模块开关来自 stm32h7xx_hal_conf.h）
#include "hardware_config.h"

// 宿主工程没在 CubeMX 里开 FDCAN 或 CAN_TX_ENABLE=0 时，本模块整体不参与编译
#if defined(HAL_FDCAN_MODULE_ENABLED) && CAN_TX_ENABLE

#include "bsp_can.hpp"
#include "status.hpp"

#include <new>
#include <string.h>

/**
 * @brief CAN 发送节点：多个槽位聚合成一帧，按心跳 / 指令有效期发送。
 * @tparam CanT 具体的 BspCanT<RX_DEPTH, TX_DEPTH> 类型。
 */
template <typename CanT>
class CanTxNodeT
{
public:
  /**
   * @brief 初始化阶段注册发送节点。
   * @param can_item 所属 CAN 实例。
   * @param can_id 标准帧 ID。
   * @param division 一帧 8 字节划分的槽位数（1/2/4/8）。
   * @param slot 本节点占用的槽位。
   */
  static CanTxNodeT *regist(CanT &can_item, uint32_t can_id, uint8_t division = 1U, uint8_t slot = 0U);

  /** @brief 初始化阶段释放一个槽位；节点保留供其他槽位继续使用。 */
  static Status unregist(CanTxNodeT *node, uint8_t slot);

  /** @brief 运行态后冻结注册。 */
  static void freeze() { _frozen = true; }

  /** @brief 指定 CAN 实例上已注册的发送节点数。 */
  static uint32_t count_for(const CanT &can_item);

  /** @brief 发送任务启动时初始化各节点的 last_send_tick。 */
  static void init_send_ticks(CanT &can_item, TickType_t now);

  /** @brief 发送任务每 1 ms 调用：处理超时清零、保底心跳和槽位聚合发送。 */
  static void service(CanT &can_item);

  /** @brief 写入一个槽位的数据；返回 OK 表示已刷新该槽位。 */
  Status filldata(uint8_t data[], uint8_t slot = 0);

private:
  CanTxMsg txBuffer;
  uint8_t  division;
  Status   _statu;
  uint8_t  bufferRegister;
  uint8_t  bufferUnsend;
  bool     _registered;

  SemaphoreHandle_t _lock;
  TickType_t        last_send_tick;
  TickType_t        bufferFillTick[8];
  uint8_t           bufferExpired;
  uint32_t          expiredCount;

  CanT       *_can;
  CanTxNodeT *next;

  static constexpr uint32_t CMD_EXPIRE_MS = CAN_CMD_EXPIRE_MS;

  static inline CanTxNodeT *_head   = nullptr;
  static inline uint32_t    _num    = 0U;
  static inline bool        _frozen = false;

  CanTxNodeT(CanT &can_item, uint32_t can_id, uint8_t division) :
    txBuffer {},
    division(division),
    _statu(Status::NOT_INIT),
    bufferRegister(0U),
    bufferUnsend(0U),
    _registered(false),
    _lock(nullptr),
    last_send_tick(0U),
    bufferFillTick {},
    bufferExpired(0xFFU),
    expiredCount(0U),
    _can(&can_item),
    next(nullptr)
  {
    txBuffer.std_id = can_id;
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
    SysInitError(_statu);
    return _statu;
  }
  next        = _head;
  _head       = this;
  _registered = true;
  _statu      = Status::OK;
  return Status::OK;
}

template <typename CanT>
CanTxNodeT<CanT> *CanTxNodeT<CanT>::regist(CanT &can_item, uint32_t can_id, uint8_t division, uint8_t slot)
{
  if (_frozen || can_id > 0x7FFU || division == 0U || division > 8U || (8U % division) != 0U || slot >= division)
  {
    SysInitError(Status::BAD_ARG);
    return nullptr;
  }
  for (CanTxNodeT *node = _head; node != nullptr; node = node->next)
  {
    if (node->_can == &can_item && node->txBuffer.std_id == can_id)
    {
      if (node->division == division && (node->bufferRegister & (1U << slot)) == 0U)
      {
        node->bufferRegister |= static_cast<uint8_t>(1U << slot);
        return node;
      }
      SysInitError(Status::FULL);
      return nullptr;
    }
  }
  if (_num >= CAN_TX_NODE_MAX)
  {
    SysInitError(Status::FULL);
    return nullptr;
  }
  CanTxNodeT *node = new (std::nothrow) CanTxNodeT(can_item, can_id, division);
  if (node == nullptr)
  {
    SysInitError(Status::FULL);
    return nullptr;
  }
  const Status status = node->init();
  if (status != Status::OK)
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
  const uint8_t mask = static_cast<uint8_t>(1U << slot);
  Status result = Status::BAD_ARG;
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
    if (node->_can != &can_item || xSemaphoreTake(node->_lock, pdMS_TO_TICKS(1U)) != pdTRUE)
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
        ++node->expiredCount;
      }
    }
    const bool ready = node->bufferRegister != 0U && node->bufferUnsend == node->bufferRegister;
    const bool due   = now - node->last_send_tick >= pdMS_TO_TICKS(CAN_TX_HEARTBEAT_MS);
    if ((ready || due) && can_item.send(node->txBuffer.std_id, node->txBuffer.data) == Status::OK)
    {
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
  memcpy(txBuffer.data + slot * 8U / division, data, 8U / division);
  bufferUnsend |= static_cast<uint8_t>(1U << slot);
  bufferFillTick[slot] = xTaskGetTickCount();
  bufferExpired &= static_cast<uint8_t>(~(1U << slot));
  (void)xSemaphoreGive(_lock);
  return Status::OK;
}

#endif // HAL_FDCAN_MODULE_ENABLED && CAN_TX_ENABLE

#endif // CAN_TX_NODE_HPP
