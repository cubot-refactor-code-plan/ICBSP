# 外置 QSPI Flash 方案实测（2026-11-10）

按 `undo.md` 阶段 6“小 Flash 型号的外置 Flash 支持”，在 H750 回归宿主上测方案 A（`QSPI_RAM`）与方案 B（`QSPI_XIP`）。
范围按约定：外置 Flash 专项 + 冒烟，不移植 MC02 的 BSP 回归套件。原理与用法见 [../外置Flash.md](../外置Flash.md)。

## 环境

- 板：H750_FULL_TEST（STM32H750VBT6，HSE 8 MHz，480 MHz / HCLK 240 MHz），外置 W25Q128JV（JEDEC `EF 40 18`）接 QUADSPI Bank 2：
  PB2 CLK、PC11 BK2_NCS、PE7~PE10 BK2_IO0~IO3。
- 探针：CMSIS-DAP（Horco v0.2，SN 132004251478），OpenOCD 0.12.0+dev；arm-none-eabi-gcc 15.3。
- 库：`phase2` 分支 `ac99b10` / `85dbbf0`（本次两个提交）。宿主用 `integrate.py --external-flash QSPI_RAM --flash-chip W25Q128` 接入。
- 宿主配置（测试过程中的调整，见“宿主侧发现”）：D-Cache 关、I-Cache 开；`defaultTask` 栈 512 字；BMI088 设备暂时从选择里去掉（SPI6 卡住，见下）。
- QSPI 时钟：bootloader 阶段 HSI 64 MHz / 3 ≈ 21 MHz，应用阶段 240 MHz / 3 = 80 MHz（`ICBSP_QSPI_PRESCALER = 2`）。

## 编译

| 项 | 结果 |
| --- | --- |
| 宿主 9 个预设（Debug / RelWithDebInfo / Release × Internal / QspiRam / QspiXip） | 8 个 0 warning；`Debug-Internal`（-O0）片内 Flash 溢出，符合预期 |
| bootloader | 1976 B（片内 Flash 1.5%） |
| Debug（-O0）大小 | 方案 A：AXI SRAM 138 KB；方案 B：QSPI 136 KB（去掉 BMI088 之后） |
| `tools/tests/compile_test.py` | 3 / 3 通过：新增 `h750_qspi`（CubeMX 无界面生成 → `integrate.py` 开外置 Flash → Debug-QspiRam、Debug-QspiXip、RelWithDebInfo-Internal 均 0 warning），`mc02_full` / `mc02_alt` 不受影响 |
| `tools/tests`（unittest） | 35 / 35 通过（新增 `test_flash.py` 22 项：分析 / 生成 / 选择校验 / `#` 生成区 / 宿主计划步骤 / `qspi_image.py`） |
| `icbsp_optimization` | 宿主编译参数里自己的等级排在工具链的 `-O0` 之后（`-O0 -Og`、HAL `-O0 -Os`）；片内 Debug + `VENDOR_OS` 95 KB、Debug=-Og 90 KB，都装得下 |

## 实板

| 编号 | 内容 | 结果 |
| --- | --- | --- |
| H1 | OpenOCD `stmqspi` 探测、擦写校验 | 通过：`win w25q128fv/jv`、16 MiB；镜像擦写 + verify（57 KiB/s 写、≈ 900 KiB/s 校验）；内存映射读 0x90000000 正常 |
| H2 | 方案 A / 方案 B 烧录、复位、**各掉电 3 次**（用户操作） | 通过：启动状态 `0x10`（跳到 0x24000000），1000 tick/s；掉电后 `fault_record` 被上电清零、`boot_rsr = 0x00FE0000`（含 POR），应用照常运行；方案 B 同样（状态 `0x11`，预先制造的 MemManage 记录被上电清零） |
| H3 | 方案 A 反例 | 通过：改镜像一个字节 → 状态 7（BAD_CRC，细节 = 算出的 CRC），应用不启动；擦掉镜像头扇区 → 状态 5（NO_IMAGE）；重烧恢复 `0x10`。正常镜像能过，说明 CRC 外设配置与 zlib 一致 |
| H4 | 方案 A 调试迭代 | 通过：`OpenOCD_flash.sh --ram` 直接下载进 AXI SRAM 运行（不写 Flash）；GDB 下进 RAM 后打 11 个**软件**断点（超过 8 个硬件断点）全部依次命中 |
| H5 | 方案 B 烧录与运行 | 通过：状态 `0x11`（跳到 0x90000400），PC 在 0x9001xxxx；`hqspi.State` 仍是 RESET（`MX_QUADSPI_Init()` 已跳过）；QUADSPI_CR = 0x02000391、CCR = 0x0F10EDEB（0xEB 四线内存映射、4 个空周期）；应用的 `SystemClock_Config()` 正常（PLL1 480 MHz，HCLK 240 MHz → QSPI 80 MHz）；VS Code 同款流程：GDB `load` 经 stmqspi 写 QSPI，`main` / `MX_TIM6_Init` 自动用硬件断点命中 |
| H6 | 方案 B 健壮性 | MPU：应用 `MPU_Config()` 之后第 13~15 区仍在（RASR 0x10000037 / 0x0602002F / 末尾 4 KB 禁止）；读 0x91000000 → MemManage（CFSR 0x82，MMFAR 0x91000000，`fault_record` kind = 5，处理函数在 QSPI 上执行）；读 0x90FFE000 正常。浸泡：见下 |
| H7 | 性能（`sys_task_stats`，DWT 计时的 sys_task 一轮） | 见下表 |
| H8 | `fault_record` 复位保留 | 通过：A、B 下人为制造 MemManage 后复位，记录仍在（bootloader 拷贝不覆盖 `.noinit`），应用重新运行 |
| H9 | 只用预设切换 | 通过：Internal → A → B → A(Debug) → B(Debug) 依次烧录都能启动；从 Internal 切回外置方案时脚本发现 bootloader 被覆盖，自动重烧 |
| H10 | EmberProbe | `variables_read` 在 A / B 下都能读 RAM 变量，但按工作区配置的 ELF（`build/Debug/…`）解析地址，与板上实际的预设不一致时不报警（`firmwareConsistency: unknown`）——外置方案要把 EmberProbe 的 ELF 指到 `build/<预设>/`。`flash_program` 只接受 interface / target 脚本，不能用 `daplink_qspi.cfg`，写不了 QSPI（按接口判断，未实际执行） |
| — | USB CDC 冒烟 | 通过（复测）：第一次枚举失败（`device not accepting address, error -71`，片内方案同样），原因是 `.ioc` 的 USB 时钟 PLL3Q = 60 MHz；用户把 DIVQ3 改成 20（48 MHz）后，方案 A、方案 B、片内三种都枚举成 `0483:5740 STM32 Virtual ComPort`（`cdc_acm` → `/dev/ttyACM1`），应用 1000 tick/s、无故障 |

### H7 性能

| 构建 | avg | max |
| --- | --- | --- |
| RelWithDebInfo（-O2）片内 Flash | 10 µs | 11 µs |
| RelWithDebInfo 方案 A | **8 µs** | 9 µs |
| RelWithDebInfo 方案 B | **75 µs** | 169 µs |
| Debug（-O0）方案 A | 21 µs | 23 µs |
| Debug（-O0）方案 B | 108 µs | 239 µs |

方案 A 比片内 Flash 还快一点（AXI SRAM 0 等待 vs Flash 4 个等待周期）。方案 B 慢 7~9 倍，比事先按 cache 命中率估计的严重：sys_task 每 10 ms 跑一轮，
其间 I-Cache 已被其他任务冲掉；D-Cache 关着，放在 QSPI 里的常量每次都走 QSPI。**时序敏感的回归只用方案 A。**

### H6 浸泡（方案 B，Debug，30 min）

- 第一次（20:15 起）：前 12 分钟 1000 tick/s、`fault_record` 无；20:27 被另一个调试客户端（同时间 `build/Debug` 被重新编译，可能是 VS Code / EmberProbe）
  复位并停在 OpenOCD 的算法代码里（PC 0x2000002E、LR 0xFFFFFFFF、QUADSPI_CR 回到复位值），不算固件问题。
- 第二次（20:53 起，独占探针）：通过。30 次采样（每分钟一次，不停核读）全部 1000 ± 2 tick/s，31 分钟 tick 到 1864017，`fault_record` 一直为空，启动状态一直是 `0x11`（没有复位），FreeRTOS 堆最低余量 904 B 不变。原始记录：`raw/external_flash_soak_xip_2026-11-10.log`

## 实测中发现并修掉的问题（库）

1. **`templates/linker_sections.ld` 的 `__dma_buffer_load` 没算段内 `ALIGN(32)` 的偏移**：`.dma_buffer` 不在 RAM 区开头时（方案 A 里它排在代码后面），
   `__dma_buffer_start` 比段首晚 0x18 字节而加载地址还是段首，启动拷贝错位，`bsp_uart1._huart` 变成空指针，`init()` 返回 `BAD_ARG`。
   改成 `LOADADDR(.dma_buffer) + (__dma_buffer_start - ADDR(.dma_buffer))`（对齐时结果不变），`integrate.py` 会等价改写老宿主的这一行。
2. **内存映射读 QSPI 芯片最后一个字会挂死总线**：QUADSPI 预取越过芯片末尾，内核停在那次读上，连调试器都停不下（DHCSR.S_HALT 一直是 0），
   只能复位。bootloader 加 MPU 第 15 区（芯片最后 4 KB 禁止访问）后变成 MemManage；镜像也不允许用到这 4 KB。
3. **GDB 连上时 stmqspi 还没探测到芯片**：`daplink_qspi.cfg` 只在 `reset-init` 里初始化 QSPI，GDB 一连上就要内存映射，此时 bank 探测超时，0x90000000 不算 Flash，
   `load` 退化成普通写内存而失败。加 `gdb-attach` 事件：先 `reset init` 再 `flash probe`。
4. **`--ram` 路径**：OpenOCD 的 `verify_image` 会比对 ld 映射进 0x20000000 的“ELF 头段”而误报不一致；寄存器名是 `xpsr`（小写）。都已改。
5. **`qspi_image.py`**：ld 会把 ELF 头映射进第一个 PT_LOAD，按 segment 取数据会把头字节当成程序 —— 改为按有内容的 ALLOC section 取。

## 宿主侧发现（不是外置 Flash 方案的问题，留给宿主处理）

- **D-Cache**：测试期间宿主打开过 D-Cache，与库“D-Cache 关闭”的约定冲突（`dcache_off` 规则报错）；按用户决定改回关闭（I-Cache 保持开）。
- **`defaultTask` 栈**：CubeMX 把 `MX_USB_DEVICE_Init()` 放在 `defaultTask`，-O0 下 128 字栈溢出（tick 1 就进 `fault_on_stack_overflow`）；
  256 字时只剩 4 字余量，改成 512 字。以前 Debug 构建装不进片内 Flash，从没在板上跑过，所以没暴露。
- **SPI6 / BMI088**：片内与方案 A 都卡在 `bmi088.init()` 的 SPI6 传输里（SPI6_SR = 0x10012：CSTART 已置、TXTF，收不到 RXP），调度器启动前
  HAL 超时又依赖被屏蔽的 tick，所以永远等下去。测试时按用户决定先从选择里去掉 BMI088；SPI6 时钟 / 接线待查。
- **USB 时钟**：`.ioc` 里 USB 取 PLL3Q，HSE 8 MHz / M 1 × N 120 / Q 16 = 60 MHz；USB FS 要 48 MHz。主机端枚举失败（error -71），片内方案同样。用户已改成 Q = 20（48 MHz），复测三种方案都能枚举。
- **调试器单写 AXI SRAM**：ECC 的写缓冲让单个 32 位写在复位后丢失，清 `fault_record` 要一次写 16 字节。

## 未做

- Windows `.bat` 脚本只做了逻辑同步，没在 Windows 上跑。
- Web UI 用服务端 API 走查（扫描、选项改动后的预览 diff 正确）；页面本身未在浏览器里点过。
