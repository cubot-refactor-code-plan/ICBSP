/**
 * @file device_motor.hpp
 * @author Rh
 * @brief 直流电机设备 —— 双 PWM（H 桥 IN1/IN2）+ 编码器 + 单套速度环 PID
 * @version 0.1
 * @date 2026-10-29
 *
 * @copyright Copyright (c) 2026
 *
 * @details 一个实例 = 一个电机：两路 PWM 做 H 桥输入，一个编码器做测速，一套 PID 做速度闭环。
 *          本类只做「器件语义」，底层全在 BSP：PWM 占空比走 BspPwm，计数走 BspEncoder，
 *          控制算法走 algorithm/pid.hpp 的 PID。
 *
 * @note 换算链：编码器线数 × 倍频 × 减速比 = 输出轴每转计数；
 *       每转行程 = π × 轮子直径 ⇒ counts_per_cm = 每转计数 / 每转行程(cm)。
 *       机械参数全部由 Config 传入，本类不写死任何整车常量。
 *
 * @note 单套 PID，不做速度分区切换（需要时在上层换参数即可）。
 * @note update() 的周期由调用方通过 dt_s 传入 —— 控制频率是「可调」的：1 kHz、2 kHz 都行。
 *       但 pid 的 ki / kd 是**每拍量纲**，改频率会改变 I、D 的等效强度，换频率要重新标定。
 *
 * @warning 双 PWM 互斥：任何时刻最多只有一路非 0，否则 H 桥直通。本类在切换方向时先写 0 再写有效路。
 * @warning 非线程安全：set_*() 只写目标值，update() 是唯一的 PWM 写入者，需与调用方同任务。
 */

#ifndef __DEVICE_MOTOR_HPP__
#define __DEVICE_MOTOR_HPP__

#include "bsp/bsp_series.hpp" // IWYU pragma: keep（HAL 模块开关 / 系列桥接）

// 宿主工程没在 CubeMX 里开 TIM（HAL_TIM_MODULE_ENABLED 未定义）时，本模块整体不参与编译
#ifdef HAL_TIM_MODULE_ENABLED

#include "bsp/bsp_encoder.hpp" // 测速反馈
#include "bsp/bsp_pwm.hpp"     // H 桥两路输出
#include "algorithm/pid.hpp"         // 速度环
#include "service/system/status.hpp"      // 统一状态码

#include <stddef.h>
#include <stdint.h>

/**
 * @brief 电机设备类
 *
 * @note 生命周期：init() 一次 → 每拍 update(dt_s)；init() 前 update() 安全返回 false。
 * @note 不绑定串口：JustFloat 帧由 pid_frame() 组好交给调用方，发送在 App 层做。
 */
class DeviceMotor
{
public:
  /* ==================== 配置 ==================== */

  /** @brief 运动模式 */
  enum class Mode : uint8_t
  {
    STOP = 0,   ///< 刹车：两路 PWM 都给满占空比（绕组短路），不再跑 PID
    SPEED,      ///< 纯速度：一直跑，直到被 stop() 或改目标
    SPEED_TIME, ///< 速度 + 时间：跑满指定时长后自动停止
  };

  /**
   * @brief 电机配置
   *
   * @note pwm_a / pwm_b / encoder 三个指针**互不相关**，按实际接线自由组合，不要求同序号对应。
   */
  struct Config
  {
    BspPwm     *pwm_a   = nullptr; ///< H 桥 IN1
    BspPwm     *pwm_b   = nullptr; ///< H 桥 IN2
    BspEncoder *encoder = nullptr; ///< 同轴编码器

    // ---- 机械参数：用来把「计数」换算成「cm」----
    float encoder_lines      = 0.0f; ///< 编码器线数（单相每转脉冲数，如 500）
    float encoder_multiplier = 4.0f; ///< 倍频：Timer 的 TI12 模式 = 4，TI1 = 2，单相 = 1
    float gear_ratio         = 0.0f; ///< 减速比：电机轴转 gear_ratio 圈 = 输出轴 1 圈（如 32）
    float wheel_diameter_cm  = 0.0f; ///< 轮子直径 (cm)（半径 35 mm ⇒ 7.0）

    float out_max = 100.0f; ///< 输出限幅（占空比 %），0 < out_max ≤ 100
    bool  invert  = false;  ///< 方向取反：实测「正目标反而倒转」时置 true

    /**
     * @brief 零速死区 (cm/s)：|目标| 与 |实测| 同时小于它时直接断输出，0 = 关闭
     *
     * @note 静止时编码器在边沿附近 ±1 抖动（1 ms 一拍 1 计数 ≈ 0.34 cm/s），
     *       纯 P 会把它放大成 0.1~0.3% 的占空比抖动，1 kHz 载波听起来就是持续嗡嗡声。
     * @note 默认 0.4：即 ±0.4 cm/s 内都当作 0；代价是小于该阈值的目标不会输出。
     */
    float deadband_cm_s = 0.4f;

    PID::Config pid {}; ///< 速度环参数（默认全 0，靠实机标定）
  };

  /* ==================== */
  /* ==================== 公有接口 ==================== */

  /** @brief 默认构造：未绑定，init() 返回 BAD_ARG */
  DeviceMotor() = default;

  /** @brief 构造：只保存配置（不碰硬件） */
  explicit DeviceMotor(const Config &cfg);

  /**
   * @brief 初始化：校验绑定与机械参数、算出 counts_per_cm、把两路 PWM 归 0
   *
   * @return OK=成功；BAD_ARG=指针/机械参数非法；NOT_INIT=编码器还没 init()
   *
   * @note 须在 board_init() 之后调用（编码器已启动、PWM 已 Start）。
   */
  Status init();

  /** @brief 纯速度模式：目标速度 (cm/s)，正负表示方向；一直保持 */
  void set_speed(float cm_s);

  /**
   * @brief 速度 + 时间模式：跑 duration_ms 后自动 stop()
   * @param cm_s 目标速度 (cm/s)
   * @param duration_ms 持续时间 (ms)
   */
  void set_speed_time(float cm_s, uint32_t duration_ms);

  /**
   * @brief 立即停止（刹车）
   *
   * @note 两路 PWM 同时给满占空比，把电机绕组短路；不跑 PID、清积分。
   *       与零速死区/`set_speed(0)` 不同：后两者是把占空比打到 0（滑行）。
   * @warning 高速时突然刹车会产生较大的制动电流，确认驱动板能受得住。
   */
  void stop();

  /**
   * @brief 跑一拍闭环（唯一碰 PWM 的地方）
   *
   * @param dt_s 距上一拍的间隔 (s)：1 kHz 填 0.001，2 kHz 填 0.0005
   * @return true = 本次调用刚好把「速度+时间」模式跑完并停了车
   *
   * @note dt_s 由调用方用 bsp_dwt.get_delta_s() 量出来最准；<=0 时本拍直接返回。
   */
  bool update(float dt_s);

  /** @brief JustFloat 可拼的数据位 */
  enum PidField : uint8_t
  {
    PID_TARGET   = 1U << 0, ///< 目标值
    PID_FEEDBACK = 1U << 1, ///< 反馈值
    PID_OUTPUT   = 1U << 2, ///< PID 原始输出
    PID_ALL      = PID_TARGET | PID_FEEDBACK | PID_OUTPUT,
  };

  /**
   * @brief 按 JustFloat 协议组帧，返回实际长度
   *
   * @param buf    输出缓冲，至少 16 字节
   * @param fields 要拼的数据：PID_TARGET / PID_FEEDBACK / PID_OUTPUT 任意组合
   * @return 实际字节数（4 × 数据个数 + 4）；buf 为空或 fields 无效返回 0
   *
   * @note 帧尾固定 00 00 80 7F；本函数只组帧，发送由调用方负责。
   */
  size_t pid_frame(uint8_t *buf, uint8_t fields = PID_ALL) const;

  /* ==================== */
  /* ==================== 查询 ==================== */

  /** @brief 三路绑定是否齐全（用来决定要不要 init/update） */
  bool is_bound() const { return _config.pwm_a != nullptr && _config.pwm_b != nullptr && _config.encoder != nullptr; }

  /** @brief 是否已成功 init() */
  bool initialized() const { return _initialized; }

  /** @brief 换算结果：每厘米多少个计数（由机械参数算出） */
  float counts_per_cm() const { return _counts_per_cm; }

  /** @brief 最近一拍实测速度 (cm/s) */
  float speed_cm_s() const { return _speed_cm_s; }

  /** @brief 最近一拍占空比（带符号，%）：正 = pwm_a 出力，负 = pwm_b 出力 */
  float output_pct() const { return _output_pct; }

  /** @brief 当前模式 */
  Mode mode() const { return _mode; }

  /** @brief 当前目标速度 (cm/s) */
  float target_cm_s() const { return _target_cm_s; }

  /* ==================== */
private:
  /* ==================== 私有方法 ==================== */

  /** @brief 把带符号占空比写到 H 桥：先给该关的那一路写 0，再给有效那一路写值 */
  void _apply_output(float pct);

  /** @brief 短路制动：两路同时给满占空比（H 桥下端/上端同时导通，绕组被短接） */
  void _brake();

  /* ==================== */
  /* ==================== 成员变量 ==================== */

  Config     _config;             ///< 绑定与机械参数
  PID        _pid;                ///< 速度环
  float      _counts_per_cm = 0.0f; ///< 计数 → cm（init() 算一次）
  float      _target_cm_s   = 0.0f; ///< 目标速度
  float      _speed_cm_s    = 0.0f; ///< 实测速度
  float      _output_pct    = 0.0f; ///< 最近一拍占空比（带符号）
  float      _remaining_s   = 0.0f; ///< 速度+时间模式的剩余时间 (s)
  Mode       _mode          = Mode::STOP;
  bool       _initialized   = false;
  bool       _idle          = false; ///< 是否已在零速死区内（只在进入时清一次积分）

  /* ==================== */
};

#endif // HAL_TIM_MODULE_ENABLED

#endif // __DEVICE_MOTOR_HPP__
