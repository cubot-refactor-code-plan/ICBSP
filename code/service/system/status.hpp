/**
 * @file status.hpp
 * @author Rh
 * @brief 统一状态码 Status 与可按场景多实例的事件状态 EventState
 * @version 0.8
 * @date 2026-10-04
 *
 * @copyright Copyright (c) 2026
 *
 * @details 一个 EventState 实例 = 一个事件组 = 一份「阶段 + 错误登记」。按场景创建多个实例
 *          互不影响（整个 MCU 用 sys_state，某条总线要单独看就再建一个）。
 *
 * @note 位分配（每个实例各一份，共 24 位）：
 *       bit 0 = OK 标记，bit 1~21 = 状态码位（位号即 Status 枚举值，掩码用 status_bit()），
 *       bit 22 = RUNNING_BIT 运行位，bit 23 = INIT_FAIL_BIT 初始化失败位（粘滞，只置不清）。
 *
 * @note 两族登记接口：error() 只记码、不动运行位（运行期错误）；init_error() 记码 + 置失败位 +
 *       清运行位（初始化错误）。正常流程 init() → complete_init() → 任务入口 wait_running()；
 *       未 init 时除 init() 外全部接口安全返回。
 *
 * @note 分层：子节点用 set_parent() 挂到父节点。错误只向上传播，父节点不聚合 running；
 *       要判断具体哪个子系统失败，查对应子节点的 error_code()。父节点必须比子节点活得久。
 *
 * @note 分层约定：只给「维护多个对象的系统」用；单个设备不许引用它，失败只 return Status。
 *       生命周期、错误传播与已知局限见同目录 status.md 与 Docs/guide/系统状态标志原理.md。
 */

#ifndef __SERVICE_STATUS_HPP__
#define __SERVICE_STATUS_HPP__

#include <stdint.h>

#include "FreeRTOS.h" // IWYU pragma: keep
#include "event_groups.h"

#include "service/system/status_code.hpp" // IWYU pragma: export（Status、status_bit）




/**
 * @brief 事件状态：一个实例 = 一个事件组 = 一份「阶段 + 错误登记」
 *
 * @note 实例数量不限，按场景创建；各实例持有自己的事件组，互不影响。
 * @note 必须先 init() 才能登记/等待；未 init 时所有接口安全返回，不会触碰空句柄。
 */
class EventState
{
public:
  /* ==================== 位分配 ==================== */

  static constexpr uint32_t RUNNING_BIT   = 1U << 22; ///< 本实例已进入运行态
  static constexpr uint32_t INIT_FAIL_BIT = 1U << 23; ///< 本实例初始化失败过（粘滞，不清除）
  static constexpr uint32_t STATUS_MAX    = 22U;      ///< 状态码只能用 bit 0 ~ 21，不得侵入系统标志位
  static constexpr uint32_t ERROR_MASK    = ((1U << STATUS_MAX) - 1U) & ~1U; ///< 错误码位 bit 1 ~ 21（不含 OK 标记）

  /* ==================== */
  /* ==================== 构造与析构 ==================== */

  /** @brief 默认构造：未创建事件组，须再调 init() */
  EventState() = default;

  /** @brief 释放事件组；未 init 时无副作用 */
  ~EventState();

  // 句柄所有权唯一：禁止拷贝，避免两个实例持有同一个事件组
  EventState(const EventState &)            = delete;
  EventState &operator=(const EventState &) = delete;

  /* ==================== */
  /* ==================== 生命周期 ==================== */

  /**
   * @brief 创建本实例的事件组（幂等：已创建过则直接返回 OK）
   * @return OK=可用；FULL=创建失败
   */
  Status init(void);

  /**
   * @brief 设置父节点：本节点的错误会向上传播到父节点
   *
   * @param parent 父节点（必须已 init，且比本节点活得久）
   * @return OK=已设置；BAD_ARG=传了自己、已有父节点、或会形成环；NOT_INIT=父节点未 init
   *
   * @note 父指针只设置一次；可在 init() 前后调用，但父节点必须先 init。
   */
  Status set_parent(EventState &parent);

  /**
   * @brief 初始化全部完成：置运行位
   * @return OK=已置运行位；NOT_SUPPORTED=此前已有初始化失败记录，运行位不置位；NOT_INIT=未 init
   */
  Status complete_init(void);

  /* ==================== */
  /* ==================== 错误登记 ==================== */

  /**
   * @brief 记录一个错误码，不影响运行位（运行期错误）
   * @param statu 错误状态码（不允许传 OK）
   * @return OK=已记录；BAD_ARG=传了 OK；NOT_INIT=未 init
   */
  Status error(Status statu);

  /**
   * @brief 记录初始化失败：记错误码 + 置粘滞失败位 + 清运行位
   * @param statu 错误状态码（不允许传 OK）
   * @return OK=已记录；BAD_ARG=传了 OK；NOT_INIT=未 init；
   *         NOT_SUPPORTED=本实例已经在运行（complete_init() 之后）：只按 error() 记码，不置失败位、不清运行位
   * @note 运行位被清且失败位粘滞，本实例此后不会再进入运行态。
   * @note 只在初始化期有效（对比评估 Q6 / N16）：运行期的错误不能让整个系统停摆，所以运行后调用降级成 error()；
   *       向父节点传播时同样按父节点自己是否在运行来决定。
   */
  Status init_error(Status statu);

  /**
   * @brief 清掉一个已记录的错误码；没有别的错误时 OK 标记回来
   * @param statu 要清的状态码（不允许传 OK）
   * @return OK=已清（原来没记录也算 OK）；BAD_ARG=传了 OK；NOT_INIT=未 init
   * @note 只清本实例，不向父节点传播（父节点上同一个码可能还来自别的子节点，由父节点的维护者自己清）。
   *       初始化失败位不受影响：初始化失败过的实例永远不再进入运行态。
   */
  Status clear_error(Status statu);

  /** @brief 清掉全部已记录的错误码（同 clear_error()，不传播、不动初始化失败位）；未 init 时返回 NOT_INIT */
  Status clear_errors(void);

  /* ==================== */
  /* ==================== 查询与等待 ==================== */

  /** @brief 事件组是否已创建；未 init 时登记/等待接口一律安全返回 */
  bool is_ready(void) const;

  /** @brief 本实例当前是否处于运行态 */
  bool running(void) const;

  /**
   * @brief 已记录的错误码里枚举值最小的那个（不是最近一次的错误）
   * @return 错误状态码；没有未清的错误时返回 Status::OK；未 init 时返回 Status::NOT_INIT
   * @note 错误的生命周期：error() / init_error() 记下的码一直保留（粘滞），直到 clear_error() / clear_errors()；
   *       多个错误并存时按枚举值从小到大取第一个。要知道具体有哪些错误，用 error_bits()。
   */
  Status error_code(void) const;

  /** @brief 已记录的错误码位图（bit n = 状态码 n，掩码用 status_bit()）；未 init 时返回 0 */
  uint32_t error_bits(void) const;

  /**
   * @brief 阻塞等待本实例进入运行态
   * @note **只能在任务中调用**：未 init、或等到的是「初始化失败」时，函数会删除当前任务（不会返回）。
   */
  void wait_running(void) const;

  /* ==================== */
private:
  /* ==================== 成员变量 ==================== */

  EventGroupHandle_t _handle = nullptr; ///< 本实例的事件组，未 init 时为 nullptr
  EventState        *_parent = nullptr; ///< 父节点（不拥有；只设置一次）

  /* ==================== */
};

// 状态码不得侵入 bit 22~23（系统标志位）；新增状态码越界时在编译期就报错
static_assert(static_cast<uint32_t>(Status::NOT_SUPPORTED) < EventState::STATUS_MAX, "Status 取值与系统标志位重叠");

#endif // __SERVICE_STATUS_HPP__
