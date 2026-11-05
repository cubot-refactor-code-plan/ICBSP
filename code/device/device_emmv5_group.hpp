/**
 * @file device_emmv5_group.hpp
 * @author Rh
 * @brief EmmV5 多电机组（单串口多电机 + MMCL 多机命令）
 * @version 1.2
 * @date 2026-10-23
 *
 * @copyright Copyright (c) 2026
 *
 * @note 一个实例 = 一路串口 + 若干个电机地址：按地址分发接收、把多条子命令攒成
 *       MMCL 总帧一次发出、汇总到位位图 / 位置 / 状态、维护每台电机的在线节点。
 *       扫描器与超时模型、MMCL 容量算术、线程与总线锁约束见 `device_emmv5_group.md`。
 *
 * @note **本类不创建任务**：字节搬运靠外部任务周期调 `update()`（建议 10 ms），
 *       与 `bsp_can` / `device_r9ds` / `dji_motor` 保持一致。
 *
 * @warning 同一路串口只允许一个任务调 `update()`（接收缓冲区只有一个消费者）；
 *          发送接口自带半双工总线锁，多步 MMCL 事务用 `lock_bus()` / `unlock_bus()` 包住。
 */

#ifndef __DEVICE_EMMV5_GROUP_HPP__
#define __DEVICE_EMMV5_GROUP_HPP__

#include "bsp_series.hpp" // IWYU pragma: keep（HAL 模块开关 / 系列桥接）

// 宿主工程没在 CubeMX 里开 UART（HAL_UART_MODULE_ENABLED 未定义）时，本模块整体不参与编译
#ifdef HAL_UART_MODULE_ENABLED

#include "bsp_uart.hpp"
#include "emmv5_protocol.hpp"
#include "online_check.hpp"
#include "status.hpp"

#include "FreeRTOS.h" // IWYU pragma: keep
#include "semphr.h"   // IWYU pragma: keep（总线互斥锁）
#include "task.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h> // memcpy


/**
 * @brief 单串口多电机组
 *
 * @note 地址表在构造时一次定完，运行期不增删；地址重复或为 0 时 init() 返回 BAD_ARG。
 * @note 未 init() 时所有发送接口返回 NOT_INIT，update() 返回 0。
 */
template <typename Uart>
class DeviceEmmv5GroupT
{
private:
  /* ==================== 内部常量 ==================== */

  static constexpr size_t   UART_BUFFER_SIZE  = 128U; ///< 与 board_cfg.cpp 的 DMA 串口发送缓冲区一致
  static constexpr size_t   MAX_MOTORS        = 8U;   ///< 组内最大电机数
  static constexpr uint16_t ONLINE_TIMEOUT_MS = 100U; ///< 超过 100 ms 无有效应答判离线

  ///< 单帧入队等待上限：921600 下 13 B ≈ 0.14 ms，20 ms 足够；缓冲卡死时返回 TIMEOUT，不无限挂住调用任务
  static constexpr uint32_t TX_TIMEOUT_MS = 20U;

  ///< MMCL 子命令总字节上限：整帧不能超过串口发送缓冲
  static constexpr size_t MULTI_PAYLOAD_MAX = UART_BUFFER_SIZE - emmv5::MULTI_OVERHEAD;

  /* ==================== */
public:
  /* ==================== 类型与配置 ==================== */

  /**
   * @brief 组配置
   *
   * @note 聚合体：推荐用指定初始化器写字，例如
   *       `{.uart = bsp_uart2, .addr = {1, 2, 3}}`；也兼容位置写法 `{bsp_uart2, {1, 2}}`。
   * @note 地址表以 **0 结尾**（0 不是合法地址，合法范围 1~255）：前导非 0 项就是组内电机，
   *       遇到第一个 0 即结束，因此**不能把 0 夹在中间**（`{1, 0, 3}` 只会识别出 1 台）。
   */
  struct Config
  {
    Uart    &uart;                  ///< bsp_uart 实例引用
    uint8_t  addr[MAX_MOTORS] = {}; ///< 地址表（1~255，0 = 结尾）
  };

  /* ==================== */
  /* ==================== 构造与析构 ==================== */

  /**
   * @brief 构造函数
   * @param cfg 组配置
   * @note 只做赋值；地址校验在 init() 里做。
   */
  explicit DeviceEmmv5GroupT(const Config &cfg);

  /** @brief 释放总线锁；各电机的 Online 节点随成员析构自动注销 */
  ~DeviceEmmv5GroupT();

  // 内部有接收窗口与 MMCL 缓冲，复制会让两份状态互相打架
  DeviceEmmv5GroupT(const DeviceEmmv5GroupT &)            = delete;
  DeviceEmmv5GroupT &operator=(const DeviceEmmv5GroupT &) = delete;
  DeviceEmmv5GroupT(DeviceEmmv5GroupT &&)                 = delete;
  DeviceEmmv5GroupT &operator=(DeviceEmmv5GroupT &&)      = delete;

  /* ==================== */
  /* ==================== 生命周期与查询 ==================== */

  /**
   * @brief 校验地址表、复位接收状态，并创建总线锁
   * @return OK=可用；BAD_ARG=地址表为空、超过 MAX_MOTORS、地址为 0 或地址重复；
   *         FULL=总线锁创建失败（堆不足）
   * @note 幂等：重复 init 不会重复创建任务。
   * @warning 应在调度器启动前调用；运行期重复 init() 会复位扫描器，可能丢掉正在接收的半帧。
   */
  Status init();

  ///< 是否已成功 init
  bool is_ready() const
  {
    return _ready;
  }

  ///< 组内电机数（未 init 时返回 0）
  size_t get_count() const
  {
    return _ready ? _count : 0U;
  }

  /* ==================== */
  /* ==================== 半双工总线锁 ==================== */

  /**
   * @brief 占用半双工总线（递归锁）
   * @param timeout_ms 最长等待时间
   * @return OK=已持有；NOT_INIT=未 init；TIMEOUT=超时
   *
   * @note RS485 是半双工：同一时刻只允许一个“会发字节的操作”在总线上。
   *       所有发送接口内部都会自己取这把锁，所以单帧命令不需要手动加锁；
   *       只有“多条子命令拼一帧”“发完命令再等应答”这类**多步事务**需要手动包。
   * @note 递归锁：同一任务可以重复 lock_bus 而不自锁，但必须 unlock 同样次数。
   */
  Status lock_bus(uint32_t timeout_ms = 200U);

  ///< 释放总线（与 lock_bus 成对，必须同任务）
  void unlock_bus();

  /* ==================== */
  /* ==================== 直接命令 ==================== */

  ///< 使能/关闭某个电机
  Status enable(uint8_t addr, bool on);

  ///< 速度模式（rpm 0~5000，accel 0~255）
  Status set_velocity(uint8_t addr, emmv5::Direction dir, uint16_t rpm, uint8_t accel);

  ///< 位置模式（pulses 带符号；绝对模式按 32 位补码写入）
  Status set_position(uint8_t addr, emmv5::Direction dir, uint16_t rpm, uint8_t accel, int32_t pulses, emmv5::PositionMode mode);

  ///< 取最近一次 MMCL 总帧的字节（调试用；未发送过时长度 0）
  const uint8_t *last_multi_frame(size_t &len) const;

  ///< 设置快速位置模式的运动参数
  Status set_quick_position_params(uint8_t addr, uint16_t rpm, uint8_t accel, emmv5::PositionMode mode);

  ///< 快速位置控制
  Status set_quick_position(uint8_t addr, int32_t pulses);

  ///< 立即停止某个电机
  Status stop(uint8_t addr);

  ///< 广播立即停止（地址 0）
  Status stop_all();

  ///< 将某个电机当前位置清零
  Status zero_position(uint8_t addr);

  ///< 解除某个电机的堵转保护
  Status clear_protection(uint8_t addr);

  ///< 修改控制模式（0=关闭 1=开环 2=闭环 3=脉冲/方向）
  Status set_control_mode(uint8_t addr, bool store, uint8_t mode);

  /**
   * @brief 读取一次系统参数
   * @note 本类只解析 `CurrentPosition`（0x36）与 `StatusFlag`（0x3A）；
   *       其余参数的应答会被 update() 丢弃。
   */
  Status read(uint8_t addr, emmv5::SystemParam param);

  ///< 请求一次 0x36 实时位置
  Status request_position(uint8_t addr);

  ///< 请求一次 0x3A 状态标志
  Status request_status(uint8_t addr);

  /* ==================== */
  /* ==================== MMCL 多机命令 ==================== */

  ///< 清空 MMCL 缓冲
  void multi_clear();

  /**
   * @brief 把 MMCL 缓冲封成一帧并发送
   * @param target 总帧地址，默认广播 0
   * @return OK=已发送并清空缓冲；NOT_INIT=未 init；BAD_ARG=组帧失败；
   *         TIMEOUT/IO_ERROR=发送失败（缓冲同样清空，避免重发“半帧 + 完整帧”）
   *
   * @note 缓冲为空时直接返回 OK，不占用串口。
   */
  Status multi_send(uint8_t target = emmv5::BROADCAST);

  ///< 追加「使能」子命令
  Status queue_enable(uint8_t addr, bool on);

  ///< 追加「速度模式」子命令
  Status queue_velocity(uint8_t addr, emmv5::Direction dir, uint16_t rpm, uint8_t accel);

  ///< 追加「位置模式」子命令
  Status queue_position(uint8_t addr, emmv5::Direction dir, uint16_t rpm, uint8_t accel, int32_t pulses, emmv5::PositionMode mode);

  ///< 追加「快速位置参数」子命令
  Status queue_quick_position_params(uint8_t addr, uint16_t rpm, uint8_t accel, emmv5::PositionMode mode);

  ///< 追加「快速位置」子命令
  Status queue_quick_position(uint8_t addr, int32_t pulses);

  ///< 追加「立即停止」子命令
  Status queue_stop(uint8_t addr);

  /* ==================== */
  /* ==================== 接收 ==================== */

  /**
   * @brief 取走累计的到位位图并清零
   * @return bit i = 下标 i 的电机自上次取走后至少到位过一次
   */
  uint32_t get_in_position();

  /**
   * @brief 查询某个电机是否累计到位（不清零）
   * @param index 下标（0 ~ count()-1，顺序与 Config 里的地址表一致）
   * @return true=自上次 get_in_position() 后至少到位过一次
   */
  bool in_position(size_t index) const;

  /**
   * @brief 取走某台电机最近一次 0x36 位置
   * @param addr 电机地址
   * @param out  位置，单位 counts（编码器一圈的计数，带符号）
   * @return true=有新数据；false=没有新数据或地址不在组内
   *
   * @note 读后清有效标志；数值与标志在短临界区内一起拷贝，允许在其他任务调用。
   */
  bool get_position(uint8_t addr, int32_t &out);

  /**
   * @brief 取走某台电机最近一次 0x3A 状态字节
   * @param addr 电机地址
   * @param out  状态字节（位定义见说明书 6.3.4）
   * @return true=有新数据；false=没有新数据或地址不在组内
   */
  bool get_status(uint8_t addr, uint8_t &out);

  /**
   * @brief 某台电机最近 100 ms 内是否收到过有效应答
   * @param addr 电机地址
   * @return true=在线；false=超时或地址不在组内
   *
   * @note 计时由 sys_task 里的 Online::update() 推进；查询周期应小于 100 ms。
   */
  bool is_online(uint8_t addr) const;

  /**
   * @brief 把串口里攒的字节读完并分发给组内电机
   * @param timeout_ms 首块等待时间：0 = 非阻塞，N = 最多等 N ms（超时即返回）
   * @return 本次命中的电机位图：bit i = 下标 i 的电机收到到位帧
   *
   * @note **必须由外部任务周期调用**，且整个组只允许一个任务调：
   *       接收缓冲区只能有一个消费者。本类不建任务、不隐藏优先级。
   * @note 只有**第一块**用 timeout_ms 等；后续块一律 0，把已到的字节一次排空。
   */
  uint32_t update(uint32_t timeout_ms = 0U);

  /* ==================== */
private:
  /* ==================== 内部实现 ==================== */

  /**
   * @brief 单个电机的接收状态
   *
   * @note 每个 Motor 自带三个扫描器和一个 Online 节点；节点在静态构造时自动挂到
   *       Online 链表，在线计时由 sys_task 的 Online::update() 统一推进。
   */
  struct Motor
  {
    Motor() : online(ONLINE_TIMEOUT_MS)
    {
    }

    uint8_t                  addr = 0;               ///< 电机地址
    emmv5::InPositionScanner inpos_scanner;          ///< 到位帧识别
    emmv5::PositionScanner   position_scanner;       ///< 0x36 位置应答
    emmv5::StatusScanner     status_scanner;         ///< 0x3A 状态应答
    Online                   online;                 ///< 在线判定节点
    bool                     in_position    = false; ///< 累计到位标志
    int32_t                  position       = 0;     ///< 最近一次位置
    uint8_t                  status         = 0;     ///< 最近一次状态
    bool                     position_valid = false; ///< 位置是否未被取走
    bool                     status_valid   = false; ///< 状态是否未被取走
  };

  static constexpr size_t RX_CHUNK_SIZE = 16U; ///< 单次从流缓冲区读多少字节
  static constexpr size_t RX_MAX_CHUNKS = 8U;  ///< update() 每次最多读几块

  ///< rpm 合法范围（说明书：0~5000）
  static bool _valid_rpm(uint16_t rpm);

  ///< 方向枚举合法性
  static bool _valid_direction(emmv5::Direction dir);

  ///< 发布位置（短临界区）
  void _publish_position(size_t index, int32_t position);

  ///< 发布状态（短临界区）
  void _publish_status(size_t index, uint8_t status);

  ///< 设置累计到位标志（短临界区）
  void _publish_in_position(size_t index);

  ///< 地址 → 下标；不在组内返回 _count
  size_t index_of(uint8_t addr) const;

  ///< 地址是否在组内
  bool _is_member(uint8_t addr) const;

  ///< 直接发送一帧（校验未初始化、地址、帧有效性）
  Status _send_for(uint8_t addr, const emmv5::Frame &frame);

  ///< 追加一帧到 MMCL 缓冲（校验未初始化、地址、帧有效性、空间）
  Status _queue_for(uint8_t addr, const emmv5::Frame &frame);

  ///< 把一块字节分发给组内所有电机
  uint32_t feed_rx(const uint8_t *data, size_t size);

  /* ==================== */
  /* ==================== 成员变量 ==================== */

  Uart  &_uart;              ///< 被驱动的串口（不拥有）
  Motor  _motor[MAX_MOTORS]; ///< 组内电机状态
  size_t _count = 0;         ///< 组内电机数
  bool   _ready = false;     ///< init() 是否成功

  uint8_t _multi[MULTI_PAYLOAD_MAX + emmv5::MULTI_OVERHEAD] = {}; ///< MMCL 总帧缓冲
  size_t  _multi_payload                                    = 0;  ///< 已装载的子命令字节数

  uint8_t _last_frame[40] = {}; ///< 最近一次发出的 MMCL 帧（调试）
  size_t  _last_frame_len = 0;  ///< 其长度

  SemaphoreHandle_t _bus_lock = nullptr; ///< 半双工总线递归锁；init() 创建

  /* ==================== */
};

/* ==================== 成员函数定义（只有头文件：任意 CAN / UART 类型都能实例化，评审 N23） ==================== */


/* ==================== 模板实例化 ==================== */


/* ==================== */
/* ==================== 构造与生命周期 ==================== */

/** @brief 只保存串口引用与地址表，地址个数由地址表的 0 结尾推导 */
template <typename Uart>
DeviceEmmv5GroupT<Uart>::DeviceEmmv5GroupT(const Config &cfg) : _uart(cfg.uart)
{
  // 只把地址表拷进来；重复 / 为 0 的地址交给 init() 拒绝，
  // 总线锁也放到 init() 里建（构造函数里不碰 FreeRTOS API）。
  for (size_t i = 0; i < MAX_MOTORS; i++)
  {
    _motor[i].addr = cfg.addr[i];
  }

  // 地址 0 不是合法地址，所以「前导非 0」就是组内电机数；MAX_MOTORS 天然封顶
  _count = 0;
  while (_count < MAX_MOTORS && cfg.addr[_count] != 0U)
  {
    ++_count;
  }
}

/** @brief 校验地址表、复位接收状态与 MMCL 缓冲，并创建总线锁 */
template <typename Uart>
Status DeviceEmmv5GroupT<Uart>::init()
{
  if (_count == 0)
  {
    return Status::BAD_ARG;
  }

  // 地址必须 1~255 且不能重复，否则分发和到位判断都会串台
  for (size_t i = 0; i < _count; i++)
  {
    if (_motor[i].addr == 0)
    {
      return Status::BAD_ARG;
    }

    for (size_t j = 0; j < i; j++)
    {
      if (_motor[j].addr == _motor[i].addr)
      {
        return Status::BAD_ARG;
      }
    }
  }

  // 各电机的复位值与 3 个协议扫描器的初值全部在 device_emmv5_group.hpp / emmv5_protocol.hpp
  // 的成员声明里（NSDMI），这里不再逐字段清一遍

  // 半双工总线锁：必须在 _ready 置位、第一次发送之前就存在。
  // 递归锁允许同一任务在“组装 MMCL 事务”里再调单帧发送接口而不自锁。
  if (_bus_lock == nullptr)
  {
    _bus_lock = xSemaphoreCreateRecursiveMutex();

    if (_bus_lock == nullptr)
    {
      return Status::FULL; // 堆不够
    }
  }

  _ready = true;

  return Status::OK;
}

/** @brief 释放总线锁（全局实例不会走到这里，只为测试 / 局部对象不泄漏） */
template <typename Uart>
DeviceEmmv5GroupT<Uart>::~DeviceEmmv5GroupT()
{
  if (_bus_lock != nullptr)
  {
    vSemaphoreDelete(_bus_lock);
    _bus_lock = nullptr;
  }
}

/* ==================== */
/* ==================== 私有查找与发送 ==================== */

/** @brief 地址 → 下标；不在组内返回 _count */
template <typename Uart>
size_t DeviceEmmv5GroupT<Uart>::index_of(uint8_t addr) const
{
  // 约定：找不到返回 _count，调用方一律用 `index >= _count` 判非法。
  for (size_t i = 0; i < _count; i++)
  {
    if (_motor[i].addr == addr)
    {
      return i;
    }
  }

  return _count;
}

/** @brief 地址是否在组内 */
template <typename Uart>
bool DeviceEmmv5GroupT<Uart>::_is_member(uint8_t addr) const
{
  return index_of(addr) < _count;
}


/* ==================== */
/* ==================== 总线互斥 ==================== */

/** @brief 取半双工总线锁（递归；timeout_ms = 0 相当于 try-lock） */
template <typename Uart>
Status DeviceEmmv5GroupT<Uart>::lock_bus(uint32_t timeout_ms)
{
  if (!_ready || (_bus_lock == nullptr))
  {
    return Status::NOT_INIT;
  }

  // timeout_ms = 0 相当于 try-lock；递归锁让同一任务可以层层嵌套而不会自锁
  if (xSemaphoreTakeRecursive(_bus_lock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
  {
    return Status::TIMEOUT;
  }

  return Status::OK;
}

/** @brief 释放总线锁，必须与 lock_bus() 成对 */
template <typename Uart>
void DeviceEmmv5GroupT<Uart>::unlock_bus()
{
  // 与 lock_bus() 成对，必须同任务调用；多给/少给会让递归计数错位
  if (_bus_lock != nullptr)
  {
    (void)xSemaphoreGiveRecursive(_bus_lock);
  }
}

/** @brief 直接发一帧：校验地址与帧有效性 → 加锁 → 入队 → 解锁 */
template <typename Uart>
Status DeviceEmmv5GroupT<Uart>::_send_for(uint8_t addr, const emmv5::Frame &frame)
{
  if (!_ready)
  {
    return Status::NOT_INIT;
  }

  if (!_is_member(addr) || !frame.valid())
  {
    // frame 有效性由协议层保证：未封口 / 已溢出 / 空帧一律不发，避免把截断帧送上总线
    return Status::BAD_ARG;
  }

  // 半双工：整帧发送期间独占总线，其他任务不得插入任何字节
  const Status ls = lock_bus();
  if (ls != Status::OK)
  {
    return ls;
  }

  // 短帧入队：正常几 ms 内就排空，卡死时靠 TX_TIMEOUT_MS 退出
  const Status status = _uart.send(frame.data, frame.size, pdMS_TO_TICKS(TX_TIMEOUT_MS), nullptr);

  unlock_bus();
  return status;
}

/** @brief 把一帧追加到 MMCL 缓冲；放不下返回 FULL，不留半条子命令 */
template <typename Uart>
Status DeviceEmmv5GroupT<Uart>::_queue_for(uint8_t addr, const emmv5::Frame &frame)
{
  if (!_ready)
  {
    return Status::NOT_INIT;
  }

  if (!_is_member(addr) || !frame.valid())
  {
    // 截断帧不能攒进总帧：否则整帧 MMCL 会带着半条子命令发出去
    return Status::BAD_ARG;
  }

  // 先保证整帧放得下，再拷贝；绝不留下半条子命令
  if (_multi_payload + frame.size > MULTI_PAYLOAD_MAX)
  {
    return Status::FULL;
  }

  // 拼帧也是总线事务的一部分：跟多任务下其他子命令串行
  const Status ls = lock_bus();
  if (ls != Status::OK)
  {
    return ls;
  }

  memcpy(_multi + emmv5::MULTI_PREFIX_SIZE + _multi_payload, frame.data, frame.size);
  _multi_payload += frame.size;

  unlock_bus();
  return Status::OK;
}


/* ==================== */
/* ==================== 直接命令 ==================== */

/** @brief 使能 / 关闭一台电机（无应答命令） */
template <typename Uart>
Status DeviceEmmv5GroupT<Uart>::enable(uint8_t addr, bool on)
{
  return _send_for(addr, emmv5::enable(addr, on));
}

/** @brief 速度模式 */
template <typename Uart>
Status DeviceEmmv5GroupT<Uart>::set_velocity(uint8_t addr, emmv5::Direction dir, uint16_t rpm, uint8_t accel)
{
  if (!_valid_direction(dir) || !_valid_rpm(rpm))
  {
    return Status::BAD_ARG;
  }

  return _send_for(addr, emmv5::velocity(addr, dir, rpm, accel));
}

/** @brief 位置模式（方向字节 + 正幅值；相对/绝对由 mode 决定） */
template <typename Uart>
Status DeviceEmmv5GroupT<Uart>::set_position(uint8_t addr, emmv5::Direction dir, uint16_t rpm, uint8_t accel, int32_t pulses, emmv5::PositionMode mode)
{
  if (!_valid_direction(dir) || !_valid_rpm(rpm))
  {
    return Status::BAD_ARG;
  }

  return _send_for(addr, emmv5::position(addr, dir, rpm, accel, pulses, mode));
}

/** @brief 只设快速位置参数，不产生运动 */
template <typename Uart>
Status DeviceEmmv5GroupT<Uart>::set_quick_position_params(uint8_t addr, uint16_t rpm, uint8_t accel, emmv5::PositionMode mode)
{
  if (!_valid_rpm(rpm))
  {
    return Status::BAD_ARG;
  }

  return _send_for(addr, emmv5::quick_position_params(addr, rpm, accel, mode));
}

/** @brief 快速位置（没有方向字节，方向靠脉冲符号） */
template <typename Uart>
Status DeviceEmmv5GroupT<Uart>::set_quick_position(uint8_t addr, int32_t pulses)
{
  return _send_for(addr, emmv5::quick_position(addr, pulses));
}

/** @brief 立即停止一台电机 */
template <typename Uart>
Status DeviceEmmv5GroupT<Uart>::stop(uint8_t addr)
{
  return _send_for(addr, emmv5::stop(addr));
}

/** @brief 广播立即停止（地址 0） */
template <typename Uart>
Status DeviceEmmv5GroupT<Uart>::stop_all()
{
  if (!_ready)
  {
    return Status::NOT_INIT;
  }

  const emmv5::Frame frame = emmv5::stop(emmv5::BROADCAST);
  if (!frame.valid())
  {
    return Status::BAD_ARG;
  }

  const Status ls = lock_bus();
  if (ls != Status::OK)
  {
    return ls;
  }

  const Status status = _uart.send(frame.data, frame.size, pdMS_TO_TICKS(TX_TIMEOUT_MS), nullptr);

  unlock_bus();
  return status;
}

/** @brief 当前位置清零（编码器零位 = 上电位置） */
template <typename Uart>
Status DeviceEmmv5GroupT<Uart>::zero_position(uint8_t addr)
{
  return _send_for(addr, emmv5::zero_position(addr));
}

/** @brief 解除堵转保护 */
template <typename Uart>
Status DeviceEmmv5GroupT<Uart>::clear_protection(uint8_t addr)
{
  return _send_for(addr, emmv5::clear_protection(addr));
}

/** @brief 改控制模式（0 关闭 / 1 开环 / 2 闭环 / 3 脉冲方向） */
template <typename Uart>
Status DeviceEmmv5GroupT<Uart>::set_control_mode(uint8_t addr, bool store, uint8_t mode)
{
  if (mode > 3U)
  {
    return Status::BAD_ARG;
  }

  return _send_for(addr, emmv5::set_control_mode(addr, store, mode));
}

/** @brief 读一次系统参数（有应答，由 update() 解析） */
template <typename Uart>
Status DeviceEmmv5GroupT<Uart>::read(uint8_t addr, emmv5::SystemParam param)
{
  return _send_for(addr, emmv5::read(addr, param));
}

/** @brief 请求一次 0x36 实时位置 */
template <typename Uart>
Status DeviceEmmv5GroupT<Uart>::request_position(uint8_t addr)
{
  return read(addr, emmv5::SystemParam::CurrentPosition);
}

/** @brief 请求一次 0x3A 状态标志 */
template <typename Uart>
Status DeviceEmmv5GroupT<Uart>::request_status(uint8_t addr)
{
  return read(addr, emmv5::SystemParam::StatusFlag);
}


/* ==================== */
/* ==================== MMCL 多机命令 ==================== */

/** @brief 取最近一次发出的 MMCL 字节（诊断用，长度写入 len） */
template <typename Uart>
const uint8_t *DeviceEmmv5GroupT<Uart>::last_multi_frame(size_t &len) const
{
  len = _last_frame_len;
  return _last_frame;
}

/** @brief 清空 MMCL 缓冲 */
template <typename Uart>
void DeviceEmmv5GroupT<Uart>::multi_clear()
{
  // 清缓冲同样要排队：否则可能清掉别的任务刚攒进去的子命令
  if (lock_bus() == Status::OK)
  {
    _multi_payload = 0;
    unlock_bus();
  }
}

/** @brief 把缓冲封成 MMCL 总帧并发送；无论成败都清空缓冲 */
template <typename Uart>
Status DeviceEmmv5GroupT<Uart>::multi_send(uint8_t target)
{
  if (!_ready)
  {
    return Status::NOT_INIT;
  }

  if (_multi_payload == 0)
  {
    return Status::OK;
  }

  const size_t total = emmv5::build_multi_frame(target, _multi + emmv5::MULTI_PREFIX_SIZE, _multi_payload, _multi, sizeof(_multi));
  if (total == 0)
  {
    return Status::BAD_ARG; // 理论上不会发生：_queue_for 已经保证整帧放得下
  }

  // 组帧 + 发送 + 清缓冲必须是一个不可分割的总线事务
  const Status ls = lock_bus();
  if (ls != Status::OK)
  {
    return ls;
  }

  const Status status = _uart.send(_multi, total, pdMS_TO_TICKS(TX_TIMEOUT_MS), nullptr);

  // 记下实际发出的字节，供控制台 dump 对账
  _last_frame_len = (total < sizeof(_last_frame)) ? total : sizeof(_last_frame);
  memcpy(_last_frame, _multi, _last_frame_len);

  // 成功就清空；超时可能已经有一部分字节进了流缓冲，不能把整帧再发一遍，
  // 否则总线上会出现“半帧 + 完整帧”。这里统一清空，由上层决定是否重建命令。
  _multi_payload = 0;

  unlock_bus();
  return status;
}

/** @brief 向 MMCL 缓冲追加「使能」子命令 */
template <typename Uart>
Status DeviceEmmv5GroupT<Uart>::queue_enable(uint8_t addr, bool on)
{
  return _queue_for(addr, emmv5::enable(addr, on));
}

/** @brief 追加「速度模式」子命令 */
template <typename Uart>
Status DeviceEmmv5GroupT<Uart>::queue_velocity(uint8_t addr, emmv5::Direction dir, uint16_t rpm, uint8_t accel)
{
  if (!_valid_direction(dir) || !_valid_rpm(rpm))
  {
    return Status::BAD_ARG;
  }

  return _queue_for(addr, emmv5::velocity(addr, dir, rpm, accel));
}

/** @brief 追加「位置模式」子命令 */
template <typename Uart>
Status DeviceEmmv5GroupT<Uart>::queue_position(uint8_t addr, emmv5::Direction dir, uint16_t rpm, uint8_t accel, int32_t pulses, emmv5::PositionMode mode)
{
  if (!_valid_direction(dir) || !_valid_rpm(rpm))
  {
    return Status::BAD_ARG;
  }

  return _queue_for(addr, emmv5::position(addr, dir, rpm, accel, pulses, mode));
}

/** @brief 追加「快速位置参数」子命令 */
template <typename Uart>
Status DeviceEmmv5GroupT<Uart>::queue_quick_position_params(uint8_t addr, uint16_t rpm, uint8_t accel, emmv5::PositionMode mode)
{
  if (!_valid_rpm(rpm))
  {
    return Status::BAD_ARG;
  }

  return _queue_for(addr, emmv5::quick_position_params(addr, rpm, accel, mode));
}

/** @brief 追加「快速位置」子命令 */
template <typename Uart>
Status DeviceEmmv5GroupT<Uart>::queue_quick_position(uint8_t addr, int32_t pulses)
{
  return _queue_for(addr, emmv5::quick_position(addr, pulses));
}

/** @brief 追加「立即停止」子命令 */
template <typename Uart>
Status DeviceEmmv5GroupT<Uart>::queue_stop(uint8_t addr)
{
  return _queue_for(addr, emmv5::stop(addr));
}


/* ==================== */
/* ==================== 接收 ==================== */

/** @brief 读串口字节并按地址分发；返回本次命中到位帧的电机位图 */
template <typename Uart>
uint32_t DeviceEmmv5GroupT<Uart>::update(uint32_t timeout_ms)
{
  if (!_ready)
  {
    return 0;
  }

  uint32_t hits = 0;
  uint8_t  buf[RX_CHUNK_SIZE];

  // 最多读 RX_MAX_CHUNKS 块：数据持续到达时也要把控制权还给调用任务
  // （一问一答的场景下每拍最多两块：0x36 应答 8 B + 可能的 0x3A 应答 4 B）
  for (size_t chunk = 0; chunk < RX_MAX_CHUNKS; chunk++)
  {
    size_t n = 0;

    // 只有第一块按调用方给的超时等；后续块一律 0，把已到的字节一次排空
    const uint32_t wait_ms = (chunk == 0U) ? timeout_ms : 0U;

    if (_uart.receive(buf, sizeof(buf), wait_ms, &n) != Status::OK || n == 0)
    {
      break; // 超时或暂时没有数据
    }

    hits |= feed_rx(buf, n);
  }

  return hits;
}

/** @brief 一块字节喂给组内每台电机的三个扫描器，并刷新在线计时 */
template <typename Uart>
uint32_t DeviceEmmv5GroupT<Uart>::feed_rx(const uint8_t *data, size_t size)
{
  if (!_ready || data == nullptr || size == 0U)
  {
    return 0;
  }

  uint32_t hits = 0;

  // 每台电机都有自己的三个扫描器；同一块字节交给全组扫一遍，
  // 由扫描器内部的“地址 + 功能码”条件决定归属，避免在设备层手写协议解析。
  for (size_t i = 0; i < _count; i++)
  {
    bool alive = false;

    if (_motor[i].inpos_scanner.feed(_motor[i].addr, data, size))
    {
      _publish_in_position(i);
      hits |= (1UL << i);
      alive = true;
    }

    int32_t position = 0;
    if (_motor[i].position_scanner.feed(_motor[i].addr, data, size, position))
    {
      _publish_position(i, position);
      alive = true;
    }

    uint8_t status = 0;
    if (_motor[i].status_scanner.feed(_motor[i].addr, data, size, status))
    {
      _publish_status(i, status);
      alive = true;
    }

    if (alive)
    {
      // 任何一类有效应答都算在线：只看 0x36 会在只查状态的阶段把电机误判成离线
      (void)_motor[i].online.refresh_task();
    }
  }

  return hits;
}

/** @brief 取走一台最近一次的 0x36 位置（读后清标志） */
template <typename Uart>
bool DeviceEmmv5GroupT<Uart>::get_position(uint8_t addr, int32_t &out)
{
  if (!_ready)
  {
    return false;
  }

  const size_t index = index_of(addr);
  if (index >= _count)
  {
    return false;
  }

  taskENTER_CRITICAL();

  // 读后清标志：同一条位置应答只能被取走一次，否则调用方会把同一个旧值当新数据
  const bool valid = _motor[index].position_valid;
  if (valid)
  {
    out                          = _motor[index].position;
    _motor[index].position_valid = false;
  }

  taskEXIT_CRITICAL();
  return valid;
}

/** @brief 取走一台最近一次的 0x3A 状态（读后清标志） */
template <typename Uart>
bool DeviceEmmv5GroupT<Uart>::get_status(uint8_t addr, uint8_t &out)
{
  if (!_ready)
  {
    return false;
  }

  const size_t index = index_of(addr);
  if (index >= _count)
  {
    return false;
  }

  taskENTER_CRITICAL();

  // 同理：状态字节也只交付一次
  const bool valid = _motor[index].status_valid;
  if (valid)
  {
    out                        = _motor[index].status;
    _motor[index].status_valid = false;
  }

  taskEXIT_CRITICAL();
  return valid;
}

/** @brief 最近 ONLINE_TIMEOUT_MS 内是否收到过有效应答 */
template <typename Uart>
bool DeviceEmmv5GroupT<Uart>::is_online(uint8_t addr) const
{
  if (!_ready)
  {
    return false;
  }

  const size_t index = index_of(addr);
  if (index >= _count)
  {
    return false;
  }

  // 计时由 sys_task 里的 Online::update() 推进，这里只查锁存结果；
  // 所以本类不建任务，但要求调用方的刷新周期小于 ONLINE_TIMEOUT_MS。
  return _motor[index].online.is_online() == Status::OK;
}

/** @brief 锁存位置并置有效标志（短临界区） */
template <typename Uart>
void DeviceEmmv5GroupT<Uart>::_publish_position(size_t index, int32_t position)
{
  // 解析在调用 update() 的任务，取数可能在另一个任务。用短临界区保证“值 + 有效标志”成对可见
  taskENTER_CRITICAL();
  _motor[index].position       = position;
  _motor[index].position_valid = true;
  taskEXIT_CRITICAL();
}

/** @brief 锁存状态并置有效标志（短临界区） */
template <typename Uart>
void DeviceEmmv5GroupT<Uart>::_publish_status(size_t index, uint8_t status)
{
  taskENTER_CRITICAL();
  _motor[index].status       = status;
  _motor[index].status_valid = true;
  taskEXIT_CRITICAL();
}

/** @brief 置累计到位标志（短临界区） */
template <typename Uart>
void DeviceEmmv5GroupT<Uart>::_publish_in_position(size_t index)
{
  taskENTER_CRITICAL();
  _motor[index].in_position = true;
  taskEXIT_CRITICAL();
}

/** @brief rpm 是否在说明书范围内（0~5000） */
template <typename Uart>
bool DeviceEmmv5GroupT<Uart>::_valid_rpm(uint16_t rpm)
{
  // 0~5000 是说明书给的范围；0 也合法（驱动器按 0 速处理）
  return rpm <= 5000U;
}

/** @brief 方向枚举是否合法 */
template <typename Uart>
bool DeviceEmmv5GroupT<Uart>::_valid_direction(emmv5::Direction dir)
{
  return (dir == emmv5::Direction::CW) || (dir == emmv5::Direction::CCW);
}

/** @brief 只查该台的累计到位标志，不清零 */
template <typename Uart>
bool DeviceEmmv5GroupT<Uart>::in_position(size_t index) const
{
  // 只查询不清零：给“看看这台到没到”用，不影响 get_in_position() 的位图
  if (!_ready || index >= _count)
  {
    return false;
  }

  taskENTER_CRITICAL();
  const bool value = _motor[index].in_position;
  taskEXIT_CRITICAL();
  return value;
}

/** @brief 取走累计到位位图并清零 */
template <typename Uart>
uint32_t DeviceEmmv5GroupT<Uart>::get_in_position()
{
  if (!_ready)
  {
    return 0;
  }

  uint32_t mask = 0;

  // 接收任务写、控制任务读并清零：必须和 _publish_in_position() 用同一把短临界区
  taskENTER_CRITICAL();
  for (size_t i = 0; i < _count; i++)
  {
    if (_motor[i].in_position)
    {
      mask |= (1UL << i);
      _motor[i].in_position = false;
    }
  }
  taskEXIT_CRITICAL();

  return mask;
}

/* ==================== */

#endif // HAL_UART_MODULE_ENABLED

#endif // __DEVICE_EMMV5_GROUP_HPP__
