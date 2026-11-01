#include "pid.hpp"
#include <math.h>

PID::PID(const Config& cfg) : _config(cfg)
{
  // 隐藏逻辑显式化：i_max 未设置（为0）时跟随 out_max
  if (_config.i_max == 0.0f)
  {
    _config.i_max = _config.out_max;
  }

  // 微分模式沿用配置值
  _diff_mode = _config.diff_mode;
}

void PID::calc_input(float target, float feedback)
{
  // 首次调用（或 reset() 后）没有上一帧：把上一帧当作本帧，差分为 0，避免从 0 跳到当前值的微分冲击
  if (!_has_last)
  {
    _input.feedback = feedback;
    _input.error    = target - feedback;
    _has_last       = true;
  }

  _input.last_feedback  = _input.feedback;
  _input.last_error   = _input.error;
  _input.target       = target;
  _input.feedback     = feedback;
  _input.error        = _input.target - _input.feedback;
  _input.delta_feedback = _input.feedback - _input.last_feedback;
  _input.delta_error  = _input.error - _input.last_error;
}

void PID::calc_d_term()
{
  switch (_diff_mode)
  {
    case DiffMode::DIFF_FEEDBACK:
      // 微分先行：error = target - feedback，目标不动时误差变化率 = -反馈变化率，所以取负号（阻尼方向）
      _term.d_term = -_config.kd * _input.delta_feedback;
      break;
    case DiffMode::DIFF_ERROR:
      _term.d_term = _config.kd * _input.delta_error; // 常规微分
      break;
    case DiffMode::DISABLE_DIFF:
    default:
      _term.d_term = 0.0f;
      break;
  }
}

float PID::apply_limits_and_output()
{
  // 对称限幅辅助 lambda：limit 为 0 表示不限制
  auto clamp_symmetric = [](float value, float limit)
  {
    if (limit == 0.0f)
    {
      return value;
    }
    if (value > limit)
    {
      return limit;
    }
    if (value < -limit)
    {
      return -limit;
    }
    return value;
  };

  _term.p_term = _config.kp * _input.error;

  // 积分分离：误差小于阈值（或阈值未设置）时累加，否则清零（注意：是清零而非冻结）
  _term.i_term = (fabsf(_input.error) < _config.i_sep || _config.i_sep == 0.0f)
                   ? _term.i_term + _config.ki * _input.error
                   : 0.0f;

  // 积分项累加后立即钳位，防止积分无限累积（即使后续总输出有限幅）
  _term.i_term = clamp_symmetric(_term.i_term, _config.i_max);

  calc_d_term();

  // 各项独立限幅
  _term.p_term = clamp_symmetric(_term.p_term, _config.p_max);
  _term.d_term = clamp_symmetric(_term.d_term, _config.d_max);
  _term.f_term = clamp_symmetric(_term.f_term, _config.f_max);

  // 总输出限幅
  _output = _term.p_term + _term.i_term + _term.d_term + _term.f_term;
  _output = clamp_symmetric(_output, _config.out_max);

  // 前馈项每周期清零，需要每周期重新喂
  _term.f_term = 0.0f;

  return _output;
}

float PID::feed_forward(float feedforward)
{
  _term.f_term += feedforward;
  return _term.f_term;
}

float PID::calc(float target, float feedback)
{
  calc_input(target, feedback);
  return apply_limits_and_output();
}

float PID::calc(float target, float feedback, float df_dt)
{
  calc_input(target, feedback);

  // 外部导入的反馈值一阶导数代替差分；DIFF_ERROR 按目标不动处理，误差变化率 = -反馈变化率
  if (_diff_mode == DiffMode::DIFF_FEEDBACK)
  {
    _input.delta_feedback = df_dt;
  }
  else if (_diff_mode == DiffMode::DIFF_ERROR)
  {
    _input.delta_error = -df_dt;
  }

  return apply_limits_and_output();
}

void PID::reset()
{
  _input    = PidInput();
  _term     = PidTerm();
  _output   = 0.0f;
  _has_last = false;
}


