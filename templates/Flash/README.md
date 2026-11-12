# Flash：烧录脚本

`tools/integrate.py` 把本目录复制到宿主的 `Flash/`（已存在的不覆盖）。脚本会自动找
`build/` 下的 `.elf`，不需要传路径，也不需要把 `.elf` 拷来拷去。

| 探针 | Linux | Windows | 连接配置 |
| --- | --- | --- | --- |
| CMSIS-DAP / DAPLink | `OpenOCD_flash.sh` | `OpenOCD_flash.bat` | `daplink.cfg` |
| SEGGER J-Link | `JLink_flash.sh` | `JLink_flash.bat` | `jlink.cfg`（OpenOCD 的 `jlink` 适配器） |

## 找 ELF 的顺序

1. `build/<BUILD_TYPE>/<工程名>.elf`（`BUILD_TYPE` 省略时用 `{{PRESET}}`）
2. `build/Debug/<工程名>.elf`、`build/Release/<工程名>.elf`
3. 递归找 `build/**/*.elf`；有多个时优先带工程名的那个，并打印全部候选（避免拿错 Debug / Release）

工程名来自根 `CMakeLists.txt` 的 `CMAKE_PROJECT_NAME`，不是文件夹名。

## 用法

```bash
bash Flash/OpenOCD_flash.sh          # 或 ./Flash/OpenOCD_flash.sh
bash Flash/JLink_flash.sh Release    # 指定构建类型
```

```bat
Flash\OpenOCD_flash.bat
Flash\JLink_flash.bat Release
```

Windows 上也可直接用 VS Code 任务 `OpenOCD_flash` / `JLink_flash`（`tasks.json` 里按平台自动选 `.sh` 或 `.bat`）。

两个脚本都依赖 `openocd`，**建议 0.12 及以上**（实测用的是 0.12；0.11 没验证过，脚本检测到旧版会提示）。
系统包管理器装的常常是 0.11，另装了新版时用环境变量 `OPENOCD` 指定：`OPENOCD=/usr/local/bin/openocd bash Flash/OpenOCD_flash.sh`（Windows：`set OPENOCD=...`）。
J-Link 版本还要求 OpenOCD 带 `jlink` 适配器（`openocd -c "adapter list" -c "shutdown"` 可确认）。
`.sh` 需要可执行权限：`chmod +x Flash/*.sh`。

## 关于 SVD

这里**不带** `.svd` 文件。SVD 只用于 VS Code 的「外设寄存器」面板，与调试器无关（OpenOCD 和 J-Link 都不需要它）：

- 宿主 `Flash/` 下有 `*.svd` 时，`integrate.py` 会在 `.vscode/launch.json` 里写 `svdFile`
- 没有就不写，只在报告里提示一次，调试本身不受影响

SVD 可以从 STM32CubeIDE 的 `plugins/com.st.stm32cube.ide.mcu.debug*/resources/svd/`、
Keil 的 `STM32H7xx_DFP` 包，或 ST 官网下载，放到 `Flash/` 下即可。

## 本目录其他文件

| 文件 | 用途 |
| --- | --- |
| `daplink.cfg` | OpenOCD 的 CMSIS-DAP 配置（SWD + 目标芯片）。部分探针（Horco v0.2）bulk 端点不响应，要取消 `cmsis-dap backend hid` 的注释 |
| `jlink.cfg` | OpenOCD 的 SEGGER J-Link 配置（`adapter driver jlink` + SWD 4 MHz） |
| `daplink_qspi.cfg` | 外置 QSPI Flash 方案专用：`reset-init` / `gdb-attach` 时配好 QSPI 引脚与 QUADSPI，注册 `stmqspi` bank。占位符 `{{QSPI_INIT}}` 由 `integrate.py` 按 `.ioc` 生成 |

## 外置 QSPI Flash（`ICBSP_FLASH_LAYOUT` = `QSPI_RAM` / `QSPI_XIP`）

`OpenOCD_flash.*` 从构建目录的 `CMakeCache.txt` 读存放方式：外置方案时用 `daplink_qspi.cfg`，bootloader（`icbsp_boot/icbsp_boot.elf`）
与板上不同才烧（`--boot` 强制），QSPI 写 `<工程名>_qspi.bin` 到 0x90000000；`--ram`（只用于 `QSPI_RAM`）直接下载进 AXI SRAM 运行、不写 Flash。
J-Link 脚本只支持片内 Flash。详见 `docs/外置Flash.md`。

下面“实测经验”里“不要注册外置 Flash bank”的经验对 `daplink.cfg` 仍然成立：外置 bank 只放在 `daplink_qspi.cfg` 里，并且它在 `reset-init` 与 `gdb-attach`
时先初始化 QSPI 再探测芯片，所以不会出现 MC02 上那种访问未初始化的外置 Flash 把调试口卡死的情况（H750 + W25Q128 实测）。

## 实测经验（DM-MC02 / STM32H723）

- **不要注册外置 Flash bank**（OCTOSPI2 上的 W25Q64 等）：注册后 OpenOCD 在 `program` / `reset` 时会访问 OCTOSPI2，
  固件没有重新初始化它时会把调试口卡死，只能重新上电。
- OpenOCD 用 0.12 及以上（实测版本）。常见情况是系统包为 0.11、另装的 0.12 在 `/usr/local/bin`，用 `OPENOCD` 环境变量指定。
- 烧录前 OpenOCD 会在目标上跑一段 CRC 校验程序，板上原来的固件若接了 `fault_on_exception()`，可能记下一条假的 HardFault
  （BFAR = 0x04C11DB7，即 CRC-32 多项式；tick 0），复位后看 `fault_record` 时注意区分。

相关配置：`.vscode/tasks.json`（烧录任务）、`.vscode/launch.json`（调试配置）。
