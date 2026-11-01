# templates：接入时复制到宿主工程的文件

`tools/integrate.py` 读取本目录自动完成接入；改模板后脚本随之生效。

| 文件 | 放到哪里 | 作用 |
| --- | --- | --- |
| `linker_sections.ld` | 宿主链接脚本 `SECTIONS { }` 里、`.data` 之前 | 可加载的 `.dma_buffer`（串口 / SPI 实例，DMA 可达的 AXI SRAM，`AT> FLASH`）与 `.noinit`（`fault_record`，复位保留） |
| `FreeRTOSConfig_user_code.h` | `Core/Inc/FreeRTOSConfig.h` 的 USER CODE 1 段 | `configASSERT` → `fault_on_assert()` |
| `freertos_user_code.c` | `Core/Src/freertos.c` 的 Includes / 4 / 5 / Init 段 | 栈溢出与分配失败钩子；`MX_FREERTOS_Init()` 调 `app_init()` |
| `user/board/hardware_config.h` | `user/board/` | 必选：唯一配置源，所有宏、功能开关与范围检查都在这里 |
| `user/board/board_cfg.*` | `user/board/` | `board_cfg.hpp` 放 inline 外设实例，`board_cfg.cpp` 放 `board_init()` |
| `user/board/board_instances.hpp` | `user/board/` | `BSP_CAN_INSTANCES` / `BSP_UART_INSTANCES` / `BSP_SPI_INSTANCES` 实例名清单，库的 HAL 回调与任务按它展开 |
| `user/app/app_main.*` | `user/app/` | `app_init()` 与 `StartDefaultTask()` |
| `user/app/app_task.hpp` | `user/app/` | 示例任务声明（heartbeat / monitor） |
| `user/app/task/demo_task.cpp` | `user/app/task/` | 示例任务实现，不自动创建 |
| `vscode/settings.json`、`vscode/tasks.json`、`vscode/launch.json` | 宿主 `.vscode/` | 编译 / 烧录任务与调试配置，宿主没有才生成；有就只补缺失项 |
| `Flash/OpenOCD_flash.*`、`Flash/JLink_flash.*`、`Flash/*.cfg` | 宿主 `Flash/` | 烧录脚本，运行时按工程名在 `build/` 下递归找 `.elf`；`.sh` 自动加可执行权限 |

`board_cfg.*` 用的是 `hfdcan1` / `huart1` / `htim12`，换成实际外设；没开的外设连同 `#include` 一起删。新增实例时同时改 `board_instances.hpp`，否则 HAL 回调找不到它。

`hardware_config.h` 是唯一配置源：所有宏都写成实际值，顶部包含 HAL / FreeRTOS，`HAL_FDCAN_MODULE_ENABLED` / `HAL_UART_MODULE_ENABLED` 未定义时会自动关闭 `CAN_RX_ENABLE` / `CAN_TX_ENABLE` / `UART_PATROL_ENABLE`。不再有默认值头文件。
SPI / BMI088 的写法在 `board_cfg.hpp` 注释里，开 SPI 后再取消注释；外设实例都是 `inline` 变量，`board_cfg.cpp` 只放 `board_init()`。

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

## .vscode 与 Flash

`vscode/` 三个文件里的占位符由 `integrate.py` 填：`{{PRESET}}` 取 `--preset`，`{{PROJECT}}` 取根
`CMakeLists.txt` 的 `CMAKE_PROJECT_NAME`，`{{MCU}}` / `{{DEVICE}}` / `{{OPENOCD_TARGET}}` 按 `.ioc` 的
`Mcu.Name` 推出来（`STM32H723VGTx` → device `STM32H723VG`、target `stm32h7x.cfg`）。

宿主已经有这三个文件时，脚本按 `settings.json` 的键名、`tasks[].label`、`configurations[].name`
补缺失项，原有的键、任务、调试配置以及注释都不动（JSONC 支持 `//`、`/* */` 与尾逗号）。

SVD 不随本目录分发：只有宿主 `Flash/` 下真的有匹配的 `.svd`，`launch.json` 才写 `svdFile`；
没有就去掉这一项并提示一次——外设寄存器面板不可用，断点、变量、内存、反汇编都不受影响。

