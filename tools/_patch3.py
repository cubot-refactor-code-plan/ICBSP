#!/usr/bin/env python3
"""一次性补丁 3：README 与 templates/README 补 .vscode / Flash 说明。跑完即可删除。"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
README = ROOT / "README.md"
TPL_README = ROOT / "templates" / "README.md"


def patch(path, pairs):
    raw = path.read_bytes()
    crlf = b"\r\n" in raw
    text = raw.decode("utf-8").replace("\r\n", "\n")
    for old, new in pairs:
        if text.count(old) != 1:
            sys.exit(f"{path.name}: 锚点出现 {text.count(old)} 次 -> {old[:50]!r}")
        text = text.replace(old, new, 1)
    path.write_bytes((text.replace("\n", "\r\n") if crlf else text).encode("utf-8"))
    print(f"已更新 {path}")


patch(
    README,
    [
        (
            "- 配置 clangd 的 compile_commands 目录\n",
            "- 配置 clangd 的 compile_commands 目录\n"
            "- 生成 / 合并 `.vscode/tasks.json`、`.vscode/launch.json`：工程名取 `CMAKE_PROJECT_NAME`，\n"
            "  `device` 与 OpenOCD target 按 `.ioc` 的 `Mcu.Name` 推出来（`STM32H723VGTx` → `STM32H723VG`）；\n"
            "  宿主已有的任务与调试配置不覆盖，只补缺失项\n"
            "- 复制 `Flash/` 烧录脚本：脚本运行时自己在 `build/` 下递归找 `.elf`，不用传路径；\n"
            "  宿主 `Flash/` 下没有 SVD 时 `launch.json` 不写 `svdFile`，只提示一次（外设寄存器面板不可用，调试本身不受影响）\n",
        )
    ],
)

patch(
    TPL_README,
    [
        (
            "| `user/app/task/demo_task.cpp` | `user/app/task/` | 示例任务实现，不自动创建 |\n",
            "| `user/app/task/demo_task.cpp` | `user/app/task/` | 示例任务实现，不自动创建 |\n"
            "| `vscode/settings.json`、`vscode/tasks.json`、`vscode/launch.json` | 宿主 `.vscode/` | 编译 / 烧录任务与调试配置，宿主没有才生成；有就只补缺失项 |\n"
            "| `Flash/OpenOCD_flash.*`、`Flash/JLink_flash.*`、`Flash/*.cfg` | 宿主 `Flash/` | 烧录脚本，运行时按工程名在 `build/` 下递归找 `.elf`；`.sh` 自动加可执行权限 |\n",
        ),
        (
            "target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE user)\n```\n",
            "target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE user)\n```\n"
            "\n"
            "## .vscode 与 Flash\n"
            "\n"
            "`vscode/` 三个文件里的占位符由 `integrate.py` 填：`{{PRESET}}` 取 `--preset`，`{{PROJECT}}` 取根\n"
            "`CMakeLists.txt` 的 `CMAKE_PROJECT_NAME`，`{{MCU}}` / `{{DEVICE}}` / `{{OPENOCD_TARGET}}` 按 `.ioc` 的\n"
            "`Mcu.Name` 推出来（`STM32H723VGTx` → device `STM32H723VG`、target `stm32h7x.cfg`）。\n"
            "\n"
            "宿主已经有这三个文件时，脚本按 `settings.json` 的键名、`tasks[].label`、`configurations[].name`\n"
            "补缺失项，原有的键、任务、调试配置以及注释都不动（JSONC 支持 `//`、`/* */` 与尾逗号）。\n"
            "\n"
            "SVD 不随本目录分发：只有宿主 `Flash/` 下真的有匹配的 `.svd`，`launch.json` 才写 `svdFile`；\n"
            "没有就去掉这一项并提示一次——外设寄存器面板不可用，断点、变量、内存、反汇编都不受影响。\n",
        ),
    ],
)
