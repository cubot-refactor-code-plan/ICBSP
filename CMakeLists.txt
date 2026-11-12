# ------------------------------------------------------------------------------
# icbsp：以 git submodule + add_subdirectory 方式接入 STM32H7 / STM32F4 CubeMX（CMake）工程的板级库
#
# 宿主工程（CubeMX 生成的根 CMakeLists.txt，##### User ##### 段）：
#
#   add_subdirectory(third_party/icbsp)
#   target_link_libraries(${CMAKE_PROJECT_NAME} icbsp)
#
# 本仓库只提供库、模板和文档，不内置示例工程。
# ------------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.22)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    message(FATAL_ERROR
        "icbsp 是库，不能单独配置。请在宿主工程里 add_subdirectory() 接入本库。")
endif()

if(NOT TARGET stm32cubemx)
    message(FATAL_ERROR
        "找不到 CubeMX 生成的 stm32cubemx 目标：请在 add_subdirectory(cmake/stm32cubemx) 之后再接入 icbsp")
endif()

enable_language(C CXX ASM)

# OBJECT 库而不是 STATIC：库里有覆盖 HAL / FreeRTOS 弱符号的强定义（HAL_*Callback、fault_on_*），
# 静态库成员不会因为“覆盖弱符号”被链接器拉进来，会静默失效。
add_library(icbsp OBJECT)

file(GLOB_RECURSE ICBSP_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/code/*.cpp
)
target_sources(icbsp PRIVATE ${ICBSP_SOURCES})

# 包含路径只有 code/：库内外一律写成 #include "bsp/bsp_uart.hpp"、"device/motor/dji_motor.hpp"，
# 分层关系从包含语句就看得出来（下层不出现上层路径），也不怕不同子目录里的文件重名。
# 库里不包含任何板级头文件：HAL 回调与库任务按 board_instances.hpp 的实例清单逐个展开
target_include_directories(icbsp PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/code
)

# HAL / CMSIS / FreeRTOS 的头文件与宏都来自 CubeMX 的 stm32cubemx（INTERFACE）目标
target_link_libraries(icbsp PUBLIC stm32cubemx)

# 用户配置 hardware_config.h：唯一配置源，必须由宿主提供（模板 templates/user/board/hardware_config.h）。
# 放在宿主 user/board/（推荐；宿主给 icbsp 加该包含路径）或 stm32cubemx 的包含目录里。
get_target_property(_icbsp_mx_inc stm32cubemx INTERFACE_INCLUDE_DIRECTORIES)
set(_icbsp_cfg_dirs ${_icbsp_mx_inc})
list(APPEND _icbsp_cfg_dirs "${CMAKE_SOURCE_DIR}/user/board")
set(_icbsp_cfg "")
foreach(_icbsp_dir IN LISTS _icbsp_cfg_dirs)
    if(NOT _icbsp_cfg AND EXISTS "${_icbsp_dir}/hardware_config.h")
        set(_icbsp_cfg "${_icbsp_dir}/hardware_config.h")
    endif()
endforeach()
if(_icbsp_cfg)
    message(STATUS "icbsp: 使用配置 ${_icbsp_cfg}")
else()
    message(STATUS "icbsp: 没找到 hardware_config.h（必须由宿主提供，模板 templates/user/board/hardware_config.h）")
endif()

# 链接脚本检查：BspUartT 实例要进 .dma_buffer、fault_record 要进 .noinit。CubeMX 新建工程的链接脚本
# 没有这两段，缺了链接器只会把它们当孤儿段随便放（可能落进 DMA 访问不到的 DTCM），运行时才静默出错。
# 所以在配置阶段检查，并把链接脚本登记为配置依赖：它一变（手改、换模板、CubeMX 重新生成），下次构建就重新检查。
# 只认 CMAKE_EXE_LINKER_FLAGS 里的 -T（CubeMX 生成的工具链文件就是这样写的），找不到就跳过。
string(REGEX MATCH "-T[ \t]*\"([^\"]+)\"" _icbsp_ld_match "${CMAKE_EXE_LINKER_FLAGS}")
set(_icbsp_ld "${CMAKE_MATCH_1}")
if(NOT _icbsp_ld)
    string(REGEX MATCH "-T[ \t]*([^ \t\"]+)" _icbsp_ld_match "${CMAKE_EXE_LINKER_FLAGS}")
    set(_icbsp_ld "${CMAKE_MATCH_1}")
endif()

# 系列探测：从 CubeMX 的包含目录里找 hal_conf.h。
# H7 需要 .dma_buffer + .noinit 两段；F4 主 SRAM 可直接被 DMA 访问，只需要 .noinit。
set(_icbsp_series "")
foreach(_icbsp_dir IN LISTS _icbsp_mx_inc)
    if(EXISTS "${_icbsp_dir}/stm32h7xx_hal_conf.h")
        set(_icbsp_series "h7")
    elseif(EXISTS "${_icbsp_dir}/stm32f4xx_hal_conf.h")
        set(_icbsp_series "f4")
    endif()
endforeach()

if(_icbsp_ld AND EXISTS "${_icbsp_ld}" AND _icbsp_series STREQUAL "f4")
    # F4：只查 .noinit（fault_record 靠它保留复位现场）；.dma_buffer 在 F4 上不生成，不用查
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_icbsp_ld}")
    file(READ "${_icbsp_ld}" _icbsp_ld_text)
    if(NOT _icbsp_ld_text MATCHES "\\.noinit[ \t]*(\\(NOLOAD\\))?[ \t\r\n]*:")
        message(FATAL_ERROR
            "icbsp: 链接脚本 ${_icbsp_ld} 缺少 .noinit 段（fault_record 用它保留复位现场）。"
            "请把 ${CMAKE_CURRENT_SOURCE_DIR}/templates/linker_sections.ld 里的 .noinit 段加进 SECTIONS。")
    endif()
    message(STATUS "icbsp: 链接脚本检查通过（F4 只需要 .noinit）：${_icbsp_ld}")
elseif(_icbsp_ld AND EXISTS "${_icbsp_ld}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_icbsp_ld}")
    file(READ "${_icbsp_ld}" _icbsp_ld_text)
    set(_icbsp_ld_missing "")
    foreach(_icbsp_sec dma_buffer noinit)
        if(NOT _icbsp_ld_text MATCHES "\\.${_icbsp_sec}[ \t]*(\\(NOLOAD\\))?[ \t\r\n]*:")
            list(APPEND _icbsp_ld_missing ".${_icbsp_sec}")
        endif()
    endforeach()
    if(_icbsp_ld_missing)
        list(JOIN _icbsp_ld_missing "、" _icbsp_ld_missing)
        message(FATAL_ERROR
            "icbsp: 链接脚本 ${_icbsp_ld} 缺少 ${_icbsp_ld_missing} 段"
            "。请把 ${CMAKE_CURRENT_SOURCE_DIR}/templates/linker_sections.ld 里的段加进去（放在 SECTIONS 里 .data 之前）。")
    endif()
    # .dma_buffer 里是 constexpr 全局实例：必须是可加载段（AT> FLASH），否则句柄/波特率等初值丢失
    string(FIND "${_icbsp_ld_text}" "\n  .dma_buffer" _icbsp_dma_pos)
    if(_icbsp_dma_pos EQUAL -1)
        message(FATAL_ERROR
            "icbsp: ${_icbsp_ld} 里找不到缩进 2 空格的 .dma_buffer 段定义。"
            "请按 templates/linker_sections.ld 的格式手动加入。")
    endif()
    string(SUBSTRING "${_icbsp_ld_text}" ${_icbsp_dma_pos} -1 _icbsp_dma_rest)
    string(FIND "${_icbsp_dma_rest}" "\n  .noinit" _icbsp_dma_len)
    if(_icbsp_dma_len EQUAL -1)
        string(LENGTH "${_icbsp_dma_rest}" _icbsp_dma_len)
    endif()
    string(SUBSTRING "${_icbsp_dma_rest}" 0 ${_icbsp_dma_len} _icbsp_dma_block)
    string(FIND "${_icbsp_dma_block}" "(NOLOAD)" _icbsp_dma_noload)
    if(NOT _icbsp_dma_noload EQUAL -1)
        message(FATAL_ERROR
            "icbsp: ${_icbsp_ld} 的 .dma_buffer 写成了 NOLOAD，实例初值不会进 Flash。"
            "请改成 `.dma_buffer : { ... } >RAM_D1 AT> FLASH`（见 templates/linker_sections.ld）。")
    endif()
    string(FIND "${_icbsp_dma_block}" "AT>" _icbsp_dma_at)
    if(_icbsp_dma_at EQUAL -1)
        message(FATAL_ERROR
            "icbsp: ${_icbsp_ld} 的 .dma_buffer 缺少 AT> FLASH 加载区。"
            "请参考 templates/linker_sections.ld，把初值放进 Flash。")
    endif()
    message(STATUS "icbsp: 链接脚本检查通过（.dma_buffer 可加载、.noinit）：${_icbsp_ld}")
else()
    message(STATUS "icbsp: CMAKE_EXE_LINKER_FLAGS 里没找到 -T 链接脚本，跳过 .dma_buffer / .noinit 检查")
endif()

set_target_properties(icbsp PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF          # 标准 C++20（-std=c++20），不依赖 GNU 扩展（评审 D3）
)

# 库自己的警告选项：“0 warning”不再取决于宿主的编译参数（评审 D3）。只作用于库的源文件，不传给宿主。
target_compile_options(icbsp PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wshadow>
)
# 第三方源码（VQF，MIT）保持上游原样，不为 -Wshadow 改它
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/code/algorithm/vqf.cpp PROPERTIES COMPILE_OPTIONS "-Wno-shadow")
