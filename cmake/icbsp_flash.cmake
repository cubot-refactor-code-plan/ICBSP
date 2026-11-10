# ------------------------------------------------------------------------------
# icbsp：外置 QSPI Flash 布局切换
#
#   INTERNAL  程序全部在片内 Flash（CubeMX 生成的链接脚本，什么都不改）
#   QSPI_RAM  方案 A：程序存 QSPI，开机由 bootloader 拷进 AXI SRAM 运行
#   QSPI_XIP  方案 B：程序在 QSPI 上就地执行（内存映射）
#
# 宿主根 CMakeLists.txt（integrate.py 生成，必须在 add_subdirectory(<icbsp>) 之前）：
#
#   include(<icbsp>/cmake/icbsp_flash.cmake)
#   icbsp_flash_layout(TARGET ${CMAKE_PROJECT_NAME} DEFAULT QSPI_RAM
#                      CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/user/board/icbsp_flash_config.h)
#
# 切换方案只改缓存变量 ICBSP_FLASH_LAYOUT（或者用 CMakePresets 里的 *-QspiRam / *-QspiXip 预设），
# 不需要重新生成 CubeMX、不需要重跑 integrate.py。
#
# 外置方案做的事：
#   1. 按模板生成链接脚本（templates/flash_layout/），替换 CMAKE_EXE_LINKER_FLAGS 里的 -T
#      （在 add_subdirectory(<icbsp>) 之前做，库的链接脚本检查看到的就是新脚本）
#   2. 给 stm32cubemx 目标加宏 ICBSP_FLASH_LAYOUT_QSPI_RAM / ICBSP_FLASH_LAYOUT_QSPI_XIP；
#      方案 A 另加 USER_VECT_TAB_ADDRESS + VECT_TAB_SRAM（SystemInit 把 VTOR 设到 AXI SRAM）
#   3. 用 ExternalProject 编译 bootloader（<icbsp>/boot，片内 Flash 0x08000000）
#   4. 构建后调用 tools/qspi_image.py 生成 <工程名>_qspi.bin / .hex（方案 B 还把镜像头写回 ELF）
# ------------------------------------------------------------------------------
include_guard(GLOBAL)

get_filename_component(ICBSP_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(ICBSP_FLASH_LAYOUTS INTERNAL QSPI_RAM QSPI_XIP)

# 从 CMAKE_EXE_LINKER_FLAGS 里找 -T（与库 CMakeLists.txt 的写法一致：带引号或不带引号）
function(_icbsp_find_ld flags out_path out_match)
    string(REGEX MATCH "-T[ \t]*\"([^\"]+)\"" _m "${flags}")
    set(_p "${CMAKE_MATCH_1}")
    if(NOT _p)
        string(REGEX MATCH "-T[ \t]*([^ \t\"]+)" _m "${flags}")
        set(_p "${CMAKE_MATCH_1}")
    endif()
    set(${out_path} "${_p}" PARENT_SCOPE)
    set(${out_match} "${_m}" PARENT_SCOPE)
endfunction()

# 读 icbsp_flash_config.h 里的 #define（只认十进制 / 十六进制整数，可带 u / U 后缀）
function(_icbsp_config_int header name out)
    file(STRINGS "${header}" _lines REGEX "^[ \t]*#[ \t]*define[ \t]+${name}[ \t]")
    if(NOT _lines)
        message(FATAL_ERROR "icbsp: ${header} 里没有 ${name}（请重新运行 integrate.py 生成）")
    endif()
    list(GET _lines 0 _line)
    string(REGEX MATCH "define[ \t]+${name}[ \t]+\\(?(0[xX][0-9a-fA-F]+|[0-9]+)[uU]?" _m "${_line}")
    if(NOT CMAKE_MATCH_1)
        message(FATAL_ERROR "icbsp: ${header} 里 ${name} 不是整数常量：${_line}")
    endif()
    math(EXPR _v "${CMAKE_MATCH_1}" OUTPUT_FORMAT DECIMAL)
    set(${out} "${_v}" PARENT_SCOPE)
endfunction()

# 从宿主原链接脚本里取堆 / 栈大小（保持和 CubeMX 设置一致）
function(_icbsp_ld_symbol text name default out)
    string(REGEX MATCH "${name}[ \t]*=[ \t]*(0[xX][0-9a-fA-F]+|[0-9]+)" _m "${text}")
    if(CMAKE_MATCH_1)
        set(${out} "${CMAKE_MATCH_1}" PARENT_SCOPE)
    else()
        set(${out} "${default}" PARENT_SCOPE)
    endif()
endfunction()

function(icbsp_flash_layout)
    cmake_parse_arguments(ARG "" "TARGET;DEFAULT;CONFIG" "" ${ARGN})
    if(NOT ARG_TARGET)
        message(FATAL_ERROR "icbsp_flash_layout: 需要 TARGET（宿主可执行目标）")
    endif()
    if(NOT ARG_DEFAULT)
        set(ARG_DEFAULT INTERNAL)
    endif()
    set(ICBSP_FLASH_LAYOUT "${ARG_DEFAULT}" CACHE STRING "icbsp 程序存放方式：INTERNAL / QSPI_RAM / QSPI_XIP")
    set_property(CACHE ICBSP_FLASH_LAYOUT PROPERTY STRINGS ${ICBSP_FLASH_LAYOUTS})
    if(NOT ICBSP_FLASH_LAYOUT IN_LIST ICBSP_FLASH_LAYOUTS)
        message(FATAL_ERROR "icbsp: ICBSP_FLASH_LAYOUT=${ICBSP_FLASH_LAYOUT} 无效，可选 ${ICBSP_FLASH_LAYOUTS}")
    endif()
    if(ICBSP_FLASH_LAYOUT STREQUAL "INTERNAL")
        message(STATUS "icbsp: 程序存放方式 INTERNAL（片内 Flash，使用 CubeMX 链接脚本）")
        return()
    endif()

    if(NOT ARG_CONFIG OR NOT EXISTS "${ARG_CONFIG}")
        message(FATAL_ERROR
            "icbsp: ICBSP_FLASH_LAYOUT=${ICBSP_FLASH_LAYOUT} 需要 icbsp_flash_config.h（${ARG_CONFIG}）。"
            "请用 integrate.py / Web UI 打开“外置 Flash”生成它。")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ARG_CONFIG}")
    _icbsp_config_int("${ARG_CONFIG}" ICBSP_QSPI_FLASH_SIZE _qspi_size)
    _icbsp_config_int("${ARG_CONFIG}" ICBSP_AXI_SRAM_SIZE _axi_size)

    _icbsp_find_ld("${CMAKE_EXE_LINKER_FLAGS}" _host_ld _host_ld_match)
    if(NOT _host_ld OR NOT EXISTS "${_host_ld}")
        message(FATAL_ERROR "icbsp: CMAKE_EXE_LINKER_FLAGS 里找不到 -T 链接脚本，无法切换到 ${ICBSP_FLASH_LAYOUT}")
    endif()
    file(READ "${_host_ld}" _host_ld_text)
    _icbsp_ld_symbol("${_host_ld_text}" "_Min_Heap_Size" "0x200" ICBSP_MIN_HEAP_SIZE)
    _icbsp_ld_symbol("${_host_ld_text}" "_Min_Stack_Size" "0x400" ICBSP_MIN_STACK_SIZE)
    set(ICBSP_QSPI_SIZE "${_qspi_size}")
    set(ICBSP_AXI_SRAM_SIZE "${_axi_size}")

    if(ICBSP_FLASH_LAYOUT STREQUAL "QSPI_RAM")
        set(_tpl "${ICBSP_ROOT}/templates/flash_layout/h7_qspi_ram.ld.in")
        set(_kind ram)
    else()
        set(_tpl "${ICBSP_ROOT}/templates/flash_layout/h7_qspi_xip.ld.in")
        set(_kind xip)
    endif()
    set(_ld "${CMAKE_BINARY_DIR}/icbsp_flash/icbsp_${_kind}.ld")
    configure_file("${_tpl}" "${_ld}" @ONLY)

    string(REPLACE "${_host_ld_match}" "-T \"${_ld}\"" _flags "${CMAKE_EXE_LINKER_FLAGS}")
    set(CMAKE_EXE_LINKER_FLAGS "${_flags}" PARENT_SCOPE)
    message(STATUS "icbsp: 程序存放方式 ${ICBSP_FLASH_LAYOUT}，链接脚本 ${_ld}（QSPI ${_qspi_size} 字节）")

    # 目标都建好之后（根目录处理完）再挂宏、bootloader 和构建后步骤
    cmake_language(EVAL CODE "
        cmake_language(DEFER DIRECTORY [[${CMAKE_SOURCE_DIR}]]
            CALL _icbsp_flash_setup [[${ARG_TARGET}]] [[${_kind}]] [[${ARG_CONFIG}]] [[${_qspi_size}]] [[${_axi_size}]])")
endfunction()

function(_icbsp_flash_setup target kind config qspi_size axi_size)
    if(NOT TARGET ${target})
        message(FATAL_ERROR "icbsp_flash_layout: 找不到目标 ${target}")
    endif()
    if(NOT TARGET stm32cubemx)
        message(FATAL_ERROR "icbsp_flash_layout: 找不到 CubeMX 的 stm32cubemx 目标")
    endif()

    # 宏：stm32cubemx 是 INTERFACE 目标，宿主、HAL（system_stm32h7xx.c）、icbsp 都能看到
    if(kind STREQUAL "ram")
        target_compile_definitions(stm32cubemx INTERFACE
            ICBSP_FLASH_LAYOUT_QSPI_RAM=1 USER_VECT_TAB_ADDRESS VECT_TAB_SRAM)
        # 代码和数据同在 AXI SRAM，ld 会提示 RWX 段：方案 A 本来就是这样，不是错误
        target_link_options(${target} PRIVATE "LINKER:--no-warn-rwx-segments")
    else()
        target_compile_definitions(stm32cubemx INTERFACE ICBSP_FLASH_LAYOUT_QSPI_XIP=1)
    endif()

    # bootloader：只需要 CMSIS 头文件与芯片型号宏（寄存器级实现，不用 HAL）
    get_target_property(_incs stm32cubemx INTERFACE_INCLUDE_DIRECTORIES)
    get_target_property(_defs stm32cubemx INTERFACE_COMPILE_DEFINITIONS)
    set(_boot_incs "")
    foreach(_i IN LISTS _incs)
        if(_i MATCHES "CMSIS" AND NOT _i MATCHES "\\$<")
            get_filename_component(_i "${_i}" ABSOLUTE)
            list(APPEND _boot_incs "${_i}")
        endif()
    endforeach()
    set(_boot_defs "")
    foreach(_d IN LISTS _defs)
        if(_d MATCHES "^STM32[A-Z0-9]+xx$")
            list(APPEND _boot_defs "${_d}")
        endif()
    endforeach()
    if(NOT _boot_incs OR NOT _boot_defs)
        message(FATAL_ERROR "icbsp: 从 stm32cubemx 目标里找不到 CMSIS 包含路径或芯片型号宏，无法编译 bootloader")
    endif()
    list(JOIN _boot_incs "|" _boot_incs)
    list(JOIN _boot_defs "|" _boot_defs)

    include(ExternalProject)
    ExternalProject_Add(icbsp_boot
        SOURCE_DIR "${ICBSP_ROOT}/boot"
        BINARY_DIR "${CMAKE_BINARY_DIR}/icbsp_boot"
        LIST_SEPARATOR "|"
        CMAKE_ARGS
            -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
            -DCMAKE_BUILD_TYPE=Release
            -DICBSP_BOOT_CONFIG=${config}
            -DICBSP_BOOT_INCLUDES=${_boot_incs}
            -DICBSP_BOOT_DEFINES=${_boot_defs}
        BUILD_ALWAYS ON
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS "${CMAKE_BINARY_DIR}/icbsp_boot/icbsp_boot.elf"
        USES_TERMINAL_BUILD ON
    )
    add_dependencies(${target} icbsp_boot)

    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${Python3_EXECUTABLE}" -I "${ICBSP_ROOT}/tools/qspi_image.py"
            --elf "$<TARGET_FILE:${target}>" --layout ${kind}
            --qspi-size ${qspi_size} --ram-size ${axi_size}
        COMMENT "icbsp: 生成 QSPI 镜像（${kind}）"
        VERBATIM
    )
endfunction()
