/**
 * @file filter_kalman.hpp
 * @author yssickjgd (1345578933@qq.com)（原作 alg_filter_kalman.h / alg_filter_kalman.cpp）；ChoseB（移植到 ICBSP 并修改）
 * @brief 线性 Kalman 滤波器：预测 / 更新分开调用（可不同频），协方差用 Joseph 形式更新
 * @version 0.2
 * @date 2026-11-26
 *
 * @copyright 原作 Copyright (c) 2025 USTC-RoboWalker，经作者授权移植；移植与修改部分 Copyright (c) 2026。
 *            本模块不适用本库的 MIT 许可证。麻老师原话:"可以的，只要是RM里的就随便用"。若使用用途超出RoboMaster赛事范畴内，建议自行再去征求原作者意见
 *
 * @details 来源：github.com/yssickjgd/damiao_mc02_bsp，dm02_test/User_File/1_Middleware/Algorithm 下的 alg_filter_kalman（0.1，2025-09-07）。
 *
 *          系统模型（w ~ N(0, Q)，v ~ N(0, R)）：
 *
 *            x_k = A·x_{k-1} + B·u_k + w_k
 *            z_k = H·x_k + v_k
 *
 *          predict(u)：x ← A·x + B·u，P ← A·P·Aᵀ + Q
 *          update(z) ：S = H·P·Hᵀ + R，K = P·Hᵀ·S⁻¹，x ← x + K·(z − H·x)，
 *                      P ← (I − K·H)·P·(I − K·H)ᵀ + K·R·Kᵀ（Joseph 形式，单精度下比 (I − K·H)·P 更能保持对称正定）
 *
 *          移植时的改动：
 *          - 命名改为本库规范（类 FilterKalman、方法小写蛇形、私有成员 _ 前缀、模板参数全大写），矩阵改用 MatrixF32；
 *          - 模型与噪声参数收进聚合体 Config；原作的公有成员 Vector_U / Vector_Z 改为 predict(u) / update(z) 的参数，
 *            状态、协方差、增益改为只读，经 get_*() 取；增加 init()，未初始化时 predict() / update() 返回 NOT_INIT；
 *          - predict() 直接把先验写进状态与协方差（原作只写 Vector_X_Prior，某个周期没有 update() 时，下一次预测仍从旧的后验出发，
 *            等于丢掉一步预测；原作的 EKF 已是本写法）。因此两次 predict() 之间连续 update() 是依次融合多次测量（原作后一次会覆盖前一次）；
 *          - 新息协方差 S 奇异时放弃本次测量、返回 BAD_ARG（原作求逆失败静默得到单位矩阵）；
 *            输入、测量或结果含 NaN / inf 时丢弃本步、状态不变（一个 NaN 会永久污染 x 与 P）；
 *          - 每步把 P 对称化，抵消单精度舍入；去掉常量单位矩阵成员（每个实例省 X_DIM² 个 float）。
 *
 * @note 纯计算，不依赖硬件与 RTOS；矩阵在栈上按值运算，适合 X_DIM ≤ 6 左右的小模型。
 *       模型时变（例如 A、B 依赖采样周期）时，在 predict() 前用 set_a() / set_b() 更新。
 *
 * @note 用法：
 *
 *   // 匀加速模型：状态 x = {位置; 速度}，输入 u = 加速度，测量 z = 位置，周期 dt
 *   constexpr float dt        = 0.001f;
 *   const float     a_data[4] = {1.0f, dt, 0.0f, 1.0f};
 *   const float     b_data[2] = {0.5f * dt * dt, dt};
 *   const float     h_data[2] = {1.0f, 0.0f};
 *   FilterKalman<2, 1, 1> kf({.a = MatrixF32<2, 2>::from_array(a_data),
 *                             .b = MatrixF32<2, 1>::from_array(b_data),
 *                             .h = MatrixF32<1, 2>::from_array(h_data),
 *                             .q = MatrixF32<2, 2>::identity() * 1e-4f,
 *                             .r = MatrixF32<1, 1>::constant(1e-2f)});
 *   ICBSP_CHECK(kf.init() == Status::OK);   // 初值 x = 0；也可传入 kf.init(x0)
 *
 *   // 每个周期先预测；有新测量时再更新（测量可以比预测慢）
 *   kf.predict(MatrixF32<1, 1>::constant(acc));
 *   if (has_new_pos)
 *   {
 *     kf.update(MatrixF32<1, 1>::constant(pos));
 *   }
 *   float vel = kf.get_state()[1][0];
 */

#ifndef __FILTER_KALMAN_HPP__
#define __FILTER_KALMAN_HPP__

#include <math.h>
#include <stdint.h>

#include "algorithm/math/matrix.hpp"
#include "service/system/status_code.hpp" // 统一状态码（不依赖 RTOS）

/**
 * @brief 线性 Kalman 滤波器
 * @tparam X_DIM 状态维度
 * @tparam U_DIM 输入维度（没有输入时取 1，B 置零）
 * @tparam Z_DIM 测量维度
 */
template <uint32_t X_DIM, uint32_t U_DIM, uint32_t Z_DIM>
class FilterKalman
{
public:
  /* ==================== 构造与初始化 ==================== */

  /** @brief 配置（聚合体，用指定初始化器按字段顺序传入，省略的字段取默认值） */
  struct Config
  {
    MatrixF32<X_DIM, X_DIM> a  = {};                                  ///< 状态转移矩阵 A
    MatrixF32<X_DIM, U_DIM> b  = {};                                  ///< 输入矩阵 B
    MatrixF32<Z_DIM, X_DIM> h  = {};                                  ///< 测量矩阵 H
    MatrixF32<X_DIM, X_DIM> q  = {};                                  ///< 过程噪声协方差 Q（对称半正定）；越大越信任测量
    MatrixF32<Z_DIM, Z_DIM> r  = {};                                  ///< 测量噪声协方差 R（对称正定）；越大越信任模型
    MatrixF32<X_DIM, X_DIM> p0 = MatrixF32<X_DIM, X_DIM>::identity(); ///< 初始误差协方差 P0；对初值越没把握取得越大
  };

  /**
   * @brief 构造函数（只做赋值）
   * @param cfg 配置
   */
  FilterKalman(const Config &cfg) : _config(cfg)
  {
  }

  /**
   * @brief 检查配置，置 x = x0、P = P0，清空计数
   * @param x0 初始状态，默认全 0
   * @return OK=成功；BAD_ARG=配置或 x0 含 NaN / inf
   * @note 可重复调用，重新开始滤波。
   */
  Status init(const MatrixF32<X_DIM, 1> &x0 = MatrixF32<X_DIM, 1>());

  /** @brief init() 是否成功过 */
  bool is_initialized() const
  {
    return _init;
  }

  /* ==================== */
  /* ==================== 计算接口 ==================== */

  /**
   * @brief 预测一步：x ← A·x + B·u，P ← A·P·Aᵀ + Q
   * @param u 本周期输入，默认全 0
   * @return OK=已预测；NOT_INIT=init() 没成功过；BAD_ARG=u 或结果含 NaN / inf，本步被丢弃、状态不变
   * @note 每个采样周期调用一次；结果直接写入状态，之后没有 update() 时 get_state() 就是预测值。
   */
  Status predict(const MatrixF32<U_DIM, 1> &u = MatrixF32<U_DIM, 1>());

  /**
   * @brief 用一次测量修正状态
   * @param z 测量值
   * @return OK=已更新；NOT_INIT=init() 没成功过；BAD_ARG=z 或结果含 NaN / inf、或新息协方差 S 奇异，本次测量被丢弃、状态不变
   * @note 有新测量时在 predict() 之后调用；两次 predict() 之间可以调用多次，依次融合。
   */
  Status update(const MatrixF32<Z_DIM, 1> &z);

  /* ==================== */
  /* ==================== 参数接口 ==================== */

  /** @brief 更新状态转移矩阵 A（下一次 predict() 起生效） */
  void set_a(const MatrixF32<X_DIM, X_DIM> &a)
  {
    _config.a = a;
  }

  /** @brief 更新输入矩阵 B（下一次 predict() 起生效） */
  void set_b(const MatrixF32<X_DIM, U_DIM> &b)
  {
    _config.b = b;
  }

  /** @brief 更新测量矩阵 H（下一次 update() 起生效） */
  void set_h(const MatrixF32<Z_DIM, X_DIM> &h)
  {
    _config.h = h;
  }

  /** @brief 更新过程噪声协方差 Q（下一次 predict() 起生效） */
  void set_q(const MatrixF32<X_DIM, X_DIM> &q)
  {
    _config.q = q;
  }

  /** @brief 更新测量噪声协方差 R（下一次 update() 起生效） */
  void set_r(const MatrixF32<Z_DIM, Z_DIM> &r)
  {
    _config.r = r;
  }

  /* ==================== */
  /* ==================== 数据接口 ==================== */

  /** @brief 状态估计 x（X_DIM×1） */
  const MatrixF32<X_DIM, 1> &get_state() const
  {
    return _x;
  }

  /** @brief 误差协方差 P（X_DIM×X_DIM）；对角元是各状态的方差 */
  const MatrixF32<X_DIM, X_DIM> &get_covariance() const
  {
    return _p;
  }

  /** @brief 最近一次成功 update() 的 Kalman 增益 K（X_DIM×Z_DIM） */
  const MatrixF32<X_DIM, Z_DIM> &get_gain() const
  {
    return _k;
  }

  /** @brief 成功 update() 的次数 */
  uint32_t get_update_count() const
  {
    return _update_cnt;
  }

  /** @brief predict() / update() 因非有限值或 S 奇异被丢弃的次数 */
  uint32_t get_reject_count() const
  {
    return _reject_cnt;
  }

private:
  /** @brief 矩阵所有元素都是有限值 */
  template <uint32_t R, uint32_t C>
  static bool all_finite(const MatrixF32<R, C> &m);

  Config                  _config;          ///< 配置（模型与噪声参数，set_*() 改的就是它）
  bool                    _init = false;    ///< init() 是否成功过
  MatrixF32<X_DIM, 1>     _x;               ///< 状态估计
  MatrixF32<X_DIM, X_DIM> _p;               ///< 误差协方差
  MatrixF32<X_DIM, Z_DIM> _k;               ///< 最近一次 Kalman 增益
  uint32_t                _update_cnt = 0U; ///< 成功更新次数
  uint32_t                _reject_cnt = 0U; ///< 丢弃次数
};

/* ==================== */
/* ==================== 模板实现（只有头文件：任意尺寸都能直接用） ==================== */

template <uint32_t X_DIM, uint32_t U_DIM, uint32_t Z_DIM>
Status FilterKalman<X_DIM, U_DIM, Z_DIM>::init(const MatrixF32<X_DIM, 1> &x0)
{
  _init = false;

  if (!all_finite(_config.a) || !all_finite(_config.b) || !all_finite(_config.h) || !all_finite(_config.q) || !all_finite(_config.r) || !all_finite(_config.p0) || !all_finite(x0))
  {
    return Status::BAD_ARG;
  }

  _x          = x0;
  _p          = _config.p0;
  _k          = MatrixF32<X_DIM, Z_DIM>();
  _update_cnt = 0U;
  _reject_cnt = 0U;
  _init       = true;
  return Status::OK;
}

template <uint32_t X_DIM, uint32_t U_DIM, uint32_t Z_DIM>
Status FilterKalman<X_DIM, U_DIM, Z_DIM>::predict(const MatrixF32<U_DIM, 1> &u)
{
  if (!_init)
  {
    return Status::NOT_INIT;
  }
  if (!all_finite(u))
  {
    ++_reject_cnt;
    return Status::BAD_ARG;
  }

  // 先算到局部变量，全部有限才写回
  const MatrixF32<X_DIM, 1> x = _config.a * _x + _config.b * u;
  MatrixF32<X_DIM, X_DIM>   p = _config.a * _p * _config.a.transpose() + _config.q;
  p                           = (p + p.transpose()) * 0.5f;

  if (!all_finite(x) || !all_finite(p))
  {
    ++_reject_cnt;
    return Status::BAD_ARG;
  }

  _x = x;
  _p = p;
  return Status::OK;
}

template <uint32_t X_DIM, uint32_t U_DIM, uint32_t Z_DIM>
Status FilterKalman<X_DIM, U_DIM, Z_DIM>::update(const MatrixF32<Z_DIM, 1> &z)
{
  if (!_init)
  {
    return Status::NOT_INIT;
  }
  if (!all_finite(z))
  {
    ++_reject_cnt;
    return Status::BAD_ARG;
  }

  // 新息协方差 S = H·P·Hᵀ + R，奇异时无法计算增益，放弃本次测量
  const MatrixF32<X_DIM, Z_DIM> pht = _p * _config.h.transpose();
  const MatrixF32<Z_DIM, Z_DIM> s   = _config.h * pht + _config.r;
  MatrixF32<Z_DIM, Z_DIM>       s_inv;
  if (!s.inverse(s_inv))
  {
    ++_reject_cnt;
    return Status::BAD_ARG;
  }

  const MatrixF32<X_DIM, Z_DIM> k = pht * s_inv;
  const MatrixF32<X_DIM, 1>     x = _x + k * (z - _config.h * _x);

  // Joseph 形式，再对称化抵消舍入
  const MatrixF32<X_DIM, X_DIM> i_kh = MatrixF32<X_DIM, X_DIM>::identity() - k * _config.h;
  MatrixF32<X_DIM, X_DIM>       p    = i_kh * _p * i_kh.transpose() + k * _config.r * k.transpose();
  p                                  = (p + p.transpose()) * 0.5f;

  if (!all_finite(x) || !all_finite(p))
  {
    ++_reject_cnt;
    return Status::BAD_ARG;
  }

  _x = x;
  _p = p;
  _k = k;
  ++_update_cnt;
  return Status::OK;
}

template <uint32_t X_DIM, uint32_t U_DIM, uint32_t Z_DIM>
template <uint32_t R, uint32_t C>
bool FilterKalman<X_DIM, U_DIM, Z_DIM>::all_finite(const MatrixF32<R, C> &m)
{
  for (uint32_t i = 0U; i < R * C; ++i)
  {
    if (!isfinite(m.data()[i]))
    {
      return false;
    }
  }
  return true;
}

/* ==================== */

#endif // __FILTER_KALMAN_HPP__
