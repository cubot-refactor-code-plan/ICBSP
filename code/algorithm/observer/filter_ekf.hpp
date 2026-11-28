/**
 * @file filter_ekf.hpp
 * @author yssickjgd (1345578933@qq.com)（原作 alg_filter_ekf.h / alg_filter_ekf.cpp）；ChoseB（移植到 ICBSP 并修改）
 * @brief 扩展 Kalman 滤波器（EKF）：非线性模型由调用方以函数指针提供，预测 / 更新分开调用（可不同频）
 * @version 0.2
 * @date 2026-11-26
 *
 * @copyright 原作 Copyright (c) 2025 USTC-RoboWalker，经作者授权移植；移植与修改部分 Copyright (c) 2026。
 *            本模块不适用本库的 MIT 许可证。麻老师原话:"可以的，只要是RM里的就随便用"。若使用用途超出RoboMaster赛事范畴内，建议自行再去征求原作者意见
 *
 * @details 来源：github.com/yssickjgd/damiao_mc02_bsp，dm02_test/User_File/1_Middleware/Algorithm 下的 alg_filter_ekf（0.1，2025-09-15）。
 *
 *          系统模型（w ~ N(0, Q)，v ~ N(0, R)）：
 *
 *            x_k = f(x_{k-1}, u_k, w_k)
 *            z_k = h(x_k, v_k)
 *
 *          过程噪声 w 与输入 u 同维（Q 为 U_DIM×U_DIM，可理解为噪声叠加在输入上），测量噪声 v 与测量同维（R 为 Z_DIM×Z_DIM）。
 *          记 F = ∂f/∂x、W = ∂f/∂w、H = ∂h/∂x、V = ∂h/∂v：
 *
 *          predict(u)：F、W 在当前 x 处求值；x ← f(x, u, dt)，P ← F·P·Fᵀ + W·Q·Wᵀ
 *          update(z) ：H、V 在当前 x 处求值；S = H·P·Hᵀ + V·R·Vᵀ，K = P·Hᵀ·S⁻¹，x ← x + K·(z − h(x, dt))，
 *                      P ← (I − K·H)·P·(I − K·H)ᵀ + K·V·R·Vᵀ·Kᵀ（Joseph 形式）
 *
 *          移植时的改动：
 *          - 命名改为本库规范（类 FilterEkf、方法小写蛇形、私有成员 _ 前缀、模板参数全大写、函数指针类型别名大驼峰），矩阵改用 MatrixF32；
 *          - 模型函数、噪声参数、周期收进聚合体 Config（原作 Config_Nonlinear_State_Model() / Config_Nonlinear_Measurement_Model() / Set_D_T()），
 *            模型函数的 dt 改为按值传 float；原作的公有成员 Vector_U / Vector_Z 改为 predict(u) / update(z) 的参数，
 *            状态、协方差、增益改为只读，经 get_*() 取；增加 init()（检查函数指针非空等），未初始化时 predict() / update() 返回 NOT_INIT；
 *          - update() 在当前状态（即 predict() 写入的先验）处线性化，两次 predict() 之间连续 update() 是依次融合多次测量（原作后一次会覆盖前一次）；
 *          - 新息协方差 S 奇异时放弃本次测量、返回 BAD_ARG（原作求逆失败静默得到单位矩阵）；
 *            输入、测量或结果（含模型函数的返回值）含 NaN / inf 时丢弃本步、状态不变；
 *          - 每步把 P 对称化，抵消单精度舍入；去掉常量单位 / 零矩阵成员；
 *          - 增加 update(z, hx, H, R, freeze)：由调用方给出线性化结果，用于不同维度的多组测量依次融合，可冻结部分状态；
 *            增加 calc_nis()：归一化新息平方，用于新息检验。
 *
 * @note 纯计算，不依赖硬件与 RTOS；矩阵在栈上按值运算，适合 X_DIM ≤ 10 左右的小模型。
 *       栈用量随维度平方增长：9 维状态、4 维测量的 update() 实测约 4.6 KB，predict() 约 4 KB（-O0），开优化后会小很多；调用任务要留足栈。
 *       模型函数在 predict() / update() 里同步调用，运行在调用者的上下文，不要在里面阻塞。
 *
 * @note 用法：
 *
 *   // 模型函数由调用方提供（普通函数或无捕获 lambda），雅可比矩阵需手推
 *   MatrixF32<2, 1> f(const MatrixF32<2, 1> &x, const MatrixF32<1, 1> &u, float dt);     // 状态方程
 *   MatrixF32<2, 2> df_dx(const MatrixF32<2, 1> &x, const MatrixF32<1, 1> &u, float dt); // ∂f/∂x
 *   MatrixF32<2, 1> df_dw(const MatrixF32<2, 1> &x, const MatrixF32<1, 1> &u, float dt); // ∂f/∂w
 *   MatrixF32<1, 1> h(const MatrixF32<2, 1> &x, float dt);                                // 测量方程
 *   MatrixF32<1, 2> dh_dx(const MatrixF32<2, 1> &x, float dt);                            // ∂h/∂x
 *   MatrixF32<1, 1> dh_dv(const MatrixF32<2, 1> &x, float dt);                            // ∂h/∂v
 *
 *   FilterEkf<2, 1, 1> ekf({.f     = f,
 *                           .df_dx = df_dx,
 *                           .df_dw = df_dw,
 *                           .h     = h,
 *                           .dh_dx = dh_dx,
 *                           .dh_dv = dh_dv,
 *                           .q     = MatrixF32<1, 1>::constant(1e-2f),
 *                           .r     = MatrixF32<1, 1>::constant(1e-3f),
 *                           .dt    = 0.001f});
 *   ICBSP_CHECK(ekf.init() == Status::OK);   // 初值 x = 0；也可传入 ekf.init(x0)
 *
 *   // 每个周期先预测；有新测量时再更新
 *   ekf.predict(MatrixF32<1, 1>::constant(u));
 *   if (has_new_z)
 *   {
 *     ekf.update(MatrixF32<1, 1>::constant(z));
 *   }
 *   float x0 = ekf.get_state()[0][0];
 */

#ifndef __FILTER_EKF_HPP__
#define __FILTER_EKF_HPP__

#include <math.h>
#include <stdint.h>

#include "algorithm/math/matrix.hpp"
#include "service/system/status_code.hpp" // 统一状态码（不依赖 RTOS）

/**
 * @brief 扩展 Kalman 滤波器
 * @tparam X_DIM 状态维度
 * @tparam U_DIM 输入维度，同时是过程噪声维度
 * @tparam Z_DIM 测量维度，同时是测量噪声维度
 */
template <uint32_t X_DIM, uint32_t U_DIM, uint32_t Z_DIM>
class FilterEkf
{
public:
  /* ==================== 模型函数类型 ==================== */

  /** @brief 状态方程 f(x, u, dt)，返回下一时刻状态 */
  using StateFunc = MatrixF32<X_DIM, 1> (*)(const MatrixF32<X_DIM, 1> &x, const MatrixF32<U_DIM, 1> &u, float dt);

  /** @brief 状态方程对状态的雅可比 ∂f/∂x */
  using StateJacobianX = MatrixF32<X_DIM, X_DIM> (*)(const MatrixF32<X_DIM, 1> &x, const MatrixF32<U_DIM, 1> &u, float dt);

  /** @brief 状态方程对过程噪声的雅可比 ∂f/∂w */
  using StateJacobianW = MatrixF32<X_DIM, U_DIM> (*)(const MatrixF32<X_DIM, 1> &x, const MatrixF32<U_DIM, 1> &u, float dt);

  /** @brief 测量方程 h(x, dt)，返回预测的测量值 */
  using MeasureFunc = MatrixF32<Z_DIM, 1> (*)(const MatrixF32<X_DIM, 1> &x, float dt);

  /** @brief 测量方程对状态的雅可比 ∂h/∂x */
  using MeasureJacobianX = MatrixF32<Z_DIM, X_DIM> (*)(const MatrixF32<X_DIM, 1> &x, float dt);

  /** @brief 测量方程对测量噪声的雅可比 ∂h/∂v */
  using MeasureJacobianV = MatrixF32<Z_DIM, Z_DIM> (*)(const MatrixF32<X_DIM, 1> &x, float dt);

  /* ==================== */
  /* ==================== 构造与初始化 ==================== */

  /** @brief 配置（聚合体，用指定初始化器按字段顺序传入，省略的字段取默认值） */
  struct Config
  {
    StateFunc               f     = nullptr;                             ///< 状态方程，必填
    StateJacobianX          df_dx = nullptr;                             ///< ∂f/∂x，必填
    StateJacobianW          df_dw = nullptr;                             ///< ∂f/∂w，必填
    MeasureFunc             h     = nullptr;                             ///< 测量方程；h、dh_dx、dh_dv 要么都给，要么都为空（只用 update(z, hx, H, R) 时）
    MeasureJacobianX        dh_dx = nullptr;                             ///< ∂h/∂x
    MeasureJacobianV        dh_dv = nullptr;                             ///< ∂h/∂v（噪声直接相加时返回单位矩阵）
    MatrixF32<U_DIM, U_DIM> q     = {};                                  ///< 过程噪声协方差 Q（对称半正定）；越大越信任测量
    MatrixF32<Z_DIM, Z_DIM> r     = {};                                  ///< 测量噪声协方差 R（对称正定）；越大越信任模型
    MatrixF32<X_DIM, X_DIM> p0    = MatrixF32<X_DIM, X_DIM>::identity(); ///< 初始误差协方差 P0；对初值越没把握取得越大
    float                   dt    = 0.001f;                              ///< 计算周期，单位 s，> 0；原样传给模型函数
  };

  /**
   * @brief 构造函数（只做赋值）
   * @param cfg 配置
   */
  FilterEkf(const Config &cfg) : _config(cfg)
  {
  }

  /**
   * @brief 检查配置，置 x = x0、P = P0，清空计数
   * @param x0 初始状态，默认全 0
   * @return OK=成功；BAD_ARG=f、df_dx、df_dw 有空的，h、dh_dx、dh_dv 只给了一部分，dt ≤ 0，配置或 x0 含 NaN / inf
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
   * @brief 预测一步：x ← f(x, u, dt)，P ← F·P·Fᵀ + W·Q·Wᵀ
   * @param u 本周期输入，默认全 0
   * @return OK=已预测；NOT_INIT=init() 没成功过；BAD_ARG=u 或结果含 NaN / inf，本步被丢弃、状态不变
   * @note 每个采样周期调用一次；结果直接写入状态，之后没有 update() 时 get_state() 就是预测值。
   */
  Status predict(const MatrixF32<U_DIM, 1> &u = MatrixF32<U_DIM, 1>());

  /**
   * @brief 用一次测量修正状态
   * @param z 测量值
   * @return OK=已更新；NOT_INIT=init() 没成功过；NOT_SUPPORTED=Config 没给测量模型；BAD_ARG=z 或结果含 NaN / inf、或新息协方差 S 奇异，本次测量被丢弃、状态不变
   * @note 有新测量时在 predict() 之后调用；两次 predict() 之间可以调用多次，依次融合。
   */
  Status update(const MatrixF32<Z_DIM, 1> &z);

  /**
   * @brief 用调用方算好的线性化测量修正状态（测量维度 M 可与 Z_DIM 不同）
   * @tparam M 测量维度
   * @param z      测量值
   * @param hx     预测测量 h(x)，在当前状态（predict() 写入的先验，或上一组测量修正后的状态）处求值
   * @param h_x    测量雅可比 H = ∂h/∂x，在同一点求值
   * @param r      测量空间的噪声协方差（即 V·R·Vᵀ，对称正定）
   * @param freeze 长 X_DIM 的标志数组，为 true 的状态分量本次不修正（K 的对应行置零）；nullptr = 全部修正
   * @return OK=已更新；NOT_INIT=init() 没成功过；BAD_ARG=输入或结果含 NaN / inf、或新息协方差 S 奇异，本次测量被丢弃、状态不变
   * @note 多传感器依次融合时用：Config 里的模型函数只能描述一组 Z_DIM 维测量，其余各组的 h、H、R 由调用方算好传入。
   *       各组测量噪声相互独立时，依次更新与把各组叠成一次更新等价。
   * @note 冻结分量相当于 Schmidt（consider）滤波：P 仍按 Joseph 形式更新，它对任意增益都给出正确的协方差。
   *       典型用法是某些状态在当前测量下不可区分（如滞空时加速度与加速度计零偏）时冻结其中之一，避免被带偏。
   * @note 不写 get_gain()（尺寸不同）。是否接受本次测量可先用 calc_nis() 检验。
   */
  template <uint32_t M>
  Status update(const MatrixF32<M, 1> &z, const MatrixF32<M, 1> &hx, const MatrixF32<M, X_DIM> &h_x, const MatrixF32<M, M> &r,
                const bool *freeze = nullptr);

  /**
   * @brief 计算一组测量的归一化新息平方 ε = νᵀ·S⁻¹·ν（ν = z − h(x)，S = H·P·Hᵀ + R），不改变状态
   * @tparam M 测量维度
   * @return ε（≥ 0）；未初始化、输入含 NaN / inf 或 S 奇异时返回 -1
   * @note 测量与模型一致时 ε ~ χ²(M)，可与 χ² 分布的分位数比较，剔除野值或检测模型失配（打滑、碰撞等）。
   *       M = 1、2、3、4 的 99% 分位数分别约为 6.63、9.21、11.34、13.28。
   */
  template <uint32_t M>
  float calc_nis(const MatrixF32<M, 1> &z, const MatrixF32<M, 1> &hx, const MatrixF32<M, X_DIM> &h_x, const MatrixF32<M, M> &r) const;

  /* ==================== */
  /* ==================== 参数接口 ==================== */

  /**
   * @brief 更新计算周期（下一次 predict() / update() 起生效）
   * @param dt 周期，单位 s
   * @return OK=已更新；BAD_ARG=dt ≤ 0 或非有限，周期不变
   */
  Status set_dt(float dt)
  {
    if (!(dt > 0.0f) || isinf(dt))
    {
      return Status::BAD_ARG;
    }
    _config.dt = dt;
    return Status::OK;
  }

  /** @brief 更新过程噪声协方差 Q（下一次 predict() 起生效） */
  void set_q(const MatrixF32<U_DIM, U_DIM> &q)
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

  /** @brief 当前计算周期，单位 s */
  float get_dt() const
  {
    return _config.dt;
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

  Config                  _config;          ///< 配置（模型函数、噪声参数、周期，set_*() 改的就是它）
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
Status FilterEkf<X_DIM, U_DIM, Z_DIM>::init(const MatrixF32<X_DIM, 1> &x0)
{
  _init = false;

  if (_config.f == nullptr || _config.df_dx == nullptr || _config.df_dw == nullptr)
  {
    return Status::BAD_ARG;
  }
  // 测量模型三个函数要么都给、要么都不给（都不给时只能用 update(z, hx, H, R)）
  const uint8_t measure_cnt = static_cast<uint8_t>((_config.h != nullptr) + (_config.dh_dx != nullptr) + (_config.dh_dv != nullptr));
  if (measure_cnt != 0U && measure_cnt != 3U)
  {
    return Status::BAD_ARG;
  }
  // 写成 !(…)：NaN 也判为非法
  if (!(_config.dt > 0.0f) || isinf(_config.dt))
  {
    return Status::BAD_ARG;
  }
  if (!all_finite(_config.q) || !all_finite(_config.r) || !all_finite(_config.p0) || !all_finite(x0))
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
Status FilterEkf<X_DIM, U_DIM, Z_DIM>::predict(const MatrixF32<U_DIM, 1> &u)
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

  // 雅可比在更新前的状态处求值
  const MatrixF32<X_DIM, X_DIM> f_x = _config.df_dx(_x, u, _config.dt);
  const MatrixF32<X_DIM, U_DIM> f_w = _config.df_dw(_x, u, _config.dt);

  // 先算到局部变量，全部有限才写回
  const MatrixF32<X_DIM, 1> x = _config.f(_x, u, _config.dt);
  MatrixF32<X_DIM, X_DIM>   p = f_x * _p * f_x.transpose() + f_w * _config.q * f_w.transpose();
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
Status FilterEkf<X_DIM, U_DIM, Z_DIM>::update(const MatrixF32<Z_DIM, 1> &z)
{
  if (!_init)
  {
    return Status::NOT_INIT;
  }
  if (_config.h == nullptr)
  {
    return Status::NOT_SUPPORTED; // 没配测量模型，只能用 update(z, hx, H, R)
  }
  if (!all_finite(z))
  {
    ++_reject_cnt;
    return Status::BAD_ARG;
  }

  // 在当前状态（predict() 写入的先验）处线性化
  const MatrixF32<Z_DIM, X_DIM> h_x = _config.dh_dx(_x, _config.dt);
  const MatrixF32<Z_DIM, Z_DIM> h_v = _config.dh_dv(_x, _config.dt);
  const MatrixF32<Z_DIM, Z_DIM> vrv = h_v * _config.r * h_v.transpose(); // 映射到测量空间的测量噪声协方差

  // 新息协方差 S = H·P·Hᵀ + V·R·Vᵀ，奇异时无法计算增益，放弃本次测量
  const MatrixF32<X_DIM, Z_DIM> pht = _p * h_x.transpose();
  const MatrixF32<Z_DIM, Z_DIM> s   = h_x * pht + vrv;
  MatrixF32<Z_DIM, Z_DIM>       s_inv;
  if (!s.inverse(s_inv))
  {
    ++_reject_cnt;
    return Status::BAD_ARG;
  }

  const MatrixF32<X_DIM, Z_DIM> k = pht * s_inv;
  const MatrixF32<X_DIM, 1>     x = _x + k * (z - _config.h(_x, _config.dt));

  // Joseph 形式，再对称化抵消舍入
  const MatrixF32<X_DIM, X_DIM> i_kh = MatrixF32<X_DIM, X_DIM>::identity() - k * h_x;
  MatrixF32<X_DIM, X_DIM>       p    = i_kh * _p * i_kh.transpose() + k * vrv * k.transpose();
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
template <uint32_t M>
Status FilterEkf<X_DIM, U_DIM, Z_DIM>::update(const MatrixF32<M, 1> &z, const MatrixF32<M, 1> &hx, const MatrixF32<M, X_DIM> &h_x,
                                              const MatrixF32<M, M> &r, const bool *freeze)
{
  if (!_init)
  {
    return Status::NOT_INIT;
  }
  if (!all_finite(z) || !all_finite(hx) || !all_finite(h_x) || !all_finite(r))
  {
    ++_reject_cnt;
    return Status::BAD_ARG;
  }

  const MatrixF32<X_DIM, M> pht = _p * h_x.transpose();
  const MatrixF32<M, M>     s   = h_x * pht + r;
  MatrixF32<M, M>           s_inv;
  if (!s.inverse(s_inv))
  {
    ++_reject_cnt;
    return Status::BAD_ARG;
  }

  MatrixF32<X_DIM, M> k = pht * s_inv;
  if (freeze != nullptr)
  {
    for (uint32_t i = 0U; i < X_DIM; ++i)
    {
      if (freeze[i])
      {
        for (uint32_t j = 0U; j < M; ++j)
        {
          k[i][j] = 0.0f;
        }
      }
    }
  }
  const MatrixF32<X_DIM, 1> x = _x + k * (z - hx);

  // Joseph 形式对任意 K 都成立（冻结时的 K 不是最优增益，不能用 (I − K·H)·P 的简化式）
  const MatrixF32<X_DIM, X_DIM> i_kh = MatrixF32<X_DIM, X_DIM>::identity() - k * h_x;
  MatrixF32<X_DIM, X_DIM>       p    = i_kh * _p * i_kh.transpose() + k * r * k.transpose();
  p                                  = (p + p.transpose()) * 0.5f;

  if (!all_finite(x) || !all_finite(p))
  {
    ++_reject_cnt;
    return Status::BAD_ARG;
  }

  _x = x;
  _p = p;
  ++_update_cnt;
  return Status::OK;
}

template <uint32_t X_DIM, uint32_t U_DIM, uint32_t Z_DIM>
template <uint32_t M>
float FilterEkf<X_DIM, U_DIM, Z_DIM>::calc_nis(const MatrixF32<M, 1> &z, const MatrixF32<M, 1> &hx, const MatrixF32<M, X_DIM> &h_x,
                                               const MatrixF32<M, M> &r) const
{
  if (!_init || !all_finite(z) || !all_finite(hx) || !all_finite(h_x) || !all_finite(r))
  {
    return -1.0f;
  }
  const MatrixF32<M, M> s = h_x * _p * h_x.transpose() + r;
  MatrixF32<M, M>       s_inv;
  if (!s.inverse(s_inv))
  {
    return -1.0f;
  }
  const MatrixF32<M, 1> nu  = z - hx;
  const float           nis = (nu.transpose() * s_inv * nu)[0][0];
  return isfinite(nis) ? nis : -1.0f;
}

template <uint32_t X_DIM, uint32_t U_DIM, uint32_t Z_DIM>
template <uint32_t R, uint32_t C>
bool FilterEkf<X_DIM, U_DIM, Z_DIM>::all_finite(const MatrixF32<R, C> &m)
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

#endif // __FILTER_EKF_HPP__
