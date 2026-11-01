#!/usr/bin/env python3
"""一次性补丁 4：芯片识别支持 H743 / H750 / H745 等系列（target 按 Flash 容量选、SVD 按参考手册借）。
跑完即可删除。
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PATH = ROOT / "tools" / "integrate.py"
README = ROOT / "README.md"
TPL_README = ROOT / "templates" / "README.md"

OLD_PICK = '''def pick_svd(paths, family):
    hits = [p for p in map(Path, paths) if family and p.stem.upper().startswith(family)]
    # 名字最短的最贴近家族级 SVD（STM32H723.svd 优于 STM32H723xx.svd 之类）
    return sorted(hits, key=lambda p: (len(p.stem), p.as_posix()))[0] if hits else None
'''

NEW_PICK = '''# 同一份参考手册的 H7 子系列：外设寄存器布局一致，SVD 可以互相借用
H7_SVD_GROUPS = (
    ("H742", "H743", "H750", "H753"),
    ("H723", "H725", "H730", "H733", "H735"),
    ("H745", "H747", "H755", "H757"),
    ("H7A0", "H7A3", "H7B0", "H7B3"),
)
# 双核 H7：OpenOCD 还要 set DUAL_CORE 1
H7_DUAL_CORE = ("H745", "H747", "H755", "H757")


def svd_names(group, family):
    """SVD 匹配顺序：先本型号，再同参考手册的兄弟型号"""
    for members in H7_SVD_GROUPS:
        if group in members:
            return [family] + [f"STM32{member}" for member in members if member != group]
    return [family]


def pick_svd(paths, prefixes):
    """按 prefixes 的顺序找 SVD，返回 (路径, 命中的前缀) 或 None"""
    files = list(map(Path, paths))
    for prefix in prefixes:
        hits = [p for p in files if prefix and p.stem.upper().startswith(prefix)]
        if hits:
            # 名字最短的最贴近家族级 SVD（STM32H723.svd 优于 STM32H723VGTx.svd 之类）
            return sorted(hits, key=lambda p: (len(p.stem), p.as_posix()))[0], prefix
    return None
'''

OLD_DETECT = '''    if mcu:
        info["MCU"] = mcu
        # STM32H723VGTx → STM32H723VG：J-Link 的 device 名去掉封装与温度代号
        name = mcu.upper()
        if name.endswith("X"):
            name = name[:-1]
        info["DEVICE"] = name[:11]
        family = name[:9]
    else:
        family = ""
        rep.warn(step, "没找到 .ioc 的 Mcu.Name：launch.json 的 device 留成 STM32H7xx，需要手动改成实际型号")

    hit = pick_svd(glob.glob(str(host / "Flash/*.svd")), family)
    if hit:
        info["SVD"] = hit.name
        rep.ok("SVD", f"宿主有 Flash/{hit.name}：launch.json 写 svdFile，外设寄存器面板可用")
    elif family:
        elsewhere = [p for pat in ("*.svd", ".vscode/*.svd", "Docs/**/*.svd") for p in glob.glob(str(host / pat), recursive=True)]
        local_hit = pick_svd(elsewhere, family)
        system_hit = None if local_hit else pick_svd([p for pat in SVD_SYSTEM_GLOBS for p in glob.glob(os.path.expanduser(pat))], family)
        if local_hit:
            rep.warn("SVD", f"宿主有 {local_hit.relative_to(host).as_posix()}，但不在 Flash/ 下：移到 Flash/ 后重跑即可启用外设寄存器面板")
        elif system_hit:
            rep.warn("SVD", f"宿主 Flash/ 下没有 SVD；本机有 {system_hit}，复制到 Flash/ 后重跑即可（不复制也不影响调试）")
        else:
            rep.warn("SVD", "宿主 Flash/ 下没有 SVD：launch.json 不写 svdFile，外设寄存器面板不可用（断点、变量、内存、反汇编不受影响）")
    return info
'''

NEW_DETECT = '''    family = group = ""
    if mcu:
        info["MCU"] = mcu
        # 型号位：STM32 + 子系列 4 位 + 引脚 1 位 + Flash 1 位 + 封装 1 位 + 温度 1 位
        #   STM32H723VGTx → STM32H723VG（J-Link device）
        #   STM32H743VITx → STM32H743VI
        #   STM32H750VBTx → STM32H750VB
        name = mcu.upper()
        if name.endswith("X"):
            name = name[:-1]
        info["DEVICE"] = name[:11]
        family = name[:9]
        group = name[5:9]
        if group in H7_DUAL_CORE:
            rep.warn(step, f"{family} 是双核：OpenOCD 还要在 launch.json 的 openOCDLaunchCommands 里加 set DUAL_CORE 1；J-Link 用 device 即可")
        if not group.startswith("H7"):
            rep.warn(step, f"{family} 不是 STM32H7：本库只支持 H7，OpenOCD target 用 stm32h7x.cfg 兜底，需手动改")
        # 2 MB 的 H7（Flash 代号 I）是双 bank，OpenOCD 要换 target
        if name[10:11] == "I":
            info["OPENOCD_TARGET"] = "stm32h7x_dual_bank.cfg"
    else:
        rep.warn(step, "没找到 .ioc 的 Mcu.Name：launch.json 的 device 留成 STM32H7xx，需要手动改成实际型号")

    names = svd_names(group, family) if family else []
    hit = pick_svd(glob.glob(str(host / "Flash/*.svd")), names)
    if hit:
        path, prefix = hit
        info["SVD"] = path.name
        if prefix == family:
            rep.ok("SVD", f"宿主有 Flash/{path.name}：launch.json 写 svdFile，外设寄存器面板可用")
        else:
            rep.warn("SVD", f"宿主没有 {family}.svd，用 Flash/{path.name} 代替（同参考手册，寄存器布局一致）：launch.json 写 svdFile")
    elif names:
        elsewhere = [p for pat in ("*.svd", ".vscode/*.svd", "Docs/**/*.svd") for p in glob.glob(str(host / pat), recursive=True)]
        local_hit = pick_svd(elsewhere, names)
        system_hit = None if local_hit else pick_svd([p for pat in SVD_SYSTEM_GLOBS for p in glob.glob(os.path.expanduser(pat))], names)
        if local_hit:
            rep.warn("SVD", f"宿主有 {local_hit[0].relative_to(host).as_posix()}，但不在 Flash/ 下：移到 Flash/ 后重跑即可启用外设寄存器面板")
        elif system_hit:
            rep.warn("SVD", f"宿主 Flash/ 下没有 SVD；本机有 {system_hit[0]}，复制到 Flash/ 后重跑即可（不复制也不影响调试）")
        else:
            rep.warn("SVD", "宿主 Flash/ 下没有 SVD：launch.json 不写 svdFile，外设寄存器面板不可用（断点、变量、内存、反汇编不受影响）")
    return info
'''

OLD_README = """  `device` 与 OpenOCD target 按 `.ioc` 的 `Mcu.Name` 推出来（`STM32H723VGTx` → `STM32H723VG`）；"""
NEW_README = """  `device` 与 OpenOCD target 按 `.ioc` 的 `Mcu.Name` 推出来（`STM32H723VGTx` → `STM32H723VG`，
  `STM32H743VITx` → `STM32H743VI`；2 MB 的 `I` 料用 `stm32h7x_dual_bank.cfg`，其余 `stm32h7x.cfg`）；"""

OLD_TPL = """`CMakeLists.txt` 的 `CMAKE_PROJECT_NAME`，`{{MCU}}` / `{{DEVICE}}` / `{{OPENOCD_TARGET}}` 按 `.ioc` 的
`Mcu.Name` 推出来（`STM32H723VGTx` → device `STM32H723VG`、target `stm32h7x.cfg`）。"""
NEW_TPL = """`CMakeLists.txt` 的 `CMAKE_PROJECT_NAME`，`{{MCU}}` / `{{DEVICE}}` / `{{OPENOCD_TARGET}}` 按 `.ioc` 的
`Mcu.Name` 推出来：`STM32H723VGTx` → device `STM32H723VG`、`STM32H743VITx` → `STM32H743VI`、
`STM32H750VBTx` → `STM32H750VB`；target 按 Flash 代号，2 MB 的 `I` 料用 `stm32h7x_dual_bank.cfg`，
其余用 `stm32h7x.cfg`（H745/H747/H755/H757 双核还要加 `DUAL_CORE 1`）。"""


def patch(path, pairs):
    raw = path.read_bytes()
    crlf = b"\r\n" in raw
    text = raw.decode("utf-8").replace("\r\n", "\n")
    for old, new in pairs:
        if text.count(old) != 1:
            sys.exit(f"{path.name}: 锚点出现 {text.count(old)} 次 -> {old[:60]!r}")
        text = text.replace(old, new, 1)
    path.write_bytes((text.replace("\n", "\r\n") if crlf else text).encode("utf-8"))
    print(f"已更新 {path.relative_to(ROOT.parents[2])}")


patch(PATH, [(OLD_PICK, NEW_PICK), (OLD_DETECT, NEW_DETECT)])
patch(README, [(OLD_README, NEW_README)])
patch(TPL_README, [(OLD_TPL, NEW_TPL)])
