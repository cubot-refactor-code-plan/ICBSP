/**
 * @file test_main.cpp
 * @brief algorithm/ 的主机侧单元测试（阶段 3）：PID（F4、NaN）、四元数（F1、零模长）、Matrix、RLS、EKF
 *
 * @details 不依赖硬件、RTOS 和第三方测试框架：每条判据失败时打印文件 / 行号，进程返回失败条数。
 *          构建与运行见同目录 CMakeLists.txt（`cmake -S tests/host -B build/host && cmake --build build/host && ctest --test-dir build/host`）。
 */

#include "algorithm/math/matrix.hpp"
#include "algorithm/pid.hpp"
#include "algorithm/math/quaternion.hpp"
#include "algorithm/observer/rls.hpp"
#include "algorithm/observer/filter_ekf.hpp"

#include <cmath>
#include <cstdio>
#include <limits>

namespace
{
int g_fail  = 0;
int g_check = 0;

#define CHECK(cond)                                                       \
  do                                                                      \
  {                                                                       \
    ++g_check;                                                            \
    if (!(cond))                                                          \
    {                                                                     \
      ++g_fail;                                                           \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
    }                                                                     \
  } while (0)

bool near(double a, double b, double tol)
{
  return std::fabs(a - b) <= tol;
}

const float NaN = std::numeric_limits<float>::quiet_NaN();

/* ==================== PID ==================== */

void test_pid_basic()
{
  PID p(PID::Config(2.0f, 0.0f, 0.0f, 100.0f));
  CHECK(near(p.calc(10.0f, 4.0f), 12.0f, 1e-6)); // 纯 P：2 × (10 − 4)
  CHECK(near(p.calc(100.0f, 0.0f), 100.0f, 1e-6)); // 输出限幅
  CHECK(near(p.calc(-100.0f, 0.0f), -100.0f, 1e-6));
  const PID::Data d = p.get();
  CHECK(d.target == -100.0f && d.feedback == 0.0f && d.output == -100.0f);
}

/** @brief F4：i_max = 0 时积分上限跟随 out_max，链式配置与 Config 配置一致 */
void test_pid_f4()
{
  PID a(PID::Config(0.0f, 1.0f, 0.0f, 50.0f)); // Config 配置，i_max = 0
  PID b{PID::Config()};
  b.ki(1.0f).limit_output(50.0f); // 链式配置
  float oa = 0.0f;
  float ob = 0.0f;
  for (int i = 0; i < 1000; ++i)
  {
    oa = a.calc(10.0f, 0.0f);
    ob = b.calc(10.0f, 0.0f);
  }
  CHECK(near(oa, 50.0f, 1e-4));
  CHECK(near(ob, 50.0f, 1e-4));
  // 反向：积分不会因为前面累积过头而迟迟退不下来（已钳位在 out_max）
  float back = 0.0f;
  for (int i = 0; i < 60; ++i)
  {
    back = a.calc(-10.0f, 0.0f);
  }
  CHECK(back < 0.0f);
  // 显式 i_max 生效
  PID c(PID::Config(0.0f, 1.0f, 0.0f, 50.0f, 0.0f, 20.0f));
  float oc = 0.0f;
  for (int i = 0; i < 1000; ++i)
  {
    oc = c.calc(10.0f, 0.0f);
  }
  CHECK(near(oc, 20.0f, 1e-4));
}

/** @brief 非有限输入丢弃并计数，输出保持上一次 */
void test_pid_nan()
{
  PID p(PID::Config(1.0f, 0.1f, 0.0f, 100.0f));
  const float o1 = p.calc(5.0f, 0.0f);
  const float o2 = p.calc(NaN, 0.0f);
  const float o3 = p.calc(5.0f, NaN);
  CHECK(std::isfinite(o2) && std::isfinite(o3));
  CHECK(o2 == o1 && o3 == o1);
  CHECK(p.get_nonfinite_count() == 2U);
  const float o4 = p.feed_forward(NaN);
  CHECK(std::isfinite(o4));
  CHECK(p.get_nonfinite_count() == 3U);
  const float o5 = p.calc(5.0f, 0.0f); // 恢复正常输入后继续工作
  CHECK(std::isfinite(o5) && o5 > o1);
}

/** @brief 积分暂停：暂停期间积分项不变（也不被积分分离清零），恢复后继续累加 */
void test_pid_integral_hold()
{
  PID p(PID::Config(0.0f, 1.0f, 0.0f, 100.0f, 0.0f, 0.0f, 0.0f, 0.0f, 5.0f)); // 纯 I，积分分离阈值 5
  const float o1 = p.calc(2.0f, 0.0f);                                        // 积分 2
  CHECK(near(o1, 2.0f, 1e-6) && !p.is_integral_held());
  p.set_integral_hold(true);
  CHECK(p.is_integral_held());
  CHECK(near(p.calc(2.0f, 0.0f), 2.0f, 1e-6));  // 不累加
  CHECK(near(p.calc(10.0f, 0.0f), 2.0f, 1e-6)); // 误差超过分离阈值也不清零
  p.reset();
  CHECK(p.is_integral_held()); // reset() 不改变暂停状态
  p.set_integral_hold(false);
  p.calc(2.0f, 0.0f);
  CHECK(near(p.calc(2.0f, 0.0f), 4.0f, 1e-6)); // 恢复累加
}

/* ==================== */


/* ==================== 四元数 ==================== */

/** @brief F1：默认 / 零模长 / NaN 都退回单位四元数，不产生 NaN */
void test_quat_f1()
{
  const Quaternion id = Quaternion::identity();
  CHECK(id.w == 1.0 && id.x == 0.0 && id.y == 0.0 && id.z == 0.0);
  const Quaternion z  = Quaternion::from_array((const double[4]){0.0, 0.0, 0.0, 0.0}).normalized();
  CHECK(z.w == 1.0 && z.x == 0.0);
  const Quaternion n = Quaternion::from_array((const double[4]){std::nan(""), 0.0, 0.0, 0.0}).normalized();
  CHECK(n.w == 1.0 && std::isfinite(n.x));
  const Vector3 rv = Quaternion::identity().rotation_vector();
  CHECK(rv.x == 0.0 && rv.y == 0.0 && rv.z == 0.0);
  const Quaternion tiny = Quaternion::from_rotation_vector({1e-12, 0.0, 0.0}); // 小角度一阶近似，不除零
  CHECK(std::isfinite(tiny.w) && near(tiny.norm(), 1.0, 1e-9));
}

void test_quat_rotation()
{
  // 绕 z 转 90°：x 轴 → y 轴
  const Quaternion q = Quaternion::from_axis_angle({0.0, 0.0, 1.0}, ICBSP_PI / 2.0);
  double r[3][3];
  q.to_matrix(r);
  CHECK(near(r[0][0], 0.0, 1e-12) && near(r[1][0], 1.0, 1e-12));
  double yaw = 0.0, pitch = 0.0, roll = 0.0;
  q.to_euler_zyx(yaw, pitch, roll);
  CHECK(near(yaw, ICBSP_PI / 2.0, 1e-9) && near(pitch, 0.0, 1e-9) && near(roll, 0.0, 1e-9));
  // 旋转向量往返
  const Quaternion q2 = Quaternion::from_rotation_vector({0.3, -0.2, 0.5});
  const Vector3    v2 = q2.rotation_vector();
  CHECK(near(v2.x, 0.3, 1e-9) && near(v2.y, -0.2, 1e-9) && near(v2.z, 0.5, 1e-9));
  // q · conj(q) = 单位
  const Quaternion e = q2 * q2.conj();
  CHECK(near(e.w, 1.0, 1e-12) && near(e.x, 0.0, 1e-12));
  // 积分：绕 x 以 1 rad/s 积分 1 s（1000 步）≈ 转 1 rad
  Quaternion s = Quaternion::identity();
  for (int i = 0; i < 1000; ++i)
  {
    s = s.integrate({1.0, 0.0, 0.0}, 0.001);
  }
  CHECK(near(s.rotation_vector().x, 1.0, 1e-6) && near(s.norm(), 1.0, 1e-12));
  // slerp 端点与中点
  const Quaternion a = Quaternion::identity();
  const Quaternion b = Quaternion::from_axis_angle({0.0, 1.0, 0.0}, 1.0);
  CHECK(near(Quaternion::slerp(a, b, 0.0).dot(a), 1.0, 1e-9));
  CHECK(near(Quaternion::slerp(a, b, 1.0).dot(b), 1.0, 1e-9));
  CHECK(near(Quaternion::slerp(a, b, 0.5).rotation_vector().y, 0.5, 1e-9));
}

/* ==================== */


/* ==================== Matrix ==================== */

void test_matrix()
{
  const float ad[6] = {1, 2, 3, 4, 5, 6};
  const float bd[6] = {7, 8, 9, 10, 11, 12};
  const auto  a     = MatrixF32<2, 3>::from_array(ad);
  const auto  b     = MatrixF32<3, 2>::from_array(bd);
  const auto  c     = a * b; // [[58, 64], [139, 154]]
  CHECK(c[0][0] == 58.0f && c[0][1] == 64.0f && c[1][0] == 139.0f && c[1][1] == 154.0f);
  const auto at = a.transpose();
  CHECK(at[2][0] == 3.0f && at[0][1] == 4.0f);
  const float md[9] = {4, 7, 2, 3, 6, 1, 2, 5, 3};
  const auto  m     = MatrixF32<3, 3>::from_array(md);
  MatrixF32<3, 3> inv;
  CHECK(m.inverse(inv));
  CHECK((m * inv).approx_equal(MatrixF32<3, 3>::identity(), 1e-4f));
  CHECK(near(m.trace(), 13.0, 1e-6));
  const float sd[9] = {1, 2, 3, 2, 4, 6, 1, 1, 1}; // 奇异
  MatrixF32<3, 3> sinv;
  CHECK((!MatrixF32<3, 3>::from_array(sd).inverse(sinv)));
  const float vd[3] = {3, 4, 0};
  CHECK(near(MatrixF32<3, 1>::from_array(vd).norm(), 5.0, 1e-6));
}

/* ==================== */


/* ==================== RLS ==================== */

void test_rls()
{
  // 辨识 y = 2.5·u − 1.0
  Rls<2, 1> rls(Rls<2, 1>::Config(1.0f, 1000.0f));
  CHECK(rls.init() == Status::OK);
  uint32_t seed = 12345U;
  for (int i = 0; i < 200; ++i)
  {
    seed             = seed * 1664525U + 1013904223U;
    const float u    = static_cast<float>(seed >> 8) / 16777216.0f * 4.0f - 2.0f;
    const float xd[2] = {u, 1.0f};
    const float yd[1] = {2.5f * u - 1.0f};
    (void)rls.update(MatrixF32<2, 1>::from_array(xd), MatrixF32<1, 1>::from_array(yd));
  }
  CHECK(near(rls.omega()[0][0], 2.5, 1e-3));
  CHECK(near(rls.omega()[1][0], -1.0, 1e-3));
  CHECK(rls.get_update_count() == 200U);
  // 非有限样本：丢弃、计数、参数不变
  const float a0    = rls.omega()[0][0];
  const float xd[2] = {NaN, 1.0f};
  const float yd[1] = {0.0f};
  CHECK(rls.update(MatrixF32<2, 1>::from_array(xd), MatrixF32<1, 1>::from_array(yd)) == Status::BAD_ARG);
  CHECK(rls.get_reject_count() == 1U);
  CHECK(rls.omega()[0][0] == a0);
  // 参数非法
  Rls<2, 1> bad(Rls<2, 1>::Config(1.5f, 1000.0f));
  CHECK(bad.init() == Status::BAD_ARG);
  // 遗忘因子 < 1 且激励不足：tr(P) 被限住，不会溢出
  Rls<2, 1> forget(Rls<2, 1>::Config(0.9f, 10.0f));
  CHECK(forget.init() == Status::OK);
  const float zx[2] = {0.0f, 0.0f};
  for (int i = 0; i < 2000; ++i)
  {
    (void)forget.update(MatrixF32<2, 1>::from_array(zx), MatrixF32<1, 1>::from_array(yd));
  }
  CHECK(forget.get_trace_limit_count() > 0U);
  CHECK(std::isfinite(forget.omega()[0][0]));
}

/* ==================== EKF ==================== */

// 二维常值模型 x_k = x_{k-1}，测量 z = x0（与线性化接口的结果对比）
MatrixF32<2, 1> ekf_f(const MatrixF32<2, 1> &x, const MatrixF32<2, 1> &, float) { return x; }
MatrixF32<2, 2> ekf_df_dx(const MatrixF32<2, 1> &, const MatrixF32<2, 1> &, float) { return MatrixF32<2, 2>::identity(); }
MatrixF32<2, 2> ekf_df_dw(const MatrixF32<2, 1> &, const MatrixF32<2, 1> &, float) { return MatrixF32<2, 2>::identity(); }
MatrixF32<1, 1> ekf_h(const MatrixF32<2, 1> &x, float) { return MatrixF32<1, 1>::constant(x[0][0]); }
MatrixF32<1, 2> ekf_dh_dx(const MatrixF32<2, 1> &, float)
{
  const float d[2] = {1.0f, 0.0f};
  return MatrixF32<1, 2>::from_array(d);
}
MatrixF32<1, 1> ekf_dh_dv(const MatrixF32<2, 1> &, float) { return MatrixF32<1, 1>::identity(); }

void test_ekf_linearized()
{
  // P0 带相关项，测量 x0 时 x1 也会被修正
  const float p0d[4] = {1.0f, 0.5f, 0.5f, 1.0f};
  const FilterEkf<2, 2, 1>::Config cfg{.f     = ekf_f,
                                       .df_dx = ekf_df_dx,
                                       .df_dw = ekf_df_dw,
                                       .h     = ekf_h,
                                       .dh_dx = ekf_dh_dx,
                                       .dh_dv = ekf_dh_dv,
                                       .q     = MatrixF32<2, 2>(),
                                       .r     = MatrixF32<1, 1>::constant(1.0f),
                                       .p0    = MatrixF32<2, 2>::from_array(p0d),
                                       .dt    = 0.001f};
  FilterEkf<2, 2, 1> native(cfg);
  FilterEkf<2, 2, 1> lin(cfg);
  CHECK(native.init() == Status::OK);
  CHECK(lin.init() == Status::OK);

  const MatrixF32<1, 1> z = MatrixF32<1, 1>::constant(2.0f);
  // ν = 2，S = 1 + 1 = 2，ε = 4 / 2 = 2
  CHECK(near(lin.calc_nis(z, ekf_h(lin.get_state(), 0.0f), ekf_dh_dx(lin.get_state(), 0.0f), MatrixF32<1, 1>::constant(1.0f)), 2.0, 1e-6));

  CHECK(native.update(z) == Status::OK);
  CHECK(lin.update(z, ekf_h(lin.get_state(), 0.0f), ekf_dh_dx(lin.get_state(), 0.0f), MatrixF32<1, 1>::constant(1.0f)) == Status::OK);
  CHECK(near(lin.get_state()[0][0], native.get_state()[0][0], 1e-6)); // K = [0.5; 0.25]
  CHECK(near(lin.get_state()[1][0], native.get_state()[1][0], 1e-6));
  CHECK(near(lin.get_state()[1][0], 0.5, 1e-6));
  CHECK(near(lin.get_covariance()[1][1], native.get_covariance()[1][1], 1e-6));

  // 冻结 x1：x1 不变，P 仍对称且 x1 的方差不减小
  FilterEkf<2, 2, 1> frz(cfg);
  CHECK(frz.init() == Status::OK);
  const bool freeze[2] = {false, true};
  CHECK(frz.update(z, ekf_h(frz.get_state(), 0.0f), ekf_dh_dx(frz.get_state(), 0.0f), MatrixF32<1, 1>::constant(1.0f), freeze) == Status::OK);
  CHECK(frz.get_state()[1][0] == 0.0f);
  CHECK(near(frz.get_state()[0][0], 1.0, 1e-6));
  CHECK(frz.get_covariance()[0][1] == frz.get_covariance()[1][0]);
  CHECK(frz.get_covariance()[1][1] >= 1.0f - 1e-6f);

  // 不同维度：一次 2 维测量（两个分量各测一次）
  const float h2d[4] = {1.0f, 0.0f, 0.0f, 1.0f};
  const float z2d[2] = {1.0f, -1.0f};
  CHECK(lin.update(MatrixF32<2, 1>::from_array(z2d), lin.get_state(), MatrixF32<2, 2>::from_array(h2d), MatrixF32<2, 2>::identity()) == Status::OK);
  // 非有限输入：丢弃、状态不变
  const MatrixF32<2, 1> before = lin.get_state();
  const float nan2[2]          = {NaN, 0.0f};
  CHECK(lin.update(MatrixF32<2, 1>::from_array(nan2), lin.get_state(), MatrixF32<2, 2>::from_array(h2d), MatrixF32<2, 2>::identity()) == Status::BAD_ARG);
  CHECK(lin.get_state() == before);
  CHECK(lin.calc_nis(MatrixF32<2, 1>::from_array(nan2), lin.get_state(), MatrixF32<2, 2>::from_array(h2d), MatrixF32<2, 2>::identity()) < 0.0f);

  // 不给测量模型：init() 可以成功，只能用线性化接口；只给一部分则 init() 失败
  FilterEkf<2, 2, 1> no_h({.f = ekf_f, .df_dx = ekf_df_dx, .df_dw = ekf_df_dw});
  CHECK(no_h.init() == Status::OK);
  CHECK(no_h.update(z) == Status::NOT_SUPPORTED);
  CHECK(no_h.update(z, ekf_h(no_h.get_state(), 0.0f), ekf_dh_dx(no_h.get_state(), 0.0f), MatrixF32<1, 1>::constant(1.0f)) == Status::OK);
  FilterEkf<2, 2, 1> part_h({.f = ekf_f, .df_dx = ekf_df_dx, .df_dw = ekf_df_dw, .h = ekf_h});
  CHECK(part_h.init() == Status::BAD_ARG);

  // init(x0, p0)：用给定的协方差重新开始；p0 非有限时拒绝
  const float x0d[2] = {3.0f, -1.0f};
  const float p1d[4] = {0.2f, 0.0f, 0.0f, 0.05f};
  CHECK(lin.init(MatrixF32<2, 1>::from_array(x0d), MatrixF32<2, 2>::from_array(p1d)) == Status::OK);
  CHECK((lin.get_state() == MatrixF32<2, 1>::from_array(x0d)));
  CHECK((lin.get_covariance() == MatrixF32<2, 2>::from_array(p1d)));
  const float pnan[4] = {NaN, 0.0f, 0.0f, 1.0f};
  CHECK(lin.init(MatrixF32<2, 1>::from_array(x0d), MatrixF32<2, 2>::from_array(pnan)) == Status::BAD_ARG);
  CHECK(!lin.is_initialized());
}

/* ==================== */
} // namespace


int main()
{
  test_pid_basic();
  test_pid_f4();
  test_pid_nan();
  test_pid_integral_hold();
  test_quat_f1();
  test_quat_rotation();
  test_matrix();
  test_rls();
  test_ekf_linearized();
  std::printf("%d checks, %d failed\n", g_check, g_fail);
  return g_fail == 0 ? 0 : 1;
}
