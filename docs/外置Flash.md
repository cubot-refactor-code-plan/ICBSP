# 外置 QSPI Flash：小 Flash 型号的程序存放方式

STM32H750 / H7B0 / H730 片内只有 128 KB Flash。接入 ICBSP 后，Debug（-O0）构建通常装不下（H750 回归宿主实测 180 KB 链接失败）。
本库提供三种程序存放方式，用一个 CMake 缓存变量切换，不用重新生成 CubeMX、不用重跑 `integrate.py`：

| `ICBSP_FLASH_LAYOUT` | 程序在哪里运行 | 容量上限 | 速度 | 调试 |
| --- | --- | --- | --- | --- |
| `INTERNAL` | 片内 Flash（CubeMX 链接脚本，原样） | 片内 Flash | 基准 | 照旧 |
| `QSPI_RAM`（方案 A） | bootloader 从 QSPI 拷进 **AXI SRAM** 运行 | 约 400 KB（512 KB AXI SRAM 扣掉 `.dma_buffer`、`.noinit`） | 实测比片内 Flash 略快（sys_task 一轮 8 µs vs 10 µs） | 直接下载进 RAM，软件断点不限个数，不用写 Flash |
| `QSPI_XIP`（方案 B） | 在 QSPI 上**就地执行**（内存映射 0x90000000） | 芯片容量（W25Q128 = 16 MB，末尾 4 KB 除外） | 实测慢 7~9 倍（sys_task 一轮 75 µs，最长 169 µs；D-Cache 关着，常量也走 QSPI） | 只能硬件断点（8 个），GDB 自动选用 |

**回归宿主推荐方案 A**：时序和 H723 在片内 Flash 上跑接近，回归数据能直接对比；日常调试不碰 QSPI。代码超过 RAM 能给的空间再用方案 B。
只支持 **QUADSPI**（H742 / H743 / H750 / H753 等）；OCTOSPI（H723 / H730 / H7B0）暂不支持。已实测芯片：W25Q128（W25Q64 / W25Q256 只做过编译验证）。

---

## 1. 启用

在 CubeMX 里先打开 QUADSPI（单 Flash：CLK + NCS + IO0~IO3，Bank 1 或 Bank 2 都行），然后：

```bash
# 命令行：启用并把默认存放方式设为方案 A，芯片 W25Q128
python3 Middlewares/Third_Party/ICBSP/tools/integrate.py --external-flash QSPI_RAM --flash-chip W25Q128
# 可选：.ioc 里 QUADSPI 的参数、引脚速度改成与芯片一致（只影响应用自己用 QSPI；bootloader 不读这些）
python3 Middlewares/Third_Party/ICBSP/tools/integrate.py --fix-ioc qspi_params --regenerate
```

或者在 Web UI（`node Middlewares/Third_Party/ICBSP/tools/web_ui/server.mjs`）的“外置 Flash”一节勾选。`.ioc` 有 QUADSPI 且片内 Flash ≤ 128 KB、
或最近一次构建占片内 Flash ≥ 85%（链接溢出也算）时，脚本会给出 `[注意]`，Web UI 顶部会出现建议横幅。

`integrate.py` 写进宿主的东西：

| 文件 | 内容 |
| --- | --- |
| 根 `CMakeLists.txt` | `# <ICBSP-GEN icbsp-options>` 生成区：`include(<icbsp>/cmake/icbsp_flash.cmake)` + `icbsp_flash_layout(TARGET … DEFAULT QSPI_RAM CONFIG …)`，放在 `add_subdirectory(<icbsp>)` 之前 |
| `CMakePresets.json` | `RelWithDebInfo`，以及每个构建类型的 `-Internal` / `-QspiRam` / `-QspiXip`（只加缺的，原有预设不动） |
| `user/board/icbsp_flash_config.h` | 芯片容量、JEDEC、Bank、预分频、引脚与复用功能号（引脚取自 `.ioc`，复用号取自 CubeMX 生成的 `quadspi.c`）。整文件生成，重跑会覆盖 |
| `Core/Src/quadspi.c` | `USER CODE BEGIN QUADSPI_Init 0`：`#if defined(ICBSP_FLASH_LAYOUT_QSPI_XIP) return; #endif`——方案 B 时程序正在 QSPI 上执行，不能重新初始化 QUADSPI |
| `Flash/daplink_qspi.cfg` | OpenOCD：引脚 / QUADSPI 初始化 + `stmqspi` bank（只在外置 Flash 方案下用；日常的 `daplink.cfg` 照旧不注册外置 bank） |
| `Flash/OpenOCD_flash.*`、`JLink_flash.*` | 旧版不认存放方式时换成新版 |
| `.vscode/launch.json` | `cmsis-dap-debug (QSPI_RAM)`、`cmsis-dap-debug (QSPI_XIP)` 两个调试配置 |

## 2. 切换

```bash
cmake --preset Debug-QspiRam   && cmake --build --preset Debug-QspiRam    # 方案 A
cmake --preset Debug-QspiXip   && cmake --build --preset Debug-QspiXip    # 方案 B
cmake --preset RelWithDebInfo-Internal && cmake --build --preset RelWithDebInfo-Internal   # 片内（装得下时）
```

不带后缀的 `Debug` / `Release` 用生成区里的 `DEFAULT`。也可以 `-DICBSP_FLASH_LAYOUT=QSPI_XIP` 直接改缓存变量。

外置方案构建出来的东西（`build/<预设>/` 下）：

| 文件 | 用途 |
| --- | --- |
| `<工程名>.elf` | 方案 A：运行地址 = 加载地址 = AXI SRAM（调试器直接下载进 RAM）；方案 B：在 0x90000400，0x90000000 的镜像头已填好 |
| `<工程名>_qspi.bin` / `.hex` | 写进 QSPI 0x90000000 的镜像（1 KB 镜像头 + 程序） |
| `icbsp_boot/icbsp_boot.elf` | bootloader（片内 Flash 0x08000000，约 2 KB）；A、B 共用同一个，切换方案不用重烧 |

## 3. 烧录与调试

```bash
bash Flash/OpenOCD_flash.sh Debug-QspiRam         # bootloader（与板上不同才烧）+ QSPI 镜像
bash Flash/OpenOCD_flash.sh Debug-QspiRam --ram   # 方案 A：直接下载进 AXI SRAM 运行，不写任何 Flash（调试迭代用，掉电即失）
bash Flash/OpenOCD_flash.sh Debug-QspiXip --boot  # 强制重烧 bootloader
```

脚本从构建目录的 `CMakeCache.txt` 读 `ICBSP_FLASH_LAYOUT`，`INTERNAL` 时和以前一样。J-Link 脚本只支持片内 Flash（遇到外置方案会提示改用 OpenOCD）。

VS Code（cortex-debug）：

- **`cmsis-dap-debug (QSPI_RAM)`**：`monitor reset halt` → `load`（写进 RAM）→ 按 0x24000000 的向量表设 `sp` / `pc`，不经过 bootloader。
  软件断点不限个数（实测 11 个全部命中）。
- **`cmsis-dap-debug (QSPI_XIP)`**：用 `Flash/daplink_qspi.cfg`，GDB 的 `load` 经 `stmqspi` 写进 QSPI，复位后由 bootloader 跳转；GDB 对只读区
  自动改用硬件断点（最多 8 个）。`daplink_qspi.cfg` 在 GDB 连上时（`gdb-attach`）先复位并初始化 QSPI、探测芯片——否则 GDB 拿到的内存映射里
  0x90000000 不是 Flash，`load` 会退化成普通写内存而失败。
- 第一次用前先跑一遍 `OpenOCD_flash`（板上要有 bootloader）。

## 4. 原理

### 4.1 bootloader（`boot/`，寄存器级，不用 HAL）

1. HSI 64 MHz 运行，不开 PLL（应用的 `SystemClock_Config()` 可以照常从复位状态配置时钟）；只用 DTCM，不碰 AXI SRAM 的 `.noinit`
2. 配 QSPI 引脚和 QUADSPI；W25Q 软复位（66h / 99h）、核对 JEDEC 厂商与容量、四线使能位 QE 没置就写上、> 16 MB 进 4 字节地址
3. 内存映射（0xEB 四线读，模式字节 0xFF + 4 个空周期），MPU 第 13~15 区（见 4.3）
4. 校验 0x90000000 的镜像头（magic、头 CRC、各字段范围）和程序 CRC-32（CRC 外设，与 `tools/qspi_image.py` 的 zlib 算法一致）
5. 方案 A：程序拷到 0x24000000，退出内存映射、复位 QUADSPI、清 MPU 区，跳转；方案 B：保持内存映射和 MPU 区，跳转
6. 跳转前关 SysTick、清 NVIC 使能与挂起、关 I-Cache，设 VTOR / MSP

**启动状态**写在 `0x3800FFE0`（RAM_D3 末尾 32 字节，外置方案的链接脚本让出了这块），调试器直接读：

| 偏移 | 内容 |
| --- | --- |
| +0 | `0x544F4F42`（'BOOT'） |
| +8 | 状态：`0x10` 已跳到方案 A、`0x11` 已跳到方案 B；`2` QSPI 超时、`3` JEDEC 不符、`4` QE 写不进、`5` 没有镜像、`6` 镜像头字段越界、`7` CRC 不对、`8` 向量表不合理 |
| +12 | 细节（JEDEC 不符时是读到的 ID，CRC 不对时是算出来的 CRC，跳转时是程序地址） |
| +16 / +20 / +24 / +28 | JEDEC ID、镜像 layout、镜像大小、QUADSPI_CR |

```bash
openocd -f Flash/daplink.cfg -c init -c "mdw 0x3800FFE0 8" -c shutdown
```

### 4.2 镜像头（`boot/icbsp_boot.h`、`tools/qspi_image.py`）

0x90000000 起 32 字节（补 0xFF 到 1 KB），全部 uint32：magic `'ICBI'`、version、layout（1 = A，2 = B）、加载地址、向量表地址、程序大小、程序 CRC、头 CRC。
程序从 0x90000400 开始（满足 VTOR 的 1 KB 对齐）。方案 B 的链接脚本在 0x90000000 预留了 `.icbsp_image_header` 段，构建后由 `qspi_image.py`
把长度和 CRC 写回 ELF，所以调试器直接下载 ELF 也是有效镜像。

### 4.3 MPU（区号 13~15 归 bootloader，应用别用）

| 区 | 范围 | 属性 | 为什么 |
| --- | --- | --- | --- |
| 13 | 0x90000000，256 MB | 禁止访问、不可执行 | M7 会推测访问 Normal 区域；访问到芯片以外的地址会让 QUADSPI 把总线卡死 |
| 14 | 0x90000000，芯片容量 | 只读、可执行、写通 cache | 程序 / 常量所在 |
| 15 | 芯片最后 4 KB | 禁止访问 | **实测**：内存映射读芯片最后一个字时 QUADSPI 预取越过芯片末尾，总线永久挂死（调试器都停不下内核，只能复位）；有了这一区只会进 MemManage |

CubeMX 生成的 `MPU_Config()` 只改它自己的区号（默认第 0 区：0x60000000~0xDFFFFFFF 禁止访问），`HAL_MPU_Disable()` 不清区配置，
所以方案 B 下 13~15 区一直有效，**不用改 .ioc 的 MPU 设置**。方案 A 跳转前 bootloader 会把这三区清掉。

### 4.4 链接脚本（`templates/flash_layout/`，构建时生成到构建目录）

- 方案 A：向量表、代码、常量、`.dma_buffer`（运行地址 = 加载地址）、`.data` 的初值都在 AXI SRAM 且排在最前；NOLOAD 段（`.noinit`、`.bss`）排在后面，
  所以 bootloader 拷贝时不会覆盖 `.noinit` 里的 `fault_record`。`.data` / `.bss` / 堆栈仍在 DTCM。
- 方案 B：镜像头 + 代码 + 常量 + 初值在 QSPI，`.dma_buffer` / `.noinit` 在 AXI SRAM，`.data` / `.bss` / 堆栈在 DTCM。
- 两者都带 `.dma_buffer` / `.noinit`，库的链接脚本检查照常通过；堆 / 栈大小取自宿主原来的 CubeMX 链接脚本。

## 5. 限制与注意

- 应用不能用 MPU 第 13~15 区；方案 B 下应用不能初始化 QUADSPI（`quadspi.c` 的 USER CODE 已处理），也不能擦写 QSPI（擦写时要退出内存映射，
  而程序正在 QSPI 上执行）。方案 A 没有这个限制：应用里 `MX_QUADSPI_Init()` 照常执行，可以把 QSPI 当存储用（注意别覆盖 0x90000000 起的镜像）。
- QUADSPI 时钟 = HCLK3 / (预分频 + 1)：预分频按应用的最终 HCLK3（240 MHz）选，默认 2 → 80 MHz；`integrate.py` 会拒绝超过芯片上限的设置。
- 方案 A 的程序上限受 AXI SRAM 限制（约 400 KB）；超过时链接器报 `RAM` 溢出，换方案 B。
- OpenOCD 校验 ELF 时会对比 ld 映射进 0x20000000 的“ELF 头段”，在方案 A 的 `--ram` 下误报不一致，脚本因此不做 `verify_image`。
- AXI SRAM 带 ECC：调试器单独写一个 32 位字会留在 ECC 写缓冲里，复位后丢失——清 `fault_record` 时一次写 16 字节（`mww <地址> 0 4`）。
- Windows 的 `.bat` 与 Linux 的 `.sh` 逻辑相同，但只在 Linux 上实测过。
- 方案 A 在 AXI SRAM 里打软件断点绝大多数情况正常（实测 11 个同时命中），但偶尔 OpenOCD 报 `can't add breakpoint: unknown reason`、
  GDB 报 `Cannot access memory`（`step` 的临时断点上遇到过两次）；遇到时改用 `hbreak`（硬件断点），原因待查。

## 6. 实测记录

H750_FULL_TEST（STM32H750VBT6 + W25Q128JV，QUADSPI Bank 2：PB2 / PC11 / PE7~PE10）、CMSIS-DAP（Horco）、OpenOCD 0.12：
见 [bsp_test/external_flash_2026-11-10.md](bsp_test/external_flash_2026-11-10.md)。
