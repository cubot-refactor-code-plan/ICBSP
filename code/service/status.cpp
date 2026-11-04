#include "status.hpp"

#include "task.h" // IWYU pragma: keep（vTaskDelete）


/* ==================== 生命周期 ==================== */


/** @brief 创建本实例的事件组：已创建过就保留原句柄直接返回，不重建（重建会泄漏句柄） */
Status EventState::init(void)
{
  if (_handle != nullptr)
  {
    return Status::OK;
  }

  _handle = xEventGroupCreate();
  if (_handle == nullptr)
  {
    return Status::FULL;
  }
  (void)xEventGroupSetBits(_handle, status_bit(Status::OK));
  return Status::OK;
}

/** @brief 挂父节点：父必须已 init，不能是自己，也不能形成环；只设置一次 */
Status EventState::set_parent(EventState &parent)
{
  if ((&parent == this) || (_parent != nullptr))
  {
    return Status::BAD_ARG;
  }
  if (!parent.is_ready())
  {
    return Status::NOT_INIT;
  }

  // 顺着父链往上走：撞到自己说明会成环
  for (const EventState *node = &parent; node != nullptr; node = node->_parent)
  {
    if (node == this)
    {
      return Status::BAD_ARG;
    }
  }

  _parent = &parent;
  return Status::OK;
}

/** @brief 初始化全部完成时置运行位；已有初始化失败记录则不置位，使"失败后不再进入运行态"成为单向约束 */
Status EventState::complete_init(void)
{
  if (_handle == nullptr)
  {
    return Status::NOT_INIT;
  }
  // 有失败记录说明初始化没走完整，不置运行位
  if ((xEventGroupGetBits(_handle) & INIT_FAIL_BIT) != 0U)
  {
    return Status::NOT_SUPPORTED;
  }

  (void)xEventGroupSetBits(_handle, RUNNING_BIT);
  return Status::OK;
}


/* ==================== */
/* ==================== 错误登记 ==================== */


/** @brief 记录一个错误码；不动运行位，运行期错误走这里 */
Status EventState::error(Status statu)
{
  if (_handle == nullptr)
  {
    return Status::NOT_INIT;
  }
  if (statu == Status::OK)
  {
    return Status::BAD_ARG;
  }
  
  (void)xEventGroupSetBits(_handle, status_bit(statu));
  (void)xEventGroupClearBits(_handle, status_bit(Status::OK));

  // 运行期错误只向上记码；父节点的运行位不动
  if (_parent != nullptr)
  {
    (void)_parent->error(statu);
  }
  return Status::OK;
}

/** @brief 记录初始化失败：先停掉本实例的运行期动作，再记错误码留下原因；运行后调用降级成 error() */
Status EventState::init_error(Status statu)
{
  if (_handle == nullptr)
  {
    return Status::NOT_INIT;
  }
  if (statu == Status::OK)
  {
    return Status::BAD_ARG;
  }
  if ((xEventGroupGetBits(_handle) & RUNNING_BIT) != 0U)
  {
    // 已在运行：运行期错误不能让本实例（以及挂在下面的任务）停摆，只记码（N16 / Q6）
    (void)error(statu);
    return Status::NOT_SUPPORTED;
  }

  (void)xEventGroupSetBits(_handle, INIT_FAIL_BIT);
  (void)xEventGroupClearBits(_handle, RUNNING_BIT);
  (void)xEventGroupSetBits(_handle, status_bit(statu));
  (void)xEventGroupClearBits(_handle, status_bit(Status::OK));

  if (_parent != nullptr)
  {
    (void)_parent->init_error(statu); // 父节点在运行时同样降级成 error()
  }
  return Status::OK;
}

/** @brief 清一个错误码：只清本实例，不传播；没有别的错误时把 OK 标记置回来 */
Status EventState::clear_error(Status statu)
{
  if (_handle == nullptr)
  {
    return Status::NOT_INIT;
  }
  if (statu == Status::OK)
  {
    return Status::BAD_ARG;
  }
  (void)xEventGroupClearBits(_handle, status_bit(statu));
  if (error_bits() == 0U)
  {
    (void)xEventGroupSetBits(_handle, status_bit(Status::OK));
  }
  return Status::OK;
}

/** @brief 清全部错误码：不传播、不动初始化失败位与运行位 */
Status EventState::clear_errors(void)
{
  if (_handle == nullptr)
  {
    return Status::NOT_INIT;
  }
  (void)xEventGroupClearBits(_handle, ERROR_MASK);
  (void)xEventGroupSetBits(_handle, status_bit(Status::OK));
  return Status::OK;
}

/* ==================== */

/* ==================== 查询与等待 ==================== */


/** @brief 事件组是否已创建 */
bool EventState::is_ready(void) const
{
  return _handle != nullptr;
}

/** @brief 本实例当前是否处于运行态 */
bool EventState::running(void) const
{
  return is_ready() && ((xEventGroupGetBits(_handle) & RUNNING_BIT) != 0U);
}

/** @brief 已记录的错误码位图 */
uint32_t EventState::error_bits(void) const
{
  return (_handle == nullptr) ? 0U : (static_cast<uint32_t>(xEventGroupGetBits(_handle)) & ERROR_MASK);
}

/** @brief 已记录的最小错误码；按位号从小到大扫，取到的即枚举值最小的那个 */
Status EventState::error_code(void) const
{
  if (_handle == nullptr)
  {
    return Status::NOT_INIT;
  }
  const uint32_t bits = error_bits();
  for (uint32_t value = 1U; value < STATUS_MAX; ++value)
  {
    if ((bits & (1U << value)) != 0U)
    {
      return static_cast<Status>(value);
    }
  }
  return Status::OK;
}

/**
 * @brief 阻塞等待本实例进入运行态；未 init 或等到初始化失败时删除当前任务
 * @note 同时等运行位与失败位：只等运行位的话，初始化失败时它永不置位，任务会永久阻塞。
 */
void EventState::wait_running(void) const
{
  if (_handle == nullptr) // 未 init，没有可等待的对象
  {
    vTaskDelete(NULL);
    return;
  }

  // xWaitForAllBits=pdFALSE：两位中任意一位置位即返回
  const EventBits_t bits = xEventGroupWaitBits(_handle, RUNNING_BIT | INIT_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
  if ((bits & RUNNING_BIT) == 0U)
  {
    vTaskDelete(NULL); // 初始化失败，任务自行退出
  }
}


/* ==================== */
/* ==================== 构造与析构 ==================== */


/** @brief 释放事件组；未 init 时无副作用 */
EventState::~EventState()
{
  if (_handle != nullptr)
  {
    vEventGroupDelete(_handle);
    _handle = nullptr;
  }
  _parent = nullptr;
}


/* ==================== */
