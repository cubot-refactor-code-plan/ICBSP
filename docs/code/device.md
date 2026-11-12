# device/ — 设备层

[← 代码层索引](README.md) · [bsp](bsp.md) · [service](service.md) · [algorithm](algorithm.md) · [tasks](tasks.md)

**职责**：把某个具体设备「绑定到哪个 BSP、注册到哪个节点、什么时候 init、怎么给指令」封成一个类。
**不负责**：底层时序（→ `bsp/`）、帧字节布局（→ `device/motor/emmv5_protocol`）、PID 数学（→ `algorithm/`）。

---

## 1. 文件清单

目录按设备种类分：`motor/`、`sensor/`、`remote/`、`actuator/`。只有头文件的是模板（按总线实例类型实例化）。

| 文件 | 设备 | 依赖 | 关键类型 |
| --- | --- | --- | --- |
| `motor/dji_motor.hpp` | DJI M3508 / M2006 / GM6020 | `BspCanT`、`CanRxNodeT`、`CanTxNodeT` | `DjiMotor<DjiMotorType, CanT>` |
| `motor/dm_motor.hpp` | 达妙 J4310 / J4340P / J8009P（MIT / 位置速度 / 速度） | `BspCanT`、`CanRxNodeT` | `DmMotor<CanT>`、`DmModel`、`DmMotorFeedback` |
| `motor/device_motor.hpp/.cpp` | 直流减速电机 + 编码器 + PID | `BspPwm`、`BspEncoder`、`PID` | `DeviceMotor` |
| `motor/device_emmv5_group.hpp` | Emm_V5 步进闭环（一条 485 上多个地址） | `BspUartT`、`emmv5_protocol` | `DeviceEmmv5GroupT<Uart>` |
| `motor/emmv5_protocol.hpp/.cpp` | Emm_V5 帧编解码（纯函数，不碰 HAL / bsp） | — | `emmv5::Frame`、`*Scanner` |
| `sensor/device_bmi088.hpp/.cpp` | BMI088 IMU + VQF 姿态 | `BspSpiDevice`、`VQF` | `DeviceBmi088`、`Bmi088RawData` |
| `sensor/device_wt102.hpp` | WT102 单轴陀螺仪（Z 轴加速度 / 角速度 / 航向角） | `BspUartT` | `DeviceWt102T<Uart>` |
| `remote/device_r9ds.hpp` | R9DS 遥控接收机（SBUS） | `BspUartT` | `DeviceR9dsT<Uart>` |
| `actuator/device_servo.hpp/.cpp` | 舵机（脉宽控制） | `BspPwm` | `DeviceServo` |
| `actuator/device_buzzer.hpp/.cpp` | 无源蜂鸣器 | `BspPwm` | `DeviceBuzzer` |

设备类的构造都不是 constexpr（构造时就要绑外设、查参数）：实例推荐定义在宿主 `board_cfg.cpp`、头文件里 `extern`；下面示例里写成 `inline` 的也能用（动态初始化，只初始化一次）。`init()` 在 `board_init()` 里逐个 `ICBSP_CHECK`。

---

## 2. motor/ — 电机

### DjiMotor（DJI 电机）

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

### DmMotor（达妙电机）

协议来自 J4310 / J4340P / J8009P 说明书的 CAN 通信一章（清单与获取方式见 [docs/sheet/README.md](../sheet/README.md)），三个系列逐项相同，所以**不按型号做模板**：
型号经 `Config::model`（`DmModel::J4310` / `J4340P` / `J4340P_V11` / `J8009P`）查 `dm_model_info()`（减速比、额定 / 峰值扭矩、空载转速），
MIT 前馈扭矩按峰值扭矩限幅。映射范围是驱动器里可改的寄存器，不能从型号推出：实测 J4340P 是 12.5 / 10 / 28，不是说明书的预设 12.5 / 30 / 10。

实板（阶段 3，2026-11-06）：J4340P（MIT）读寄存器、`sync_ranges()`、使能 / 失能、零力矩 / 阻尼速度 / 位置控制都通过（宿主 M05 / M09）；J4310P 只做了读寄存器与状态反馈（它的 CAN_ID 与同总线 GM6020 冲突，没有使能）；J8009P 没有实物，只按说明书核对。

```cpp
inline DmMotor<decltype(bsp_can2)> yaw_motor(bsp_can2, {.model = DmModel::J4340P, .can_id = 0x02, .master_id = 0x12,
                                                      .mode = DmMode::MIT});   // 映射范围留 0
ICBSP_CHECK(yaw_motor.init() == Status::OK);   // board_init()
yaw_motor.sync_ranges();                        // 任务里、system_start() 之后：从驱动器读 PMAX / VMAX / TMAX / 模式
yaw_motor.enable();                             // 上电是失能态
yaw_motor.set_mit(p, v, kp, kd, t);             // 控制任务每周期调用
```

- `p_max` / `v_max` / `t_max`：填和驱动器一致的值，或留 0、启动后 `sync_ranges()` 从驱动器读（未知时 `set_*()` 返回 `NOT_INIT`）。填了但和驱动器不一致时 `sync_ranges()` 计入 `Diag::range_mismatch`、在默认树节点记 `BAD_ARG`，之后以驱动器为准；模式同理；Kp ∈ [0, 500]、Kd ∈ [0, 5]，超范围限幅，非有限值返回 `BAD_ARG` 不发。
- 控制帧**不经过 `CanTxNodeT`**：发送槽位过期清零后，MIT 帧全 0 映射成 p = −P_MAX、t = −T_MAX（满负扭矩）。控制帧由 `send_latest()` 直接发，调用方按控制周期持续调用；停发后靠电机自己的通讯丢失保护失能。使能 / 失能 / 清错误 / 设零点用 `send()` 排队发，不会被同 ID 的控制帧挤掉。
- 反馈是问询式的（电机收到发给自己的帧才回），不发控制帧时会判离线。每台电机的 `master_id` 要不同；反馈里的 ID 只有低 4 位，本类只认 `can_id & 0x0F` 对得上的帧。
- `switch_mode()` 写 CTRL_MODE 寄存器（不存 flash）；写寄存器的回复也走 `master_id`，本类按挂起的那次写识别并吃掉，不当反馈解。
- **ID 冲突**：驱动器只核对帧 ID 的低 8 位。和 DJI 电机同总线时 `can_id` 的低 8 位要避开 0x01~0x0B（DJI 反馈 0x201~0x20B）和 0xFE / 0xFF / 0x00（DJI 控制帧）。实测 CAN_ID 0x05（速度模式，控制帧 0x205）会把 GM6020 ID 1 的反馈 0x205 当成速度给定。
- 只需要 `ICBSP_CAN_RX_ENABLE`（控制帧不走发送节点），关了接收路径时包含 `dm_motor.hpp` 会 `#error`。
- 默认树：节点挂在 CAN 总线节点下，掉线记 `TIMEOUT`，反馈进入故障态（ERR 不是 0 失能 / 1 使能：校准 / 传感器异常、超压 / 欠压 / 过流 / 过温 / 通讯丢失 / 过载）记 `IO_ERROR`；`read_register()` 读寄存器（不用使能，可用来核对 PMAX / VMAX / TMAX / 控制模式）。

### DeviceMotor（直流电机）

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
| `size_t get_pid_frame(uint8_t *buf, uint8_t fields = PID_ALL) const` | **只组 JustFloat 帧**，发不发、发到哪由 App 决定 |
| `void stop()` / `Mode::STOP` | 两路 PWM 给满占空比（绕组短路刹车） |

`get_pid_frame()` 的字段位：

```cpp
DeviceMotor::PID_TARGET | DeviceMotor::PID_FEEDBACK | DeviceMotor::PID_OUTPUT | DeviceMotor::PID_ALL
```

帧尾固定 `00 00 80 7F`（JustFloat 结束符），返回值是实际写入的字节数。

**为什么设备不持有串口**：波形发给上位机是调试需求，不是电机的一部分。App 拿 `get_pid_frame()` 组好的缓冲
用哪个串口发都行，换传输方式也不用改设备。

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

**帧编解码（`motor/emmv5_protocol`）**

纯函数式：没有状态机以外的成员，不 include HAL / bsp；改协议只动它，改收发方式只动设备类。

帧布局（EmmV5.0 说明书第 6 章）：

```text
普通命令：  地址(1) + 功能码(1) + 参数(...) + 校验(1)
MMCL 总帧：地址(1) + 0xAA(1) + 总长度(2, 大端，含头尾) + 子命令串(N) + 校验(1)
            子命令 = 一条完整的普通命令（各自带校验字节）
```

- 地址 0x00 是广播（`emmv5::BROADCAST`）。
- 校验默认是固定字节 `0x6B`（`Checksum::FIXED_6B`）；驱动器菜单选 XOR 时用 `Checksum::XOR`（前面全部字节的异或）。回复帧两种都接受。

组帧用现成的函数，每个返回一帧封好口的 `emmv5::Frame`（`data[]` / `size` 是成员，`is_valid()` 检查有没有溢出）：

```cpp
emmv5::Frame f = emmv5::enable(addr, true);               // 也有 velocity / position / stop / zero_position /
                                                           // clear_protection / set_control_mode / quick_position* / read
f.apply_checksum(emmv5::Checksum::XOR);                    // 驱动器菜单选了 XOR 时
if (f.is_valid()) { uart.send(f.data, f.size); }
size_t n = emmv5::build_multi_frame(emmv5::BROADCAST, sub, sub_len, out, sizeof(out));  // MMCL 总帧；0 = 放不下
```

解析回复（流式）：回复是「到位」「位置」「状态」之类的通知帧，字节流可能粘连 / 截断，所以用扫描器、不一次性解析：

```cpp
struct InPositionScanner { void reset(); bool feed(uint8_t addr, const uint8_t *data, size_t size); };
struct PositionScanner   { void reset(); bool feed(uint8_t addr, const uint8_t *data, size_t size, int32_t &position); };
struct StatusScanner     { void reset(); bool feed(uint8_t addr, const uint8_t *data, size_t size, uint8_t &status); };

emmv5::PositionScanner scanner;
if (scanner.feed(addr, buf, len, position)) { /* 凑满一帧：更新该地址的当前位置 */ }
```

扫描器内部保存半帧状态，**同一路总线的扫描器只能在一个任务里用**。

---

## 3. sensor/ — 传感器

### DeviceBmi088

```cpp
// board_cfg.cpp（BspSpiDevice / DeviceBmi088 的构造不是 constexpr，不能写成 inline）
BspSpiDevice bmi088_acc_spi(bsp_spi2, {ACC_CS_GPIO_Port, ACC_CS_Pin, 0x80U, 1U});    // 读标志 0x80，加速度计多一个空字节
BspSpiDevice bmi088_gyro_spi(bsp_spi2, {GYRO_CS_GPIO_Port, GYRO_CS_Pin, 0x80U, 0U});
DeviceBmi088 imu(bmi088_acc_spi, bmi088_gyro_spi, {DeviceBmi088::AccRange::G24, DeviceBmi088::GyroRange::DPS2000});
```

- `init()` 做自检与量程配置（加速度计和陀螺仪是两条独立片选）
- `update(timeout_ms)` 读原始数据并跑 VQF，`snapshot(ImuData&, Bmi088RawData*)` 取一致的一份快照
- 诊断计数：`get_acc_saturation_count()`、`get_gyro_saturation_count()`、`get_update_fail_count()`、`get_attitude_skip_count()`
- `is_attitude_ready()`：姿态解算过至少一帧才为真；之前 `ImuData` 的四元数是单位四元数（不是全 0），姿态控制要先等它（评审 F1）
- 运行中重新 `init()` 与内部刷新任务互斥（刷新锁），不会和正在解算的那一帧并发改 VQF / 芯片（评审 F8）
- 调度器启动前的等待用 DWT 忙等，不依赖 HAL 时基中断的优先级（评审 A3）
- 自动模式只按陀螺仪节奏刷新，加速度计 INT1 只计数：不需要时宿主可以不转调、或者不开对应 EXTI
- `attach_task(handle)` 可绑定任务句柄，让中断/错误能唤醒该任务

### DeviceWt102T&lt;Uart&gt;

```cpp
DeviceWt102T<decltype(bsp_uart7)> gyro_z(bsp_uart7);   // board_cfg.cpp；串口在 CubeMX 里配 230400 8N1
```

- `init()` 挂进默认树并兜底校验串口帧格式（不是 230400 8N1 返回 `BAD_ARG`）；可重复调用，会清掉数据和诊断计数
- `update(timeout_ms)` 把串口里攒的字节全部解完；设备 500 Hz 发帧，**要在某个任务里周期调用**（不建任务、不注册 Online）
- `get_z_newest()` / `get_z_previous()` 取最近两帧的 Z 轴原始 int16（`acc_z` / `gyro_z` / `yaw`），单位换算：
  `wz = gyro_z / 32768 × 2000 °/s`、`yaw = yaw / 32768 × 180 °`、`az = acc_z / 32768 × 16 g`
- `is_online()` 按 tick 现算（50 ms 没有校验通过的帧就离线）；`get_diag()` 区分“没字节 / 切不出帧 / 校验错”
- 不是线程安全的：`update()` 和读数据要在同一个任务里

---

## 4. remote/ — 遥控接收机

### DeviceR9dsT&lt;Uart&gt;

```cpp
inline DeviceR9dsT<decltype(bsp_uart3)> rc(bsp_uart3);
```

- `init()` 挂进默认树，并兜底校验串口帧格式（SBUS：100000 bit/s、8E2，不对返回 `BAD_ARG`）；SBUS 是反相电平，要么在 CubeMX 里开 RX 引脚反相（Advanced Features），要么板上有硬件反相器
- `update(timeout_ms)` 把串口里攒的字节解成通道值，要在某个任务里周期调用（本类不建任务）；控制逻辑读 `get_rc()`（摇杆 / 旋钮 −100 ~ +100，开关给挡位）
- `is_online()` 同时看三条：帧超时、失控保护位、连续丢帧，失控保护时怎么处置由上层决定；单看 `is_frame_lost()` / `is_failsafe()` 也可以
- `update()` 在别的任务里跑时用 `snapshot(Rc &, uint16_t *raw)` 读（评审 F5）；诊断计数 `byte_cnt` / `frame_cnt` 是 `uint32_t`（N34）

---

## 5. actuator/ — 执行器

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

### DeviceBuzzer

```cpp
inline DeviceBuzzer buzzer(bsp_pwm_buzzer);
buzzer.beep(2000, 100);   // 2 kHz 响 100 ms
buzzer.tone(1000, 50);    // 持续音，音量 50%
```

---

## 6. 默认树里的设备节点（Q6）

挂在总线上的设备各有一个 EventState 节点，在自己的 `init()` 里挂到所用总线的节点下（`get_state()` 读取）：

| 设备 | 挂在 | 记什么 |
| --- | --- | --- |
| `DjiMotor` | CAN 总线节点 | 初始化失败；掉线（Online 由在线变离线）→ `TIMEOUT` |
| `DmMotor` | CAN 总线节点 | 初始化失败；掉线 → `TIMEOUT`；反馈进入故障态 → `IO_ERROR` |
| `DeviceEmmv5GroupT` | 串口总线节点 | 初始化失败；任一台电机掉线 → `TIMEOUT` |
| `DeviceR9dsT` | 串口总线节点 | 初始化失败；`update()` 发现由在线变离线 → `TIMEOUT` |
| `DeviceWt102T` | 串口总线节点 | 初始化失败；`update()` 发现由在线变离线 → `TIMEOUT` |
| `DeviceBmi088` | SPI 总线节点 | 初始化失败；读帧失败 → 读帧返回的错误码 |

`DeviceMotor`、`DeviceServo`、`DeviceBuzzer` 不在总线上（PWM / 编码器），没有节点。

---

## 7. 常见坑

| 现象 | 原因 |
| --- | --- |
| `DjiMotor::init()` 返回 `BAD_ARG` | 电调 ID 超范围：M3508 / M2006 是 1~8，GM6020 是 1~7（构造时检查） |
| GM6020 橙灯常亮、不转 | 电调在电流模式，却发了电压指令（0x1FF / 0x2FF）；构造时传 `DjiMotorControlMode::CURRENT`（发 0x1FE / 0x2FE） |
| 达妙电机被“别人的帧”带动 | 驱动器只看帧 ID 低 8 位，和 DJI 电机同总线时 `can_id` 撞上了 0x01~0x0B 等（见上） |
| 电机抖动不动 | `DeviceMotor` 的 `encoder_multiplier` 与 CubeMX 编码器模式不匹配（TI12 = 4，TI1 = 2） |
| 电机低速嗡嗡响 | `deadband_cm_s` 太小，静止时的 ±1 计数被 P 放大成占空比抖动 |
| 多电机只动一个 | 同总线两台同 ID（DJI 电调拨码 / 达妙 can_id、master_id） |
| Emm_V5 偶发丢指令 | 多任务共用 485 却没 `lock_bus()` |
| BMI088 读数乱 / 芯片 ID 不对 | `BspSpiDevice` 的 `read_dummy` 配反（加速度计读寄存器多一个空字节 = 1，陀螺仪 = 0），或 CubeMX 没开 `MasterKeepIOState` |
| 波形发不出去 | `get_pid_frame()` 只组帧；要在 App 里调 `bsp_uart1.send()` |
| Emm_V5 不响应 | 地址与驱动器设置不一致，或校验方式和驱动器菜单（Checksum）不一致 |
| Emm_V5 偶发丢回复 | 用「读一次就解析」而不是扫描器，遇到半帧就丢 |
| Emm_V5 位置值跳变 | `PositionScanner` 的 `addr` 传错，把别的电机回复当成自己的 |
| Emm_V5 多任务抢 485 | 扫描器状态被两个任务共享；应在 `DeviceEmmv5GroupT` 上 `lock_bus()` |
