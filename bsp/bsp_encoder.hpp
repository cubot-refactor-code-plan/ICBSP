/**
 * @file bsp_encoder.hpp
 * @author Rh
 * @brief 增量式编码器驱动 —— 定时器 Encoder 接口封装（只读计数，不碰 PSC/ARR/滤波）
 * @version 0.1
 * @date 2026-10-29
 *
 * @copyright Copyright (c) 2026
 *
 * @details 一个实例 = 一个定时器的编码器接口（TI1/TI2 上升沿 4 倍频，由 CubeMX 配好）。
 *          HAL 的 Encoder 模式把 CNT 当成 16 位可加可减的计数器，本类只做两件事：
 *          把 16 位计数换算成「增量」和「累计位置」，并处理 0/65535 的回绕。
 *
 * @note 用法：init() 启动计数 → 在固定周期任务里调 read_delta() 取本周期增量，
 *       或直接读 position() 取上电以来的累计位置（可正可负，int32）。
 *
 * @note 增量靠 `(int16_t)(now - last)` 算：只要两次调用之间走过不到 32768 个计数，
 *       回绕就是自动正确的。所以调用周期越短越安全。
 *
 * @warning 非线程安全：同一实例的 read_delta() / reset() 只允许一个任务调用。
 * @warning 编码器计数含抖动：A/B 相位噪声会表现成 ±1 的来回，需要就在上层做软件滤波。
 */

#ifndef __BSP_ENCODER_HPP__
#define __BSP_ENCODER_HPP__

#include "status.hpp" // 统一状态码
#include "stm32h7xx_hal.h" // IWYU pragma: keep（TIM_HandleTypeDef / __HAL_TIM_*）

#include <stdint.h>


/**
 * @brief 编码器驱动类
 *
 * @note 只在 init() 里调一次 HAL_TIM_Encoder_Start()，之后全是读 CNT 的纯软件操作。
 */
class BspEncoder
{
public:
  // ---------------- 配置 ----------------

  /**
   * @brief 编码器配置
   *
   * @note 计数器位数/方向/滤波全部来自 CubeMX 的 MX_TIMx_Init()，这里只有绑定与方向。
   */
  struct Config
  {
    TIM_HandleTypeDef *htim   = nullptr; ///< 定时器句柄（CubeMX 里已配成 Encoder TI12 模式）
    bool               invert = false;   ///< 计数方向取反（编码器装反或 A/B 接反时用）
  };

  // ----------------
  // ---------------- 公共接口 ----------------

  /** @brief 默认构造：未绑定定时器，init() 会返回 BAD_ARG */
  BspEncoder() = default;

  /**
   * @brief 构造：只保存配置（纯赋值，不碰硬件）
   *
   * @note 必须是 constexpr：board_cfg.hpp 里的全局实例是 inline 变量，需要常量初始化。
   */
  constexpr explicit BspEncoder(const Config &cfg) : _config(cfg) {}

  /**
   * @brief 启动编码器计数（幂等）
   *
   * @return OK=成功；BAD_ARG=句柄为空；IO_ERROR=HAL 启动失败
   *
   * @note 启动后以「当前计数值」为基准清零累计位置，避免把上电残留值当成增量。
   */
  Status init();

  /**
   * @brief 取上次调用以来的增量（带 16 位回绕处理），并累加进 position()
   *
   * @return 本次增量，向前为正（invert 时定义相反）
   *
   * @warning 两次调用之间走过超过 32767 个计数就会判错方向：任务周期别太长。
   */
  int32_t read_delta();

  /** @brief 上电（或上次 reset()）以来的累计位置，int32 多圈计数 */
  int32_t position() const { return _position; }

  /** @brief 计数器原始值，已按 invert 取反（int16，范围 -32768~32767） */
  int16_t read_raw() const;

  /**
   * @brief 计数器清零，累计位置与增量基准一起清零
   *
   * @note 不停止计数；清零瞬间正好有脉冲进来会丢这 1 个计数，属正常现象。
   */
  void reset();

  /** @brief 是否已成功 init() */
  bool initialized() const { return _initialized; }

  // ----------------
private:
  // ---------------- 成员变量 ----------------

  Config  _config;               ///< 绑定与方向
  int16_t _last = 0;             ///< 上次读到的计数值（增量基准）
  int32_t _position = 0;         ///< 累计位置
  bool    _initialized = false;  ///< 是否已成功 init()

  // ----------------
};

#endif // __BSP_ENCODER_HPP__
