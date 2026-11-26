/**
 * @file iic_oled.cpp
 * @brief I²C OLED（SSD1306，0.96 寸 128×64）驱动实现
 * @date 2026-11-26
 *
 * @details 逻辑与原参考实现（OLED.c）一致，只把 BspI2c 换成宿主 CubeMX 的 HAL 句柄：
 *          命令 = {0x00, cmd}，数据 = {0x40, 128 列像素}，逐页发送。
 */

#include "bsp/bsp_series.hpp" // IWYU pragma: keep（HAL 模块开关 / 系列桥接）

// 宿主工程没在 CubeMX 里开 I2C（HAL_I2C_MODULE_ENABLED 未定义）时，本文件不参与编译
#ifdef HAL_I2C_MODULE_ENABLED

#include "device/screen/iic_oled.hpp"

#include "fort.h" // font_8x16[95][16]，ASCII 32~126

namespace
{
/** @brief 单次 HAL 传输超时 (ms)：一页 129 B @400 kHz ≈ 3 ms，给足余量 */
constexpr uint32_t OLED_I2C_TIMEOUT_MS = 50U;

/** @brief 字节内 bit 顺序反转（字形上下翻转用） */
inline uint8_t reverse_bits8(uint8_t b)
{
  b = static_cast<uint8_t>(((b & 0xF0U) >> 4) | ((b & 0x0FU) << 4));
  b = static_cast<uint8_t>(((b & 0xCCU) >> 2) | ((b & 0x33U) << 2));
  b = static_cast<uint8_t>(((b & 0xAAU) >> 1) | ((b & 0x55U) << 1));
  return b;
}
} // namespace


/* ==================== 初始化 ==================== */

bool IicOled::init(I2C_HandleTypeDef *i2c, uint8_t dev_addr, Rotation rot)
{
  if (i2c == nullptr)
  {
    return false;
  }

  _i2c      = i2c;
  _dev_addr = dev_addr;

  // 段重映射（A0/A1）与 COM 扫描方向（C0/C8）
  uint8_t seg_remap = 0xA0;
  uint8_t com_scan  = 0xC0;
  switch (rot)
  {
  case Rotation::ROT_180:
    seg_remap = 0xA1;
    com_scan  = 0xC8;
    break;
  case Rotation::MIRROR_X:
    seg_remap = 0xA1;
    com_scan  = 0xC0;
    break;
  case Rotation::MIRROR_Y:
    seg_remap = 0xA0;
    com_scan  = 0xC8;
    break;
  case Rotation::ROT_0:
  default:
    break;
  }

  /* SSD1306 标准初始化序列：先发一条命令探测应答，没接屏/接线错时尽快返回 false */
  if (!write_cmd(0xAE)) // 关闭显示
  {
    _i2c = nullptr;
    return false;
  }
  write_cmd(0x00);      // 列低地址
  write_cmd(0x10);      // 列高地址
  write_cmd(0x40);      // 起始行
  write_cmd(0x81);      // 对比度
  write_cmd(0xCF);      // 对比度值
  write_cmd(seg_remap); // 段重映射（方向）
  write_cmd(com_scan);  // COM 扫描方向（方向）
  write_cmd(0xA6);      // 正常显示（非反色）
  write_cmd(0xA8);      // 多路复用比
  write_cmd(0x3F);      // 1/64 占空比
  write_cmd(0xD3);      // 显示偏移
  write_cmd(0x00);      // 偏移 0
  write_cmd(0xD5);      // 显示时钟分频/振荡频率
  write_cmd(0x80);      // 默认值
  write_cmd(0xD9);      // 预充电周期
  write_cmd(0xF1);
  write_cmd(0xDA);      // COM 引脚配置
  write_cmd(0x12);      // 交替模式
  write_cmd(0xDB);      // VCOMH 电平
  write_cmd(0x30);
  write_cmd(0x20);      // 寻址模式
  write_cmd(0x02);      // 页寻址模式
  write_cmd(0x8D);      // 电荷泵
  write_cmd(0x14);      // 使能
  clear();
  (void)refresh();      // 先把清屏结果刷上去，避免上电花屏

  if (!write_cmd(0xAF)) // 开启显示（最后一条也校验，失败就当没初始化成功）
  {
    _i2c = nullptr;
    return false;
  }

  return true;
}


/* ==================== 命令/数据 ==================== */

bool IicOled::bus_recover()
{
  // 只复位 I²C 外设：能解决总线错误/状态机卡死，但不能解救从机一直拉低 SDA 的情况
  if (_i2c == nullptr)
  {
    return false;
  }
  (void)HAL_I2C_DeInit(_i2c);
  return HAL_I2C_Init(_i2c) == HAL_OK;
}

bool IicOled::send(const uint8_t *buf, uint16_t len)
{
  if (_i2c == nullptr || buf == nullptr || len == 0U)
  {
    return false;
  }

  const uint16_t addr = static_cast<uint16_t>(_dev_addr << 1);

  if (HAL_I2C_Master_Transmit(_i2c, addr, const_cast<uint8_t *>(buf), len, OLED_I2C_TIMEOUT_MS) == HAL_OK)
  {
    return true;
  }

  // 失败：重初始化外设后重试一次
  if (!bus_recover())
  {
    return false;
  }

  return HAL_I2C_Master_Transmit(_i2c, addr, const_cast<uint8_t *>(buf), len, OLED_I2C_TIMEOUT_MS) == HAL_OK;
}

bool IicOled::write_cmd(uint8_t cmd)
{
  uint8_t buf[2] = {0x00U, cmd}; // Co=0, D/C#=0 → 命令模式
  return send(buf, 2U);
}


/* ==================== 显示控制 ==================== */

void IicOled::display_on()
{
  write_cmd(0x8D); // 电荷泵使能
  write_cmd(0x14); // 开启电荷泵
  write_cmd(0xAF); // 点亮屏幕
}

void IicOled::display_off()
{
  write_cmd(0x8D); // 电荷泵使能
  write_cmd(0x10); // 关闭电荷泵
  write_cmd(0xAE); // 关闭屏幕
}

void IicOled::color_turn(bool enable)
{
  write_cmd(enable ? 0xA7 : 0xA6);
}

void IicOled::display_turn(bool enable)
{
  // enable = 旋转 180°（段重映射 A1 + COM 扫描 C8），与 init() 的 Rotation::ROT_180 一致
  if (enable)
  {
    write_cmd(0xA1);
    write_cmd(0xC8);
  }
  else
  {
    write_cmd(0xA0);
    write_cmd(0xC0);
  }
}


/* ==================== 显存操作 ==================== */

void IicOled::clear()
{
  for (uint8_t page = 0; page < 8; page++)
  {
    for (uint8_t col = 0; col < 144; col++)
    {
      _gram[col][page] = 0x00U;
    }
  }
}

bool IicOled::refresh()
{
  uint8_t buf[129];
  buf[0] = 0x40U; // Co=0, D/C#=1 → 数据模式

  bool ok = true;

  for (uint8_t page = 0; page < 8; page++)
  {
    /* 设置页地址和列起始地址 */
    write_cmd(static_cast<uint8_t>(0xB0U + page));
    write_cmd(0x00U);
    write_cmd(0x10U);

    for (uint8_t col = 0; col < 128; col++)
    {
      buf[col + 1U] = _gram[col][page];
    }

    /* 一次写入 129 字节（1 个控制字节 + 128 列像素） */
    if (!send(buf, 129U))
    {
      ok = false;
    }
  }

  return ok;
}

void IicOled::show_char(uint8_t x, uint8_t y, uint8_t chr)
{
  /* 8x16 字模：每个字符占 8 列 × 2 page。
   * 字模位序与面板扫描约定不一致时，用 _font_flip 在**字形层面**修正
   * （只影响单字，不影响字符位置与排版）。 */
  if (x > 15U || y > 3U) // 越界直接丢弃：_gram 是 [144][8]，写出去会踩内存
  {
    return;
  }

  if (chr < 32U || chr > 126U)
  {
    chr = static_cast<uint8_t>(' ');
  }

  const uint8_t *glyph  = font_8x16[chr - 32U];
  const bool     flip_h = (_font_flip == FontFlip::H || _font_flip == FontFlip::HV);
  const bool     flip_v = (_font_flip == FontFlip::V || _font_flip == FontFlip::HV);

  for (uint8_t i = 0; i < 8U; i++)
  {
    const uint8_t col = flip_h ? static_cast<uint8_t>(7U - i) : i;

    uint8_t lo = glyph[i];      // 上半页：第 0~7 行
    uint8_t hi = glyph[i + 8U]; // 下半页：第 8~15 行

    if (flip_v)
    {
      // 上下翻转 = 两个半页交换，且每个字节内的 bit 顺序也要反
      lo = reverse_bits8(glyph[i + 8U]);
      hi = reverse_bits8(glyph[i]);
    }

    _gram[(x * 8U) + col][y * 2U]        = lo;
    _gram[(x * 8U) + col][(y * 2U) + 1U] = hi;
  }
}

void IicOled::show_string(uint8_t x, uint8_t y, const char *str)
{
  if (str == nullptr || x > 15U || y > 3U)
  {
    return;
  }

  uint8_t j = 0U;
  while (str[j] != '\0' && x <= 15U)
  {
    show_char(x, y, static_cast<uint8_t>(str[j]));
    x++;
    j++;
  }
}

#endif // HAL_I2C_MODULE_ENABLED
