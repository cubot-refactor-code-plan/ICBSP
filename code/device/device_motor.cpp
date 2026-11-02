#include "device_motor.hpp"

#include <string.h>

namespace
{
/** @brief 圆周率：本文件只用来做一次「直径 → 周长」换算，不必引 <cmath> */
constexpr float PI_F = 3.14159265358979f;
} // namespace


/* ==================== 构造 ==================== */

/** @brief 构造：保存配置并按配置构造速度环（PID 的参数是构造时定下来的） */
DeviceMotor::DeviceMotor(const Config &cfg) : _config(cfg), _pid(cfg.pid)
{
}


/* ==================== */
/* ==================== 公共接口 ==================== */

/**
 * @brief 初始化：校验绑定与机械参数 → 算 counts_per_cm → 两路 PWM 归 0
 */
Status DeviceMotor::init()
{
  if (!is_bound())
    return Status::BAD_ARG;

  // 机械参数必须全给：少一个就算不出 cm/s
  if (_config.encoder_lines <= 0.0f || _config.encoder_multiplier <= 0.0f || _config.gear_ratio <= 0.0f ||
      _config.wheel_diameter_cm <= 0.0f)
    return Status::BAD_ARG;

  if (_config.out_max <= 0.0f)
    return Status::BAD_ARG;

  // 编码器必须已在 board_init() 里启动过，否则 read_delta() 恒为 0、闭环会一路积分到限幅
  if (!_config.encoder->initialized())
    return Status::NOT_INIT;

  // 输出轴每转计数 = 线数 × 倍频 × 减速比；每转行程 = π × 直径
  const float counts_per_round = _config.encoder_lines * _config.encoder_multiplier * _config.gear_ratio;
  const float travel_cm        = PI_F * _config.wheel_diameter_cm;
  _counts_per_cm               = counts_per_round / travel_cm;

  // 上电即刹车：模式是 STOP，和 update() 里 STOP 分支保持一致（否则到第一拍之前是滑行）
  _brake();

  _target_cm_s = 0.0f;
  _speed_cm_s  = 0.0f;
  _output_pct  = 0.0f;
  _remaining_s = 0.0f;
  _mode        = Mode::STOP;
  _idle        = false;
  _pid.reset();

  _initialized = true;

  return Status::OK;
}

/** @brief 纯速度模式：只记目标，真正出力等 update() */
void DeviceMotor::set_speed(float cm_s)
{
  _mode        = Mode::SPEED;
  _target_cm_s = cm_s;
}

/** @brief 速度+时间模式：remaining 用秒记，按 update() 传入的 dt 递减（不依赖 tick 精度） */
void DeviceMotor::set_speed_time(float cm_s, uint32_t duration_ms)
{
  _mode        = Mode::SPEED_TIME;
  _target_cm_s = cm_s;
  _remaining_s = static_cast<float>(duration_ms) / 1000.0f;
}

/** @brief 停止（刹车）：模式置 STOP、清积分、两路拉满做短路制动 */
void DeviceMotor::stop()
{
  if (!is_bound())
    return;

  _mode        = Mode::STOP;
  _target_cm_s = 0.0f;
  _remaining_s = 0.0f;
  _idle        = false;

  _pid.reset();
  _brake();
}

/**
 * @brief 跑一拍速度环
 *
 * @note 顺序固定：读增量 → 算速度 → （计时模式）扣时间 → PID → 限幅输出。
 *       停止模式下不跑 PID，但照样读增量，保证 speed_cm_s() 一直有效（手转电机能看数）。
 */
bool DeviceMotor::update(float dt_s)
{
  if (!_initialized || dt_s <= 0.0f)
    return false;

  // 1. 本拍增量 → 实测速度 (cm/s)：计数 / (计数每 cm) / 秒
  const int32_t delta = _config.encoder->read_delta();
  _speed_cm_s         = static_cast<float>(delta) / _counts_per_cm / dt_s;

  // 2. 停止模式：保持刹车，不跑 PID
  if (_mode == Mode::STOP)
  {
    _brake();
    return false;
  }

  // 3. 速度+时间模式：先扣时间，到点立刻停并报告「刚刚完成」
  if (_mode == Mode::SPEED_TIME)
  {
    _remaining_s -= dt_s;
    if (_remaining_s <= 0.0f)
    {
      stop();
      return true;
    }
  }

  // 5. 零速死区：目标和实测都贴着 0 时直接断输出（否则编码器 ±1 抖动会被 P 放大成嗡嗡声）
  const float db = _config.deadband_cm_s;
  if ((db > 0.0f) && (_target_cm_s < db) && (_target_cm_s > -db) && (_speed_cm_s < db) && (_speed_cm_s > -db))
  {
    if (!_idle)
    {
      // 只在刚进入死区时清一次：每拍都 reset 会把微分历史也清掉，重新起步时容易出阶跃
      _pid.reset();
      _idle = true;
    }

    _apply_output(0.0f);
    return false;
  }
  _idle = false;

  // 6. 速度环：目标与反馈同量纲 (cm/s)，输出量纲 = 占空比 %
  const float u = _pid.calc(_target_cm_s, _speed_cm_s);

  // 7. 限幅 + 方向 + 双 PWM 互斥输出
  _apply_output(u);

  return false;
}

/** @brief JustFloat 组帧：按 fields 选择 1~3 个 PID 量，帧尾固定 00 00 80 7F */
size_t DeviceMotor::pid_frame(uint8_t *buf, uint8_t fields) const
{
  fields &= PID_ALL;
  if (buf == nullptr || fields == 0U)
    return 0U;

  const PID::Data d = _pid.get();
  float values[3];
  size_t n = 0U;
  if ((fields & PID_TARGET) != 0U)
    values[n++] = d.target;
  if ((fields & PID_FEEDBACK) != 0U)
    values[n++] = d.feedback;
  if ((fields & PID_OUTPUT) != 0U)
    values[n++] = d.output;

  memcpy(buf, values, n * sizeof(float));
  uint8_t *tail = buf + n * sizeof(float);
  tail[0]       = 0x00U;
  tail[1]       = 0x00U;
  tail[2]       = 0x80U;
  tail[3]       = 0x7FU;

  return n * sizeof(float) + 4U;
}


/* ==================== */
/* ==================== 私有实现 ==================== */

/** @brief 带符号占空比 → H 桥两路：任何时刻最多一路非 0（H 桥直通保护） */
void DeviceMotor::_apply_output(float pct)
{
  if (_config.invert)
    pct = -pct;

  // 限幅（PID 内部若已按 out_max 限过，这里再兜一次，防止 invert/外部直调绕过）
  if (pct > _config.out_max)
    pct = _config.out_max;
  else if (pct < -_config.out_max)
    pct = -_config.out_max;

  // 关键：先把该关的那一路写 0，再让有效那一路出力，避免瞬间两路同时为高（H 桥直通）
  if (pct > 0.0f)
  {
    _config.pwm_b->set_duty(0.0f);
    _config.pwm_a->set_duty(pct);
  }
  else if (pct < 0.0f)
  {
    _config.pwm_a->set_duty(0.0f);
    _config.pwm_b->set_duty(-pct);
  }
  else
  {
    _config.pwm_a->set_duty(0.0f);
    _config.pwm_b->set_duty(0.0f);
  }

  _output_pct = pct;
}

/** @brief 短路制动：两路都给满占空比，逻辑输出记为 0 */
void DeviceMotor::_brake()
{
  _config.pwm_a->set_duty(100.0f);
  _config.pwm_b->set_duty(100.0f);
  _output_pct = 0.0f;
}

/* ==================== */

