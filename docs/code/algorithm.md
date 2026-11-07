# algorithm/ — 算法层

[← 代码层索引](README.md) · [device](device.md)

**职责**：纯数学。不 include HAL、不碰 FreeRTOS 任务、不知道外设存在。
**不负责**：什么时候算、算什么目标（→ `device/` 或 `app/`）。

只依赖 `service/system/status_code.hpp` 的 `Status`（纯枚举，不包含 FreeRTOS 头文件；1.4 从 `status.hpp` 拆出来，评审 A6），主机上也能直接编译：
`tests/host/` 是主机侧单元测试（PID、四元数、Matrix、RLS，CI 里跑；`cmake -S tests/host -B build/host && cmake --build build/host && ctest --test-dir build/host`）。

---

## 1. 文件清单

| 文件 | 内容 | 关键类型 |
| --- | --- | --- |
| `pid.hpp/.cpp` | 标量 PID：积分限幅、微分模式、前馈、输出限幅 | `PID`、`PID::Config`、`DiffMode` |
| `quaternion_pid.hpp/.cpp` | 四元数姿态 PID：误差转轴角 → 三轴 PID | `QuaternionPID` |
| `quaternion.hpp/.cpp` | 四元数与向量基础运算 | `Quaternion`、`Vector3` |
| `vqf.hpp/.cpp` | VQF 姿态解算（陀螺 + 加速度 + 可选磁力计） | `VQF`、`VQFParams` |
| `matrix.hpp/.cpp` | 定长矩阵，编译期维度 | `MatrixF32<ROWS, COLS>` |
| `rls.hpp` | 递推最小二乘（只有头文件） | `Rls<X_DIM, Y_DIM>` |
| `math_constants.hpp` | 常量 | `ICBSP_PI`（double）、`ICBSP_PI_F`（float） |

---

## 2. PID

```cpp
PID::Config cfg {
  /* kp / ki / kd、积分限幅、输出限幅、微分模式（差分 or 陀螺仪直给）… */
};
PID speed(cfg);

float out = speed.calc(target, feedback);              // 常规
float out = speed.calc(target, feedback, delta_feedback); // 微分项外部直给：本拍反馈增量（例如陀螺仪角速度 × 周期），不是每秒导数
speed.feed_forward(ff);                                // 前馈，叠加在输出上
speed.reset();

PID::Data d = speed.get();   // {target, feedback, output}（不加锁：跨任务读时由调用方保证）
```

- `i_max = 0` 表示积分上限跟随 `out_max`，**计算时取**：构造后再用链式 `limit_output()` 改输出限幅，积分上限也跟着变（评审 F4）。
- 输入（目标、反馈、增量、前馈）不是有限值（NaN / Inf）时丢弃这一拍：状态不变、输出保持上一拍，计入 `get_nonfinite_count()`。
  NaN 进了积分就永远出不来，所以在入口挡住（评审 F1）。
- `Quaternion::rotation_vector()` / `normalized()` 对零四元数、非有限值有防护（分别返回零向量 / 单位四元数）。

- `calc()` 每周期调一次，内部顺序是「算输入 → 算微分 → 算积分 → 限幅输出」
- 抗饱和靠**积分项单独限幅**（`i_max`，0 = 跟随 `out_max`）和**积分分离**（`i_sep`：误差绝对值超过它时积分项**清零**、不是冻结，0 = 不分离）；输出饱和时积分不会回退
- 想自己看波形：`get()` 返回的结构可以直接塞进 `DeviceMotor::pid_frame()`

---

## 3. 四元数 PID

标量 PID 管「一个轴」，姿态控制需要「把姿态误差变成三轴角速度指令」：

```cpp
QuaternionPID attitude(cfg);

Vector3 omega_ref = attitude.calc(q_target, q_feedback);                    // 只用姿态
Vector3 omega_ref = attitude.calc(q_target, q_feedback, omega_feedback);    // 带角速度反馈
Vector3 omega_ref = attitude.calc(q_target, q_feedback, omega_target, omega_feedback);
attitude.feed_forward(ff);
attitude.reset();
```

内部流程（`step()` / `run_axes()`）：四元数误差 → 转成旋转向量（轴×角）→ 三个轴各自跑一遍标量 PID。
和 `PID` 分开成两个头文件，只用标量 PID 的代码不需要包含四元数部分。

---

## 4. VQF 姿态解算

```cpp
VQF vqf(VQFParams{ /* 采样周期、滤波带宽、是否用磁力计… */ });

vqf.update(gyr, acc);              // 6D：陀螺 + 加速度
vqf.update(gyr, acc, mag);         // 9D：再加磁力计
vqf.getQuat6D(out4);               // 也提供 getQuat9D / getQuat3D
```

- 采样周期 `delta` 必须与调用频率一致（`update()` 的频率就是 IMU 采样率）
- 支持 `updateBatch()` 批量处理，以及 `getBiasEstimate()` / `setBiasEstimate()` 读写零偏
- `DeviceBmi088` 内部就是这样喂 VQF 的，业务侧一般只看 `snapshot()` 出来的姿态

---

## 5. MatrixF32 / Rls

```cpp
const float a[9] = {4, 7, 2, 3, 6, 1, 2, 5, 3};
auto A = MatrixF32<3, 3>::from_array(a);   // 按行；也有 identity()、transpose()、operator*
MatrixF32<3, 3> inv;
A.inverse(inv);          // 奇异时返回 false
A.norm();  A.normalized();  A.trace();  A.approx_equal(B, tol);
```

```cpp
Rls<2, 1> rls(Rls<2, 1>::Config(0.99f, 1000.0f));  // 遗忘因子 λ ∈ (0, 1]、初始协方差 p0；只给一项时别用花括号（有二义性）
rls.init();              // 可传初始参数估计 omega0；参数非法返回 BAD_ARG
rls.update(x, y);        // 递推一步；样本含 NaN / Inf 时丢弃并返回 BAD_ARG
rls.omega();             // 当前参数估计
rls.get_reject_count();  // 诊断：丢弃的非有限样本；另有 get_update_count() / get_trace_limit_count() / get_covariance_reset_count()
```

`Rls` 内部限制协方差矩阵的迹（激励不足时遗忘因子 < 1 会让 P 一直长大），P 失去正定时重置为 p0·I，避免长时间运行后数值发散。

---

## 6. 常见坑

| 现象 | 原因 |
| --- | --- |
| PID 输出在 0 附近高频抖 | 微分用差分且编码器有 ±1 抖动 → 改用 `calc(target, feedback, delta_feedback)` 喂陀螺仪 × 周期 / 滤波值 |
| 积分饱和退不回来 | 显式给了比 `out_max` 大得多的 `i_max`；`i_max = 0` 时积分上限自动跟随 `out_max`（评审 F4） |
| 姿态跟着电机转但方向反 | 四元数误差的轴向定义与实际机体轴不一致（检查轴的符号） |
| VQF 越跑越偏 | `delta` 与真实采样周期不一致，或陀螺零偏没标定 |
| `inverse()` 总返回 false | 矩阵奇异；RLS 里通常是激励不足（输入长期不变） |
