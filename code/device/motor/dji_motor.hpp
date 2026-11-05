/**
 * @file dji_motor.hpp
 * @author ChoseB
 * @brief DJI 电机反馈解析、在线状态和控制帧聚合框架
 * @version 0.1
 * @date 2026-09-01
 *
 * @details init() 将控制帧槽位注册到 CanTxNode，将反馈 ID 注册到 CanRxNode。
 *          can_rx_task 自动调用反馈解包，并在成功后刷新在线状态。
 * @note 构造阶段仅校验参数；成功初始化的对象必须存活至系统停止。
 */
#ifndef __DJI_MOTOR_HPP__
#define __DJI_MOTOR_HPP__

#include "bsp/bsp_series.hpp" // IWYU pragma: keep（HAL 模块开关 / 系列桥接）
#include "FreeRTOS.h" // IWYU pragma: keep
#include "task.h"     // taskENTER_CRITICAL（snapshot / DataUnpack 写回）
#include "service/system/config_check.hpp" // 宿主配置（经检查）

// DjiMotor 同时依赖 CAN 接收和发送路径
// 依赖（Q4）：CubeMX 开了 CAN、却关了收发任一路径时，用到 DjiMotor 就直接报错，而不是静默变成空头文件
#if defined(HAL_FDCAN_MODULE_ENABLED) && !(ICBSP_CAN_RX_ENABLE && ICBSP_CAN_TX_ENABLE)
#  error "DjiMotor 需要 CAN 收发都开：hardware_config.h 里 ICBSP_CAN_RX_ENABLE 与 ICBSP_CAN_TX_ENABLE 都要为 1"
#endif

#if defined(HAL_FDCAN_MODULE_ENABLED) && ICBSP_CAN_RX_ENABLE && ICBSP_CAN_TX_ENABLE

#include "bsp/bsp_can.hpp"
#include "service/definition/motor_definition.hpp"
#include "service/monitor/online_check.hpp"
#include "service/comm/can_tx_node.hpp"
#include "service/comm/can_rx_node.hpp"

#include <stdint.h>
#include "service/system/system.hpp"
#include <string.h>
#include <cmath>
#include <cstdlib>

/** @todo 将以下临时协议常量迁移到按 MotorType 特化的 traits，随后删除宏。 */
// #define K_ECD_TO_ANGLE 0.043945f       ///< 编码器计数到机械角度的临时换算系数，单位：度/count
// #define ECD_RANGE_FOR_3508 8191        ///< M3508 编码器原始值上限（包含 0，共 8192 个计数）
// #define CURRENT_LIMIT_FOR_3508 16000   ///< M3508/C620 软件限制使用的原始电流指令绝对值
// #define ECD_RANGE_FOR_6020 8191        ///< GM6020 编码器原始值上限（包含 0，共 8192 个计数）
// #define CURRENT_LIMIT_FOR_6020 16384   ///< GM6020 电流模式原始控制指令绝对值上限
// #define VOLTAGE_LIMIT_FOR_6020 25000   ///< GM6020 电压模式原始控制指令绝对值上限
// #define ECD_RANGE_FOR_2006 8191        ///< M2006 编码器原始值上限（包含 0，共 8192 个计数）
// #define CURRENT_LIMIT_FOR_2006 10000   ///< M2006/C610 软件限制使用的原始电流指令绝对值

/**
 * @brief 框架支持的 DJI 电机型号
 *
 * @note 电机型号用于选择反馈格式、合法 ID 范围、控制帧 ID、槽位和默认减速比。
 */
enum MotorType
{
  Motor3508 = 0x00U, ///< M3508 电机配合 C620 电调
  Motor6020 = 0x01U, ///< GM6020 电机
  Motor2006 = 0x02U, ///< M2006 电机配合 C610 电调
};

/**
 * @brief GM6020 控制帧模式
 *
 * @note M3508/C620 和 M2006/C610 只有固定的电流控制协议，此参数对它们无效。
 */
enum class DjiMotorControlMode : uint8_t
{
  VOLTAGE = 0, ///< GM6020 电压控制：0x1FF/0x2FF
  CURRENT,     ///< GM6020 电流控制：0x1FE/0x2FE，需在电机固件中启用电流环
};

/**
 * @brief DJI 电调反馈协议特有的原始数据
 */
struct DjiMotorRawData
{
  int16_t ecd;            ///< 编码器原始位置计数
  int16_t rpm;            ///< 电调反馈的有符号转速，单位：rpm
  int16_t torque_current; ///< 电调反馈的有符号原始力矩电流
  uint8_t temperature;    ///< 电调反馈温度，单位：摄氏度；无温度反馈的型号保持为 0
};

/**
 * @brief DJI 协议解析及控制限幅参数
 */
struct DjiMotorParam
{
  uint16_t ecd_full_range; ///< 编码器一圈的计数总数，例如 8192
  uint16_t current_limit;  ///< 原始控制指令允许的绝对值上限
};

template <MotorType type, typename CanT>
class DjiMotor;

/**
 * @brief 单个 DJI 电机设备对象
 *
 * @tparam type 电机型号，决定合法 ID、反馈解析、控制帧映射、限幅和默认减速比
 *
 * @details CanTxNode 聚合发送槽位，CanRxNode 按总线和反馈 ID 分发接收帧。
 * @note 对象不可复制或移动，以保证在线检查节点和控制帧槽位的身份稳定。
 */
template <MotorType type, typename CanT>
class DjiMotor
{
private:
  MotorData           _data;        ///< 最终机构输出轴的通用运动学数据
  DjiMotorRawData     _raw_data;    ///< DJI 电调反馈协议原始数据
  DjiMotorParam       _param;       ///< DJI 编码器和原始控制限幅参数
  Online              _online;      ///< 由有效反馈帧刷新的在线检查对象
  LuenbergerMotorData _lvbo_data;   ///< 可选的 Luenberger 观测结果

  CanT              *_can_item;     ///< 接收反馈所用的物理 CAN
  uint8_t            _motor_id;     ///< 电调配置的 DJI 协议 ID
  uint8_t            _bus_slot;     ///< 当前电机在控制帧中的槽位，范围 0~3
  CanTxNodeT<CanT> *_tx_node;
  CanRxNodeT<CanT> *_rx_node;
  static Status rx_callback(void *context, const CanRxMsg &rx);
  DjiMotorControlMode _control_mode; ///< GM6020 控制模式；其他型号忽略
  Status             _statu;        ///< 构造校验或初始化状态，运行期收发结果由函数返回


  int64_t _total_ecd;      ///< 跨零展开后的累计转子编码器计数
  bool    _feedback_ready; ///< 是否已接收过至少一帧有效反馈


public:
  /**
   * @brief 获取只读电机数据
   * @return 内部 MotorData 的常量引用。
   * @warning 返回实时引用而非快照：can_rx_task 随时可能在改它，跨任务读请用 snapshot()。
   */
  const MotorData &data(void) const;

  /**
   * @brief 拷贝一份同一帧的反馈（临界区内拷贝，跨任务读用它，F5）
   * @param data 运动学数据
   * @param raw  可选：同一帧的电调原始数据；nullptr = 不要
   */
  void snapshot(MotorData &data, DjiMotorRawData *raw = nullptr) const;

  /**
   * @brief 获取 DJI 电调反馈协议原始数据
   * @return 内部 DjiMotorRawData 的常量引用。
   * @warning 返回实时引用而非快照；跨任务读请用 snapshot()。
   */
  const DjiMotorRawData &raw_data(void) const;

  /**
   * @brief 获取 DJI 编码器和原始控制限幅参数
   * @return 内部 DjiMotorParam 的常量引用，构造完成后不再修改。
   */
  const DjiMotorParam &param(void) const;

  /**
   * @brief 获取本电机的在线检查对象
   * @return Online 的常量引用，可用于调用 isOnline() 查询状态。
   */
  const Online &online(void) const;

  /**
   * @brief 获取可选的 Luenberger 观测数据
   * @return 内部 LuenbergerMotorData 的常量引用。
   * @note 观测器未启用时，该数据不应作为有效控制依据。
   */
  const LuenbergerMotorData &lvboData(void) const;

  /**
   * @brief 查询对象构造校验或初始化状态
   * @return Status::OK 表示初始化成功；其他值表示未初始化或初始化错误。
   * @note 收发操作结果由 DataUnpack() 和 FillData() 的返回值提供，不更新此状态。
   * @warning 不得与 init() 并发调用。
   */
  Status statu(void) const;

  /**
   * @brief 构造单个 DJI 电机对象并校验参数
   *
   * @param can_item   电机连接的物理 CAN BSP 对象
   * @param motor_id   电调设置的 DJI 协议 ID
   * @param ratio      电机转子到最终机构输出轴的减速比；传 0 时按型号选择默认值：
   *                  M3508 为 3591/187，GM6020 为 1，M2006 为 36
   * @param offset     电机转子机械零位偏移，单位：rad
   * @param control_mode GM6020 的控制模式；其他电机型号忽略该参数
   * @param online_timeout_ms 多久收不到有效反馈判离线（ms，默认 30，评审 I4）
   *
   * @note 构造后调用 init() 注册收发节点；错误可通过 statu() 查询。
   * @note 构造阶段不会调用 BspCanT::init() 或发送 CAN 帧。
   */
  DjiMotor(CanT &can_item,
           uint8_t motor_id,
           float ratio = 0,
           float offset = 0.0f,
           DjiMotorControlMode control_mode = DjiMotorControlMode::VOLTAGE,
           uint16_t online_timeout_ms = 30U);

  /**
   * @brief 初始化阶段注销收发节点并销毁电机对象。
   * @note 接收节点注销成功后，释放发送槽位。
   * @warning 成功初始化后不支持运行期销毁；接收注册表冻结后析构会触发断言/终止。
   */
  ~DjiMotor();

  /** @brief 系统运行前注册发送槽位、标准帧反馈节点；重复成功初始化无副作用。 */
  Status init(void);

  /**
   * @brief 解析一帧属于本电机的 DJI CAN 反馈
   *
   * @param rx 待解析的 CAN 标准帧
   * @return Status::OK 反馈有效且数据已更新；Status::BAD_ARG 标识符或帧格式不匹配；
   *         Status::NOT_INIT 电机未成功完成内部注册。
   *
   * @note 由 CanRxNode 在接收任务中调用；失败帧进入 BSP 回退缓冲。
   *       写回在一段短临界区里做（和 snapshot() 互斥），不会因为读者占着锁而丢帧（F5：原来 try-lock 失败返回 BUSY，帧被当成没人认领）。
   * @note 只有完整且有效的反馈才能刷新 _online。相邻反馈转子位移必须小于半圈。
   * @note 首帧以绝对编码器位置初始化多圈角度；单圈角度为输出轴角度对 2π 取模。
   * @note 不计算角加速度；通用数据中的 acceleration 保持初始零值，不代表实际角加速度。
   */
  Status DataUnpack(const CanRxMsg &rx);

  /**
   * @brief 设置下一周期发送的原始控制量
   *
   * @param output 有符号原始控制指令；实现时应按具体型号的限制进行饱和限幅
   * @return Status::OK 写入成功；Status::NOT_INIT 未成功初始化；
   *         Status::BAD_ARG 或其他状态表示参数或发送槽位操作失败。
   *
   * @note 本函数只更新发送槽位，实际发送由 can_tx_task 统一完成。
   */
  Status FillData(int16_t output);

  // 禁止复制/移动
  DjiMotor(const DjiMotor &) = delete;
  DjiMotor &operator=(const DjiMotor &) = delete;
  DjiMotor(DjiMotor &&) = delete;
  DjiMotor &operator=(DjiMotor &&) = delete;

private:

};

/* ==================== 成员函数定义（只有头文件：任意 CAN / UART 类型都能实例化，评审 N23） ==================== */


template<MotorType type, typename CanT>
DjiMotor<type, CanT>::DjiMotor(CanT &can_item,
        uint8_t motor_id,
        float ratio,
        float offset,
        DjiMotorControlMode control_mode,
        uint16_t online_timeout_ms) : 
_data(),
_raw_data(),
_param(),
_online(online_timeout_ms),
_lvbo_data(),
_can_item(&can_item),
_motor_id(motor_id),
_bus_slot((motor_id - 1U) % 4),
_tx_node(nullptr),
_rx_node(nullptr),
_control_mode(control_mode),
_statu(Status::NOT_INIT),
_total_ecd(0),
_feedback_ready(false)
{
  if (sys_state.running())  // 只允许在初始化期使用
  {
    return;
  }
  // 参数校验
  if ( motor_id > 8 || motor_id == 0){
    this->_statu = Status::BAD_ARG;
    return;
  }
  else if ( motor_id == 8 && type == Motor6020 )  // 6020无ID8
  {
    this->_statu = Status::BAD_ARG;
    return;
  }
  // 在默认减速比处理前拒绝非有限值，避免将负无穷误当作默认配置。
  if (!std::isfinite(ratio) || !std::isfinite(offset))
  {
    _statu = Status::BAD_ARG;
    return;
  }
  if (ratio <= 0)   // 按照说明书中的减速比配置默认减速比
  {
    switch (type) {
      case Motor2006:{
        ratio = 36;
        break;
      }
      case Motor3508:{
        ratio = 3591.0f/187.0f;
        break;
      }
      case Motor6020:{
        ratio = 1;
        break;
      }
    }
  }

  _data.param.ratio = ratio;
  _data.param.offset = offset;
  _param.ecd_full_range = 8192;
  switch (type) {
    case Motor3508:
      _param.current_limit = 16000;
      break;
    case Motor2006:
      _param.current_limit = 10000;
      break;
    case Motor6020:
      _param.current_limit = _control_mode == DjiMotorControlMode::CURRENT ? 16384 : 25000;
      break;
  }
}

template <MotorType type, typename CanT>
DjiMotor<type, CanT>::~DjiMotor()
{
  // 接收节点持有 this；必须在上下文销毁之前成功注销。
  if (_rx_node != nullptr)
  {
    const Status result = CanRxNodeT<CanT>::unregist(*_can_item, _rx_node);
    configASSERT(result == Status::OK);
    if (result != Status::OK)
    {
      // 运行期注册表已冻结，继续析构会留下悬空回调。
      std::abort();
    }
    _rx_node = nullptr;
  }
  if (_tx_node != nullptr)
  {
    const Status result = CanTxNodeT<CanT>::unregist(_tx_node, _bus_slot);
    configASSERT(result == Status::OK);
    if (result != Status::OK)
    {
      (void)sys_state.error(result);
    }
    _tx_node = nullptr;
  }

  // _online 是成员对象，其析构函数会自动注销在线检查节点。
}

template<MotorType type, typename CanT>
Status DjiMotor<type, CanT>::init()
{
  // 系统事件未就绪时保留对象状态，允许初始化流程稍后重试。
  if (!sys_state.is_ready())
  {
    return Status::NOT_INIT;
  }
  if (sys_state.running())  // 只允许在初始化期使用
  {
    return Status::NOT_SUPPORTED;
  }
  // 初始化期重复调用成功对象，不重复申请资源，也不记录系统错误。
  if (_statu == Status::OK)
  {
    return Status::OK;
  }
  if (_statu != Status::NOT_INIT)
  {
    sys_state.init_error(_statu);
    return _statu;
  }

  // 确定发送can id
  uint32_t tx_can_id = 0x00;
  switch (type) {
    case Motor2006:
    case Motor3508:
    {
      if (_motor_id <= 4 )
      {
        tx_can_id = 0x200;
      }
      else
      {
        tx_can_id = 0x1FF;
      }
      break;
    }
    case Motor6020:
    {
      switch (_control_mode) {
        case DjiMotorControlMode::CURRENT:
        {
          if (_motor_id <= 4)
          {
            tx_can_id = 0x1FE;
          }
          else  
          {
            tx_can_id = 0x2FE;
          }
          break;
        }
        case DjiMotorControlMode::VOLTAGE:
        {
          if (_motor_id <= 4)
          {
            tx_can_id = 0x1FF;
          }
          else  
          {
            tx_can_id = 0x2FF;
          }
          break;
        }
        default:
        {
          _statu = Status::BAD_ARG;
          sys_state.init_error(_statu);
          return _statu;
        }
      }
      break;
    }
    default:
    {
      _statu = Status::BAD_ARG;
      sys_state.init_error(_statu);
      return _statu;
    }
  }

  // 注册
  _tx_node = CanTxNodeT<CanT>::regist(*_can_item, tx_can_id, 4U, _bus_slot);
  if ( _tx_node == nullptr )
  {
    _statu = Status::FULL;
    sys_state.init_error(_statu);
    return _statu;
  }

  const uint32_t feedback_id = (type == Motor6020 ? 0x204U : 0x200U) + _motor_id;
  _rx_node = CanRxNodeT<CanT>::regist(*_can_item, feedback_id, &DjiMotor<type, CanT>::rx_callback, this);
  if (_rx_node == nullptr)
  {
    // 注册器已记录具体冲突/资源错误；撤销本次初始化取得的发送槽位和锁。
    const Status result = CanTxNodeT<CanT>::unregist(_tx_node, _bus_slot);
    configASSERT(result == Status::OK);
    (void)result;
    _tx_node = nullptr;
    _statu = Status::IO_ERROR;
    return _statu;
  }

  _statu = Status::OK;
  return _statu;
}

template <MotorType type, typename CanT>
Status DjiMotor<type, CanT>::rx_callback(void *context, const CanRxMsg &rx)
{
  return static_cast<DjiMotor<type, CanT> *>(context)->DataUnpack(rx);
}

template <MotorType type, typename CanT>
Status DjiMotor<type, CanT>::DataUnpack(const CanRxMsg &rx)
{
  if (_tx_node == nullptr)
  {
    return Status::NOT_INIT;
  }
  if (rx.header.Identifier != ((type == Motor6020 ? 0x204U : 0x200U) + _motor_id) || rx.header.IdType != FDCAN_STANDARD_ID || rx.header.RxFrameType != FDCAN_DATA_FRAME || rx.header.FDFormat != FDCAN_CLASSIC_CAN || rx.header.DataLength != FDCAN_DLC_BYTES_8)
  {
    return Status::BAD_ARG;
  }

  const uint16_t ecd = (static_cast<uint16_t>(rx.data[0]) << 8) | rx.data[1];
  if (ecd >= _param.ecd_full_range)
  {
    return Status::BAD_ARG;
  }
  // 先在局部算好，再在短临界区里整体写回：snapshot() 拿到的是同一帧（F5，替代原来的 try-lock）
  MotorData       data = _data;
  DjiMotorRawData raw  = _raw_data;
  raw.rpm              = static_cast<int16_t>((rx.data[2] << 8) | rx.data[3]);
  raw.torque_current   = static_cast<int16_t>((rx.data[4] << 8) | rx.data[5]);
  raw.temperature      = type == Motor2006 ? 0U : rx.data[6];
  data.radian_data.velocity = raw.rpm / data.param.ratio * (PI / 30.0f);

  if (!_feedback_ready)
  {
    _total_ecd = ecd;
  }
  else
  {
    // 前后编码器差值超过半圈表示跨过零点，按方向补偿一圈计数。
    int32_t delta = static_cast<int32_t>(ecd) - static_cast<uint16_t>(raw.ecd);
    if (delta > _param.ecd_full_range / 2)
    {
      delta -= _param.ecd_full_range;
    }
    else if (delta < -static_cast<int32_t>(_param.ecd_full_range / 2))
    {
      delta += _param.ecd_full_range;
    }
    _total_ecd += delta;
  }

  raw.ecd = static_cast<int16_t>(ecd);

  // 保留双精度中间角度，避免多圈累计后单圈取模损失精度；多圈角度字段是 double，直接存（F11：原来先转 float 再存，精度还是 float）
  const double angle                  = (static_cast<double>(_total_ecd) * (2.0 * PI) / _param.ecd_full_range - data.param.offset) / data.param.ratio;
  data.radian_data.angle_multi_round  = angle;
  data.radian_data.angle_single_round = static_cast<float>(std::fmod(angle, 2.0 * PI));
  if (data.radian_data.angle_single_round < 0.0f)
  {
    data.radian_data.angle_single_round += 2.0f * PI;
  }

  taskENTER_CRITICAL();
  _data     = data;
  _raw_data = raw;
  taskEXIT_CRITICAL();

  _feedback_ready = true;
  _online.refresh_task();
  return Status::OK;
}

template<MotorType type, typename CanT>
Status DjiMotor<type, CanT>::FillData(int16_t output)
{
  if (_statu != Status::OK )
  {
    return _statu;
  }
  if (output > _param.current_limit)
  {
    output = _param.current_limit;
  }
  if (output < -_param.current_limit)
  {
    output = -_param.current_limit;
  }

  uint8_t tx_data[2];
  tx_data[0] = static_cast<uint8_t>(static_cast<uint16_t>(output)>>8);
  tx_data[1] = static_cast<uint8_t>(output);

  return _tx_node->filldata(tx_data, _bus_slot);

}

template <MotorType type, typename CanT>
const MotorData &DjiMotor<type, CanT>::data(void) const
{
  return _data;
}

template <MotorType type, typename CanT>
void DjiMotor<type, CanT>::snapshot(MotorData &data, DjiMotorRawData *raw) const
{
  taskENTER_CRITICAL();
  data = _data;
  if (raw != nullptr)
  {
    *raw = _raw_data;
  }
  taskEXIT_CRITICAL();
}

template <MotorType type, typename CanT>
const DjiMotorRawData &DjiMotor<type, CanT>::raw_data(void) const
{
  return _raw_data;
}

template <MotorType type, typename CanT>
const DjiMotorParam &DjiMotor<type, CanT>::param(void) const
{
  return _param;
}

template <MotorType type, typename CanT>
const Online &DjiMotor<type, CanT>::online(void) const
{
  return _online;
}

template <MotorType type, typename CanT>
const LuenbergerMotorData &DjiMotor<type, CanT>::lvboData(void) const
{
  return _lvbo_data;
}

template <MotorType type, typename CanT>
Status DjiMotor<type, CanT>::statu(void) const
{
  return _statu;
}

#endif // HAL_FDCAN_MODULE_ENABLED && ICBSP_CAN_RX_ENABLE && ICBSP_CAN_TX_ENABLE

#endif // __DJI_MOTOR_HPP__
