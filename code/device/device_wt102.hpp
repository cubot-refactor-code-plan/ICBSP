/**
 * @file device_wt102.hpp
 * @author Rh
 * @brief WT102 单轴陀螺仪设备 —— 在 BspUartT 之上把 0x55 帧解成 Z 轴角速度 / 航向角
 * @version 0.1
 * @date 2026-11-06
 *
 * @copyright Copyright (c) 2026
 *
 * @details 只做两件事：从 BspUartT 的接收流缓冲区里切出 11 字节的 0x55 帧，然后把
 *          **Z 轴的三个量**（加速度 az / 角速度 wz / 航向角 yaw）各存最近两帧的原始 int16。
 *          不算 °/s、不算 °、也不组帧：单位换算交给用的人（比例见下面的换算说明）。
 *          不拥有串口、不创建任务、不注册到 Online 链表：纯解析，谁调用 update() 谁负责读。
 *
 * @note 协议（寄存器手册「读格式」章节 + 2026-11-06 实测校正）：
 *       帧长固定 11 字节 = `0x55 | TYPE | D0 D1 D2 D3 D4 D5 D6 D7 | SUM`，
 *       数据为**小端有符号 int16**，`SUM` = 前 10 字节之和取低 8 位。
 *       三型帧轮流发，每型一个固定周期（实测 0x51/0x52/0x53 各约 333~500 Hz）。
 *       数据段一律是 4 个 int16：w0=帧[2..3]、w1=[4..5]、w2=[6..7]、w3=[8..9]。
 *
 *       | TYPE | 含义   | w0            | w1         | w2            | w3            |
 *       |------|--------|---------------|------------|---------------|---------------|
 *       | 0x51 | 加速度 | ax            | ay         | az            | 温度 ×0.01 ℃  |
 *       | 0x52 | 角速度 | 缓变量(19~20) | 另一根轴   | **Z 轴角速度** | 0x8A0E 常量   |
 *       | 0x53 | 角度   | 0             | 另一轴角度 | **Yaw**       | 0x0101 常量   |
 *
 *       判据（实测）：`0x52[6..7]` 按 500 Hz 积分后与 `0x53[6..7]` 的每秒变化量完全吻合
 *       （dyaw=+9.0° vs ∫=+9.0°、dyaw=-16.8° vs ∫=-16.9°），而 `[4..5]` 的积分与之无关。
 *       所以 **[6..7] 就是校准后的 Z 轴角速度**，`[4..5]` 不是原始 Z（手册写的 0x00 也不对）。
 *
 *       换算：`Wz = w2 / 32768 * 2000 (°/s)`、`Yaw = w2 / 32768 * 180 (°)`（同一组比例已被积分对账验证）。
 *
 * @warning 不是线程安全的：`update()` 只允许由一个任务调用（串口流缓冲区也是单读者）；
 *          查询接口返回实时值，读取期间不得并发执行 update()。
 */

#ifndef __DEVICE_WT102_HPP__
#define __DEVICE_WT102_HPP__

#include "bsp_series.hpp" // IWYU pragma: keep（HAL 模块开关 / 系列桥接）

// 宿主工程没在 CubeMX 里开 UART（HAL_UART_MODULE_ENABLED 未定义）时，本模块整体不参与编译
#ifdef HAL_UART_MODULE_ENABLED


#include "status.hpp"
#include <stdint.h>
#include "bsp_uart.hpp" // IWYU pragma: keep（BspUartT 是本身的模板参数；UART 宏也靠它）


/**
 * @brief WT102 单轴陀螺仪设备
 *
 * @note 不拥有串口、不创建任务、不创建 RTOS 资源：只解析调用方喂进来的字节流，
 *       在线判定自己按 tick 算（与 DeviceR9dsT 同一套约定）。
 */
template <typename Uart>
class DeviceWt102T
{
public:
  /* ==================== 帧结构常量 ==================== */

  static constexpr uint32_t FRAME_SIZE   = 11U;   ///< 一帧字节数：帧头 + 帧型 + 8 字节数据 + 校验
  static constexpr uint8_t  FRAME_HEADER = 0x55U; ///< 帧头标识
  static constexpr uint8_t  TYPE_ACC     = 0x51U; ///< 帧型：加速度（[6][7] = az）
  static constexpr uint8_t  TYPE_GYRO    = 0x52U; ///< 帧型：角速度（[6][7] = wz）
  static constexpr uint8_t  TYPE_ANGLE   = 0x53U; ///< 帧型：角度（[6][7] = yaw）
  static constexpr uint8_t  SUM_INDEX    = FRAME_SIZE - 1U; ///< 校验和所在下标
  /* ==================== */
  /* ==================== 协议数值常量 ==================== */

  static constexpr float GYRO_FULL_SCALE_DPS  = 2000.0f;  ///< 角速度满量程 ±2000 °/s
  static constexpr float ANGLE_FULL_SCALE_DEG = 180.0f;   ///< 航向角满量程 ±180 °
  static constexpr float ACC_FULL_SCALE_G     = 16.0f;    ///< 加速度满量程 ±16 g（实测：静止时 az≈-2065→-1 g）
  static constexpr float RAW_FULL_SCALE       = 32768.0f; ///< int16 原始值满量程
  /* ==================== */
  /* ==================== 时基常量 ==================== */

  /**
   * @brief 静默多久就判离线 (ms)
   *
   * @note 帧周期 2 ms（500 Hz），取 50 ms = 25 帧，足够抗偶发丢帧、又能在拔线后 50 ms 内报出。
   * @note 计时在 is_online() 里按 tick 现算，不需要任何周期任务推进它。
   */
  static constexpr uint16_t ONLINE_TIMEOUT_MS = 50U;
  /* ==================== */
  /* ==================== 接收诊断 ==================== */

  /**
   * @brief 接收诊断：全部只增不减
   *
   * @note 专门用来区分几种"收不到数据"：byte_cnt 为 0 说明一个字节都没进来（查接线 / 波特率）；
   *       byte_cnt 在涨而 frame_cnt 不动说明字节进来了但切不出帧（查帧格式 / 校验）；
   *       csum_err_cnt 在涨说明帧头对上了但校验不过（查波特率偏差 / 干扰）。
   */
  struct Diag
  {
    uint32_t byte_cnt;      ///< 从串口读到的总字节数
    uint32_t frame_cnt;     ///< 校验通过、已解析的帧数（全部帧型合计）
    uint32_t gyro_cnt;      ///< 其中 0x52（角速度）帧数
    uint32_t angle_cnt;     ///< 其中 0x53（角度）帧数
    uint32_t acc_cnt;       ///< 其中 0x51（加速度）帧数
    uint32_t other_cnt;     ///< 其它没见过的帧型
    uint32_t csum_err_cnt;  ///< 长度凑够但校验和不对的帧数
    uint32_t skip_byte_cnt; ///< 为了找帧头而丢掉的字节数（正常应远小于 byte_cnt）
  };
  /* ==================== */
  /* ==================== 构造与析构 ==================== */

  /** @brief 必须绑定一路已配成 230400 8N1 的串口，禁用默认构造 */
  DeviceWt102T() = delete;

  /** @brief 绑定串口（只赋值，不碰硬件、不读串口） */
  explicit DeviceWt102T(Uart &uart);

  /** @brief 默认析构（不持有任何需要释放的资源） */
  ~DeviceWt102T() = default;

  // 内部持有解析状态（半截帧、最新值等），复制会让两份状态各自推进
  DeviceWt102T(const DeviceWt102T &)            = delete;
  DeviceWt102T &operator=(const DeviceWt102T &) = delete;
  DeviceWt102T(DeviceWt102T &&)                 = delete;
  DeviceWt102T &operator=(DeviceWt102T &&)      = delete;
  /* ==================== */
  /* ==================== 生命周期 ==================== */

  /**
   * @brief 校验串口帧格式并复位解析状态 / 诊断计数
   * @return OK=已复位；BAD_ARG=CubeMX 里的帧格式不是 230400 8N1
   *
   * @note 串口本身由 board_init() 负责 init()，本函数一个字节都不改，只做兜底校验；
   *       重复调用是幂等的（会清掉已有数据和诊断计数）。
   */
  Status init(void);

  /**
   * @brief 把串口里攒的字节全部消化掉（可能一次解出多帧）
   * @param timeout_ms 首字节等待时间：0 = 非阻塞，N = 最多等 N ms（超时即返回）
   *
   * @note 逐字节推进「找 0x55 → 认帧型 → 凑满 11 字节 → 验校验和」的状态机。
   *       逐字节读是因为 StreamBuffer 的 receive 只保证有多少给多少，先按 11 字节读
   *       可能只拿到一部分，反而会把帧切歪。
   * @note 只有**首字节**用 timeout_ms 等；一旦开始解帧，后续字节一律 0，把已有数据排空。
   */
  void update(uint32_t timeout_ms = 0U);
  /* ==================== */
  /* ==================== 查询接口 ==================== */

  /**
   * @brief Z 轴的三个量（都是协议原始 int16，没做单位换算）
   *
   * @note 每解出一帧就整体往后挪一位：`z_newest()` 是最新一帧的快照、`z_previous()` 是上一帧。
   *       三个量分别来自三种帧（都在各自帧的 [6][7]），所以相邻两帧通常只有一个字段变。
   * @note 换算：`az = acc_z / 32768 * 16 (g)`、`wz = gyro_z / 32768 * 2000 (°/s)`、
   *       `yaw = yaw / 32768 * 180 (°)`。
   */
  struct ZData
  {
    int16_t acc_z;  ///< 0x51 帧 [6][7]：Z 轴加速度
    int16_t gyro_z; ///< 0x52 帧 [6][7]：Z 轴角速度（已含零偏补偿）
    int16_t yaw;    ///< 0x53 帧 [6][7]：航向角
  };

  /** @brief 最近 ONLINE_TIMEOUT_MS 内有过一帧校验通过的帧（从没收到过帧 = 离线） */
  bool is_online(void) const;

  /** @brief 最新一帧的三个 Z 轴原始值 */
  const ZData &z_newest(void) const;

  /** @brief 上一帧的三个 Z 轴原始值 */
  const ZData &z_previous(void) const;

  /** @brief 诊断计数快照 */
  Diag get_diag(void) const;
  /* ==================== */
private:
  /* ==================== 帧解析 ==================== */

  /** @brief 解析一帧已通过校验的数据：把该帧的 Z 轴量写进最新快照，并记录帧时刻 */
  void _parse_frame(const uint8_t *frame);

  /** @brief 取小端有符号 int16（逐字节拼，不依赖内存对齐） */
  static int16_t _le16(const uint8_t *p);
  /* ==================== */
  /* ==================== 成员变量 ==================== */

  Uart &_uart; ///< 被解析的串口（不拥有）

  uint8_t  _frame[FRAME_SIZE] {}; ///< 正在拼的帧
  uint32_t _fill = 0U;            ///< 已拼到第几个字节，0 = 还没进入帧

  ZData    _z[2] {};              ///< [0] 最新一帧、[1] 上一帧（一共 6 个 int16）

  Diag     _diag {};              ///< 诊断计数（frame_cnt 顺带当“收到过帧吗”的标志用）
  uint32_t _last_frame_tick = 0U; ///< 最近一帧的 tick，is_online() 拿它算“静默了多久”
  /* ==================== */
};

/* 成员函数定义在 device_wt102.cpp
 * 本类是模板，实现放 .cpp、靠文件末尾的显式实例化提供符号：
 * 已实例化 <dma,128,128> / <it,128,128> / <it,128,32> 三种组合，
 * 板级换别的组合要去 device_wt102.cpp 补一行，否则会在**链接期**报 undefined reference。 */

#endif // HAL_UART_MODULE_ENABLED

#endif // __DEVICE_WT102_HPP__
