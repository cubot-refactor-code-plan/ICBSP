/**
 * @file iic_oled.hpp
 * @brief I²C OLED 显示屏驱动 —— **SSD1306，只支持 128×64（0.96 寸单色模块，地址 0x3C/0x3D）**
 * @date 2026-11-26
 *
 * @details
 *   - 控制器：SSD1306，I²C 接口（页寻址）
 *   - **分辨率：仅支持 128×64**（128×32 / 64×48 等变体不支持）：初始化序列里写死了
 *     `0xA8/0x3F`（1/64 duty）、`0xD3/0x00`（偏移 0）、8 页刷新、16 列 × 4 行字符网格
 *   - 时序：标准 SSD1306 初始化序列（0xAE…0xAF、0x8D/0x14 电荷泵、0x20/0x02 页寻址）
 *   - 显存：`_gram[144][8]` 列寻址，每 page 8 行像素；实际刷新 0~127 列 × 0~7 页
 *   - 字模：8×16 ASCII（32~126），由 `fort.h` 的 `font_8x16[95][16]` 提供
 *
 * @note 本类只绑定宿主 CubeMX 生成的 `I2C_HandleTypeDef`（如 `&hi2c1`），不依赖 BspI2c；
 *       使用前宿主必须已调用 `MX_I2Cx_Init()`。
 * @note 所有传输都是**阻塞式** HAL 调用：一帧 8 页 × 129 B ≈ 1080 B，400 kHz 下约 24~26 ms。
 *       只适合低频刷新（建议 ≤ 5 Hz），不要放在实时任务里。
 * @note 本类不做任务/缓存管理：谁调用 refresh() 谁负责节拍。
 * @note 实例自带 144×8 = 1152 B 显存，**请定义在静态区/全局**（任务栈一般只有几 KB）；
 *       本工程放在 `board_cfg.cpp`（`IicOled oled;`）。
 * @note 发送失败时会重初始化一次 I²C 外设（`HAL_I2C_DeInit/Init`）再重试；这能解决
 *       外设状态卡死/总线错误，但**不能**解救从机一直拉低 SDA 的情况（那需要 GPIO 手动发
 *       9 个时钟，驱动不持有引脚信息）。
 *
 * 用法：
 * @code
 *   IicOled oled;                          // 本工程已定义在 board_cfg.cpp
 *   if (oled.init(&hi2c1, 0x3C)) {
 *     oled.clear();                       // 清显存（不上屏）
 *     oled.show_string(0, 0, "M1  12.3"); // 16 列 × 4 行
 *     oled.refresh();                     // 整屏上屏
 *   }
 * @endcode
 */

#ifndef __IIC_OLED_HPP__
#define __IIC_OLED_HPP__

#include "bsp/bsp_series.hpp" // IWYU pragma: keep（HAL 模块开关 / 系列桥接）

// 宿主工程没在 CubeMX 里开 I2C（HAL_I2C_MODULE_ENABLED 未定义）时，本模块整体不参与编译
#ifdef HAL_I2C_MODULE_ENABLED

#include <stdint.h>

class IicOled
{
public:
  /** @brief 屏幕方向（SSD1306 的段重映射位 + COM 扫描方向位） */
  enum class Rotation : uint8_t
  {
    ROT_0 = 0, ///< A0 + C0（默认）
    ROT_180,   ///< A1 + C8（左右 + 上下翻转）
    MIRROR_X,  ///< A1 + C0（仅左右）
    MIRROR_Y,  ///< A0 + C8（仅上下）
  };

  /** @brief 字模镜像（只影响单个字形，不影响字符位置/排版） */
  enum class FontFlip : uint8_t
  {
    NONE = 0,
    H,
    V,
    HV,
  };

  IicOled() = default;

  /** @brief 是否已成功绑定并初始化（init 失败时内部已解绑，可据此重试） */
  bool is_ready() const { return _i2c != nullptr; }

  /**
   * @brief 初始化 OLED 并绑定 I²C 句柄
   * @param i2c      已由 CubeMX 初始化的 I²C 句柄（如 &hi2c1）
   * @param dev_addr 7 位地址（SSD1306 常见 0x3C，部分模块 0x3D）
   * @param rot      屏幕方向
   * @return true = 初始化序列发送成功（内部会清屏并刷新一次）
   */
  bool init(I2C_HandleTypeDef *i2c, uint8_t dev_addr = 0x3C, Rotation rot = Rotation::ROT_0);

  /* ==================== 显示控制 ==================== */

  void display_on();              ///< 电荷泵开 + 点亮
  void display_off();             ///< 电荷泵关 + 熄灭
  void color_turn(bool enable);   ///< true = 反色显示
  void display_turn(bool enable); ///< true = 屏幕旋转 180°（A1 + C8），false = 正常（A0 + C0）

  /* ==================== 显存操作 ==================== */

  /** @brief 只清显存，**不上屏**（需要再调 refresh()） */
  void clear();

  /** @brief 把显存整屏发送到 OLED（8 页 × 129 B）
   *  @return true = 8 页全部发送成功；false = 至少一页无应答/超时 */
  bool refresh();

  /**
   * @brief 显示一个 ASCII 字符
   * @param x   列位置（0~15，每字符 8 像素宽）
   * @param y   行位置（0~3，每行 16 像素高 = 2 page）
   * @param chr 可见 ASCII（32~126），越界画空格
   */
  void show_char(uint8_t x, uint8_t y, uint8_t chr);

  /**
   * @brief 显示字符串（自动换列，x > 15 停止）
   * @note 每行最多 16 个字符（16 × 8 = 128 像素）。
   */
  void show_string(uint8_t x, uint8_t y, const char *str);

  /** @brief 设置字模镜像（字形看起来上下/左右反了时用） */
  void set_font_flip(FontFlip f) { _font_flip = f; }

  /** @brief 直接写一个像素字节（col 0~127，page 0~7，bit0 = 该页顶部；越界直接丢弃） */
  void set_pixel(uint8_t col, uint8_t page, uint8_t data)
  {
    if (col < 128U && page < 8U)
    {
      _gram[col][page] = data;
    }
  }

  /** @brief 取显存指针（列寻址，144×8） */
  uint8_t (*get_gram())[8] { return _gram; }

private:
  /** @brief 发送一段数据，失败时重初始化 I²C 外设并重试一次
   *  @return true = 从机应答；false = 两次都失败 */
  bool send(const uint8_t *buf, uint16_t len);

  /** @brief 发送一个命令字节：{0x00, cmd}（Co=0, D/C#=0） */
  bool write_cmd(uint8_t cmd);

  /** @brief 发送失败后重初始化 I²C 外设（HAL_I2C_DeInit + HAL_I2C_Init）
   *  @return true = 恢复成功；false = 句柄无效 / 重新初始化失败 */
  bool bus_recover();

  I2C_HandleTypeDef *_i2c      = nullptr;
  uint8_t            _dev_addr = 0x3C;
  FontFlip           _font_flip = FontFlip::NONE;
  uint8_t            _gram[144][8]{};
};

#endif // HAL_I2C_MODULE_ENABLED

#endif // __IIC_OLED_HPP__
