/**
 * @file status_code.hpp
 * @brief 统一状态码 Status（纯定义，不依赖 RTOS / HAL；评审 A6）
 *
 * @note 从 status.hpp 拆出来：algorithm/ 等纯计算代码只要状态码，不应为此间接依赖 FreeRTOS 的事件组头文件
 *       （主机侧单元测试可以直接编译）。EventState 仍在 status.hpp，它包含本文件。
 */

#ifndef __SERVICE_STATUS_CODE_HPP__
#define __SERVICE_STATUS_CODE_HPP__

#include <stdint.h>

/**
 * @brief 统一状态码
 *
 * @note 枚举值即事件组位号，因此**顺序不可随意调整**（调整会改变对外可观测行为）。
 */
enum class Status : uint8_t
{
  OK = 0,        ///< 成功
  BUSY,          ///< 资源被占用（DMA 正在发送等）
  TIMEOUT,       ///< 等待超时
  FULL,          ///< 缓冲满
  IO_ERROR,      ///< 硬件/传输错误
  BAD_ARG,       ///< 参数非法
  NOT_INIT,      ///< 资源未初始化（未调用 init 或创建失败）
  NOT_SUPPORTED, ///< 不支持的操作/模式
};

/**
 * @brief 取某个状态码对应的事件组位掩码
 * @param statu 状态码
 * @return 该状态码对应的位掩码
 */
constexpr uint32_t status_bit(Status statu)
{
  return 1U << static_cast<uint8_t>(statu);
}

#endif // __SERVICE_STATUS_CODE_HPP__
