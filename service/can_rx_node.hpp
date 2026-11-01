#ifndef CAN_RX_NODE_HPP
#define CAN_RX_NODE_HPP

#include "stm32h7xx_hal.h" // IWYU pragma: keep（HAL 模块开关来自 stm32h7xx_hal_conf.h）
#include "hardware_config.h"

// 宿主工程没在 CubeMX 里开 FDCAN 或 CAN_RX_ENABLE=0 时，本模块整体不参与编译
#if defined(HAL_FDCAN_MODULE_ENABLED) && CAN_RX_ENABLE

#include "bsp_can.hpp"
#include "status.hpp"

#include <new>

/**
 * @brief 初始化阶段注册、运行阶段只读的 CAN 接收节点。
 * @tparam CanT 具体的 BspCanT<RX_DEPTH, TX_DEPTH> 类型。
 * @note 注册/注销仅由单一初始化任务执行；CAN 对象和 context 必须保持有效。
 */
template <typename CanT>
class CanRxNodeT
{
public:
  /** @note 在接收任务中同步调用，应短且不阻塞；rx 仅在回调期间有效。 */
  using Callback = Status (*)(void *context, const CanRxMsg &rx);

  /**
   * @brief 初始化阶段注册接收节点，相同 CAN、帧类型和 ID 不允许重复。
   * @return 成功返回节点；失败返回 nullptr，参数/冲突/资源错误记入系统初始化错误。
   */
  static CanRxNodeT *regist(CanT &can_item, uint32_t can_id, Callback callback, void *context = nullptr,
                            uint32_t id_type = FDCAN_STANDARD_ID);

  /** @brief 初始化阶段注销并销毁接收节点。 */
  static Status unregist(CanT &can_item, CanRxNodeT *node);

  /** @brief 运行态后冻结注册，防止任务运行期间修改链表。 */
  static void freeze() { _frozen = true; }

  /**
   * @brief 接收任务调用：把一帧分发给本类型 CAN 上匹配的节点。
   * @return true=有节点消费了这帧；false=没有节点认领。
   */
  static bool dispatch(CanT &can_item, const CanRxMsg &rx);

  /** @brief 指定 CAN 实例上已注册的接收节点数。 */
  static uint32_t count_for(const CanT &can_item);

private:
  CanT       *_can;
  uint32_t    _can_id;
  uint32_t    _id_type;
  Callback    _callback;
  void       *_context;
  CanRxNodeT *next;

  CanRxNodeT(CanT &can_item, uint32_t can_id, Callback callback, void *context, uint32_t id_type) :
    _can(&can_item), _can_id(can_id), _id_type(id_type), _callback(callback), _context(context), next(nullptr)
  {
  }

  ~CanRxNodeT() = default;

  static inline CanRxNodeT *_head   = nullptr;
  static inline uint32_t    _num    = 0U;
  static inline bool        _frozen = false;
};

template <typename CanT>
CanRxNodeT<CanT> *CanRxNodeT<CanT>::regist(CanT &can_item, uint32_t can_id, Callback callback, void *context,
                                           uint32_t id_type)
{
  if (_frozen)
  {
    return nullptr;
  }
  if (callback == nullptr || can_id > ((id_type == FDCAN_EXTENDED_ID) ? 0x1FFFFFFFU : 0x7FFU) ||
      (id_type != FDCAN_STANDARD_ID && id_type != FDCAN_EXTENDED_ID))
  {
    SysInitError(Status::BAD_ARG);
    return nullptr;
  }
  for (CanRxNodeT *node = _head; node != nullptr; node = node->next)
  {
    if (node->_can == &can_item && node->_can_id == can_id && node->_id_type == id_type)
    {
      SysInitError(Status::BUSY);
      return nullptr;
    }
  }
  if (_num >= CAN_RX_NODE_MAX)
  {
    SysInitError(Status::FULL);
    return nullptr;
  }
  CanRxNodeT *node = new (std::nothrow) CanRxNodeT(can_item, can_id, callback, context, id_type);
  if (node == nullptr)
  {
    SysInitError(Status::FULL);
    return nullptr;
  }
  node->next = _head;
  _head      = node;
  ++_num;
  return node;
}

template <typename CanT>
Status CanRxNodeT<CanT>::unregist(CanT &can_item, CanRxNodeT *node)
{
  if (node == nullptr || _frozen)
  {
    return Status::NOT_SUPPORTED;
  }
  CanRxNodeT **link = &_head;
  while (*link != nullptr)
  {
    if (*link == node)
    {
      if (node->_can != &can_item)
      {
        return Status::BAD_ARG;
      }
      *link = node->next;
      delete node;
      --_num;
      return Status::OK;
    }
    link = &(*link)->next;
  }
  return Status::BAD_ARG;
}

template <typename CanT>
bool CanRxNodeT<CanT>::dispatch(CanT &can_item, const CanRxMsg &rx)
{
  for (CanRxNodeT *node = _head; node != nullptr; node = node->next)
  {
    if (node->_can == &can_item && node->_can_id == rx.header.Identifier && node->_id_type == rx.header.IdType)
    {
      return node->_callback(node->_context, rx) == Status::OK;
    }
  }
  return false;
}

template <typename CanT>
uint32_t CanRxNodeT<CanT>::count_for(const CanT &can_item)
{
  uint32_t count = 0U;
  for (CanRxNodeT *node = _head; node != nullptr; node = node->next)
  {
    if (node->_can == &can_item)
    {
      ++count;
    }
  }
  return count;
}

#endif // HAL_FDCAN_MODULE_ENABLED && CAN_RX_ENABLE

#endif // CAN_RX_NODE_HPP
