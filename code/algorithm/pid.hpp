/**
 * @file pid.hpp
 * @author ChoseB (ChoseB@cumt.edu.cn)
 * @brief PID算法的封装
 * @version 0.2
 * @date 2026-03-07
 *
 * @copyright Copyright (c) 2026
 *
 * @note 使用示例
 *
 * @note 实例化（链式配置，参数含义一目了然）
 *
 *      // 只给系数，其余全默认
 *      PID motor1;
 *      motor1.kp(0.8).ki(0).kd(0);
 *
 *      // 全参数链式配置
 *      PID motor2;
 *      motor2.kp(0.8).ki(1).kd(1)
 *            .limit_output(10000).limit_p(3000)
 *            .limit_d(8000).integral_sep(50);
 *
 *      // 用配置结构体一次性配置（匿名按序传入，参数顺序 = 字段顺序）
 *      // kp, ki, kd, out_max, p_max, i_max, d_max, f_max, i_sep, diff_mode
 *      PID motor3({0.8f, 1.0f, 1.0f, 10000.0f, 3000.0f, 0.0f, 8000.0f, 0.0f, 50.0f});
 *
 *      // 复制（数组批量创建）
 *      PID motors[4] = {motor2, motor2, motor2, motor2};
 *
 *      // 微分模式默认 DIFF_FEEDBACK（微分先行），不适合时切换
 *      motor3.diff_mode(DiffMode::DIFF_ERROR);
 *
 * @note 使用(假设已有PID对象pid)
 *
 *      pid.feed_forward(FrictionFF());           // optional
 *      pid.feed_forward(FollowFF(dTarget));      // optional
 *      pid.calc(tar, motor.angle, motor.speed); // 最后一个参数是可去掉的
 *
 * @note 姿态 PID 见 quaternion_pid.hpp。
 */

#ifndef __PID_HPP__
#define __PID_HPP__

#include <stdint.h>


/**
 * @brief 微分项计算模式
 */
enum class DiffMode
{
  DISABLE_DIFF  = 0, ///< 不计算微分项
  DIFF_FEEDBACK = 1, ///< 微分先行：只对反馈值微分，D = -kd·Δfeedback，目标阶跃不产生微分冲击
  DIFF_ERROR    = 2, ///< 常规微分：对误差微分，D = kd·Δerror
};

/**
 * @brief PID 输入缓存结构体
 */
struct PidInput
{
  float target       = 0.0f; ///< 当前目标值
  float feedback     = 0.0f; ///< 当前反馈值
  float error        = 0.0f; ///< 当前误差，error = target - feedback
  float last_feedback  = 0.0f; ///< 上一反馈值
  float last_error   = 0.0f; ///< 上一误差值
  float delta_feedback = 0.0f; ///< 反馈值微分，delta_feedback = feedback - last_feedback
  float delta_error  = 0.0f; ///< 误差值微分，delta_error = error - last_error
};

/**
 * @brief PID 各项计算值结构体
 */
struct PidTerm
{
  float p_term = 0.0f; ///< 比例项计算值
  float i_term = 0.0f; ///< 积分项计算值
  float d_term = 0.0f; ///< 微分项计算值
  float f_term = 0.0f; ///< 前馈项计算值
};

/**
 * @brief PID 控制器类
 *
 * @note 位置式 PID，支持积分分离、微分先行、前馈、各项独立限幅
 */
class PID
{
public:
  /* ==================== 构造与链式配置 ==================== */

  /**
   * @brief PID 配置结构体（可匿名按序传入）
   *
   * @note 限幅写 0 表示不限制；i_max 写 0 表示跟随 out_max
   */
  struct Config
  {
    /**
     * @brief 按序构造配置（参数顺序 = 字段顺序）
     */
    Config(float kp_ = 0.0f, float ki_ = 0.0f, float kd_ = 0.0f, float out_max_ = 0.0f, float p_max_ = 0.0f,
           float i_max_ = 0.0f, float d_max_ = 0.0f, float f_max_ = 0.0f, float i_sep_ = 0.0f,
           DiffMode diff_mode_ = DiffMode::DIFF_FEEDBACK)
      : kp(kp_),
        ki(ki_),
        kd(kd_),
        out_max(out_max_),
        p_max(p_max_),
        i_max(i_max_),
        d_max(d_max_),
        f_max(f_max_),
        i_sep(i_sep_),
        diff_mode(diff_mode_)
    {
    }

    float kp;       ///< 比例项系数
    float ki;       ///< 积分项系数
    float kd;       ///< 微分项系数
    float out_max;  ///< 总输出限幅，0=不限
    float p_max;    ///< 比例项限幅，0=不限
    float i_max;    ///< 积分项限幅，0=跟随out_max
    float d_max;    ///< 微分项限幅，0=不限
    float f_max;    ///< 前馈项限幅，0=不限
    float i_sep;    ///< 积分分离阈值，0=不分离
    DiffMode diff_mode; ///< 微分计算模式
  };

  PID() = default;

  /**
   * @brief 用配置结构体构造
   * @param cfg PID 配置（可匿名按序传入）
   */
  PID(const Config& cfg);

  /**
   * @brief 设置比例系数
   */
  PID& kp(float value)
  {
    _config.kp = value;
    return *this;
  }

  /**
   * @brief 设置积分系数
   */
  PID& ki(float value)
  {
    _config.ki = value;
    return *this;
  }

  /**
   * @brief 设置微分系数
   */
  PID& kd(float value)
  {
    _config.kd = value;
    return *this;
  }

  /**
   * @brief 设置总输出限幅
   */
  PID& limit_output(float value)
  {
    _config.out_max = value;
    return *this;
  }

  /**
   * @brief 设置比例项限幅
   */
  PID& limit_p(float value)
  {
    _config.p_max = value;
    return *this;
  }

  /**
   * @brief 设置积分项限幅
   */
  PID& limit_i(float value)
  {
    _config.i_max = value;
    return *this;
  }

  /**
   * @brief 设置微分项限幅
   */
  PID& limit_d(float value)
  {
    _config.d_max = value;
    return *this;
  }

  /**
   * @brief 设置前馈项限幅
   */
  PID& limit_f(float value)
  {
    _config.f_max = value;
    return *this;
  }

  /**
   * @brief 设置积分分离阈值
   */
  PID& integral_sep(float value)
  {
    _config.i_sep = value;
    return *this;
  }

  /**
   * @brief 切换微分项计算模式
   */
  PID& diff_mode(DiffMode mode)
  {
    _diff_mode = mode;
    return *this;
  }

  /* ==================== */
  /* ==================== 计算接口 ==================== */

  /**
   * @brief 将某一前馈函数预测的值引入计算
   *
   * @param feedforward 某一前馈函数返回的数值
   * @return float 前馈项总和
   */
  float feed_forward(float feedforward);

  /**
   * @brief pid计算函数
   *
   * @param target 目标值
   * @param feedback 反馈值
   * @return float 输出
   * @note 输入不是有限值（NaN / Inf）时丢弃这一拍：状态不变、返回上一拍的输出，计入 nonfinite_count()（F1）。
   */
  float calc(float target, float feedback);

  /**
   * @brief pid计算函数，反馈的每拍增量自行导入
   *
   * @param target 目标值
   * @param feedback 反馈值
   * @param delta_feedback 本拍反馈相对上一拍的增量（代替相邻两次反馈的差分，**不是每秒的导数**，N37）。
   *              DIFF_FEEDBACK：D = -kd·delta_feedback；DIFF_ERROR：按目标不动处理，误差变化率 = -delta_feedback，D = -kd·delta_feedback
   * @note 示例：被控量为位移 x、控制周期为 dt 时，填“速度 v × dt”能提高微分项精度；直接填速度 v 相当于 kd 放大了 1/dt 倍。
   * @note 任一输入不是有限值时丢弃这一拍（同两参数版本）。
   * @return float 输出
   */
  float calc(float target, float feedback, float delta_feedback);

  /**
   * @brief 清空运行状态：积分项、前馈项、输入缓存与输出；配置（含链式设置）不变
   * @note 控制器停用一段时间后重新启用时调用，避免旧积分与旧反馈造成冲击；之后第一次 calc() 的差分微分为 0。
   */
  void reset();

  /** @brief PID 最近一拍的输入与原始输出（供波形 / 调试读取） */
  struct Data
  {
    float target;   ///< 最近一次 calc() 的目标值
    float feedback; ///< 最近一次 calc() 的反馈值
    float output;   ///< 最近一次限幅后的原始输出
  };

  /** @brief 读取目标值、当前值、原始输出 */
  Data get() const { return {_input.target, _input.feedback, _output}; }

  /** @brief 因输入不是有限值（NaN / Inf）而丢弃的 calc() / feed_forward() 次数 */
  uint32_t nonfinite_count() const { return _nonfinite_count; }

  /* ==================== */
  /* ==================== 数据接口 ==================== */

  PidInput _input;         ///< 输入缓存
  float    _output = 0.0f; ///< 输出

protected:
  /**
   * @brief 根据传入计算出其他输入项
   */
  void calc_input(float target, float feedback);

private:
  /* ==================== */
  /* ==================== 私有成员函数 ==================== */

  /**
   * @brief 计算微分项（依据当前微分模式）
   */
  void calc_d_term();

  /**
   * @brief 计算 P/I 项、各项限幅与总输出
   * @return float 输出
   */
  float apply_limits_and_output();

  /* ==================== */
  /* ==================== 私有成员变量 ==================== */

  Config _config;                                 ///< PID 配置
  PidTerm   _term;                              ///< 各项计算值
  DiffMode  _diff_mode = DiffMode::DIFF_FEEDBACK; ///< 微分计算模式
  bool      _has_last  = false;                   ///< 是否有上一帧输入（首次调用 / reset() 后为 false，该帧微分为 0）
  uint32_t  _nonfinite_count = 0U;                ///< 丢弃的非有限输入次数（F1）
};

  /* ==================== */

#endif // __PID_HPP__
