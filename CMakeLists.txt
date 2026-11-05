# ------------------------------------------------------------------------------
# stm32_framework：以 git submodule + add_subdirectory 方式接入 STM32H7 CubeMX（CMake）工程的板级库
#
# 宿主工程（CubeMX 生成的根 CMakeLists.txt，##### User ##### 段）：
#
#   add_subdirectory(third_party/stm32-framework)
#   target_link_libraries(${CMAKE_PROJECT_NAME} stm32_framework)
#
# 本仓库只提供库、模板和文档，不内置示例工程。
# ------------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.22)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    message(FATAL_ERROR
        "stm32_framework 是库，不能单独配置。请在宿主工程里 add_subdirectory() 接入本库。")
endif()

if(NOT TARGET stm32cubemx)
    message(FATAL_ERROR
        "找不到 CubeMX 生成的 stm32cubemx 目标：请在 add_subdirectory(cmake/stm32cubemx) 之后再接入 stm32_framework")
endif()

enable_language(C CXX ASM)

# OBJECT 库而不是 STATIC：库里有覆盖 HAL / FreeRTOS 弱符号的强定义（HAL_*Callback、fault_on_*），
# 静态库成员不会因为“覆盖弱符号”被链接器拉进来，会静默失效。
add_library(stm32_framework OBJECT)

file(GLOB_RECURSE STM32_FRAMEWORK_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/code/bsp/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/code/service/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/code/tasks/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/code/device/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/code/protocol/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/code/algorithm/*.cpp
)
target_sources(stm32_framework PRIVATE ${STM32_FRAMEWORK_SOURCES})

# 库里不包含任何板级头文件：HAL 回调与库任务按 board_instances.hpp 的实例清单逐个展开
target_include_directories(stm32_framework PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/code/bsp
    ${CMAKE_CURRENT_SOURCE_DIR}/code/service
    ${CMAKE_CURRENT_SOURCE_DIR}/code/tasks
    ${CMAKE_CURRENT_SOURCE_DIR}/code/device
    ${CMAKE_CURRENT_SOURCE_DIR}/code/protocol
    ${CMAKE_CURRENT_SOURCE_DIR}/code/algorithm
)

# HAL / CMSIS / FreeRTOS 的头文件与宏都来自 CubeMX 的 stm32cubemx（INTERFACE）目标
target_link_libraries(stm32_framework PUBLIC stm32cubemx)

# 用户配置 hardware_config.h：唯一配置源，必须由宿主提供（模板 templates/user/board/hardware_config.h）。
# 放在宿主 user/board/（推荐；宿主给 stm32_framework 加该包含路径）或 stm32cubemx 的包含目录里。
get_target_property(_stm32_framework_mx_inc stm32cubemx INTERFACE_INCLUDE_DIRECTORIES)
set(_stm32_framework_cfg_dirs ${_stm32_framework_mx_inc})
list(APPEND _stm32_framework_cfg_dirs "${CMAKE_SOURCE_DIR}/user/board")
set(_stm32_framework_cfg "")
foreach(_stm32_framework_dir IN LISTS _stm32_framework_cfg_dirs)
    if(NOT _stm32_framework_cfg AND EXISTS "${_stm32_framework_dir}/hardware_config.h")
        set(_stm32_framework_cfg "${_stm32_framework_dir}/hardware_config.h")
    endif()
endforeach()
if(_stm32_framework_cfg)
    message(STATUS "stm32_framework: 使用配置 ${_stm32_framework_cfg}")
else()
    message(STATUS "stm32_framework: 没找到 hardware_config.h（必须由宿主提供，模板 templates/user/board/hardware_config.h）")
endif()

# 链接脚本检查：BspUartT 实例要进 .dma_buffer、fault_record 要进 .noinit。CubeMX 新建工程的链接脚本
# 没有这两段，缺了链接器只会把它们当孤儿段随便放（可能落进 DMA 访问不到的 DTCM），运行时才静默出错。
# 所以在配置阶段检查，并把链接脚本登记为配置依赖：它一变（手改、换模板、CubeMX 重新生成），下次构建就重新检查。
# 只认 CMAKE_EXE_LINKER_FLAGS 里的 -T（CubeMX 生成的工具链文件就是这样写的），找不到就跳过。
string(REGEX MATCH "-T[ \t]*\"([^\"]+)\"" _stm32_framework_ld_match "${CMAKE_EXE_LINKER_FLAGS}")
set(_stm32_framework_ld "${CMAKE_MATCH_1}")
if(NOT _stm32_framework_ld)
    string(REGEX MATCH "-T[ \t]*([^ \t\"]+)" _stm32_framework_ld_match "${CMAKE_EXE_LINKER_FLAGS}")
    set(_stm32_framework_ld "${CMAKE_MATCH_1}")
endif()
if(_stm32_framework_ld AND EXISTS "${_stm32_framework_ld}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_stm32_framework_ld}")
    file(READ "${_stm32_framework_ld}" _stm32_framework_ld_text)
    set(_stm32_framework_ld_missing "")
    foreach(_stm32_framework_sec dma_buffer noinit)
        if(NOT _stm32_framework_ld_text MATCHES "\\.${_stm32_framework_sec}[ \t]*(\\(NOLOAD\\))?[ \t\r\n]*:")
            list(APPEND _stm32_framework_ld_missing ".${_stm32_framework_sec}")
        endif()
    endforeach()
    if(_stm32_framework_ld_missing)
        list(JOIN _stm32_framework_ld_missing "、" _stm32_framework_ld_missing)
        message(FATAL_ERROR
            "stm32_framework: 链接脚本 ${_stm32_framework_ld} 缺少 ${_stm32_framework_ld_missing} 段"
            "。请把 ${CMAKE_CURRENT_SOURCE_DIR}/templates/linker_sections.ld 里的段加进去（放在 SECTIONS 里 .data 之前）。")
    endif()
    # .dma_buffer 里是 constexpr 全局实例：必须是可加载段（AT> FLASH），否则句柄/波特率等初值丢失
    string(FIND "${_stm32_framework_ld_text}" "\n  .dma_buffer" _stm32_framework_dma_pos)
    if(_stm32_framework_dma_pos EQUAL -1)
        message(FATAL_ERROR
            "stm32_framework: ${_stm32_framework_ld} 里找不到缩进 2 空格的 .dma_buffer 段定义。"
            "请按 templates/linker_sections.ld 的格式手动加入。")
    endif()
    string(SUBSTRING "${_stm32_framework_ld_text}" ${_stm32_framework_dma_pos} -1 _stm32_framework_dma_rest)
    string(FIND "${_stm32_framework_dma_rest}" "\n  .noinit" _stm32_framework_dma_len)
    if(_stm32_framework_dma_len EQUAL -1)
        string(LENGTH "${_stm32_framework_dma_rest}" _stm32_framework_dma_len)
    endif()
    string(SUBSTRING "${_stm32_framework_dma_rest}" 0 ${_stm32_framework_dma_len} _stm32_framework_dma_block)
    string(FIND "${_stm32_framework_dma_block}" "(NOLOAD)" _stm32_framework_dma_noload)
    if(NOT _stm32_framework_dma_noload EQUAL -1)
        message(FATAL_ERROR
            "stm32_framework: ${_stm32_framework_ld} 的 .dma_buffer 写成了 NOLOAD，实例初值不会进 Flash。"
            "请改成 `.dma_buffer : { ... } >RAM_D1 AT> FLASH`（见 templates/linker_sections.ld）。")
    endif()
    string(FIND "${_stm32_framework_dma_block}" "AT>" _stm32_framework_dma_at)
    if(_stm32_framework_dma_at EQUAL -1)
        message(FATAL_ERROR
            "stm32_framework: ${_stm32_framework_ld} 的 .dma_buffer 缺少 AT> FLASH 加载区。"
            "请参考 templates/linker_sections.ld，把初值放进 Flash。")
    endif()
    message(STATUS "stm32_framework: 链接脚本检查通过（.dma_buffer 可加载、.noinit）：${_stm32_framework_ld}")
else()
    message(STATUS "stm32_framework: CMAKE_EXE_LINKER_FLAGS 里没找到 -T 链接脚本，跳过 .dma_buffer / .noinit 检查")
endif()

set_target_properties(stm32_framework PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF          # 标准 C++20（-std=c++20），不依赖 GNU 扩展（评审 D3）
)

# 库自己的警告选项：“0 warning”不再取决于宿主的编译参数（评审 D3）。只作用于库的源文件，不传给宿主。
target_compile_options(stm32_framework PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wshadow>
)
# 第三方源码（VQF，MIT）保持上游原样，不为 -Wshadow 改它
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/code/algorithm/vqf.cpp PROPERTIES COMPILE_OPTIONS "-Wno-shadow")
