/**
 * @file quaternion_pid.hpp
 * @brief 姿态 PID：目标 / 反馈四元数 → 机体系三轴控制量
 */

#ifndef __QUATERNION_PID_HPP__
#define __QUATERNION_PID_HPP__

#include <stdint.h>

#include "pid.hpp"
#include "quaternion.hpp"


/**
 * @brief 姿态 PID：输入目标 / 反馈姿态四元数，输出机体系三轴控制量
 *
 * @details 误差用旋转向量代替减法：e = feedback.error_to(target) = Log(feedback* ⊗ target)，
 *          即从反馈姿态转到目标姿态的旋转（机体系，rad，取 [0, π] 的短路径；小误差时 e 的三个分量 ≈ 绕机体 x / y / z 的角度误差）。
 *          三轴各交给一个 PID（复用比例、积分分离、各项限幅、前馈），输出向量 u 也在机体系；
 *          作外环时 u 是角速度目标（交给三轴标量角速度 PID，反馈直接用陀螺仪），作单环时 u 是力矩。
 *
 * @note 微分项 = 误差变化率 = ω_target（转到当前机体系）− ω_feedback，按每周期转角计（与标量 PID 的每周期差分一致）：
 *       - DIFF_FEEDBACK（默认，微分先行）：只对反馈微分，D = −kd · ω_fb · dt，目标突变不产生微分冲击；
 *       - DIFF_ERROR：D = kd · (ω_tar − ω_fb) · dt；
 *       - DISABLE_DIFF：D = 0。
 *       不传角速度时，ω_fb · dt 由相邻两次反馈四元数估算（Log(q_fb,上次* ⊗ q_fb)），ω_tar · dt 同理由相邻两次目标估算；
 *       传入陀螺仪角速度时按 Config::dt 换算，kd 的含义不变。不传角速度时，首次调用（或 reset() 后）没有上一帧，D = 0。
 *
 * @note 限幅：先按各轴 PID::Config 的限幅（p / i / d / f / out_max，分量限幅），再按 out_norm_max 把输出向量等比例缩小（保持方向）。
 *       只用分量限幅时，饱和会改变输出向量的方向（机构沿歪掉的轴转），三轴耦合的场合建议设 out_norm_max。
 *
 * @note 适用：转轴就是机体 x / y / z 的全姿态控制（飞行器、平衡、三轴云台）。两轴云台的 yaw 电机转轴是底盘竖直方向而不是机体 z 轴，
 *       用 Quaternion::pointing() 取水平角 / 仰角、再配标量 PID（误差用 angle_diff()）更直接。
 * @note 误差正好越过 180° 时短路径方向反转，输出随之反向（任何姿态误差定义都有这一点）；正常控制下误差不会到这么大。
 * @note 三轴的微分模式由本类统一决定：构造时把三轴 PID 设为 DIFF_ERROR，再把算好的误差变化率 de 以 PID::calc(e, 0, -de) 传入（df_dt 是反馈变化率，等于 -de），
 *       之后不要再对 axis(i) 调 diff_mode()。
 */
class QuaternionPID
{
public:
  /* ==================== 构造 ==================== */

  /**
   * @brief 姿态 PID 配置（可匿名按序传入：{{x 轴}, {y 轴}, {z 轴}, diff_mode, dt, out_norm_max}）
   * @note 各轴 PID::Config::diff_mode 不起作用，微分模式统一用这里的 diff_mode。
   */
  struct Config
  {
    /** @brief 按序构造（参数顺序 = 字段顺序） */
    Config(const PID::Config &x = PID::Config(), const PID::Config &y = PID::Config(), const PID::Config &z = PID::Config(),
           DiffMode diff_mode_ = DiffMode::DIFF_FEEDBACK, float dt_ = 0.001f, float out_norm_max_ = 0.0f)
      : axis{x, y, z},
        diff_mode(diff_mode_),
        dt(dt_),
        out_norm_max(out_norm_max_)
    {
    }

    PID::Config axis[3];      ///< 机体 x / y / z 三轴的 PID 配置（误差单位 rad）
    DiffMode    diff_mode;    ///< 微分模式（三轴统一）
    float       dt;           ///< 控制周期 (s)：只用于把传入的角速度 (rad/s) 换算成每周期转角
    float       out_norm_max; ///< 输出向量模长限幅，0=不限；等比例缩放、保持方向
  };

  QuaternionPID() = default;

  /**
   * @brief 用配置结构体构造
   * @param cfg 姿态 PID 配置（可匿名按序传入）
   */
  QuaternionPID(const Config &cfg);

  /* ==================== */
  /* ==================== 计算接口 ==================== */

  /**
   * @brief 姿态 PID 计算，微分输入由相邻两次的目标 / 反馈四元数估算
   *
   * @param target   目标姿态（机体 → 世界，单位四元数）
   * @param feedback 反馈姿态（同上，通常来自 IMU）
   * @return Vector3 机体系三轴输出
   */
  Vector3 calc(const Quaternion &target, const Quaternion &feedback);

  /**
   * @brief 姿态 PID 计算，反馈角速度由陀螺仪导入（推荐：比四元数差分噪声小、没有一帧延迟）
   *
   * @param target         目标姿态
   * @param feedback       反馈姿态
   * @param omega_feedback 反馈角速度，机体系 rad/s（陀螺仪读数）
   * @note 目标角速度按 0 处理：DIFF_ERROR 下 D = −kd · ω_fb · dt，与 DIFF_FEEDBACK 相同；目标在动时用下面带 omega_target 的重载。
   * @return Vector3 机体系三轴输出
   */
  Vector3 calc(const Quaternion &target, const Quaternion &feedback, const Vector3 &omega_feedback);

  /**
   * @brief 姿态 PID 计算，目标与反馈角速度都由外部导入
   *
   * @param target         目标姿态
   * @param feedback       反馈姿态
   * @param omega_target   目标角速度，目标机体系 rad/s（例如生成目标轨迹时用的角速度）；内部按姿态误差转到当前机体系
   * @param omega_feedback 反馈角速度，机体系 rad/s
   * @note 只有 DIFF_ERROR 用到 omega_target；DIFF_FEEDBACK 按定义只对反馈微分。
   * @return Vector3 机体系三轴输出
   */
  Vector3 calc(const Quaternion &target, const Quaternion &feedback, const Vector3 &omega_target, const Vector3 &omega_feedback);

  /**
   * @brief 三轴前馈（机体系），每周期需要重新喂，语义同 PID::feed_forward()
   * @param feedforward 三轴前馈值（例如重力矩补偿）
   */
  void feed_forward(const Vector3 &feedforward);

  /**
   * @brief 清空运行状态：三轴 PID 的积分 / 历史输入、上一帧目标 / 反馈；配置不变
   */
  void reset();

  /* ==================== */
  /* ==================== 数据接口 ==================== */

  /**
   * @brief 取某一轴的 PID：调参（链式设置 kp / ki / kd / 限幅）或查看该轴各项
   * @param i 0 / 1 / 2 = 机体 x / y / z；越界时取 z 轴
   */
  PID &axis(uint32_t i)
  {
    return _axis[(i < 3U) ? i : 2U];
  }

  /** @brief 最近一次的姿态误差（机体系旋转向量，rad） */
  const Vector3 &error() const
  {
    return _error;
  }

  /** @brief 最近一次的输出（机体系） */
  const Vector3 &output() const
  {
    return _output;
  }

private:
  /**
   * @brief 三个 calc() 的公共部分：求姿态误差、按微分模式求误差变化率、记下本帧
   * @param rot_target   本周期目标转角（目标机体系，rad）
   * @param rot_feedback 本周期反馈转角（当前机体系，rad）
   */
  Vector3 step(const Quaternion &target, const Quaternion &feedback, const Vector3 &rot_target, const Vector3 &rot_feedback);

  /**
   * @brief 三轴 PID + 输出模长限幅
   * @param error 姿态误差（rad）
   * @param d_in  三轴误差变化率（每周期，已按微分模式算好）
   */
  Vector3 run_axes(const Vector3 &error, const Vector3 &d_in);

  Config     _config;                               ///< 配置
  PID        _axis[3];                              ///< 机体 x / y / z 三轴 PID
  Vector3    _error         = {0.0, 0.0, 0.0};      ///< 最近一次姿态误差
  Vector3    _output        = {0.0, 0.0, 0.0};      ///< 最近一次输出
  Quaternion _last_target   = {1.0, 0.0, 0.0, 0.0}; ///< 上一次目标（差分估算 ω_tar 用）
  Quaternion _last_feedback = {1.0, 0.0, 0.0, 0.0}; ///< 上一次反馈（差分估算 ω_fb 用）
  bool       _has_last      = false;                ///< 是否有上一帧（首次调用 / reset() 后为 false，D = 0）
};

  /* ==================== */

#endif // __QUATERNION_PID_HPP__
