#include "bsp/bsp_series.hpp" // IWYU pragma: keep（HAL 模块开关 / 系列桥接）

// 宿主工程没在 CubeMX 里开 TIM（HAL_TIM_MODULE_ENABLED 未定义）时，本文件不参与编译
#ifdef HAL_TIM_MODULE_ENABLED

#include "bsp/bsp_encoder.hpp"


/* ==================== 公共接口 ==================== */


/**
 * @brief 启动编码器：PSC/ARR/IC 滤波全按 MX_TIMx_Init 的配置，这里只负责 Start
 */
Status BspEncoder::init()
{
  // 已启动过就直接返回：HAL_TIM_Encoder_Start() 会把通道状态置 BUSY，重复调用只会得到 HAL_ERROR
  if (_initialized)
    return Status::OK;

  if (_config.htim == nullptr)
    return Status::BAD_ARG;
  // read_delta() 按计数器位宽回绕算增量：计数器必须在 0xFFFF（16 位定时器）或 0xFFFFFFFF（32 位定时器）处回绕，
  // CubeMX 里 Period 填别的值（例如按编码器线数填 4095）时跨零的那一拍增量是错的（N37）
  const uint32_t arr = __HAL_TIM_GET_AUTORELOAD(_config.htim);
  if (arr != 0xFFFFU && arr != 0xFFFFFFFFU)
    return Status::BAD_ARG;

  if (HAL_TIM_Encoder_Start(_config.htim, TIM_CHANNEL_ALL) != HAL_OK)
    return Status::IO_ERROR;

  // 以当前计数值为增量基准，并让累计位置从 0 开始（上电时 CNT 里可能是任意值）
  _last     = read_raw();
  _position = 0;

  _initialized = true;

  return Status::OK;
}

/**
 * @brief 读计数器原始值：按计数器位宽转成有符号（16 位改补码 / 32 位直接用），再按 invert 取反
 *
 * @note 16 位与 32 位必须分开算：F4 的 TIM2 / TIM5 是 32 位计数器，
 *       一律按 int16 截断会把多圈计数的值算错（H7 的 TIM2 / TIM5 同样是 32 位）。
 */
int32_t BspEncoder::read_raw() const
{
  if (_config.htim == nullptr)
    return 0;

  const uint32_t cnt = static_cast<uint32_t>(__HAL_TIM_GET_COUNTER(_config.htim));

  if (IS_TIM_32B_COUNTER_INSTANCE(_config.htim->Instance) != 0U)
  {
    // 32 位：原值即补码；取反用无符号减法，避开 INT32_MIN 取负的溢出
    return _config.invert ? static_cast<int32_t>(0U - cnt) : static_cast<int32_t>(cnt);
  }

  // 16 位：先截到 uint16 再转 int16 得补码值；用 0 - raw 而不是 -raw（0x8000 取负会溢出）
  const int16_t raw = static_cast<int16_t>(static_cast<uint16_t>(cnt));
  return _config.invert ? static_cast<int32_t>(static_cast<int16_t>(0 - raw)) : static_cast<int32_t>(raw);
}

/** @brief 本周期增量：按计数器位宽做无符号减法后解释成有符号，跨 0 / 满量程回绕自动正确 */
int32_t BspEncoder::read_delta()
{
  if (!_initialized)
    return 0;

  const int32_t now = read_raw();

  // 先按位宽做无符号相减，再按同宽有符号解释：跨过 0 或满量程时结果依然是正确的小增量
  int32_t delta = 0;
  if (IS_TIM_32B_COUNTER_INSTANCE(_config.htim->Instance) != 0U)
  {
    delta = static_cast<int32_t>(static_cast<uint32_t>(now) - static_cast<uint32_t>(_last));
  }
  else
  {
    delta = static_cast<int16_t>(static_cast<uint16_t>(now) - static_cast<uint16_t>(_last));
  }

  _last = now;
  _position += delta;

  return delta;
}

/** @brief 计数器清零：CNT、增量基准、累计位置三者一起归零 */
void BspEncoder::reset()
{
  if (_config.htim == nullptr)
    return;

  __HAL_TIM_SET_COUNTER(_config.htim, 0U);

  _last     = 0;
  _position = 0;
}

/* ==================== */

#endif // HAL_TIM_MODULE_ENABLED
