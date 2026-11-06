/**
 * @file dm_motor.hpp
 * @brief 达妙（DAMIAO）CAN 电机：MIT / 位置速度 / 速度模式，使能、失能、清错误、设零点，反馈解包（阶段 2.9）
 *
 * @details 协议来源：《DM-J4310-2EC V1.2》《DM-J4340P-2EC V1.2 / V1.1》《DM-J8009P-2EC V1.3 / V1.1》说明书的“CAN 通信”一章
 *          （库里 docs/sheet/），映射函数照 J4340P 说明书给出的 float_to_uint / uint_to_float（截断取整）。
 *
 *   **型号**：三个系列的 CAN 协议逐项相同（帧 ID、帧格式、映射、命令、ERR 状态码），不同的只是数值，所以不做成按型号的模板
 *   （DJI 用模板是因为不同型号的反馈 / 控制帧 ID、ID 范围本身就不同）。型号经 `Config::model` 给出，用来查
 *   `dm_model_info()` 的减速比 / 额定与峰值扭矩 / 空载转速，并把 MIT 前馈扭矩限在峰值扭矩以内。
 *   映射范围 P_MAX / V_MAX / T_MAX 是驱动器里**可改的寄存器**（实测 J4340P 是 12.5 / 10 / 28，不是说明书写的预设 12.5 / 30 / 10），
 *   不能从型号推出来：要么在 Config 里填和调试助手一致的值，要么填 0、启动后调 `sync_ranges()` 从驱动器读。
 *
 *   | 帧 | 帧 ID | 数据 |
 *   | --- | --- | --- |
 *   | MIT 控制 | `can_id` | p_des 16 位、v_des / Kp / Kd / t_ff 各 12 位，按位拼进 8 字节 |
 *   | 位置速度控制 | `0x100 + can_id` | p_des、v_des 两个 float，低字节在前 |
 *   | 速度控制 | `0x200 + can_id` | v_des 一个 float（4 字节） |
 *   | 使能 / 失能 / 保存零点 / 清错误 | 当前模式的控制帧 ID | `FF ×7` + `FC` / `FD` / `FE` / `FB` |
 *   | 读 / 写寄存器 | `0x7FF` | `CANID_L CANID_H 0x33 RID`（读）/ `CANID_L CANID_H 0x55 RID` + 4 字节数据（写），回复发到 master_id |
 *   | 反馈 | `master_id`（调试助手里的 Master ID） | `ID|ERR<<4`、POS 16 位、VEL 12 位、T 12 位、T_MOS、T_Rotor |
 *
 *   位置 / 速度 / 扭矩按 [−P_MAX, P_MAX]、[−V_MAX, V_MAX]、[−T_MAX, T_MAX] 线性映射成无符号定点数，
 *   Kp 范围 [0, 500]、Kd 范围 [0, 5]（说明书原文）。P_MAX / V_MAX / T_MAX 必须和驱动器里的一致，否则指令和反馈按比例缩放。
 *
 * @note 用法（实例写在 board_cfg.hpp，init() 写在 board_init()）：
 *
 *   inline DmMotor<decltype(bsp_can2)> yaw_motor(bsp_can2, {.model = DmModel::J4340P, .can_id = 0x02, .master_id = 0x12,
 *                                                         .mode = DmMode::MIT});      // 映射范围留 0：启动后从驱动器读
 *   ICBSP_CHECK(yaw_motor.init() == Status::OK);              // board_init() 里，system_start() 之前
 *   yaw_motor.sync_ranges();                                   // 任务里、system_start() 之后：读 PMAX / VMAX / TMAX / 控制模式
 *   yaw_motor.enable();                                        // 上电后电机是失能态
 *   yaw_motor.set_mit(0.0f, 0.0f, 0.0f, 1.0f, 0.0f);           // 控制任务里每个周期发一次
 *   DmMotorFeedback fb; yaw_motor.snapshot(fb);                 // 跨任务读反馈
 *
 * @warning **控制帧不经过 CanTxNodeT**：发送节点的槽位过期会被清零，而 MIT 帧全 0 映射出来是 p = −P_MAX、t = −T_MAX，
 *          等于给满负扭矩。控制帧由本类直接 `send_latest()` 发；**调用方要按控制周期持续调用 set_*()**，
 *          停发后由电机自己的“通讯丢失”保护（调试助手里设的超时）退出使能。
 * @note 反馈是问询式的：电机只在收到发给自己的帧时回一帧反馈，所以不发控制帧时 is_online() 会变成 false。
 * @note 每台电机的 master_id 要不同（反馈帧按 master_id 分发到接收节点）；反馈里的 ID 只有低 4 位
 *       （高 4 位是 ERR），所以同一 master_id 下靠 can_id 低 4 位区分，本类只认 `can_id & 0x0F` 对得上的帧。
 * @warning **ID 冲突**：驱动器只核对帧 ID 的**低 8 位**（说明书原文）。同一总线上任何帧的 ID 低 8 位等于 can_id，电机都会回一帧反馈；
 *          帧 ID 恰好等于当前模式的控制帧 ID 时还会被当成控制指令。实测（2026-11-06）：CAN2 上 GM6020 ID 1 的反馈 0x205
 *          正好是 CAN_ID 0x05 的 DM 电机在速度模式下的控制帧 ID，DM 每收到一帧 6020 反馈就回一帧（约 1 kHz），
 *          使能后会把 6020 的编码器 / 转速字节当成速度给定。与 DJI 电机同总线时，can_id 的低 8 位要避开
 *          0x01~0x0B（DJI 反馈 0x201~0x20B）和 0xFE / 0xFF / 0x00（DJI 控制帧 0x1FE / 0x1FF / 0x200 等），例如用 0x10 以上。
 * @note 默认树（Q6）：本电机的节点挂在所用 CAN 的总线节点下；掉线记 TIMEOUT，反馈进入故障态（ERR 不是 0 / 1）记 IO_ERROR。
 * @note **只编译验证，未上板**（本工程没有达妙电机）。
 */

#ifndef __DM_MOTOR_HPP__
#define __DM_MOTOR_HPP__

#include "bsp/bsp_series.hpp" // IWYU pragma: keep（HAL 模块开关 / 系列桥接）
#include "FreeRTOS.h"         // IWYU pragma: keep
#include "task.h"             // taskENTER_CRITICAL（snapshot / 反馈写回）
#include "service/system/config_check.hpp" // 宿主配置（经检查）

// 依赖（Q4）：反馈要靠接收节点分发，CubeMX 开了 CAN 却关了接收路径时，用到 DmMotor 就直接报错
#if defined(HAL_FDCAN_MODULE_ENABLED) && !ICBSP_CAN_RX_ENABLE
#  error "DmMotor 需要 CAN 接收路径：hardware_config.h 里 ICBSP_CAN_RX_ENABLE 要为 1（控制帧直接发，不需要 ICBSP_CAN_TX_ENABLE）"
#endif

#if defined(HAL_FDCAN_MODULE_ENABLED) && ICBSP_CAN_RX_ENABLE

#include "bsp/bsp_can.hpp"
#include "service/comm/can_rx_node.hpp"
#include "service/monitor/online_check.hpp"
#include "service/system/state_tree.hpp" // bus_state()：默认树的总线节点
#include "service/system/status.hpp"
#include "service/system/system.hpp"

#include <cmath>
#include <cstdlib>
#include <stdint.h>
#include <string.h>

/** @brief 控制模式（驱动器 CTRL_MODE 寄存器 0x0A 的取值；力位混控 4 暂不支持） */
enum class DmMode : uint8_t
{
  MIT     = 1U, ///< MIT：p_des / v_des / Kp / Kd / t_ff
  POS_VEL = 2U, ///< 位置速度：梯形加减速到 p_des，v_des 为匀速段速度
  VEL     = 3U, ///< 速度
};

/** @brief 反馈帧里的状态（ERR 字段） */
enum class DmState : uint8_t
{
  DISABLED       = 0x0U, ///< 失能
  ENABLED        = 0x1U, ///< 使能
  SHAFT_CAL_ERR  = 0x3U, ///< 输出轴校准异常（J4340P 说明书）
  SENSOR_ERR     = 0x4U, ///< 传感器输出异常（J4340P 说明书）
  ENCODER_CAL_ERR = 0x5U, ///< 电机编码器校准异常（J4340P 说明书）
  OVER_VOLTAGE   = 0x8U, ///< 超压
  UNDER_VOLTAGE  = 0x9U, ///< 欠压
  OVER_CURRENT   = 0xAU, ///< 过电流
  MOS_OVER_TEMP  = 0xBU, ///< MOS 过温
  COIL_OVER_TEMP = 0xCU, ///< 电机线圈过温
  COMM_LOST      = 0xDU, ///< 通讯丢失
  OVERLOAD       = 0xEU, ///< 过载
};

/**
 * @brief 达妙电机型号（只决定 dm_model_info() 的数值，协议都一样）
 * @note J4310P 仓库里没有说明书，暂按 J4310 V1.2 的数值；P 型号的参数以厂家资料为准。
 */
enum class DmModel : uint8_t
{
  UNKNOWN = 0, ///< 不限型号：不按峰值扭矩限幅，dm_model_info() 全 0
  J4310,       ///< DM-J4310-2EC V1.2（也用于 J4310P，见上）
  J4340P,      ///< DM-J4340P-2EC（说明书 V1.2）
  J4340P_V11,  ///< DM-J4340P-2EC V1.1（说明书 V1.4，峰值扭矩更大）
  J8009P,      ///< DM-J8009P-2EC 与 V1.1（两版参数相同，只差编码器位数）
};

/** @brief 型号的说明书参数（24 V 版本，48 V 版本另列空载转速） */
struct DmModelInfo
{
  const char *name;           ///< 型号名
  float       gear_ratio;     ///< 减速比（输出轴 : 电机轴 = 1 : gear_ratio）
  float       rated_torque;   ///< 额定扭矩 (N·m)
  float       peak_torque;    ///< 峰值扭矩 (N·m)，MIT 前馈扭矩按它限幅
  float       no_load_rpm_24; ///< 24 V 空载最高转速 (rpm，输出轴)
  float       no_load_rpm_48; ///< 48 V 空载最高转速 (rpm，输出轴)
};

/** @brief 查型号参数（数值抄自各型号说明书的“特征参数”表） */
constexpr DmModelInfo dm_model_info(DmModel model)
{
  switch (model)
  {
    case DmModel::J4310:
      return {"DM-J4310-2EC", 10.0f, 3.5f, 12.5f, 200.0f, 450.0f};
    case DmModel::J4340P:
      return {"DM-J4340P-2EC", 40.0f, 9.0f, 27.0f, 52.5f, 100.0f};
    case DmModel::J4340P_V11:
      return {"DM-J4340P-2EC V1.1", 40.0f, 12.0f, 40.0f, 56.0f, 112.0f};
    case DmModel::J8009P:
      return {"DM-J8009P-2EC", 9.0f, 20.0f, 40.0f, 168.0f, 335.0f};
    case DmModel::UNKNOWN:
    default:
      return {"unknown", 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  }
}

/** @brief 一帧反馈解出来的数据 */
struct DmMotorFeedback
{
  bool     valid    = false; ///< 是否收到过有效反馈
  DmState  state    = DmState::DISABLED;
  float    position = 0.0f; ///< 输出轴位置 (rad)，范围 [−P_MAX, P_MAX]
  float    velocity = 0.0f; ///< 输出轴速度 (rad/s)
  float    torque   = 0.0f; ///< 扭矩 (N·m)
  uint8_t  t_mos    = 0U;   ///< 驱动 MOS 平均温度 (℃)
  uint8_t  t_rotor  = 0U;   ///< 电机线圈平均温度 (℃)
  uint16_t raw_pos  = 0U;   ///< 原始定点值（16 位）
  uint16_t raw_vel  = 0U;   ///< 原始定点值（12 位）
  uint16_t raw_tor  = 0U;   ///< 原始定点值（12 位）
};

namespace dm
{
constexpr float KP_MIN = 0.0f;   ///< MIT Kp 下限（说明书）
constexpr float KP_MAX = 500.0f; ///< MIT Kp 上限（说明书）
constexpr float KD_MIN = 0.0f;   ///< MIT Kd 下限
constexpr float KD_MAX = 5.0f;   ///< MIT Kd 上限

constexpr uint32_t PARAM_ID      = 0x7FFU; ///< 读写寄存器的帧 ID
constexpr uint8_t  CMD_READ_REG  = 0x33U;  ///< 读寄存器
constexpr uint8_t  CMD_WRITE_REG = 0x55U;  ///< 写寄存器
constexpr uint8_t  RID_MST_ID    = 0x07U;  ///< 反馈 ID（uint32）
constexpr uint8_t  RID_ESC_ID    = 0x08U;  ///< 接收 ID（uint32）
constexpr uint8_t  RID_CTRL_MODE = 0x0AU;  ///< 控制模式（uint32）
constexpr uint8_t  RID_PMAX      = 0x15U;  ///< 位置映射范围（float）
constexpr uint8_t  RID_VMAX      = 0x16U;  ///< 速度映射范围（float）
constexpr uint8_t  RID_TMAX      = 0x17U;  ///< 扭矩映射范围（float）

/** @brief 寄存器原始值（低字节在前的 4 字节）当 float 解释 */
inline float reg_as_float(uint32_t raw)
{
  float f = 0.0f;
  memcpy(&f, &raw, sizeof(f));
  return f;
}

/** @brief 浮点 → 定点：先限幅到 [x_min, x_max]，再按说明书的 float_to_uint 线性映射到 [0, 2^bits − 1]（截断取整，与驱动器一致） */
inline uint16_t float_to_uint(float x, float x_min, float x_max, uint8_t bits)
{
  const float top = static_cast<float>((1U << bits) - 1U);
  x               = (x < x_min) ? x_min : ((x > x_max) ? x_max : x);
  return static_cast<uint16_t>((x - x_min) * top / (x_max - x_min));
}

/** @brief 定点 → 浮点：float_to_uint() 的逆映射 */
inline float uint_to_float(uint32_t v, float x_min, float x_max, uint8_t bits)
{
  const float top = static_cast<float>((1U << bits) - 1U);
  return static_cast<float>(v) * (x_max - x_min) / top + x_min;
}

/** @brief float 按小端写进 4 字节 */
inline void put_f32_le(uint8_t *out, float v)
{
  uint32_t u = 0U;
  memcpy(&u, &v, sizeof(u));
  out[0] = static_cast<uint8_t>(u);
  out[1] = static_cast<uint8_t>(u >> 8);
  out[2] = static_cast<uint8_t>(u >> 16);
  out[3] = static_cast<uint8_t>(u >> 24);
}
} // namespace dm

/**
 * @brief 达妙 CAN 电机
 * @tparam CanT 所在总线的 BspCanT 类型（`decltype(bsp_can1)`）
 * @note 对象不可复制或移动（Online / EventState / 接收节点持有 this）。
 */
template <typename CanT>
class DmMotor
{
public:
  /** @brief 配置（聚合体，用指定初始化器写） */
  struct Config
  {
    DmModel  model             = DmModel::UNKNOWN; ///< 型号（查 dm_model_info()；UNKNOWN = 不按峰值扭矩限幅）
    uint16_t can_id            = 0U;          ///< 电机 CAN ID（1~0xFF；控制帧 ID 由它和模式决定）
    uint16_t master_id         = 0U;          ///< 反馈帧 ID（调试助手里的 Master ID，每台不同，不能是 0x7FF）
    DmMode   mode              = DmMode::MIT; ///< 驱动器当前的控制模式（决定控制帧 ID；sync_ranges() 会按驱动器的值改；要改驱动器用 switch_mode()）
    float    p_max             = 0.0f;        ///< 位置映射范围 P_MAX（rad），与驱动器一致；0 = 启动后用 sync_ranges() 从驱动器读
    float    v_max             = 0.0f;        ///< 速度映射范围 V_MAX（rad/s）；0 = 同上
    float    t_max             = 0.0f;        ///< 扭矩映射范围 T_MAX（N·m）；0 = 同上
    uint16_t online_timeout_ms = 30U;         ///< 多久收不到反馈判离线 (ms)
  };

  /** @brief 运行诊断（只增不减） */
  struct Diag
  {
    uint32_t feedback_cnt;  ///< 有效反馈帧数
    uint32_t reject_cnt;    ///< 发到 master_id 但不当反馈解的帧数（ID 低 4 位不符 / 长度不对 / 不是本对象发起的寄存器回复）
    uint32_t reg_reply_cnt; ///< 收到的写寄存器回复数
    uint32_t send_busy_cnt; ///< send_latest() 因同 ID 上一帧还没发出而丢掉的控制帧数
    uint32_t fault_cnt;     ///< 反馈进入故障态（ERR 不是 0 / 1）的次数
    uint32_t range_mismatch; ///< sync_ranges() 发现 Config 里填的映射范围 / 模式和驱动器不一致的次数（之后以驱动器为准）
  };

  DmMotor(CanT &can, const Config &cfg) :
    _can(can), _cfg(cfg), _mode(cfg.mode), _p_max(cfg.p_max), _v_max(cfg.v_max), _t_max(cfg.t_max), _online(cfg.online_timeout_ms)
  {
  }
  /** @brief 注销反馈接收节点（运行态冻结后注销会失败：此时继续析构会留下悬空回调，直接 abort，同 DjiMotor） */
  ~DmMotor()
  {
    if (_rx_node != nullptr && CanRxNodeT<CanT>::unregist(_can, _rx_node) != Status::OK)
    {
      std::abort();
    }
  }

  DmMotor(const DmMotor &)            = delete;
  DmMotor &operator=(const DmMotor &) = delete;

  /**
   * @brief 校验配置、注册反馈接收节点、挂到默认树
   * @return OK；BAD_ARG = 配置非法；FULL / BUSY = 接收节点注册失败（原因已记在 sys_state）；
   *         NOT_INIT = system_init() 还没调；NOT_SUPPORTED = 系统已在运行（接收节点只能在初始化期注册）
   * @note 只在初始化期调用（board_init()）；重复调用成功后无副作用。
   */
  Status init();

  /** @brief 使能（上电后电机处于失能态，必须先使能才能控制） */
  Status enable() { return send_command(0xFCU); }
  /** @brief 失能 */
  Status disable() { return send_command(0xFDU); }
  /** @brief 把当前输出轴位置设为零点（位置给定同时清零） */
  Status save_zero() { return send_command(0xFEU); }
  /** @brief 清除错误（过热等） */
  Status clear_error() { return send_command(0xFBU); }

  /**
   * @brief MIT 模式控制
   * @param p 位置 (rad)  @param v 速度 (rad/s)  @param kp [0, 500]  @param kd [0, 5]  @param t 前馈扭矩 (N·m)
   * @return OK；BAD_ARG = 有非有限值（不发）；NOT_SUPPORTED = 当前不是 MIT 模式；BUSY = 上一帧还没发出（本帧丢弃）；其余同 BspCanT::send()
   * @note 超出映射范围的值限幅后再发。位置控制时 kd 不能为 0（说明书：会震荡甚至失控）。
   */
  Status set_mit(float p, float v, float kp, float kd, float t);

  /** @brief 位置速度模式控制：p 目标位置 (rad)，v 匀速段速度 (rad/s) */
  Status set_pos_vel(float p, float v);

  /** @brief 速度模式控制：v 目标速度 (rad/s) */
  Status set_vel(float v);

  /**
   * @brief 经 CAN 写 CTRL_MODE 寄存器切换控制模式（不存 flash，掉电恢复成上次保存的模式）
   * @note 说明书：切换时电机清零位置 / 速度 / 扭矩 / Kp / Kd 指令，建议零速时切换。发出去之后本类就按新模式选帧 ID。
   */
  Status switch_mode(DmMode mode);

  /**
   * @brief 读一个寄存器（阻塞等回复，只能在任务里、system_start() 之后调用：回复要靠 can_rx_task 分发）
   * @param rid        寄存器地址（dm::RID_*，见说明书“寄存器列表及范围”）
   * @param raw        回复里的 4 字节数据（低字节在前）；float 寄存器用 dm::reg_as_float() 转
   * @param timeout_ms 最多等多久
   * @return OK；TIMEOUT = 没收到回复（ID 不对、电机没上电、总线不通）；BUSY = 上一次寄存器操作还没完；其余同 send()
   * @note 不需要使能，读寄存器不会让电机动作——用来确认通信、核对 PMAX / VMAX / TMAX / 控制模式。
   */
  Status read_register(uint8_t rid, uint32_t &raw, uint32_t timeout_ms = 20U);

  /** @brief 当前按哪个模式发控制帧 */
  DmMode get_mode() const { return _mode; }

  /** @brief 实时反馈（can_rx_task 随时会改；跨任务读用 snapshot()） */
  const DmMotorFeedback &get_feedback() const { return _fb; }

  /** @brief 在临界区里拷一份同一帧的反馈 */
  void snapshot(DmMotorFeedback &out) const
  {
    taskENTER_CRITICAL();
    out = _fb;
    taskEXIT_CRITICAL();
  }

  /** @brief 是否在线（收到反馈后 online_timeout_ms 内） */
  bool is_online() const { return _online.is_online(); }
  /** @brief 在线检查对象 */
  const Online &get_online() const { return _online; }
  /** @brief 本电机在 EventState 默认树里的节点（第三层） */
  const EventState &get_state() const { return _state; }
  /** @brief 初始化结果：OK = init() 成功，NOT_INIT = 还没 init，其余为失败原因 */
  Status get_status() const { return _statu; }
  /** @brief 诊断计数 */
  Diag get_diag() const { return {_feedback_cnt, _reject_cnt, _reg_reply_cnt, _send_busy_cnt, _fault_cnt, _range_mismatch}; }

  /**
   * @brief 从驱动器读 PMAX / VMAX / TMAX / CTRL_MODE，以驱动器的值为准更新本对象（只能在任务里、system_start() 之后调用）
   * @return OK；其余同 read_register()（读失败时保持原来的值）
   * @note Config 里填了非 0 的映射范围、或模式和驱动器不一致时，计入 Diag::range_mismatch，并在默认树节点上记 BAD_ARG。
   * @note 不需要使能，不会让电机动作。CTRL_MODE 是力位混控（4）等本类不支持的模式时返回 NOT_SUPPORTED，映射范围照常更新。
   */
  Status sync_ranges(uint32_t timeout_ms = 20U);

  /** @brief 映射范围是否已知（Config 填了，或 sync_ranges() 成功过）；未知时 set_*() 返回 NOT_INIT、反馈只有原始值 */
  bool has_ranges() const { return _p_max > 0.0f && _v_max > 0.0f && _t_max > 0.0f; }
  /** @brief 当前使用的映射范围 */
  float get_p_max() const { return _p_max; }
  float get_v_max() const { return _v_max; }
  float get_t_max() const { return _t_max; }
  /** @brief 型号参数（Config::model） */
  DmModelInfo get_model_info() const { return dm_model_info(_cfg.model); }

  /** @brief 解一帧发到 master_id 的报文（接收节点回调；不属于本电机返回 BAD_ARG，帧进回退缓冲） */
  Status unpack(const CanRxMsg &rx);

private:
  uint32_t control_id() const
  {
    switch (_mode)
    {
      case DmMode::POS_VEL:
        return 0x100U + _cfg.can_id;
      case DmMode::VEL:
        return 0x200U + _cfg.can_id;
      case DmMode::MIT:
      default:
        return _cfg.can_id;
    }
  }

  Status send_command(uint8_t last)
  {
    if (_statu != Status::OK)
    {
      return Status::NOT_INIT;
    }
    const uint8_t d[8] = {0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, last};
    return _can.send(control_id(), d, 8U); // 命令帧排队发，不被同 ID 的控制帧挤掉
  }

  Status send_control(const uint8_t *d, uint8_t dlc)
  {
    const Status st = _can.send_latest(control_id(), d, dlc);
    if (st == Status::BUSY)
    {
      _send_busy_cnt = _send_busy_cnt + 1U;
    }
    return st;
  }

  static Status rx_callback(void *context, const CanRxMsg &rx) { return static_cast<DmMotor *>(context)->unpack(rx); }

  CanT                  &_can;
  Config                 _cfg;
  DmMode                 _mode;
  volatile float         _p_max;  ///< 当前映射范围（Config 或 sync_ranges()；任务写、can_rx_task 读，32 位读写原子）
  volatile float         _v_max;
  volatile float         _t_max;
  Status                 _statu   = Status::NOT_INIT;
  CanRxNodeT<CanT>      *_rx_node = nullptr;
  Online                 _online;
  EventState             _state;
  DmMotorFeedback        _fb;
  volatile bool          _reg_pending = false; ///< 发过读 / 写寄存器、回复还没到（回复也走 master_id，要和反馈区分开）
  volatile uint8_t       _reg_cmd     = 0U;     ///< 挂起的是读（0x33）还是写（0x55）
  volatile uint8_t       _reg_rid     = 0U;
  volatile uint32_t      _reg_value   = 0U;     ///< 最近一次寄存器回复的数据
  volatile uint32_t      _feedback_cnt  = 0U;
  volatile uint32_t      _reject_cnt    = 0U;
  volatile uint32_t      _reg_reply_cnt = 0U;
  volatile uint32_t      _send_busy_cnt = 0U;
  volatile uint32_t      _fault_cnt     = 0U;
  volatile uint32_t      _range_mismatch = 0U;
};

/* ==================== 成员函数定义（只有头文件） ==================== */

template <typename CanT>
Status DmMotor<CanT>::init()
{
  if (!sys_state.is_ready())
  {
    return Status::NOT_INIT;
  }
  if (sys_state.is_running())
  {
    return Status::NOT_SUPPORTED; // 接收节点只能在初始化期注册
  }
  if (_statu == Status::OK)
  {
    return Status::OK;
  }

  // 默认树第三层（Q6）：挂在所用 CAN 实例的总线节点下（实例不在清单里时挂 sys_state）
  if (_state.init() != Status::OK)
  {
    (void)sys_state.init_error(Status::FULL);
    return Status::FULL;
  }
  EventState *bus = bus_state(&_can);
  (void)_state.set_parent((bus != nullptr) ? *bus : sys_state);

  // 映射范围：0 = 启动后 sync_ranges() 再读；填了就必须是正的有限值
  const bool range_ok = std::isfinite(_cfg.p_max) && std::isfinite(_cfg.v_max) && std::isfinite(_cfg.t_max) && _cfg.p_max >= 0.0f &&
                        _cfg.v_max >= 0.0f && _cfg.t_max >= 0.0f;
  const bool mode_ok  = _cfg.mode == DmMode::MIT || _cfg.mode == DmMode::POS_VEL || _cfg.mode == DmMode::VEL;
  if (!range_ok || !mode_ok || _cfg.can_id == 0U || _cfg.can_id > 0xFFU || _cfg.master_id > 0x7FFU || _cfg.master_id == dm::PARAM_ID)
  {
    _statu = Status::BAD_ARG;
    (void)_state.init_error(_statu);
    return _statu;
  }

  _rx_node = CanRxNodeT<CanT>::regist(_can, _cfg.master_id, &DmMotor::rx_callback, this);
  if (_rx_node == nullptr)
  {
    _statu = Status::BUSY; // 注册器已在 sys_state 上记了具体原因（ID 冲突 / 节点数满）
    (void)_state.init_error(_statu);
    return _statu;
  }

  _online.set_state(&_state); // 由在线变离线时在本节点记 TIMEOUT
  (void)_state.complete_init();
  _statu = Status::OK;
  return _statu;
}

template <typename CanT>
Status DmMotor<CanT>::set_mit(float p, float v, float kp, float kd, float t)
{
  if (_statu != Status::OK)
  {
    return Status::NOT_INIT;
  }
  if (_mode != DmMode::MIT)
  {
    return Status::NOT_SUPPORTED;
  }
  if (!std::isfinite(p) || !std::isfinite(v) || !std::isfinite(kp) || !std::isfinite(kd) || !std::isfinite(t))
  {
    return Status::BAD_ARG; // 不把 NaN 映射成某个极值发出去
  }
  if (!has_ranges())
  {
    return Status::NOT_INIT; // 映射范围未知：调 sync_ranges() 或在 Config 里填
  }
  const float peak = dm_model_info(_cfg.model).peak_torque;
  if (peak > 0.0f)
  {
    t = (t > peak) ? peak : ((t < -peak) ? -peak : t); // 型号的峰值扭矩以内（映射范围可能被设得比它大）
  }
  const uint16_t pu  = dm::float_to_uint(p, -_p_max, _p_max, 16U);
  const uint16_t vu  = dm::float_to_uint(v, -_v_max, _v_max, 12U);
  const uint16_t kpu = dm::float_to_uint(kp, dm::KP_MIN, dm::KP_MAX, 12U);
  const uint16_t kdu = dm::float_to_uint(kd, dm::KD_MIN, dm::KD_MAX, 12U);
  const uint16_t tu  = dm::float_to_uint(t, -_t_max, _t_max, 12U);
  const uint8_t  d[8] = {static_cast<uint8_t>(pu >> 8),
                         static_cast<uint8_t>(pu),
                         static_cast<uint8_t>(vu >> 4),
                         static_cast<uint8_t>(((vu & 0x0FU) << 4) | (kpu >> 8)),
                         static_cast<uint8_t>(kpu),
                         static_cast<uint8_t>(kdu >> 4),
                         static_cast<uint8_t>(((kdu & 0x0FU) << 4) | (tu >> 8)),
                         static_cast<uint8_t>(tu)};
  return send_control(d, 8U);
}

template <typename CanT>
Status DmMotor<CanT>::set_pos_vel(float p, float v)
{
  if (_statu != Status::OK)
  {
    return Status::NOT_INIT;
  }
  if (_mode != DmMode::POS_VEL)
  {
    return Status::NOT_SUPPORTED;
  }
  if (!std::isfinite(p) || !std::isfinite(v))
  {
    return Status::BAD_ARG;
  }
  uint8_t d[8];
  dm::put_f32_le(d, p);
  dm::put_f32_le(d + 4, v);
  return send_control(d, 8U);
}

template <typename CanT>
Status DmMotor<CanT>::set_vel(float v)
{
  if (_statu != Status::OK)
  {
    return Status::NOT_INIT;
  }
  if (_mode != DmMode::VEL)
  {
    return Status::NOT_SUPPORTED;
  }
  if (!std::isfinite(v))
  {
    return Status::BAD_ARG;
  }
  uint8_t d[4];
  dm::put_f32_le(d, v);
  return send_control(d, 4U);
}

template <typename CanT>
Status DmMotor<CanT>::switch_mode(DmMode mode)
{
  if (_statu != Status::OK)
  {
    return Status::NOT_INIT;
  }
  if (mode != DmMode::MIT && mode != DmMode::POS_VEL && mode != DmMode::VEL)
  {
    return Status::BAD_ARG;
  }
  uint8_t d[8] = {static_cast<uint8_t>(_cfg.can_id), static_cast<uint8_t>(_cfg.can_id >> 8), dm::CMD_WRITE_REG, dm::RID_CTRL_MODE,
                  static_cast<uint8_t>(mode), 0U, 0U, 0U}; // uint32，低字节在 D4
  if (_reg_pending)
  {
    return Status::BUSY;
  }
  _reg_cmd     = dm::CMD_WRITE_REG;
  _reg_rid     = dm::RID_CTRL_MODE;
  _reg_pending = true;
  const Status st = _can.send(dm::PARAM_ID, d, 8U);
  if (st == Status::OK)
  {
    _mode = mode;
  }
  else
  {
    _reg_pending = false;
  }
  return st;
}

template <typename CanT>
Status DmMotor<CanT>::read_register(uint8_t rid, uint32_t &raw, uint32_t timeout_ms)
{
  if (_statu != Status::OK)
  {
    return Status::NOT_INIT;
  }
  if (_reg_pending)
  {
    return Status::BUSY;
  }
  const uint8_t d[4] = {static_cast<uint8_t>(_cfg.can_id), static_cast<uint8_t>(_cfg.can_id >> 8), dm::CMD_READ_REG, rid};
  _reg_cmd     = dm::CMD_READ_REG;
  _reg_rid     = rid;
  _reg_pending = true;
  const Status st = _can.send(dm::PARAM_ID, d, 4U);
  if (st != Status::OK)
  {
    _reg_pending = false;
    return st;
  }
  const TickType_t t0 = xTaskGetTickCount();
  while (_reg_pending)
  {
    if ((xTaskGetTickCount() - t0) >= pdMS_TO_TICKS(timeout_ms))
    {
      _reg_pending = false;
      return Status::TIMEOUT;
    }
    vTaskDelay(1U);
  }
  raw = _reg_value;
  return Status::OK;
}

template <typename CanT>
Status DmMotor<CanT>::sync_ranges(uint32_t timeout_ms)
{
  uint32_t p = 0U, v = 0U, t = 0U, m = 0U;
  Status   st = read_register(dm::RID_PMAX, p, timeout_ms);
  if (st == Status::OK)
  {
    st = read_register(dm::RID_VMAX, v, timeout_ms);
  }
  if (st == Status::OK)
  {
    st = read_register(dm::RID_TMAX, t, timeout_ms);
  }
  if (st == Status::OK)
  {
    st = read_register(dm::RID_CTRL_MODE, m, timeout_ms);
  }
  if (st != Status::OK)
  {
    return st;
  }
  const float pf = dm::reg_as_float(p);
  const float vf = dm::reg_as_float(v);
  const float tf = dm::reg_as_float(t);
  if (!(pf > 0.0f) || !(vf > 0.0f) || !(tf > 0.0f) || !std::isfinite(pf) || !std::isfinite(vf) || !std::isfinite(tf))
  {
    return Status::IO_ERROR; // 读回来的不是合理的映射范围
  }
  // Config 填了值却和驱动器不一致：说明配置写错了（指令 / 反馈会被按比例缩放），记一笔
  auto differs = [](float cfg, float drv) { return cfg > 0.0f && std::fabs(cfg - drv) > 1e-3f * drv; };
  const bool mode_known = m == static_cast<uint32_t>(DmMode::MIT) || m == static_cast<uint32_t>(DmMode::POS_VEL) ||
                          m == static_cast<uint32_t>(DmMode::VEL);
  if (differs(_cfg.p_max, pf) || differs(_cfg.v_max, vf) || differs(_cfg.t_max, tf) ||
      (mode_known && static_cast<uint32_t>(_cfg.mode) != m))
  {
    _range_mismatch = _range_mismatch + 1U;
    (void)_state.error(Status::BAD_ARG);
  }
  _p_max = pf;
  _v_max = vf;
  _t_max = tf;
  if (!mode_known)
  {
    return Status::NOT_SUPPORTED; // 例如力位混控：本类不发这种控制帧
  }
  _mode = static_cast<DmMode>(m);
  return Status::OK;
}

template <typename CanT>
Status DmMotor<CanT>::unpack(const CanRxMsg &rx)
{
  if (rx.header.IdType != FDCAN_STANDARD_ID || rx.header.RxFrameType != FDCAN_DATA_FRAME || rx.header.DataLength != FDCAN_DLC_BYTES_8)
  {
    _reject_cnt = _reject_cnt + 1U;
    return Status::BAD_ARG;
  }
  const uint8_t *d = rx.data;

  // 读 / 写寄存器的回复也发到 master_id：CANID_L CANID_H 0x33/0x55 RID 数据，按挂起的那次操作对上了就吃掉，不当反馈解
  if (_reg_pending && d[0] == static_cast<uint8_t>(_cfg.can_id) && d[1] == static_cast<uint8_t>(_cfg.can_id >> 8) &&
      d[2] == _reg_cmd && d[3] == _reg_rid)
  {
    _reg_value     = static_cast<uint32_t>(d[4]) | (static_cast<uint32_t>(d[5]) << 8) | (static_cast<uint32_t>(d[6]) << 16) |
                 (static_cast<uint32_t>(d[7]) << 24);
    _reg_pending   = false;
    _reg_reply_cnt = _reg_reply_cnt + 1U;
    return Status::OK;
  }

  // 不是本对象发起的读 / 写 / 存储寄存器的回复（别的代码、上位机发的请求）：同样发到 master_id，不能当反馈解，
  // 否则控制回路里会出现一帧假位置（2026-11-06 实板扫描时发现）。代价：位置恰好落在 −PMAX 附近的极小区间
  // （POS[15:8] == CANID_H 且 POS[7:0] 是这三个值之一，约占量程的 3 / 65536）时丢一帧真反馈
  if (d[0] == static_cast<uint8_t>(_cfg.can_id) && d[1] == static_cast<uint8_t>(_cfg.can_id >> 8) &&
      (d[2] == dm::CMD_READ_REG || d[2] == dm::CMD_WRITE_REG || d[2] == 0xAAU))
  {
    _reject_cnt = _reject_cnt + 1U;
    return Status::OK; // 吃掉，不进回退缓冲
  }

  if ((d[0] & 0x0FU) != (_cfg.can_id & 0x0FU))
  {
    _reject_cnt = _reject_cnt + 1U;
    return Status::BAD_ARG;
  }

  DmMotorFeedback fb;
  fb.valid    = true;
  fb.state    = static_cast<DmState>(d[0] >> 4);
  fb.raw_pos  = static_cast<uint16_t>((d[1] << 8) | d[2]);
  fb.raw_vel  = static_cast<uint16_t>((d[3] << 4) | (d[4] >> 4));
  fb.raw_tor  = static_cast<uint16_t>(((d[4] & 0x0FU) << 8) | d[5]);
  if (has_ranges()) // 映射范围未知时只留原始值
  {
    fb.position = dm::uint_to_float(fb.raw_pos, -_p_max, _p_max, 16U);
    fb.velocity = dm::uint_to_float(fb.raw_vel, -_v_max, _v_max, 12U);
    fb.torque   = dm::uint_to_float(fb.raw_tor, -_t_max, _t_max, 12U);
  }
  fb.t_mos    = d[6];
  fb.t_rotor  = d[7];

  // 由正常态进入故障态（ERR 不是失能 / 使能）时计一次、在默认树节点上记 IO_ERROR
  const bool was_fault = _fb.valid && _fb.state != DmState::DISABLED && _fb.state != DmState::ENABLED;
  const bool fault     = fb.state != DmState::DISABLED && fb.state != DmState::ENABLED;

  taskENTER_CRITICAL();
  _fb = fb;
  taskEXIT_CRITICAL();

  _feedback_cnt = _feedback_cnt + 1U;
  (void)_online.refresh_task();
  if (fault && !was_fault)
  {
    _fault_cnt = _fault_cnt + 1U;
    (void)_state.error(Status::IO_ERROR);
  }
  return Status::OK;
}

#endif // HAL_FDCAN_MODULE_ENABLED && ICBSP_CAN_RX_ENABLE

#endif // __DM_MOTOR_HPP__
