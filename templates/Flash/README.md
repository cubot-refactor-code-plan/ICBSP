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

两个脚本都依赖 `openocd`；J-Link 版本还要求 OpenOCD 带 `jlink` 适配器（`openocd -c "adapter list" -c "shutdown"` 可确认）。
`.sh` 需要可执行权限：`chmod +x Flash/*.sh`。

## 关于 SVD

这里**不带** `.svd` 文件。SVD 只用于 VS Code 的「外设寄存器」面板，与调试器无关（OpenOCD 和 J-Link 都要靠它）：

- 宿主 `Flash/` 下有 `*.svd` 时，`integrate.py` 会在 `.vscode/launch.json` 里写 `svdFile`
- 没有就不写，只在报告里提示一次，调试本身不受影响

SVD 可以从 STM32CubeIDE 的 `plugins/com.st.stm32cube.ide.mcu.debug*/resources/svd/`、
Keil 的 `STM32H7xx_DFP` 包，或 ST 官网下载，放到 `Flash/` 下即可。

## 本目录其他文件

| 文件 | 用途 |
| --- | --- |
| `daplink.cfg` | OpenOCD 的 CMSIS-DAP 配置（SWD + 目标芯片） |
| `jlink.cfg` | OpenOCD 的 SEGGER J-Link 配置（`adapter driver jlink` + SWD 4 MHz） |

相关配置：`.vscode/tasks.json`（烧录任务）、`.vscode/launch.json`（调试配置）。
