/**
 * @file device_r9ds.hpp
 * @author Rh
 * @brief R9DS 遥控接收机设备 —— 在 BspUartT 之上把 SBUS 字节流解成通道值
 * @version 0.2
 * @date 2026-10-18
 *
 * @copyright Copyright (c) 2026
 *
 * @details 只做三件事：从 BspUartT 的接收流缓冲区里切出 25 字节的 SBUS 帧、把那
 *          16 × 11 bit 拆成 16 个通道、再把通道映射成「摇杆 / 旋钮 / 开关」的语义。
 *
 * @note SBUS 协议（来源：R9DS 使用说明）：
 *       100000 bps、8 数据位、偶校验、2 停止位（每字节 12 位）、**反相 TTL**；
 *       一帧 25 字节 = 0x0F 帧头 + 22 字节通道数据（16 通道 × 11 bit，小端）+ flags + 0x00 帧尾。
 *       串口参数在 CubeMX 里配，`init()` 只做兜底校验，一个字节都不改；接收机侧要拨到 SBUS 模式。
 *
 * @note flags = frame[23]：bit0/bit1 是数字通道 17/18，bit2 = 本帧丢失，bit3 = 失控保护激活
 *       （用 `is_frame_lost()` / `is_failsafe()` 暴露；失控时怎么处置是上层的事）。
 *
 * @note 两套量别混用：`get_raw_channels()` 是协议原始值（上 200 / 中 1000 / 下 1800）；
 *       `get_rc()` 是遥控器语义值（连续通道 −100 ~ +100，开关给挡位），**写控制逻辑用这一套**。
 *       通道表与开关挡位见下面 Switch / Rc 的注释，用法见 docs/code/device.md 的 remote/ 一节。
 *
 * @warning 不是线程安全的：update() 只允许由一个任务调用；查询接口返回实时引用，
 *          读取期间不得并发执行 update()。
 * @warning 在线判定**不依赖 Online 节点**，由本类自己按 tick 算（见 is_online()）：所以它只在
 *          is_online() 被调用那一刻成立，调用频率只影响分辨率、不会漏判。
 */

#ifndef __DEVICE_R9DS_HPP__
#define __DEVICE_R9DS_HPP__

#include "bsp/bsp_series.hpp" // IWYU pragma: keep（HAL 模块开关 / 系列桥接）

// 宿主工程没在 CubeMX 里开 UART（HAL_UART_MODULE_ENABLED 未定义）时，本模块整体不参与编译
#ifdef HAL_UART_MODULE_ENABLED


#include "service/system/state_tree.hpp" // bus_state()：默认树的总线节点
#include "service/system/status.hpp"
#include "service/system/system.hpp"     // sys_state
#include <stdint.h>
#include "FreeRTOS.h"  // IWYU pragma: keep (pdMS_TO_TICKS)
#include "bsp/bsp_uart.hpp" // IWYU pragma: keep（BspUartT 实例化与 UART 宏）
#include "task.h"     // xTaskGetTickCount


/**
 * @brief R9DS 遥控接收机设备
 *
 * @note 不拥有串口、不创建任务、不注册到 Online 链表（唯一的 RTOS 资源是 init() 建的默认树节点事件组）：只解析调用方喂进来的
 *       时间片，在线判定自己按 tick 算。
 */
template <typename Uart>
class DeviceR9dsT
{
public:
  /* ==================== 帧结构常量 ==================== */

  static constexpr uint32_t FRAME_SIZE   = 25U;     ///< 一帧字节数：帧头 + 22 字节通道数据 + flags + 帧尾
  static constexpr uint8_t  FRAME_HEADER = 0x0FU;   ///< 帧头标识
  static constexpr uint8_t  FRAME_FOOTER = 0x00U;   ///< 帧尾标识（扩展帧另有取值，本类只认它）
  static constexpr uint32_t CHANNEL_NUM  = 16U;     ///< 通道数
  static constexpr uint16_t CHANNEL_MASK = 0x07FFU; ///< 单个通道占 11 bit
  /* ==================== */
  /* ==================== 协议数值常量 ==================== */

  static constexpr uint16_t CHANNEL_MID = 1000U; ///< 通道中位原始值
  static constexpr uint16_t CHANNEL_MIN = 200U;  ///< 一端满（遥控器的上挡）的原始值
  static constexpr uint16_t CHANNEL_MAX = 1800U; ///< 另一端满（遥控器的下挡）的原始值

  /**
   * @brief 连续通道换算：每多少个原始值对应 1 个语义单位
   *
   * @note 两端 200 / 1800 对应 +100 / -100 ⇒ (1800 - 200) / (100 + 100) = 8。
   */
  static constexpr int32_t RAW_PER_UNIT = 8;
  /* ==================== */
  /* ==================== 时基常量 ==================== */

  /**
   * @brief 接收机静默多久就判离线 (ms)
   *
   * @note 帧周期约 14 ms（慢速）/ 7 ms（快速），取 100 ms ≈ 7 帧。
   * @note 计时在 is_online() 里按 tick 现算，不需要任何周期任务推进。
   */
  static constexpr uint16_t ONLINE_TIMEOUT_MS = 100U;

  /**
   * @brief 连续多少帧带「本帧丢失」就判离线
   *
   * @note 实测遥控器关机时 **lost 先置 1 并保持一段时间，fs 之后才置 1**，所以拿它当提前量：
   *       3 帧 × 13.5 ms ≈ 40 ms，比等 fs 早、也比上面的 100 ms 帧超时早。
   * @note 要求“连续”是为了抗单帧偶发丢包：正常遥控时偶发一帧 lost 不应该判离线。
   * @note 调这个数的依据看 diag().lost_streak：正常遥控时它应该一直是 0；若能看到涨到 1 ~ 2，
   *       说明链路本身在零星丢帧，把它调大（5 或 8）。
   */
  static constexpr uint32_t LOST_STREAK_TO_OFFLINE = 5U;
  /* ==================== */
  /* ==================== 开关挡位 ==================== */

  /**
   * @brief 开关挡位
   *
   * @note 枚举值刻意取 +1 / 0 / -1：乘 100 就是和连续通道同一量纲的语义值
   *       （上挡 +100、中挡 0、下挡 -100）。
   */
  enum class Switch : int8_t
  {
    UP   = 1,  ///< 上挡（原始值 200；语义值 +100）
    MID  = 0,  ///< 中挡（原始值 1000；语义值 0）
    DOWN = -1, ///< 下挡（原始值 1800；语义值 -100）
  };
  /* ==================== */
  /* ==================== 遥控器通道 ==================== */

  /**
   * @brief 遥控器通道语义值（遥控器侧视图，**写控制逻辑用这一套**）
   *
   * @note 字段顺序就是 CH1 ~ CH10 的顺序，通道含义见文件头那张表。
   * @note 连续通道范围 -100 ~ +100（上/一端满 +100、中位 0、下/另一端满 -100）；
   *       开关字段是挡位，不是数值。
   */
  struct Rc
  {
    int16_t right_vertical;   ///< CH1 右摇杆 上下，-100 ~ +100
    int16_t right_horizontal; ///< CH2 右摇杆 左右
    int16_t left_vertical;    ///< CH3 左摇杆 上下
    int16_t left_horizontal;  ///< CH4 左摇杆 左右
    Switch  switch_c;         ///< CH5 C 三挡开关：UP / MID / DOWN
    int16_t knob_left;        ///< CH6 正面左旋钮
    int16_t slider_left;      ///< CH7 背面左推杆
    int16_t knob_right;       ///< CH8 正面右旋钮
    Switch  switch_b;         ///< CH9 正面 B 开关：只会是 UP / DOWN
    Switch  switch_a;         ///< CH10 正面 A 开关：只会是 UP / DOWN
  };
  /* ==================== */
  /* ==================== 接收诊断 ==================== */

  /**
   * @brief 接收诊断：前四个计数只增不减，最后一个是实时值
   *
   * @note 专门用来区分两种"收不到数据"：byte_cnt 为 0 说明一个字节都没进来（查接线 / 波特率 /
   *       反相），byte_cnt 在涨而 frame_cnt 不动说明字节进来了但切不出帧（查帧格式 / 反相）。
   */
  struct Diag
  {
    uint32_t byte_cnt;    ///< 从串口读到的总字节数（N34：原来是 float，过 2^24 后就不再增加）
    uint32_t frame_cnt;   ///< 帧头帧尾都正确、已解析的帧数
    uint32_t bad_footer_cnt; ///< 帧头对上了但帧尾不是 0x00 的帧数
    uint32_t skip_byte_cnt;  ///< 为了找帧头而丢掉的字节数（正常应远小于 byte_cnt）
    uint32_t lost_streak;    ///< 【实时值】连续多少帧带「本帧丢失」，收到一帧不带就清 0
  };
  /* ==================== */
  /* ==================== 构造与析构 ==================== */

  /** @brief 必须绑定一路已配成 SBUS 的串口，禁用默认构造 */
  DeviceR9dsT() = delete;

  /** @brief 绑定串口（只赋值，不碰硬件、不读串口） */
  explicit DeviceR9dsT(Uart &uart);

  /** @brief 默认析构（不持有任何需要释放的资源） */
  ~DeviceR9dsT() = default;

  // 内部持有解析状态（半截帧、通道值等），复制会让两份状态各自推进
  DeviceR9dsT(const DeviceR9dsT &)            = delete;
  DeviceR9dsT &operator=(const DeviceR9dsT &) = delete;
  DeviceR9dsT(DeviceR9dsT &&)                 = delete;
  DeviceR9dsT &operator=(DeviceR9dsT &&)      = delete;
  /* ==================== */
  /* ==================== 生命周期 ==================== */

  /**
   * @brief 复位解析状态与诊断计数
   * @return OK=已复位（串口由 board_init() 负责，本函数没有会失败的事）
   *
   * @note 清掉半截帧、清零诊断计数，并把通道值置成一组安全默认（摇杆/旋钮全回中位 0、
   *       开关全回 DOWN），这样"还没收到过任何帧"时上层读到的也不是野值。
   */
  Status init(void);

  /**
   * @brief 把串口里攒的字节全部消化掉（可能一次解出多帧）
   * @param timeout_ms 首字节等待时间：0 = 非阻塞，N = 最多等 N ms（超时即返回）
   *
   * @note 逐字节推进「找帧头 → 凑满 25 字节 → 验帧尾」的同步状态机。之所以不先确认
   *       "够 25 字节"再读：StreamBuffer 的 receive 只保证有多少给多少，一次要 24 字节
   *       可能只拿到一部分，反而会把帧切歪；逐字节读天然不受分片影响。
   * @note 只有**首字节**用 timeout_ms 等；一旦开始解帧，后续字节一律 0，把已有数据排空。
   */
  void update(uint32_t timeout_ms = 0U);
  /* ==================== */
  /* ==================== 查询接口 ==================== */

  /**
   * @brief 遥控链路是否可用
   *
   * @note 三个条件同时成立才算在线，各管一段故障、互相兜底：
   *       ① 收到过帧，且最近一帧在 ONLINE_TIMEOUT_MS 内 —— 管“接收机自己静默了”（拔线 / 掉电）；
   *       ② 最近那一帧的 flags 没置「失控保护」—— 管“遥控器失联了”；
   *       ③ 连续带「本帧丢失」的帧数没到 LOST_STREAK_TO_OFFLINE —— 管“链路正在变差”，是②的提前量。
   * @note 为什么光看帧不够：**遥控器一关机，接收机不会安静下来**，它继续按帧周期发 SBUS，
   *       只把 flags 的位置起来，所以只看“有没有帧”就会一直显示在线。
   * @note 为什么还要③：实测关机时 **lost 先置 1 并保持一段时间，fs 之后才置 1**，所以拿连续的
   *       lost 当提前量，比等 fs 早几十毫秒报出失联；要求“连续”是为抗单帧偶发丢包。
   * @note ① 的计时由本类自己按 tick 现算（**不走 Online 节点**），所以不需要任何周期任务喂它，
   *       也就不用关心调用频率；代价是结果只在调用那一刻成立，需要稳定的调用update才行，例如sys_task。
   */
  bool is_online(void) const;

  /** @brief 遥控器语义值（**控制逻辑用这个**），实时引用：update() 在别的任务里跑时请用 snapshot() */
  const Rc &get_rc(void) const;

  /** @brief 最近一帧的 16 个原始通道值（协议侧：上 200 / 中 1000 / 下 1800），下标 = 通道号 - 1 */
  const uint16_t *get_raw_channels(void) const;

  /** @brief 最近一帧的「本帧丢失」标志（frame[23] 的 bit2） */
  bool is_frame_lost(void) const;

  /** @brief 最近一帧的「失控保护激活」标志（frame[23] 的 bit3） */
  bool is_failsafe(void) const;

  /** @brief 诊断计数快照 */
  Diag get_diag(void) const;

  /**
   * @brief 本接收机在 EventState 默认树里的节点（第三层，挂在所用串口的总线节点下）
   * @return `is_running()` = init() 成功；`get_error_bits()` 里的 TIMEOUT = 掉过线（粘滞）。掉线在 update() 里检测
   */
  const EventState &get_state(void) const { return _state; }

  /**
   * @brief 拷贝一份同一帧的遥控器数据（临界区内拷贝，跨任务读用它，F5）
   * @param rc  遥控器语义值
   * @param raw 可选：同一帧的 16 个原始通道值（CHANNEL_NUM 个）；nullptr = 不要
   */
  void snapshot(Rc &rc, uint16_t *raw = nullptr) const;
  /* ==================== */
private:
  /* ==================== 帧解析 ==================== */

  /** @brief 解析一帧已通过帧尾校验的数据：16 通道 → 语义值 → flags → 记录帧时刻 */
  void _parse_frame(const uint8_t *frame);
  /* ==================== */
  /* ==================== 原始值换算 ==================== */

  /** @brief 原始值 → 遥控器语义值：raw 200 → +100、1000 → 0、1800 → -100（截断取整） */
  static int16_t _to_rc_value(uint16_t raw);
  /* ==================== */
  /* ==================== 开关判挡 ==================== */

  /** @brief 两挡开关判挡：比离 200 / 1800 哪个近 */
  static Switch _to_switch_2pos(uint16_t raw);

  /** @brief 三挡开关判挡：比离 200 / 1000 / 1800 哪个近 */
  static Switch _to_switch_3pos(uint16_t raw);
  /* ==================== */
  /* ==================== 成员变量 ==================== */

  Uart &_uart; ///< 被解析的串口（不拥有）

  Rc       _rc {0, 0, 0, 0, Switch::DOWN, 0, 0, 0, Switch::DOWN, Switch::DOWN}; ///< 最近一帧的遥控器语义值（未收到帧 = 通道回中位、开关回下挡）
  uint16_t _raw[CHANNEL_NUM] {};  ///< 最近一帧的原始通道值（协议侧）；还没收到帧时由构造函数填中位
  uint8_t  _frame[FRAME_SIZE] {}; ///< 正在拼的帧
  uint32_t _fill = 0U;            ///< 已拼到第几个字节，0 = 还没进入帧
  bool     _frame_lost = false;   ///< 最近一帧 flags 的 bit2：本帧丢失
  bool     _failsafe   = false;   ///< 最近一帧 flags 的 bit3：失控保护激活
  Diag     _diag {};              ///< 诊断计数（frame_cnt 顺带当“收到过帧吗”的标志用）
  EventState _state;              ///< 默认树节点（Q6）
  bool       _was_online = false; ///< 上一次 update() 结束时是否在线（检测由在线变离线）
  uint32_t _last_frame_tick = 0U; ///< 最近一帧的 tick，is_online() 拿它算“接收机静默了多久”
  /* ==================== */
};

/* ==================== 成员函数定义（只有头文件：任意 CAN / UART 类型都能实例化，评审 N23） ==================== */


/* ==================== */
/* ==================== 模板实例化 ==================== */


/* ==================== */
/* ==================== 构造与析构 ==================== */

/**
 * @brief 绑定串口；原始通道先填中位
 *
 * @note 其余解析状态的初值都在 device_r9ds.hpp 的成员声明里（NSDMI），
 *       只有 `_raw` 需要整表填 CHANNEL_MID，没法写成默认成员初始化器，所以在这里遍历。
 */
template <typename Uart>
DeviceR9dsT<Uart>::DeviceR9dsT(Uart &uart) : _uart(uart)
{
  for (uint32_t i = 0U; i < CHANNEL_NUM; ++i)
  {
    _raw[i] = CHANNEL_MID; // 还没收到帧，原始通道先按中位显示，避免上层读到 0 这种野值
  }
}
/* ==================== */
/* ==================== 生命周期 ==================== */

/**
 * @brief 校验串口帧格式（SBUS：100000 / 8E2）
 *
 * @note 解析状态已在构造时复位，所以本函数只做校验；重复调用是幂等的。
 */
template <typename Uart>
Status DeviceR9dsT<Uart>::init(void)
{
  // 默认树第三层（Q6）：挂在所用串口的总线节点下（串口不在清单里时挂 sys_state）；重复调用无害
  if (_state.init() == Status::OK)
  {
    EventState *bus = bus_state(&_uart);
    (void)_state.set_parent((bus != nullptr) ? *bus : sys_state);
  }

  if (!_uart.check_format(100000U, UART_WORDLENGTH_9B, UART_PARITY_EVEN, UART_STOPBITS_2))
  {
    (void)_state.init_error(Status::BAD_ARG);
    return Status::BAD_ARG;
  }

  (void)_state.complete_init();
  return Status::OK;
}

/**
 * @brief 把 BspUartT 接收流缓冲区里的字节全部消化掉
 *
 * @note 状态机只有三档：_fill == 0 时在找 0x0F；0 < _fill < 25 时在拼帧；
 *       拼满 25 字节后用帧尾 0x00 确认，不匹配就整帧丢掉、重新找帧头。
 * @note 帧体内部出现 0x0F 不会误判 —— 拼帧期间不看内容，只看长度和帧尾。
 */
template <typename Uart>
void DeviceR9dsT<Uart>::update(uint32_t timeout_ms)
{
  uint32_t wait_ms = timeout_ms;

  for (;;)
  {
    uint8_t byte = 0U;
    size_t  got  = 0U;

    // 首字节按调用方给的超时等；一旦开始解帧就一律 0，把已有数据排空
    if ((_uart.receive(&byte, 1U, wait_ms, &got) != Status::OK) || (got == 0U))
    {
      // 这一拍的字节取完了：由在线变离线时在本节点记 TIMEOUT（构造后从没上线过的不算）
      const bool online = is_online();
      if (_was_online && !online)
      {
        (void)_state.error(Status::TIMEOUT);
      }
      _was_online = online;
      return;
    }

    wait_ms = 0U; // 后续字节不再等待

    _diag.byte_cnt = _diag.byte_cnt + 1U;

    // 还没进入帧：跳过一切非帧头的字节
    if ((_fill == 0U) && (byte != FRAME_HEADER))
    {
      ++_diag.skip_byte_cnt;
      continue;
    }

    _frame[_fill] = byte;
    ++_fill;

    if (_fill < FRAME_SIZE)
    {
      continue; // 还没拼满，等后面的字节
    }

    // 拼满 25 字节：帧尾必须是 0x00，否则整帧作废、重新找帧头
    if (_frame[FRAME_SIZE - 1U] == FRAME_FOOTER)
    {
      _diag.frame_cnt = _diag.frame_cnt + 1U;
      _parse_frame(_frame);
    }
    else
    {
      ++_diag.bad_footer_cnt;
    }

    _fill = 0U;
  }
}
/* ==================== */
/* ==================== 查询接口 ==================== */

/**
 * @brief 在线判定：帧到 + 没失控 + 没在连续丢帧
 *
 * @note 三个条件各管一段，且大致按“能多早发现”排列：
 *       ① 接收机静默超时 —— 拔线 / 掉电时一个字节都不会来，帧只有最后一次的值，只有这条能发现；反过来说：遥控器关机不在此列，接收机照发不误；
 *       ② 连续丢帧数 —— 链路刚开始变差就报，是最早的一条（lost 比 fs 先置 1）；
 *       ③ 最新一帧的失控位 —— 遥控器已确认失联（接收机继续发帧，不把那个位置起来区分不出来）。
 *
 * @note ① 自己按 tick 现算，不走 Online 节点、也不需要任何周期任务喂它：
 *       好处是调用频率只影响分辨率、不会漏判；代价是结果只在调用这一刻成立
 *       需要定时调用update,如果以此工程为准的话，推荐在sys_task中10ms调用一次
 * @note 单独判一次 frame_cnt == 0，是为了让“从没收到过帧”的初始状态就是离线，
 *       而不必在 init() 里提前去读 tick。
 */
template <typename Uart>
bool DeviceR9dsT<Uart>::is_online(void) const
{
  // ① 收到过帧，且最近一帧还在 ONLINE_TIMEOUT_MS 之内（无符号差值，tick 回绕安全）
  if ((_diag.frame_cnt == 0U) ||
      ((static_cast<uint32_t>(xTaskGetTickCount()) - _last_frame_tick) >= pdMS_TO_TICKS(ONLINE_TIMEOUT_MS)))
  {
    return false;
  }

  // ② 最新一帧没置失控保护，③ 也没在连续丢帧
  return (!_failsafe) && (_diag.lost_streak < LOST_STREAK_TO_OFFLINE);
}

/** @brief 遥控器语义值（控制逻辑用这个返回值），实时引用 */
template <typename Uart>
const typename DeviceR9dsT<Uart>::Rc &DeviceR9dsT<Uart>::get_rc(void) const
{
  return _rc;
}

/** @brief 最近一帧的 16 个原始通道值（协议侧） */
template <typename Uart>
const uint16_t *DeviceR9dsT<Uart>::get_raw_channels(void) const
{
  return _raw;
}

/** @brief 最近一帧的「本帧丢失」标志 */
template <typename Uart>
bool DeviceR9dsT<Uart>::is_frame_lost(void) const
{
  return _frame_lost;
}

/** @brief 最近一帧的「失控保护激活」标志 */
template <typename Uart>
bool DeviceR9dsT<Uart>::is_failsafe(void) const
{
  return _failsafe;
}

/** @brief 诊断计数快照 */
template <typename Uart>
typename DeviceR9dsT<Uart>::Diag DeviceR9dsT<Uart>::get_diag(void) const
{
  return _diag;
}
/* ==================== */
/* ==================== 帧解析 ==================== */

/**
 * @brief 解析一帧已通过帧尾校验的数据
 *
 * @note 位提取：16 个通道 × 11 bit = 176 bit，小端排在 frame[1] ~ frame[22] 里。
 *       每个通道都跨字节边界，所以统一取「起始字节 + 后两字节」拼成 24 位窗口，
 *       右移掉窗口内偏移后取低 11 位。算例（ch10）：
 *
 *       bit 偏移 = 10×11 = 110 → 起始字节 = 110/8 + 1 = 14、窗口内偏移 = 110%8 = 6
 *       ⇒ (frame[14] | frame[15] << 8 | frame[16] << 16) >> 6 & 0x7FF
 *
 * @note 用统一下标循环算，不手抄 16 条移位表达式 —— 手抄一旦有一位下标写错，现象会是
 *       某个通道只在特定范围才不对，很难查。
 */
template <typename Uart>
void DeviceR9dsT<Uart>::_parse_frame(const uint8_t *frame)
{
  // 解析结果整帧写回，与 snapshot() 互斥（几百条指令，F5）
  taskENTER_CRITICAL();
  uint32_t bit_index = 0U;
  for (uint32_t i = 0U; i < CHANNEL_NUM; ++i)
  {
    const uint32_t byte_index = (bit_index >> 3U) + 1U; // +1 = 跳过帧头
    const uint32_t bit_offset = bit_index & 7U;

    const uint32_t window = static_cast<uint32_t>(frame[byte_index]) |
                            (static_cast<uint32_t>(frame[byte_index + 1U]) << 8U) |
                            (static_cast<uint32_t>(frame[byte_index + 2U]) << 16U);

    _raw[i] = static_cast<uint16_t>((window >> bit_offset) & CHANNEL_MASK);

    bit_index += 11U;
  }

  // CH1 ~ CH10，下标 = 通道号 - 1。含义见 device_r9ds.hpp 文件头那张表

  // 四个摇杆：连续值
  _rc.right_vertical   = _to_rc_value(_raw[0]); // CH1 右摇杆 上下
  _rc.right_horizontal = _to_rc_value(_raw[1]); // CH2 右摇杆 左右
  _rc.left_vertical    = _to_rc_value(_raw[2]); // CH3 左摇杆 上下
  _rc.left_horizontal  = _to_rc_value(_raw[3]); // CH4 左摇杆 左右

  // 三个旋钮/推杆：连续值
  _rc.knob_left   = _to_rc_value(_raw[5]); // CH6 正面左旋钮
  _rc.slider_left = _to_rc_value(_raw[6]); // CH7 背面左推杆
  _rc.knob_right  = _to_rc_value(_raw[7]); // CH8 正面右旋钮

  // 三个开关：挡位（CH5 是三挡，CH9/CH10 是两挡）
  _rc.switch_c = _to_switch_3pos(_raw[4]); // CH5  C 三挡开关
  _rc.switch_b = _to_switch_2pos(_raw[8]); // CH9  正面 B 开关
  _rc.switch_a = _to_switch_2pos(_raw[9]); // CH10 正面 A 开关

  // frame[23] 是 flags：bit2 = 本帧丢失，bit3 = 失控保护激活（bit0/bit1 是数字通道 17/18）
  const uint8_t flags = frame[FRAME_SIZE - 2U];
  _frame_lost         = (flags & 0x04U) != 0U;
  _failsafe           = (flags & 0x08U) != 0U;

  // 连续丢帧计数：带标志就累加、收到一帧干净的就清 0。实测关机时它先于 fs 涨起来，
  // 所以 is_online() 拿它当提前量（阈值见 LOST_STREAK_TO_OFFLINE）。
  if (_frame_lost)
  {
    ++_diag.lost_streak;
  }
  else
  {
    _diag.lost_streak = 0U;
  }

  // 帧完整且帧尾正确 = 接收机确实在发帧：记下时刻，作为①“接收机没静默”的依据。
  // 注意这里只记“接收机还在发帧”这一件事：遥控器跟它有没有失联，由 is_online() 用 flags 判。
  _last_frame_tick = static_cast<uint32_t>(xTaskGetTickCount());
  taskEXIT_CRITICAL();
}

template <typename Uart>
void DeviceR9dsT<Uart>::snapshot(Rc &rc, uint16_t *raw) const
{
  taskENTER_CRITICAL();
  rc = _rc;
  if (raw != nullptr)
  {
    for (uint32_t i = 0U; i < CHANNEL_NUM; ++i)
    {
      raw[i] = _raw[i];
    }
  }
  taskEXIT_CRITICAL();
}
/* ==================== */
/* ==================== 原始值换算 ==================== */

/**
 * @brief 原始值 → 遥控器语义值（连续通道）
 *
 * @note 两端 200 / 1800 对应 +100 / -100，中位 1000 对应 0，线性：
 *       值 = (1000 - raw) / 8 
 *       算例：raw 200 → +100、raw 1000 → 0、raw 1800 → -100、raw 1200 → -25。
 *
 * @note 整数除法是截断，所以 raw 993 ~ 1007 都得到 0（不足 1 个单位就舍掉），等于自带一个
 *       15 个原始值宽的小死区。
 * @note 故意不限幅到 ±100：原始值跑出 200 ~ 1800（噪声或通道异常）时结果会跟着超出去，
 *       一眼看得见；限了幅反而会把"通道根本没接上"这种故障糊过去。
 */
template <typename Uart>
int16_t DeviceR9dsT<Uart>::_to_rc_value(uint16_t raw)
{
  return static_cast<int16_t>((static_cast<int32_t>(CHANNEL_MID) - static_cast<int32_t>(raw)) / RAW_PER_UNIT);
}
/* ==================== */
/* ==================== 开关判挡 ==================== */

/** @brief 两挡开关判挡：比离 200 / 1800 哪个近，分界正好是中位 1000 */
template <typename Uart>
typename DeviceR9dsT<Uart>::Switch DeviceR9dsT<Uart>::_to_switch_2pos(uint16_t raw)
{
  return (raw < CHANNEL_MID) ? Switch::UP : Switch::DOWN;
}

/**
 * @brief 三挡开关判挡：比离 200 / 1000 / 1800 哪个近
 *
 * @note 分界取相邻两挡的中点：400 与 1400。物理开关是硬换向的，实测就落在三个标称值上，
 *       所以"就近"比"划阈值区间"稳。
 */
template <typename Uart>
typename DeviceR9dsT<Uart>::Switch DeviceR9dsT<Uart>::_to_switch_3pos(uint16_t raw)
{
  if (raw < (CHANNEL_MIN + CHANNEL_MID) / 2U) // < 600
  {
    return Switch::UP;
  }
  if (raw < (CHANNEL_MID + CHANNEL_MAX) / 2U) // < 1400
  {
    return Switch::MID;
  }
  return Switch::DOWN;
}
/* ==================== */

#endif // HAL_UART_MODULE_ENABLED

#endif // __DEVICE_R9DS_HPP__
