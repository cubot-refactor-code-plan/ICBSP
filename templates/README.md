# templates：接入时复制到宿主工程的文件

`tools/integrate.py` 读取本目录自动完成接入；改模板后脚本随之生效。

| 文件 | 放到哪里 | 作用 |
| --- | --- | --- |
| `linker_sections.ld` | 宿主链接脚本 `SECTIONS { }` 里、`.data` 之前 | 可加载的 `.dma_buffer`（串口 / SPI 实例，DMA 可达的 AXI SRAM，`AT> FLASH`）与 `.noinit`（`fault_record`，复位保留） |
| `flash_layout/h7_qspi_ram.ld.in`、`h7_qspi_xip.ld.in` | 不复制：`cmake/icbsp_flash.cmake` 构建时 `configure_file` 到构建目录 | 外置 QSPI Flash 方案 A（拷进 AXI SRAM 运行）/ 方案 B（XIP）的链接脚本，见 `docs/外置Flash.md` |
| `gitignore` | 宿主根 `.gitignore` | 示例忽略项：宿主没有就整份复制，有就只追加缺的行（`# >>> icbsp` … `# <<< icbsp`） |
| `FreeRTOSConfig_user_code.h` | `Core/Inc/FreeRTOSConfig.h` 的 USER CODE 1 段 | `configASSERT` → `fault_on_assert()` |
| `freertos_user_code.c` | `Core/Src/freertos.c` 的 Includes / 4 / 5 / Init 段 | 栈溢出与分配失败钩子；`MX_FREERTOS_Init()` 调 `app_init()` |
| `user/board/hardware_config.h` | `user/board/` | 必选：唯一配置源，所有宏、功能开关与范围检查都在这里 |
| `user/board/board_cfg.*` | `user/board/` | `board_cfg.hpp` 放 inline 外设实例，`board_cfg.cpp` 放 `board_init()` |
| `user/board/board_instances.hpp` | `user/board/` | `ICBSP_CAN_INSTANCES` / `ICBSP_UART_INSTANCES` / `ICBSP_SPI_INSTANCES` 实例名清单，库的 HAL 回调与任务按它展开 |
| `user/app/app_main.*` | `user/app/` | `app_init()` 与默认任务体 `app_default_task()`（`freertos.c` 的 `StartDefaultTask()` 在 USER CODE 段里转调） |
| `user/app/app_task.hpp` | `user/app/` | 示例任务声明（heartbeat / monitor） |
| `user/app/task/demo_task.cpp` | `user/app/task/` | 示例任务实现，不自动创建 |
| `vscode/settings.json`、`vscode/tasks.json`、`vscode/launch.json` | 宿主 `.vscode/` | 编译 / 烧录任务与调试配置，宿主没有才生成；有就只补缺失项 |
| `vscode/launch_qspi.json` | 合并进宿主 `.vscode/launch.json` | 外置 Flash 启用时加的两个调试配置：`cmsis-dap-debug (QSPI_RAM)`、`cmsis-dap-debug (QSPI_XIP)` |
| `Flash/OpenOCD_flash.*`、`Flash/JLink_flash.*`、`Flash/*.cfg` | 宿主 `Flash/` | 烧录脚本，运行时按工程名在 `build/` 下递归找 `.elf`，按构建目录的 `ICBSP_FLASH_LAYOUT` 决定烧法；`.sh` 自动加可执行权限 |
| `Flash/daplink_qspi.cfg` | 宿主 `Flash/`（外置 Flash 启用时，按 `.ioc` 的 QSPI 引脚生成） | OpenOCD：QSPI 引脚 / QUADSPI 初始化 + `stmqspi` bank，写 QSPI 和调试方案 B 用 |
| `clang/.clangd` | 宿主根 `.clangd` | clangd 配置：`CompilationDatabase` 指向该预设的构建目录。缺了它，CubeMX 的 Cortex-M 工程在 clangd 里会满屏假错误。**`--query-driver` 不能写这里**（会被当编译器参数，报 `drv_unknown_argument`），它属于 `.vscode/settings.json` 的 `clangd.arguments` |
| `clang/compile_flags.yaml` | 同上，插入宿主已有的 `.clangd` 顶部 | 只含 `CompileFlags` 一段：宿主已手写 `.clangd`（有 InlayHints / Diagnostics 等）时用它合并，不动原有配置 |
| `clang/ignore_templates.yaml` | 同上，追加到 `.clangd` **末尾**（自带 `---` 分隔） | 让库的 `templates/` 不用宿主编译数据库、不进后台索引、不报诊断：模板与宿主有同名文件（`board_cfg.hpp` 等），留着会干扰补全与跳转 |
| `clang/ignore_thirdparty.yaml` | 同上，追加到 `.clangd` 末尾 | 屏蔽 CubeMX 的 `Drivers/`（CMSIS + HAL）与 `Middlewares/Third_Party/FreeRTOS/` 的诊断；**保留编译数据库与索引**，跳进实现不受影响 |
| `clang/ignore_core_inc.yaml` | 同上，追加到 `.clangd` 末尾 | 屏蔽 CubeMX 生成的 `Core/Inc/`（`main.h`、`stm32h7xx_hal_conf.h` 等）的诊断；不含 `Core/Src`，那里的 USER CODE 保留诊断 |

`.clang-format` 不在本目录：直接复制库根目录的那一份（唯一来源，避免两处风格文件不同步）。
它带 `Language: Cpp`，只作用于 C++，CubeMX 生成的 `Core/Src/*.c` 不会被格式化。

`board_cfg.*` 用的是 `hfdcan1` / `huart1` / `htim12`，换成实际外设；没开的外设连同 `#include` 一起删。新增实例时同时改 `board_instances.hpp`，否则 HAL 回调找不到它。

`hardware_config.h` 是唯一配置源：所有宏都写成实际值，顶部包含 HAL / FreeRTOS，`HAL_FDCAN_MODULE_ENABLED` / `HAL_UART_MODULE_ENABLED` 未定义时会自动关闭 `ICBSP_CAN_RX_ENABLE` / `ICBSP_CAN_TX_ENABLE` / `ICBSP_UART_PATROL_ENABLE`。不再有默认值头文件。
SPI / BMI088 的写法在 `board_cfg.hpp` 注释里，开 SPI 后再取消注释；外设实例都是 `inline` 变量，`board_cfg.cpp` 只放 `board_init()`。

宿主 CMake 只需要包含路径和 `user/**/*.cpp` 收集：

```cmake
add_subdirectory(Middlewares/Third_Party/ICBSP)
target_include_directories(icbsp PRIVATE user/board)  # 库自身也要看到 hardware_config.h
target_link_libraries(${CMAKE_PROJECT_NAME} icbsp)
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
`Mcu.Name` 推出来：`STM32H723VGTx` → device `STM32H723VG`、`STM32H743VITx` → `STM32H743VI`、
`STM32H750VBTx` → `STM32H750VB`、`STM32F407VGTx` → device `STM32F407VG`；OpenOCD target 按系列：
H7 按 Flash 代号，2 MB 的 `I` 料用 `stm32h7x_dual_bank.cfg`，其余用 `stm32h7x.cfg`（H745/H747/H755/H757
双核还要加 `DUAL_CORE 1`）；F4 统一用 `stm32f4x.cfg`。

宿主已经有这三个文件时，脚本按 `settings.json` 的键名、`tasks[].label`、`configurations[].name`
补缺失项，原有的键、任务、调试配置以及注释都不动（JSONC 支持 `//`、`/* */` 与尾逗号）。

SVD 不随本目录分发：只有宿主 `Flash/` 下真的有匹配的 `.svd`，`launch.json` 才写 `svdFile`；
没有就去掉这一项并提示一次——外设寄存器面板不可用，断点、变量、内存、反汇编都不受影响。

