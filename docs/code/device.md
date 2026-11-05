# device/ — 设备层

[← 代码层索引](README.md) · [bsp](bsp.md) · [service](service.md) · [protocol](protocol.md) · [algorithm](algorithm.md)

**职责**：把某个具体设备「绑定到哪个 BSP、注册到哪个节点、什么时候 init、怎么给指令」封成一个类。
**不负责**：底层时序（→ `bsp/`）、帧字节布局（→ `device/motor/emmv5_protocol`）、PID 数学（→ `algorithm/`）。

---

## 1. 文件清单

| 文件 | 设备 | 依赖 | 关键类型 |
| --- | --- | --- | --- |
| `dji_motor.hpp/.cpp` | DJI M3508 / M2006 / GM6020 | `BspCanT`、`CanRxNodeT`、`CanTxNodeT` | `DjiMotor<DjiMotorType, CanT>` |
| `device_motor.hpp/.cpp` | 直流减速电机 + 编码器 + PID | `BspPwm`、`BspEncoder`、`PID` | `DeviceMotor` |
| `device_servo.hpp/.cpp` | 舵机（脉宽控制） | `BspPwm` | `DeviceServo` |
| `device_r9ds.hpp/.cpp` | R9DS 遥控接收机 | `BspUartT` | `DeviceR9dsT<Uart>` |
| `device_emmv5_group.hpp/.cpp` | Emm_V5 步进闭环（多地址总线） | `BspUartT`、`emmv5` | `DeviceEmmv5GroupT<Uart>` |
| `device_bmi088.hpp/.cpp` | BMI088 IMU + VQF 姿态 | `BspSpi`、`VQF` | `DeviceBmi088`、`Bmi088RawData` |
| `device_buzzer.hpp/.cpp` | 无源蜂鸣器 | `BspPwm` | `DeviceBuzzer` |

设备实例都在宿主 `user/board/board_cfg.hpp` 里 `inline` 定义，init 在 `board_init()`。

---

## 2. DJI 电机

```cpp
enum class DjiMotorType : uint8_t { M3508 = 0x00, GM6020 = 0x01, M2006 = 0x02 };

inline DjiMotor<DjiMotorType::M3508, decltype(bsp_can1)> chassis_lf(bsp_can1, 1U);  // 1 = 电调 ID
```

- 模板参数 `type` 决定：**合法 ID 范围**、反馈解析、控制帧映射、限幅、默认减速比
- 构造后必须 `init()`：内部会 `CanRxNodeT::regist()` 注册反馈帧、`CanTxNodeT` 申请发送槽位
- `DjiMotorControlMode::VOLTAGE` / `CURRENT` 只对 GM6020 有效（M3508/C620、M2006/C610 固定电流协议）
- 跨任务读反馈用 `snapshot(MotorData &, DjiMotorRawData *)`（临界区内拷贝同一帧）；`get_data()` 是实时引用，can_rx_task 随时在改。
  原来的互斥锁去掉了（读者拿不到锁，try-lock 失败时反而把帧当成没人认领，评审 F5）
- 构造函数最后一个参数 `online_timeout_ms`（默认 30）：多久收不到有效反馈判离线（评审 I4）
- 多圈角度 `angle_multi_round` 是 double，直接存双精度结果（评审 F11）
- 对象**不可复制/移动**：在线检查节点与控制帧槽位靠对象地址标识
- `unpack(rx)` 由接收节点回调调用，`fill_data(output)` 往发送槽位填值，一般业务只用 `get_status()` 和设置输出

---

## 3. 直流电机（DeviceMotor）

```cpp
inline DeviceMotor motor(
    {.pwm_a = &bsp_pwm1, .pwm_b = &bsp_pwm2, .encoder = &bsp_encoder1,
     .encoder_lines = 500.0f, .encoder_multiplier = 4.0f,
     .gear_ratio = 32.0f, .wheel_diameter_cm = 7.0f,
     .out_max = 100.0f, .deadband_cm_s = 0.4f,
     .pid = {/* 速度环参数，默认全 0，靠实机标定 */}});
```

| 方法 | 说明 |
| --- | --- |
| `Status init()` | 校验三个绑定指针，计算 `counts_per_cm` |
| `void set_speed(float cm_s)` | 设目标线速度 |
| `void set_speed_time(float cm_s, uint32_t ms)` | 带时长的目标（到期自动停） |
| `bool update(float dt_s)` | 每周期调用：算编码器速度 → PID → 写 PWM |
| `size_t pid_frame(uint8_t *buf, uint8_t fields = PID_ALL)` | **只组 JustFloat 帧**，发不发、发到哪由 App 决定 |
| `void stop()` / `Mode::STOP` | 两路 PWM 给满占空比（绕组短路刹车） |

`pid_frame()` 的字段位：

```cpp
DeviceMotor::PID_TARGET | DeviceMotor::PID_FEEDBACK | DeviceMotor::PID_OUTPUT | DeviceMotor::PID_ALL
```

帧尾固定 `00 00 80 7F`（JustFloat 结束符），返回值是实际写入的字节数。

**为什么设备不持有串口**：波形发给上位机是调试需求，不是电机的一部分。App 拿 `pid_frame()` 的缓冲
用哪个串口发都行，换传输方式也不用改设备。

---

## 4. 其它设备

### DeviceServo

```cpp
inline DeviceServo servo({.pwm = &bsp_pwm_servo,
                          .pulse_min_us = 500.0f,   // 角度 0 端的脉宽
                          .pulse_max_us = 2500.0f,  // range_deg 端的脉宽
                          .range_deg = 270.0f,      // 180° 舵机填 180
                          .max_speed_dps = 300.0f,  // 0 = 不限速
                          .max_accel_dps2 = 1500.0f,// 0 = 不限加速度
                          .reversed = false,
                          .trim_us = 0.0f});
```

`init()` 后 `set_angle(deg)` 设目标，`update()` 周期调用做缓动，`off()` 关输出。
角度按 `range_deg` 限幅，`set_pulse_us(us)` 可直接给脉宽。PWM 通道必须已配成 50 Hz / 20 ms。

### DeviceR9dsT&lt;Uart&gt;

```cpp
inline DeviceR9dsT<decltype(bsp_uart3)> rc(bsp_uart3);
```

`init()` 注册串口帧处理，`update(timeout_ms)` 取最新通道值，`is_online()` / `is_frame_lost()` / `is_failsafe()`；
`update()` 在别的任务里跑时用 `snapshot(Rc &, uint16_t *raw)` 读（评审 F5）。诊断计数 `byte_cnt` / `frame_cnt` 是 `uint32_t`（N34）
用于失控保护。

### DeviceEmmv5GroupT&lt;Uart&gt;

一条 485 总线上挂多个地址的 Emm_V5：

```cpp
Status enable(uint8_t addr, bool on);
Status set_velocity(uint8_t addr, emmv5::Direction dir, uint16_t rpm, uint8_t accel);
Status set_position(uint8_t addr, emmv5::Direction dir, uint16_t rpm, uint8_t accel,
                    int32_t pulses, emmv5::PositionMode mode);
Status stop(uint8_t addr);
```

`lock_bus()` / `unlock_bus()` 用于多条指令的原子性（多任务共用一条 485 时必须包起来）。
每一帧的字节由 `device/motor/emmv5_protocol.hpp` 负责，设备层只管「发给谁、发什么参数、等不等回复」。

- 回复的读取：`take_position()` / `take_status()` / `take_in_position()` **读完就清**（`take_`，N33）；`is_in_position(i)` / `is_online(addr)` 只查不清。
- 校验方式：`Config::checksum` 必须和驱动器菜单 Checksum 一致，支持 `emmv5::Checksum::FIXED_6B`（默认）和 `XOR`（**未实测**；回复帧两种都接受，因为说明书没写回复用哪种）。CRC-8 没有实现（说明书没给参数）。
- 帧缓冲跟随串口实例的 `TX_SIZE`（`BspUartT::TX_CAPACITY`），MMCL 子命令最多 `TX_SIZE − 5` 字节。

### 默认树里的设备节点（Q6）

挂在总线上的设备各有一个 EventState 节点，在自己的 `init()` 里挂到所用总线的节点下（`get_state()` 读取）：

| 设备 | 挂在 | 记什么 |
| --- | --- | --- |
| `DjiMotor` | CAN 总线节点 | 初始化失败；掉线（Online 由在线变离线）→ `TIMEOUT` |
| `DeviceEmmv5GroupT` | 串口总线节点 | 初始化失败；任一台电机掉线 → `TIMEOUT` |
| `DeviceR9dsT` | 串口总线节点 | 初始化失败；`update()` 发现由在线变离线 → `TIMEOUT` |
| `DeviceBmi088` | SPI 总线节点 | 初始化失败；读帧失败 → 读帧返回的错误码 |

`DeviceMotor`、`DeviceServo`、`DeviceBuzzer` 不在总线上（PWM / 编码器），没有节点。

### DeviceBmi088

```cpp
inline DeviceBmi088 imu(bsp_spi_acc, bsp_spi_gyro,
                        {.acc_range = DeviceBmi088::AccRange::G24,
                         .gyro_range = DeviceBmi088::GyroRange::DPS2000});
```

- `init()` 做自检与量程配置（加速度计和陀螺仪是两条独立片选）
- `update(timeout_ms)` 读原始数据并跑 VQF，`snapshot(ImuData&, Bmi088RawData*)` 取一致的一份快照
- 诊断计数：`get_acc_saturation_count()`、`get_gyro_saturation_count()`、`get_update_fail_count()`、`get_attitude_skip_count()`
- `is_attitude_ready()`：姿态解算过至少一帧才为真；之前 `ImuData` 的四元数是单位四元数（不是全 0），姿态控制要先等它（评审 F1）
- 运行中重新 `init()` 与内部刷新任务互斥（刷新锁），不会和正在解算的那一帧并发改 VQF / 芯片（评审 F8）
- 调度器启动前的等待用 DWT 忙等，不依赖 HAL 时基中断的优先级（评审 A3）
- 自动模式只按陀螺仪节奏刷新，加速度计 INT1 只计数：不需要时宿主可以不转调、或者不开对应 EXTI
- `attach_task(handle)` 可绑定任务句柄，让中断/错误能唤醒该任务

### DeviceBuzzer

```cpp
inline DeviceBuzzer buzzer(bsp_pwm_buzzer);
buzzer.beep(2000, 100);   // 2 kHz 响 100 ms
buzzer.tone(1000, 50);    // 持续音，音量 50%
```

---

## 5. 常见坑

| 现象 | 原因 |
| --- | --- |
| `DjiMotor` 编译报 ID 越界 | `DjiMotorType::M3508` 只接受 1–8，`M2006` 1–8，`GM6020` 1–7（以头文件注释为准） |
| 电机抖动不动 | `DeviceMotor` 的 `encoder_multiplier` 与 CubeMX 编码器模式不匹配（TI12 = 4，TI1 = 2） |
| 电机低速嗡嗡响 | `deadband_cm_s` 太小，静止时的 ±1 计数被 P 放大成占空比抖动 |
| 多电机只动一个 | 同 ID 冲突，或没写进 `ICBSP_CAN_INSTANCES` |
| Emm_V5 偶发丢指令 | 多任务共用 485 却没 `lock_bus()` |
| BMI088 姿态乱跳 | 片选 `read_dummy` 配错（陀螺仪 1、加速度计 0），或没开 `MasterKeepIOState` |
| 波形发不出去 | `pid_frame()` 只组帧；要在 App 里调 `bsp_uart1.send()` |
