#include "service/system/state_tree.hpp"

#include "board_instances.hpp"
#include "bsp/bsp_series.hpp" // IWYU pragma: keep（HAL 模块开关、ICBSP_CAN_MODULE_ENABLED）
#include "service/system/system.hpp" // sys_state

#include <stdint.h>

// 三张清单按 HAL 模块开关取舍：没开的外设没有实例类型，清单也不能展开
#if ICBSP_CAN_MODULE_ENABLED
#  define TREE_CAN(X) ICBSP_CAN_INSTANCES(X)
#else
#  define TREE_CAN(X)
#endif
#ifdef HAL_UART_MODULE_ENABLED
#  define TREE_UART(X) ICBSP_UART_INSTANCES(X)
#else
#  define TREE_UART(X)
#endif
#ifdef HAL_SPI_MODULE_ENABLED
#  define TREE_SPI(X) ICBSP_SPI_INSTANCES(X)
#else
#  define TREE_SPI(X)
#endif

namespace
{
#define TREE_COUNT_ONE(name) +1U
constexpr unsigned NODE_COUNT = 0U TREE_CAN(TREE_COUNT_ONE) TREE_UART(TREE_COUNT_ONE) TREE_SPI(TREE_COUNT_ONE);
#undef TREE_COUNT_ONE
constexpr unsigned NODE_SLOTS = (NODE_COUNT > 0U) ? NODE_COUNT : 1U; // 零长数组不合法

/** @brief 一个实例按错误码归类的计数和（只增不减，比较时用 != 处理回绕） */
struct ErrCounts
{
  uint32_t io;
  uint32_t full;
  uint32_t timeout;
  uint32_t busy;
};

EventState nodes[NODE_SLOTS];     ///< 总线节点，顺序 = CAN 清单、UART 清单、SPI 清单
ErrCounts  last_counts[NODE_SLOTS]; ///< 上一次 poll 时的计数

template <class Can> ErrCounts can_counts(const Can &can)
{
  const auto &d = can.diagnostics;
  return {d.bus_off_events,
          d.rx_dropped + d.rx_lost + d.rx_return_dropped + d.tx_dropped + d.tx_buf_full + d.tx_flushed,
          d.tx_stall_recover,
          0U};
}

template <class Uart> ErrCounts uart_counts(const Uart &uart)
{
  const auto rx = uart.get_rx_diag();
  const auto tx = uart.get_tx_diag();
  return {rx.hw_error_cnt + rx.error_cnt + tx.error_cnt, rx.drop_bytes + tx.drop_bytes, tx.stall_recover_cnt, 0U};
}

template <class Spi> ErrCounts spi_counts(const Spi &spi)
{
  const auto d = spi.get_diag();
  return {d.error_cnt, 0U, d.timeout_cnt, d.lock_timeout_cnt};
}

/** @brief 某一类计数涨了就在节点上记对应的错误码（向上传到 sys_state，不动运行位） */
void check(unsigned index, const ErrCounts &now)
{
  ErrCounts &last = last_counts[index];
  if (now.io != last.io)
  {
    (void)nodes[index].error(Status::IO_ERROR);
  }
  if (now.full != last.full)
  {
    (void)nodes[index].error(Status::FULL);
  }
  if (now.timeout != last.timeout)
  {
    (void)nodes[index].error(Status::TIMEOUT);
  }
  if (now.busy != last.busy)
  {
    (void)nodes[index].error(Status::BUSY);
  }
  last = now;
}

/** @brief 取全部实例的当前计数，按清单顺序交给 fn(index, counts) */
template <class Fn> void for_each_counts(Fn fn)
{
  unsigned index = 0U;
#define X(name) fn(index++, can_counts(name));
  TREE_CAN(X)
#undef X
#define X(name) fn(index++, uart_counts(name));
  TREE_UART(X)
#undef X
#define X(name) fn(index++, spi_counts(name));
  TREE_SPI(X)
#undef X
  (void)index;
}
} // namespace

EventState *bus_state(const void *bsp_instance)
{
  unsigned index = 0U;
#define X(name)                                         \
  if (static_cast<const void *>(&name) == bsp_instance) \
  {                                                     \
    return &nodes[index];                               \
  }                                                     \
  ++index;
  TREE_CAN(X)
  TREE_UART(X)
  TREE_SPI(X)
#undef X
  (void)index;
  return nullptr;
}

unsigned bus_state_count(void)
{
  return NODE_COUNT;
}

Status state_tree_init(void)
{
  for (unsigned i = 0U; i < NODE_COUNT; ++i)
  {
    const Status st = nodes[i].init();
    if (st != Status::OK)
    {
      return st;
    }
    (void)nodes[i].set_parent(sys_state); // 只会在重复调用时返回 BAD_ARG（父指针已设），无害
  }
  return Status::OK;
}

void state_tree_start(void)
{
  for (unsigned i = 0U; i < NODE_COUNT; ++i)
  {
    // 有初始化失败记录（设备 init_error 传上来）时返回 NOT_SUPPORTED；sys_state 同样带着失败位，由 system_start() 停机
    (void)nodes[i].complete_init();
  }
  // 基线：初始化期的计数（例如 init 时启动接收失败后重试成功）不算运行期错误
  for_each_counts([](unsigned index, const ErrCounts &now) { last_counts[index] = now; });
}

void state_tree_poll(void)
{
  for_each_counts([](unsigned index, const ErrCounts &now) { check(index, now); });
}
