# templates：接入时复制到宿主工程的文件

`tools/integrate.py` 读取本目录自动完成接入；改模板后脚本随之生效。

| 文件 | 放到哪里 | 作用 |
| --- | --- | --- |
| `linker_sections.ld` | 宿主链接脚本 `SECTIONS { }` 里、`.data` 之前 | `.dma_buffer`（串口实例，DMA 可达的 AXI SRAM）与 `.noinit`（`fault_record`，复位保留） |
| `FreeRTOSConfig_user_code.h` | `Core/Inc/FreeRTOSConfig.h` 的 USER CODE 1 段 | `configASSERT` → `fault_on_assert()` |
| `freertos_user_code.c` | `Core/Src/freertos.c` 的 Includes / 4 / 5 / Init 段 | 栈溢出与分配失败钩子；`MX_FREERTOS_Init()` 调 `app_init()` |
| `hardware_config.h` | `user/board/` | 必选：唯一配置源，所有宏、功能开关与范围检查都在这里 |
| `user/board/board_cfg.*` | `user/board/` | 外设与设备实例、`board_init()` |
| `user/app/app_main.*` | `user/app/` | `app_init()` 与 `StartDefaultTask()` |
| `user/app/app_task.hpp` | `user/app/` | 示例任务声明（heartbeat / monitor） |
| `user/app/task/demo_task.cpp` | `user/app/task/` | 示例任务实现，不自动创建 |

`board_cfg.*` 用的是 `hfdcan1` / `huart1` / `htim12`，换成实际外设；没开的外设连同 `#include` 一起删。

`hardware_config.h` 是唯一配置源：所有宏都写成实际值，顶部包含 HAL / FreeRTOS，`HAL_FDCAN_MODULE_ENABLED` / `HAL_UART_MODULE_ENABLED` 未定义时会自动关闭 `CAN_RX_ENABLE` / `CAN_TX_ENABLE` / `UART_PATROL_ENABLE`。不再有默认值头文件。
SPI / BMI088 的写法在 `board_cfg.cpp` 注释里，开 SPI 后再取消注释。

宿主 CMake 只需要包含路径和 `user/**/*.cpp` 收集：

```cmake
add_subdirectory(third_party/stm32-framework)
target_include_directories(stm32_framework PRIVATE user/board)  # 库自身也要看到 hardware_config.h
target_link_libraries(${CMAKE_PROJECT_NAME} stm32_framework)
target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_20)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE user/board user/app user/module)
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE -u _printf_float)

file(GLOB_RECURSE USER_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/user/*.c ${CMAKE_CURRENT_SOURCE_DIR}/user/*.cpp)
target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${USER_SOURCES})
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE user)
```

