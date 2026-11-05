/**
 * @file math_constants.hpp
 * @brief 库共用的数学常量（带 ICBSP_ 前缀，不和 arm_math.h 等的 PI 宏冲突；评审 I6）
 */

#ifndef __ALGORITHM_MATH_CONSTANTS_HPP__
#define __ALGORITHM_MATH_CONSTANTS_HPP__

inline constexpr double ICBSP_PI   = 3.14159265358979323846;  ///< π（double）
inline constexpr float  ICBSP_PI_F = 3.14159265358979323846f; ///< π（float，给 float 运算用，免得整式提升成 double）

#endif // __ALGORITHM_MATH_CONSTANTS_HPP__
