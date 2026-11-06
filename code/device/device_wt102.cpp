/**
 * @file device_wt102.cpp
 * @author Rh
 * @brief WT102 单轴痣螺仪设备：帧解析与 Z 轴最近两帧数据的实现
 * @version 0.1
 * @date 2026-11-06
 *
 * @copyright Copyright (c) 2026
 *
 * @note 本类是模板（串口类型由宿主决定），实现放在本文件，靠**文件末尾的显式实例化**
 *       提供三种 `BspUartT` 组合的符号。板级若换成别的组合，在这里补一行实例化即可，
 *       否则会在链接期报 undefined reference（而不是编译期，注意看链接错误）。
 *
 * @note 加新函数时：声明写 `device_wt102.hpp`、定义写这里（不要写回头文件），
 *       否则会退化成头文件实现、每个 TU 各编一份。
 */

#include "device_wt102.hpp"

// 宿主工程没在 CubeMX 里开 UART 时，头文件里整个类都不存在，这里也不参与编译
#ifdef HAL_UART_MODULE_ENABLED

#include "FreeRTOS.h"  // IWYU pragma: keep (pdMS_TO_TICKS)
#include "bsp_uart.hpp" // IWYU pragma: keep（UART_WORDLENGTH_8B / BspUartT）
#include "task.h"      // xTaskGetTickCount


/* ==================== 构造与析构 ==================== */

/** @brief 绑定串口（解析状态全部走成员声明里的 NSDMI，构造函数不做事） */
template <typename Uart>
DeviceWt102T<Uart>::DeviceWt102T(Uart &uart) : _uart(uart)
{
}
/* ==================== */
/* ==================== 生命周期 ==================== */

/**
 * @brief 校验帧格式（230400 8N1）并复位解析状态
 *
 * @note 帧格式在 CubeMX 里配，这里只是兜底：配错了直接 BAD_ARG，比"收不到数据"好查。
 */
template <typename Uart>
Status DeviceWt102T<Uart>::init(void)
{
  if (!_uart.check_format(230400U, UART_WORDLENGTH_8B, UART_PARITY_NONE, UART_STOPBITS_1))
  {
    return Status::BAD_ARG;
  }

  _fill            = 0U;
  _diag            = Diag {};
  _last_frame_tick = 0U;

  for (uint32_t i = 0U; i < FRAME_SIZE; ++i)
  {
    _frame[i] = 0U;
  }

  for (uint32_t i = 0U; i < 2U; ++i)
  {
    _z[i] = ZData {}; // 6 个 int16 全清零
  }

  return Status::OK;
}

/**
 * @brief 把 BspUartT 接收流缓冲区里的字节全部消化掉
 *
 * @note 状态机三档：_fill == 0 找 0x55；1 ~ 10 拼帧；拼满 11 字节后验校验和，
 *       不对就整帧丢掉、重新找帧头。
 * @note **不按帧型过滤**：设备实际会发 0x51/0x52/0x53 三型帧，若只认其中两型，
 *       第三型剩下的 9 字节会被当垃圾逐字节丢弃（实测每秒多扫 4500 次），靠校验和判真假就够了。
 * @note 帧体内部出现 0x55 不会误判 —— 拼帧期间不看内容，只看长度和校验和。
 */
template <typename Uart>
void DeviceWt102T<Uart>::update(uint32_t timeout_ms)
{
  uint32_t wait_ms = timeout_ms;

  for (;;)
  {
    uint8_t byte = 0U;
    size_t  got  = 0U;

    // 首字节按调用方给的超时等；一旦开始解帧就一律 0，把已有数据排空
    if ((_uart.receive(&byte, 1U, wait_ms, &got) != Status::OK) || (got == 0U))
    {
      return; // 这一拍的字节取完了
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

    // 拼满 11 字节：校验和 = 前 10 字节之和取低 8 位
    uint8_t sum = 0U;
    for (uint32_t i = 0U; i < SUM_INDEX; ++i)
    {
      sum = static_cast<uint8_t>(sum + _frame[i]);
    }

    if (sum == _frame[SUM_INDEX])
    {
      ++_diag.frame_cnt;
      _parse_frame(_frame);
    }
    else
    {
      ++_diag.csum_err_cnt;
    }

    _fill = 0U;
  }
}
/* ==================== */
/* ==================== 查询接口 ==================== */

/**
 * @brief 在线判定：收到过帧，且最近一帧在 ONLINE_TIMEOUT_MS 之内
 *
 * @note 自己按 tick 现算（无符号差值，tick 回绕安全），不走 Online 节点、也不需要周期任务喂它；
 *       代价是结果只在调用这一刻成立。
 */
template <typename Uart>
bool DeviceWt102T<Uart>::is_online(void) const
{
  if (_diag.frame_cnt == 0U)
  {
    return false; // 从没收到过帧
  }

  return (static_cast<uint32_t>(xTaskGetTickCount()) - _last_frame_tick) < pdMS_TO_TICKS(ONLINE_TIMEOUT_MS);
}

/** @brief 取小端有符号 int16（逐字节拼，不依赖内存对齐） */
template <typename Uart>
int16_t DeviceWt102T<Uart>::_le16(const uint8_t *p)
{
  return static_cast<int16_t>(static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8U));
}

/** @brief 最新一帧的三个 Z 轴原始值 */
template <typename Uart>
const typename DeviceWt102T<Uart>::ZData &DeviceWt102T<Uart>::z_newest(void) const
{
  return _z[0];
}

/** @brief 上一帧的三个 Z 轴原始值 */
template <typename Uart>
const typename DeviceWt102T<Uart>::ZData &DeviceWt102T<Uart>::z_previous(void) const
{
  return _z[1];
}

/** @brief 诊断计数快照 */
template <typename Uart>
typename DeviceWt102T<Uart>::Diag DeviceWt102T<Uart>::get_diag(void) const
{
  return _diag;
}
/* ==================== */
/* ==================== 帧解析 ==================== */

/**
 * @brief 解析一帧已通过校验的数据
 *
 * @note 先把上一份快照往后挪一位，再把本帧的 Z 轴量写进最新一帧。
 *       三个量都在各自帧的 [6][7]（实测校正过）：0x51 = az、0x52 = wz、0x53 = yaw。
 * @note 用 `_le16()` 统一做小端拼接 + 有符号强转，否则负数的符号位会被当成大正数。
 */
template <typename Uart>
void DeviceWt102T<Uart>::_parse_frame(const uint8_t *frame)
{
  _z[1] = _z[0]; // 上一帧

  if (frame[1] == TYPE_ACC)
  {
    _z[0].acc_z = _le16(&frame[6]);
    ++_diag.acc_cnt;
  }
  else if (frame[1] == TYPE_GYRO)
  {
    _z[0].gyro_z = _le16(&frame[6]);
    ++_diag.gyro_cnt;
  }
  else if (frame[1] == TYPE_ANGLE)
  {
    _z[0].yaw = _le16(&frame[6]);
    ++_diag.angle_cnt;
  }
  else
  {
    ++_diag.other_cnt; // 没见过的帧型：不解析，只计数
  }

  _last_frame_tick = static_cast<uint32_t>(xTaskGetTickCount());
}
/* ==================== */


/* ==================== 显式实例化 ==================== */
/* 框架只允许这三种 BspUartT 组合（见 board_cfg.hpp 的硬规矩）；板级换组合就在下面补一行 */
template class DeviceWt102T<BspUartT<uart_mode::dma, 128, 128>>;
template class DeviceWt102T<BspUartT<uart_mode::it, 128, 128>>;
template class DeviceWt102T<BspUartT<uart_mode::it, 128, 32>>;
/* ==================== */


#endif // HAL_UART_MODULE_ENABLED
