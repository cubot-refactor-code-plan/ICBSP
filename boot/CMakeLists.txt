# ------------------------------------------------------------------------------
# icbsp bootloader：外置 QSPI Flash 方案 A / B 的片内引导程序（0x08000000）
#
# 不单独使用：由宿主的 cmake/icbsp_flash.cmake 用 ExternalProject 编译，参数：
#   ICBSP_BOOT_CONFIG    宿主的 icbsp_flash_config.h（引脚、芯片、分频，integrate.py 生成）
#   ICBSP_BOOT_INCLUDES  CMSIS 包含路径（来自宿主 stm32cubemx 目标，| 分隔）
#   ICBSP_BOOT_DEFINES   芯片型号宏（如 STM32H750xx，| 分隔）
#
# 寄存器级实现，只依赖 CMSIS 头文件，不用 HAL。同一个 bootloader 同时支持方案 A 和 B
# （看 QSPI 镜像头里的 layout 字段），所以在 A / B 之间切换不用重烧 bootloader。
# ------------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.22)
project(icbsp_boot C)

foreach(_v ICBSP_BOOT_CONFIG ICBSP_BOOT_INCLUDES ICBSP_BOOT_DEFINES)
    if(NOT ${_v})
        message(FATAL_ERROR "icbsp_boot: 缺少 ${_v}（应由宿主的 cmake/icbsp_flash.cmake 传入）")
    endif()
endforeach()

# 宿主的工具链文件把宿主的 -T 链接脚本和 -Map 写进了 CMAKE_EXE_LINKER_FLAGS，换成自己的
string(REGEX REPLACE "-T[ \t]*\"[^\"]*\"" "" CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
string(REGEX REPLACE "-T[ \t]*[^ \t\"]+" "" CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
string(REGEX REPLACE "-Wl,-Map=[^ \t]*" "" CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")

add_executable(icbsp_boot main.c startup.c)
set_target_properties(icbsp_boot PROPERTIES SUFFIX ".elf")
target_include_directories(icbsp_boot PRIVATE ${ICBSP_BOOT_INCLUDES})
target_compile_definitions(icbsp_boot PRIVATE
    ${ICBSP_BOOT_DEFINES}
    ICBSP_BOOT_CONFIG_HEADER="${ICBSP_BOOT_CONFIG}"
)
target_compile_options(icbsp_boot PRIVATE -Os -g3 -Wall -Wextra -Wshadow -std=gnu11)
target_link_options(icbsp_boot PRIVATE
    "-T${CMAKE_CURRENT_SOURCE_DIR}/boot.ld"
    -nostartfiles
    "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/icbsp_boot.map"
)
set_property(TARGET icbsp_boot APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/boot.ld")
