/**
 * @file test_main.cpp
 * @brief algorithm/ 的主机侧单元测试（阶段 3）：PID（F4、NaN）、四元数（F1、零模长）、Matrix、RLS
 *
 * @details 不依赖硬件、RTOS 和第三方测试框架：每条判据失败时打印文件 / 行号，进程返回失败条数。
 *          构建与运行见同目录 CMakeLists.txt（`cmake -S tests/host -B build/host && cmake --build build/host && ctest --test-dir build/host`）。
 */

#include "algorithm/matrix.hpp"
#include "algorithm/pid.hpp"
#include "algorithm/quaternion.hpp"
#include "algorithm/rls.hpp"

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
} // namespace

int main()
{
  test_pid_basic();
  test_pid_f4();
  test_pid_nan();
  test_quat_f1();
  test_quat_rotation();
  test_matrix();
  test_rls();
  std::printf("%d checks, %d failed\n", g_check, g_fail);
  return g_fail == 0 ? 0 : 1;
}
