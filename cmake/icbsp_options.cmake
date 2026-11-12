# ------------------------------------------------------------------------------
# icbsp：编译优化选项（integrate.py / Web UI 的“编译优化”生成）
#
#   include(<icbsp>/cmake/icbsp_options.cmake)
#   icbsp_optimization(TARGET ${CMAKE_PROJECT_NAME}
#                      Debug -Og RelWithDebInfo -O2 Release -Os
#                      VENDOR_OS)
#
# - 每个构建类型一个优化等级，作用于宿主目标、icbsp、CubeMX 生成的全部源码目标（HAL、FreeRTOS、USB…）。
#   用 target_compile_options 追加：它排在工具链文件的 CMAKE_<LANG>_FLAGS_<CONFIG>（如 -O0 -g3）之后，
#   GCC 以最后一个 -O 为准，所以不用改 CubeMX 生成的工具链文件；-g 级别保持不变。
# - VENDOR_OS：CubeMX 生成的源码目标（cmake/stm32cubemx 下的 HAL / FreeRTOS / USB 等）在所有构建类型下都用 -Os，
#   Debug 时自己的代码仍按上面的等级（例如 -O0），可以省下二三十 KB。
# - 没写的构建类型保持工具链文件原样。
# ------------------------------------------------------------------------------
include_guard(GLOBAL)

set(ICBSP_OPT_LEVELS -O0 -Og -O1 -O2 -O3 -Os)

function(icbsp_optimization)
    cmake_parse_arguments(ARG "VENDOR_OS" "TARGET;Debug;Release;RelWithDebInfo;MinSizeRel" "" ${ARGN})
    if(NOT ARG_TARGET)
        message(FATAL_ERROR "icbsp_optimization: 需要 TARGET（宿主可执行目标）")
    endif()
    set(_genex "")
    set(_desc "")
    foreach(_cfg Debug Release RelWithDebInfo MinSizeRel)
        if(ARG_${_cfg})
            if(NOT ARG_${_cfg} IN_LIST ICBSP_OPT_LEVELS)
                message(FATAL_ERROR "icbsp_optimization: ${_cfg} 的等级 ${ARG_${_cfg}} 无效，可选 ${ICBSP_OPT_LEVELS}")
            endif()
            string(APPEND _genex "$<$<CONFIG:${_cfg}>:${ARG_${_cfg}}>")
            list(APPEND _desc "${_cfg}=${ARG_${_cfg}}")
        endif()
    endforeach()
    list(JOIN _desc " " _desc)
    if(ARG_VENDOR_OS)
        string(APPEND _desc "，CubeMX 源码固定 -Os")
    endif()
    message(STATUS "icbsp: 编译优化 ${_desc}")
    cmake_language(EVAL CODE "
        cmake_language(DEFER DIRECTORY [[${CMAKE_SOURCE_DIR}]]
            CALL _icbsp_optimization_apply [[${ARG_TARGET}]] [[${_genex}]] [[${ARG_VENDOR_OS}]])")
endfunction()

function(_icbsp_optimization_apply target genex vendor_os)
    set(_lang "$<COMPILE_LANGUAGE:C,CXX>")
    set(_own ${target})
    if(TARGET icbsp)
        list(APPEND _own icbsp)
    endif()
    set(_vendor "")
    if(EXISTS "${CMAKE_SOURCE_DIR}/cmake/stm32cubemx")
        get_property(_vendor DIRECTORY "${CMAKE_SOURCE_DIR}/cmake/stm32cubemx" PROPERTY BUILDSYSTEM_TARGETS)
    endif()
    foreach(_t IN LISTS _own _vendor)
        get_target_property(_type ${_t} TYPE)
        if(_type STREQUAL "INTERFACE_LIBRARY" OR _type STREQUAL "UTILITY")
            continue()
        endif()
        if(vendor_os AND _t IN_LIST _vendor)
            target_compile_options(${_t} PRIVATE "$<${_lang}:-Os>")
        elseif(genex)
            target_compile_options(${_t} PRIVATE "$<${_lang}:${genex}>")
        endif()
    endforeach()
endfunction()
