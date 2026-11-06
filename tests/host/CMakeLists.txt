# algorithm/ 的主机侧单元测试（阶段 3）：不依赖硬件和 RTOS，用主机编译器构建
#
#   cmake -S tests/host -B build/host && cmake --build build/host && ctest --test-dir build/host --output-on-failure
#
# 只编 algorithm/ 里纯计算的源文件（vqf 是第三方代码，不在这里测）。
cmake_minimum_required(VERSION 3.22)
project(icbsp_host_tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ICBSP_CODE ${CMAKE_CURRENT_SOURCE_DIR}/../../code)

add_executable(icbsp_host_tests
    test_main.cpp
    ${ICBSP_CODE}/algorithm/pid.cpp
    ${ICBSP_CODE}/algorithm/quaternion.cpp
    ${ICBSP_CODE}/algorithm/quaternion_pid.cpp
    ${ICBSP_CODE}/algorithm/matrix.cpp
)
target_include_directories(icbsp_host_tests PRIVATE ${ICBSP_CODE})
target_compile_options(icbsp_host_tests PRIVATE -Wall -Wextra -Wshadow -Werror)

enable_testing()
add_test(NAME algorithm COMMAND icbsp_host_tests)
