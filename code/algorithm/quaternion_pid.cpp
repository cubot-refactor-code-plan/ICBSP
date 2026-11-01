#include "quaternion_pid.hpp"


/* ==================== QuaternionPID ==================== */

QuaternionPID::QuaternionPID(const Config &cfg) : _config(cfg), _axis {PID(cfg.axis[0]), PID(cfg.axis[1]), PID(cfg.axis[2])}
{
  // 隐藏逻辑显式化：三轴一律用 DIFF_ERROR，本类自己按 _config.diff_mode 算好误差变化率再传进去
  for (uint32_t i = 0U; i < 3U; ++i)
  {
    _axis[i].diff_mode(DiffMode::DIFF_ERROR);
  }
}

Vector3 QuaternionPID::calc(const Quaternion &target, const Quaternion &feedback)
{
  // 每周期转角由相邻两次估算：反馈表在上一帧机体系、目标表在上一帧目标机体系（相邻两帧相差很小，按当前系用）
  Vector3 rot_target   = {0.0, 0.0, 0.0};
  Vector3 rot_feedback = {0.0, 0.0, 0.0};
  if (_has_last)
  {
    rot_target   = _last_target.error_to(target);
    rot_feedback = _last_feedback.error_to(feedback);
  }
  return step(target, feedback, rot_target, rot_feedback);
}

Vector3 QuaternionPID::calc(const Quaternion &target, const Quaternion &feedback, const Vector3 &omega_feedback)
{
  const double dt = static_cast<double>(_config.dt);
  return step(target, feedback, {0.0, 0.0, 0.0}, {omega_feedback.x * dt, omega_feedback.y * dt, omega_feedback.z * dt});
}

Vector3 QuaternionPID::calc(const Quaternion &target, const Quaternion &feedback, const Vector3 &omega_target, const Vector3 &omega_feedback)
{
  const double dt = static_cast<double>(_config.dt);
  return step(target, feedback, {omega_target.x * dt, omega_target.y * dt, omega_target.z * dt}, {omega_feedback.x * dt, omega_feedback.y * dt, omega_feedback.z * dt});
}

void QuaternionPID::feed_forward(const Vector3 &feedforward)
{
  _axis[0].feed_forward(static_cast<float>(feedforward.x));
  _axis[1].feed_forward(static_cast<float>(feedforward.y));
  _axis[2].feed_forward(static_cast<float>(feedforward.z));
}

void QuaternionPID::reset()
{
  for (uint32_t i = 0U; i < 3U; ++i)
  {
    _axis[i].reset();
  }
  _error    = {0.0, 0.0, 0.0};
  _output   = {0.0, 0.0, 0.0};
  _has_last = false;
}

Vector3 QuaternionPID::step(const Quaternion &target, const Quaternion &feedback, const Vector3 &rot_target, const Vector3 &rot_feedback)
{
  // 误差：从反馈姿态转到目标姿态的旋转向量（当前机体系），代替 target - feedback
  const Quaternion q_err = feedback.conj() * target;
  _error                 = q_err.rotation_vector();

  // 误差变化率（每周期）= 目标转角（转到当前机体系）- 反馈转角
  Vector3 de = {0.0, 0.0, 0.0};
  switch (_config.diff_mode)
  {
    case DiffMode::DIFF_FEEDBACK:
      de = {-rot_feedback.x, -rot_feedback.y, -rot_feedback.z}; // 只对反馈微分
      break;
    case DiffMode::DIFF_ERROR:
    {
      const Vector3 rt = q_err.rotate(rot_target); // 目标机体系 → 当前机体系
      de               = {rt.x - rot_feedback.x, rt.y - rot_feedback.y, rt.z - rot_feedback.z};
      break;
    }
    case DiffMode::DISABLE_DIFF:
    default:
      break;
  }

  _last_target   = target;
  _last_feedback = feedback;
  _has_last      = true;

  return run_axes(_error, de);
}

Vector3 QuaternionPID::run_axes(const Vector3 &error, const Vector3 &d_in)
{
  // 三轴 PID 都是 DIFF_ERROR：df_dt 为反馈变化率，等于 -误差变化率
  Vector3 out = {static_cast<double>(_axis[0].calc(static_cast<float>(error.x), 0.0f, static_cast<float>(-d_in.x))),
                 static_cast<double>(_axis[1].calc(static_cast<float>(error.y), 0.0f, static_cast<float>(-d_in.y))),
                 static_cast<double>(_axis[2].calc(static_cast<float>(error.z), 0.0f, static_cast<float>(-d_in.z)))};

  // 模长限幅：等比例缩小，保持输出方向（各轴分量限幅已在 PID 里做过）
  if (_config.out_norm_max > 0.0f)
  {
    const double n   = out.norm();
    const double max = static_cast<double>(_config.out_norm_max);
    if (n > max)
    {
      const double k = max / n;
      out            = {out.x * k, out.y * k, out.z * k};
    }
  }

  _output = out;
  return _output;
}
