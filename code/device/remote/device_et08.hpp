/**
 * @file device_et08.hpp
 * @author ChoseB
 * @brief 天地飞 ET08 遥控器（RF206S 接收机，W.BUS 口）设备 —— 在 BspUartT 之上把 SBUS 字节流解成通道值
 * @version 0.1
 * @date 2026-11-25
 *
 * @copyright Copyright (c) 2026
 *
 * @details 结构与 DeviceR9dsT 一致：从 BspUartT 的接收流缓冲区里切出 25 字节的 SBUS 帧、把
 *          16 × 11 bit 拆成 16 个通道、再把通道映射成「摇杆 / 开关」的语义。
 *          移植自 Sentry2026_Refactor 的 device_et08.cpp（原 et08.c），数值与通道分配沿用旧工程。
 *
 * @note 接收机侧（来源：《ET08 说明书》）：RF206S 共 6 个端口，**最后一个（6 号）端口**可在
 *       【通信设置】→【PPM/W.BUS】或【接收机端口设置】里切到 W.BUS 模式（W.BUS 兼容 S.BUS），
 *       接收机 LED **绿色常亮** = W.BUS 模式工作正常、红色常亮 = 无信号。
 *       发射机 8 通道；SA / SD 两挡、SB / SC 三挡，开关与通道的对应关系都可在【通用功能】→【通道设置】改。
 *
 * @note SBUS 帧：100000 bps、8 数据位、偶校验、2 停止位、**反相 TTL**；
 *       一帧 25 字节 = 0x0F 帧头 + 22 字节通道数据（16 通道 × 11 bit，小端）+ flags + 帧尾。
 *       帧尾接受 0x00（S.BUS）和 0x04 / 0x14 / 0x24 / 0x34（S.BUS2）；ET08 只有 8 通道，CH9 ~ CH16 无意义。
 *       停止位 1 或 2 都能收（接收端只采第一个停止位）：旧工程用的就是 8E1，`init()` 两种都放行。
 *
 * @note 原始值（由旧工程开关判值反推）：一端满 353、中位 1024、另一端满 1694（舵角 100% 时），
 *       与 Futaba S.BUS 的标称范围一致。旧工程把 11 位值截成 uint8_t 后比 97 / 158 / 0，
 *       对应的正是 353 (0x161) / 1694 (0x69E) / 1024 (0x400)。
 *
 * @note 通道分配（旧工程的发射机设置，改了【通道设置】要同步改这里的注释 / 用法）：
 *
 *       | 通道 | 用途（旧工程字段） | 本类字段 | 说明 |
 *       | --- | --- | --- | --- |
 *       | CH1 | 摇杆（ch0） | `ch1` | 哪根摇杆、哪个方向取决于【摇杆模式】 |
 *       | CH2 | 摇杆（ch1） | `ch2` | |
 *       | CH3 | 摇杆（ch2） | `ch3` | 默认是油门通道 |
 *       | CH4 | 摇杆（ch3） | `ch4` | |
 *       | CH5 | 未用 | `ch5` | 旧工程没解析（sD 恒为 0） |
 *       | CH6 | SA 两挡（sA） | `switch_a` | 旧工程 sA = 1（解锁）↔ UP |
 *       | CH7 | SB 三挡（s1） | `switch_b` | 旧工程 s1：1 ↔ UP、3 ↔ MID、2 ↔ DOWN |
 *       | CH8 | SC 三挡（s2） | `switch_c` | 旧工程 s2：同上 |
 *
 * @note 两套量别混用：`get_raw_channels()` 是协议原始值（353 / 1024 / 1694）；
 *       `get_rc()` 是语义值（连续通道约 −100 ~ +100，开关给挡位），**写控制逻辑用这一套**。
 *       连续通道的正负号与旧工程的 `ch - 1024` 相同（原始值大 = 正），移植旧控制代码不用改符号；
 *       方向不合适就在发射机【正反设置】里反，不在这里反。
 *
 * @warning 不是线程安全的：update() 只允许由一个任务调用；查询接口返回实时引用，
 *          读取期间不得并发执行 update()（跨任务读用 snapshot()）。
 * @warning 在线判定**不依赖 Online 节点**，由本类自己按 tick 算（见 is_online()），理由同 DeviceR9dsT：
 *          遥控器关机后接收机通常仍在发帧，只看“有没有帧”判不出离线。
 *          **ET08 的 flags（失控 / 丢帧位）未实测**：若 RF206S 不置这两位，就只剩帧超时这一条，
 *          此时需要在发射机【失控保护】里给某个通道设 F/S 预设值，由上层按该值判（旧工程判 sA 解锁 + CH3）。
 */

#ifndef __DEVICE_ET08_HPP__
#define __DEVICE_ET08_HPP__

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
 * @brief ET08 遥控器设备（RF206S 接收机 W.BUS 口）
 *
 * @note 不拥有串口、不创建任务、不注册到 Online 链表（唯一的 RTOS 资源是 init() 建的默认树节点事件组）：只解析调用方喂进来的
 *       时间片，在线判定自己按 tick 算。
 */
template <typename Uart>
class DeviceEt08T
{
public:
  /* ==================== 帧结构常量 ==================== */

  static constexpr uint32_t FRAME_SIZE   = 25U;     ///< 一帧字节数：帧头 + 22 字节通道数据 + flags + 帧尾
  static constexpr uint8_t  FRAME_HEADER = 0x0FU;   ///< 帧头标识
  static constexpr uint32_t CHANNEL_NUM  = 16U;     ///< 协议通道数（ET08 只用前 8 个）
  static constexpr uint16_t CHANNEL_MASK = 0x07FFU; ///< 单个通道占 11 bit
  /* ==================== */
  /* ==================== 协议数值常量 ==================== */

  static constexpr uint16_t CHANNEL_MID = 1024U; ///< 通道中位原始值
  static constexpr uint16_t CHANNEL_MIN = 353U;  ///< 一端满（开关的 UP 挡）的原始值
  static constexpr uint16_t CHANNEL_MAX = 1694U; ///< 另一端满（开关的 DOWN 挡）的原始值

  /**
   * @brief 连续通道：中位到一端满的原始值跨度（对应 100 个语义单位）
   *
   * @note 1694 - 1024 = 1024 - 353 = 671 ⇒ 每个语义单位约 6.7 个原始值。
   */
  static constexpr int32_t RAW_HALF_SPAN = 671;

  /**
   * @brief 摇杆死区（原始值，离中位不超过它就当 0）
   *
   * @note 旧工程是 990 < raw < 1050 回中（−33 ~ +25），这里取对称的 ±30。
   */
  static constexpr uint16_t STICK_DEADBAND_RAW = 30U;
  /* ==================== */
  /* ==================== 时基常量 ==================== */

  /**
   * @brief 接收机静默多久就判离线 (ms)
   *
   * @note S.BUS 帧周期 7 ~ 14 ms，100 ms 约 7 帧（旧工程用 200 ms）。
   * @note 计时在 is_online() 里按 tick 现算，不需要任何周期任务推进。
   */
  static constexpr uint16_t ONLINE_TIMEOUT_MS = 100U;

  /**
   * @brief 连续多少帧带「本帧丢失」就判离线
   *
   * @note 与 DeviceR9dsT 同值：要求“连续”是为了抗单帧偶发丢包。ET08 是否置这一位未实测，
   *       不置的话 lost_streak 一直是 0，这一条不起作用、也不会误判。
   */
  static constexpr uint32_t LOST_STREAK_TO_OFFLINE = 5U;
  /* ==================== */
  /* ==================== 开关挡位 ==================== */

  /**
   * @brief 开关挡位
   *
   * @note 按原始值命名：UP = 353、MID = 1024、DOWN = 1694，与 DeviceR9dsT 的约定相同（UP = 原始值小的一端）。
   *       枚举值取 +1 / 0 / -1；和旧工程（DJI 风格）的对应：1 ↔ UP、3 ↔ MID、2 ↔ DOWN。
   */
  enum class Switch : int8_t
  {
    UP   = 1,  ///< 原始值 353
    MID  = 0,  ///< 原始值 1024（两挡开关不会出现）
    DOWN = -1, ///< 原始值 1694
  };
  /* ==================== */
  /* ==================== 遥控器通道 ==================== */

  /**
   * @brief 遥控器通道语义值（**写控制逻辑用这一套**）
   *
   * @note 字段顺序就是 CH1 ~ CH8 的顺序，通道含义见文件头那张表。
   * @note 连续通道：原始值 1694 → +100、1024 → 0、353 → −100，中位 ±STICK_DEADBAND_RAW 内为 0；
   *       开关字段是挡位，不是数值。
   */
  struct Rc
  {
    int16_t ch1;      ///< CH1 摇杆，-100 ~ +100
    int16_t ch2;      ///< CH2 摇杆
    int16_t ch3;      ///< CH3 摇杆（默认油门）
    int16_t ch4;      ///< CH4 摇杆
    int16_t ch5;      ///< CH5（旧工程未用；分配了开关的话也按连续值给出）
    Switch  switch_a; ///< CH6 SA 两挡开关：只会是 UP / DOWN
    Switch  switch_b; ///< CH7 SB 三挡开关：UP / MID / DOWN
    Switch  switch_c; ///< CH8 SC 三挡开关：UP / MID / DOWN
  };
  /* ==================== */
  /* ==================== 接收诊断 ==================== */

  /**
   * @brief 接收诊断：前四个计数只增不减，最后一个是实时值
   *
   * @note byte_cnt 为 0 说明一个字节都没进来（查接线 / 波特率 / 反相 / 接收机是否切到 W.BUS），
   *       byte_cnt 在涨而 frame_cnt 不动说明字节进来了但切不出帧（查帧格式 / 反相）。
   */
  struct Diag
  {
    uint32_t byte_cnt;       ///< 从串口读到的总字节数
    uint32_t frame_cnt;      ///< 帧头帧尾都正确、已解析的帧数
    uint32_t bad_footer_cnt; ///< 帧头对上了但帧尾不认识的帧数
    uint32_t skip_byte_cnt;  ///< 为了找帧头而丢掉的字节数（正常应远小于 byte_cnt）
    uint32_t lost_streak;    ///< 【实时值】连续多少帧带「本帧丢失」，收到一帧不带就清 0
  };
  /* ==================== */
  /* ==================== 构造与析构 ==================== */

  /** @brief 必须绑定一路已配成 SBUS 的串口，禁用默认构造 */
  DeviceEt08T() = delete;

  /** @brief 绑定串口（只赋值，不碰硬件、不读串口） */
  explicit DeviceEt08T(Uart &uart);

  /** @brief 默认析构（不持有任何需要释放的资源） */
  ~DeviceEt08T() = default;

  // 内部持有解析状态（半截帧、通道值等），复制会让两份状态各自推进
  DeviceEt08T(const DeviceEt08T &)            = delete;
  DeviceEt08T &operator=(const DeviceEt08T &) = delete;
  DeviceEt08T(DeviceEt08T &&)                 = delete;
  DeviceEt08T &operator=(DeviceEt08T &&)      = delete;
  /* ==================== */
  /* ==================== 生命周期 ==================== */

  /**
   * @brief 挂进默认树，并兜底校验串口帧格式
   * @return OK = 串口是 100000 bps / 8E1 或 8E2；BAD_ARG = 帧格式不对（串口参数在 CubeMX 里改，本函数一个字节都不改）
   *
   * @note 解析状态已在构造时复位，所以本函数只做校验；重复调用是幂等的。
   */
  Status init(void);

  /**
   * @brief 把串口里攒的字节全部消化掉（可能一次解出多帧）
   * @param timeout_ms 首字节等待时间：0 = 非阻塞，N = 最多等 N ms（超时即返回）
   *
   * @note 逐字节推进「找帧头 → 凑满 25 字节 → 验帧尾」的同步状态机（理由见 DeviceR9dsT::update()）。
   * @note 只有**首字节**用 timeout_ms 等；一旦开始解帧，后续字节一律 0，把已有数据排空。
   */
  void update(uint32_t timeout_ms = 0U);
  /* ==================== */
  /* ==================== 查询接口 ==================== */

  /**
   * @brief 遥控链路是否可用
   *
   * @note 三个条件同时成立才算在线（与 DeviceR9dsT 相同）：
   *       ① 收到过帧，且最近一帧在 ONLINE_TIMEOUT_MS 内 —— 管“接收机自己静默了”（拔线 / 掉电）；
   *       ② 最近那一帧的 flags 没置「失控保护」—— 管“遥控器失联了”；
   *       ③ 连续带「本帧丢失」的帧数没到 LOST_STREAK_TO_OFFLINE —— ②的提前量。
   * @note 结果只在调用那一刻成立，需要稳定地调用 update()。约定见 docs/code/device.md「在线判定约定」。
   */
  bool is_online(void) const;

  /** @brief 遥控器语义值（**控制逻辑用这个**），实时引用：update() 在别的任务里跑时请用 snapshot() */
  const Rc &get_rc(void) const;

  /** @brief 最近一帧的 16 个原始通道值（协议侧：353 / 1024 / 1694），下标 = 通道号 - 1 */
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
   * @brief 拷贝一份同一帧的遥控器数据（临界区内拷贝，跨任务读用它）
   * @param rc  遥控器语义值
   * @param raw 可选：同一帧的 16 个原始通道值（CHANNEL_NUM 个）；nullptr = 不要
   */
  void snapshot(Rc &rc, uint16_t *raw = nullptr) const;
  /* ==================== */
private:
  /* ==================== 帧解析 ==================== */

  /** @brief 帧尾是否认识：S.BUS 0x00，或 S.BUS2 的 0x04 / 0x14 / 0x24 / 0x34 */
  static bool _is_valid_footer(uint8_t footer);

  /** @brief 解析一帧已通过帧尾校验的数据：16 通道 → 语义值 → flags → 记录帧时刻 */
  void _parse_frame(const uint8_t *frame);
  /* ==================== */
  /* ==================== 原始值换算 ==================== */

  /** @brief 原始值 → 语义值：raw 1694 → +100、1024 → 0、353 → -100（截断取整，带死区） */
  static int16_t _to_rc_value(uint16_t raw);
  /* ==================== */
  /* ==================== 开关判挡 ==================== */

  /** @brief 两挡开关判挡：比离 353 / 1694 哪个近 */
  static Switch _to_switch_2pos(uint16_t raw);

  /** @brief 三挡开关判挡：比离 353 / 1024 / 1694 哪个近 */
  static Switch _to_switch_3pos(uint16_t raw);
  /* ==================== */
  /* ==================== 成员变量 ==================== */

  Uart &_uart; ///< 被解析的串口（不拥有）

  /// 最近一帧的遥控器语义值（未收到帧 = 通道回中位；SA 取 DOWN = 旧工程的“未解锁”，SB / SC 取 MID = 旧工程初值 3）
  Rc       _rc {0, 0, 0, 0, 0, Switch::DOWN, Switch::MID, Switch::MID};
  uint16_t _raw[CHANNEL_NUM] {};  ///< 最近一帧的原始通道值（协议侧）；还没收到帧时由构造函数填中位
  uint8_t  _frame[FRAME_SIZE] {}; ///< 正在拼的帧
  uint32_t _fill = 0U;            ///< 已拼到第几个字节，0 = 还没进入帧
  bool     _frame_lost = false;   ///< 最近一帧 flags 的 bit2：本帧丢失
  bool     _failsafe   = false;   ///< 最近一帧 flags 的 bit3：失控保护激活
  Diag     _diag {};              ///< 诊断计数（frame_cnt 顺带当“收到过帧吗”的标志用）
  EventState _state;              ///< 默认树节点
  bool       _was_online = false; ///< 上一次 update() 结束时是否在线（检测由在线变离线）
  uint32_t _last_frame_tick = 0U; ///< 最近一帧的 tick，is_online() 拿它算“接收机静默了多久”
  /* ==================== */
};

/* ==================== 成员函数定义（只有头文件：任意 UART 类型都能实例化） ==================== */


/* ==================== */
/* ==================== 构造与析构 ==================== */

/** @brief 绑定串口；原始通道先填中位（其余初值见成员声明） */
template <typename Uart>
DeviceEt08T<Uart>::DeviceEt08T(Uart &uart) : _uart(uart)
{
  for (uint32_t i = 0U; i < CHANNEL_NUM; ++i)
  {
    _raw[i] = CHANNEL_MID; // 还没收到帧，原始通道先按中位显示，避免上层读到 0 这种野值
  }
}
/* ==================== */
/* ==================== 生命周期 ==================== */

/**
 * @brief 校验串口帧格式（100000 / 8E1 或 8E2）
 */
template <typename Uart>
Status DeviceEt08T<Uart>::init(void)
{
  // 默认树第三层：挂在所用串口的总线节点下（串口不在清单里时挂 sys_state）；重复调用无害
  if (_state.init() == Status::OK)
  {
    EventState *bus = bus_state(&_uart);
    (void)_state.set_parent((bus != nullptr) ? *bus : sys_state);
  }

  // 8 数据位 + 偶校验 = HAL 的 9 位字长；停止位 1 / 2 都能正确接收 SBUS
  if (!_uart.check_format(100000U, UART_WORDLENGTH_9B, UART_PARITY_EVEN, UART_STOPBITS_2) &&
      !_uart.check_format(100000U, UART_WORDLENGTH_9B, UART_PARITY_EVEN, UART_STOPBITS_1))
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
 *       拼满 25 字节后验帧尾，不认识就整帧丢掉、重新找帧头。
 *       帧体内部出现 0x0F 不会误判 —— 拼帧期间不看内容，只看长度和帧尾。
 */
template <typename Uart>
void DeviceEt08T<Uart>::update(uint32_t timeout_ms)
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

    ++_diag.byte_cnt;

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

    if (_is_valid_footer(_frame[FRAME_SIZE - 1U]))
    {
      ++_diag.frame_cnt;
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

/** @brief 在线判定：帧到 + 没失控 + 没在连续丢帧（各条的作用见声明处） */
template <typename Uart>
bool DeviceEt08T<Uart>::is_online(void) const
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
const typename DeviceEt08T<Uart>::Rc &DeviceEt08T<Uart>::get_rc(void) const
{
  return _rc;
}

/** @brief 最近一帧的 16 个原始通道值（协议侧） */
template <typename Uart>
const uint16_t *DeviceEt08T<Uart>::get_raw_channels(void) const
{
  return _raw;
}

/** @brief 最近一帧的「本帧丢失」标志 */
template <typename Uart>
bool DeviceEt08T<Uart>::is_frame_lost(void) const
{
  return _frame_lost;
}

/** @brief 最近一帧的「失控保护激活」标志 */
template <typename Uart>
bool DeviceEt08T<Uart>::is_failsafe(void) const
{
  return _failsafe;
}

/** @brief 诊断计数快照 */
template <typename Uart>
typename DeviceEt08T<Uart>::Diag DeviceEt08T<Uart>::get_diag(void) const
{
  return _diag;
}

template <typename Uart>
void DeviceEt08T<Uart>::snapshot(Rc &rc, uint16_t *raw) const
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
/* ==================== 帧解析 ==================== */

/**
 * @brief 帧尾是否认识
 *
 * @note 旧工程只认帧头、不看帧尾；这里按 S.BUS / S.BUS2 的帧尾校验，避免帧切歪时把错位数据当通道值。
 *       S.BUS2 的帧尾低 4 位固定 0x4、高 4 位是遥测槽号 0 ~ 3。
 */
template <typename Uart>
bool DeviceEt08T<Uart>::_is_valid_footer(uint8_t footer)
{
  return (footer == 0x00U) || ((footer & 0xCFU) == 0x04U);
}

/**
 * @brief 解析一帧已通过帧尾校验的数据
 *
 * @note 位提取与 DeviceR9dsT::_parse_frame() 相同：第 i 个通道从 bit 11·i 开始，取「起始字节 + 后两字节」
 *       拼成 24 位窗口，右移窗口内偏移后取低 11 位。旧工程手写的 CH1 ~ CH4 与 CH6 ~ CH8 移位式与此一致。
 */
template <typename Uart>
void DeviceEt08T<Uart>::_parse_frame(const uint8_t *frame)
{
  // 解析结果整帧写回，与 snapshot() 互斥
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

  // CH1 ~ CH8，下标 = 通道号 - 1。含义见文件头那张表
  _rc.ch1 = _to_rc_value(_raw[0]);
  _rc.ch2 = _to_rc_value(_raw[1]);
  _rc.ch3 = _to_rc_value(_raw[2]);
  _rc.ch4 = _to_rc_value(_raw[3]);
  _rc.ch5 = _to_rc_value(_raw[4]);

  _rc.switch_a = _to_switch_2pos(_raw[5]); // CH6 SA 两挡
  _rc.switch_b = _to_switch_3pos(_raw[6]); // CH7 SB 三挡
  _rc.switch_c = _to_switch_3pos(_raw[7]); // CH8 SC 三挡

  // frame[23] 是 flags：bit2 = 本帧丢失，bit3 = 失控保护激活（bit0/bit1 是数字通道 17/18）
  const uint8_t flags = frame[FRAME_SIZE - 2U];
  _frame_lost         = (flags & 0x04U) != 0U;
  _failsafe           = (flags & 0x08U) != 0U;

  if (_frame_lost)
  {
    ++_diag.lost_streak;
  }
  else
  {
    _diag.lost_streak = 0U;
  }

  // 帧完整且帧尾正确 = 接收机确实在发帧：记下时刻，作为①“接收机没静默”的依据
  _last_frame_tick = static_cast<uint32_t>(xTaskGetTickCount());
  taskEXIT_CRITICAL();
}
/* ==================== */
/* ==================== 原始值换算 ==================== */

/**
 * @brief 原始值 → 语义值（连续通道）
 *
 * @note 值 = (raw - 1024) × 100 / 671，中位 ±STICK_DEADBAND_RAW 内直接给 0（对应旧工程的回中死区）。
 *       算例：raw 1694 → +100、raw 353 → -100、raw 1360 → +50、raw 1040 → 0（死区内）。
 * @note 故意不限幅到 ±100：舵角调到 100% 以上或通道异常时结果会跟着超出去，一眼看得见。
 */
template <typename Uart>
int16_t DeviceEt08T<Uart>::_to_rc_value(uint16_t raw)
{
  const int32_t offset = static_cast<int32_t>(raw) - static_cast<int32_t>(CHANNEL_MID);
  if ((offset <= static_cast<int32_t>(STICK_DEADBAND_RAW)) && (offset >= -static_cast<int32_t>(STICK_DEADBAND_RAW)))
  {
    return 0;
  }
  return static_cast<int16_t>((offset * 100) / RAW_HALF_SPAN);
}
/* ==================== */
/* ==================== 开关判挡 ==================== */

/** @brief 两挡开关判挡：比离 353 / 1694 哪个近，分界取中位 1024 */
template <typename Uart>
typename DeviceEt08T<Uart>::Switch DeviceEt08T<Uart>::_to_switch_2pos(uint16_t raw)
{
  return (raw < CHANNEL_MID) ? Switch::UP : Switch::DOWN;
}

/**
 * @brief 三挡开关判挡：比离 353 / 1024 / 1694 哪个近
 *
 * @note 分界取相邻两挡的中点：688 与 1359。旧工程是精确匹配（不匹配就保持上一帧），
 *       这里改成就近，舵角 / 微调稍有偏差也能判对。
 */
template <typename Uart>
typename DeviceEt08T<Uart>::Switch DeviceEt08T<Uart>::_to_switch_3pos(uint16_t raw)
{
  if (raw < (CHANNEL_MIN + CHANNEL_MID) / 2U) // < 688
  {
    return Switch::UP;
  }
  if (raw < (CHANNEL_MID + CHANNEL_MAX) / 2U) // < 1359
  {
    return Switch::MID;
  }
  return Switch::DOWN;
}
/* ==================== */

#endif // HAL_UART_MODULE_ENABLED

#endif // __DEVICE_ET08_HPP__
