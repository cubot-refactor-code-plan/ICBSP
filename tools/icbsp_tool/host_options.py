"""
宿主工程级选项：外置 QSPI Flash、编译优化、示例 .gitignore（integrate.py 的计划步骤）。

选项存在选择（icbsp_selection.json）的 external_flash / optimization / gitignore 三个键里（见 selection.py）。
写进宿主的东西：

    根 CMakeLists.txt      生成区 icbsp-options（# 注释标记，放在 add_subdirectory(<icbsp>) 之前）：
                           include(cmake/icbsp_flash.cmake) + icbsp_flash_layout(...)、
                           include(cmake/icbsp_options.cmake) + icbsp_optimization(...)
    CMakePresets.json      RelWithDebInfo，外置 Flash 启用时再加 <类型>-Internal / -QspiRam / -QspiXip
    <board>/icbsp_flash_config.h   引脚 / 芯片 / 分频（bootloader 与 CMake 共用；整文件生成）
    Core/Src/quadspi.c     USER CODE QUADSPI_Init 0：方案 B 时直接返回（程序正在 QSPI 上执行）
    Flash/daplink_qspi.cfg OpenOCD 的 QSPI 初始化与 stmqspi bank（整文件生成）
    .vscode/launch.json    QSPI_RAM / QSPI_XIP 两个调试配置（由 integrate.py 的 JSONC 合并完成）
    .gitignore             templates/gitignore，已有时只追加缺的行

所有函数只往 plan 里登记修改，不直接写盘。
"""

import json
import re
from pathlib import Path

from . import flash
from . import regions as R

CMAKE_REGION = "icbsp-options"
LAYOUT_SUFFIX = {"INTERNAL": "Internal", "QSPI_RAM": "QspiRam", "QSPI_XIP": "QspiXip"}
QSPI_GUARD = (
    "#if defined(ICBSP_FLASH_LAYOUT_QSPI_XIP)\n"
    "  /* icbsp 外置 Flash 方案 B：程序正在 QSPI 上执行，bootloader 已配好内存映射，这里不能重新初始化 QUADSPI */\n"
    "  return;\n"
    "#endif\n"
)
GITIGNORE_BEGIN = "# >>> icbsp（integrate.py 追加的示例忽略项）"
GITIGNORE_END = "# <<< icbsp"


def _rel(path, host):
    return Path(path).relative_to(host).as_posix()


def _file(host, plan, text_file_cls, path):
    """同一个文件在本次计划里可能已经被别的步骤改过：沿用那份，没有就新读"""
    path = Path(path)
    if path in plan.edits:
        return plan.edits[path]
    if path in plan.new_files and isinstance(plan.new_files[path], str):
        return None
    return text_file_cls(path) if path.exists() else None


# ---------------------------------------------------------------- 根 CMakeLists.txt


def cmake_region_body(sel, lib_rel, board_rel, manifest):
    ef = sel.get("external_flash") or {}
    op = sel.get("optimization") or {}
    lines = []
    if ef.get("enabled") or op.get("enabled"):
        lines.append("# icbsp：外置 Flash 与编译优化（integrate.py 按 " + board_rel + "/icbsp_selection.json 生成，改选择后重跑；")
        lines.append("# 切换程序存放方式用预设 <类型>-Internal / -QspiRam / -QspiXip，或 -DICBSP_FLASH_LAYOUT=…，见 docs/接入指南.md）")
    if ef.get("enabled"):
        lines += [
            f"include({lib_rel}/cmake/icbsp_flash.cmake)",
            f"icbsp_flash_layout(TARGET ${{CMAKE_PROJECT_NAME}} DEFAULT {ef.get('layout', 'QSPI_RAM')}",
            f"                   CONFIG ${{CMAKE_CURRENT_SOURCE_DIR}}/{board_rel}/icbsp_flash_config.h)",
        ]
    if op.get("enabled"):
        levels = " ".join(f"{bt} {op[bt]}" for bt in manifest.get("options", {}).get("build_types", []) if op.get(bt))
        vendor = " VENDOR_OS" if op.get("vendor_os") else ""
        lines += [
            f"include({lib_rel}/cmake/icbsp_options.cmake)",
            f"icbsp_optimization(TARGET ${{CMAKE_PROJECT_NAME}} {levels}{vendor})",
        ]
    return "\n".join(lines)


def plan_cmake_region(host, plan, rep, text_file_cls, sel, lib_rel, board_rel, manifest, overwrite_edited):
    step = "CMakeLists.txt"
    path = host / "CMakeLists.txt"
    tf = _file(host, plan, text_file_cls, path)
    if tf is None:
        rep.error(step, "读不到根 CMakeLists.txt")
        return
    body = cmake_region_body(sel, lib_rel, board_rel, manifest)
    regs, bad = R.parse(tf.text, "#")
    if bad:
        rep.error(step, "生成区标记有问题：" + "；".join(bad))
        return
    if CMAKE_REGION in regs:
        r = regs[CMAKE_REGION]
        if r["recorded"] != r["actual"] and not overwrite_edited:
            rep.error(step, f"生成区 {CMAKE_REGION} 被手改过：把改动挪到区外（或加 --overwrite-edited 覆盖）")
            return
        if r["body"] == body:
            rep.skip(step, "外置 Flash / 编译优化生成区已是最新")
            return
        tf.text = R.replace(tf.text, CMAKE_REGION, body, "#")
        plan.edit(tf)
        rep.ok(step, "更新外置 Flash / 编译优化生成区" if body else "外置 Flash / 编译优化都已关闭：清空生成区")
        return
    if not body:
        return
    # 放在 add_subdirectory(<icbsp>) 之前（连同它上面那行 icbsp 注释）：icbsp_flash_layout 要先换掉 -T，
    # 库的链接脚本检查才会检查新脚本
    lines = tf.text.split("\n")
    at = next((i for i, l in enumerate(lines) if re.match(r"\s*add_subdirectory\(\s*" + re.escape(lib_rel) + r"\b", l)), None)
    if at is None:
        rep.error(step, f"根 CMakeLists.txt 里没有 add_subdirectory({lib_rel})，无法放外置 Flash / 编译优化生成区")
        return
    if at > 0 and lines[at - 1].lstrip().startswith("# icbsp"):
        at -= 1
    lines[at:at] = R.block(CMAKE_REGION, body, "#").split("\n") + [""]
    tf.text = "\n".join(lines)
    plan.edit(tf)
    rep.ok(step, f"在 add_subdirectory({lib_rel}) 之前加外置 Flash / 编译优化生成区")


# ---------------------------------------------------------------- CMakePresets.json


def plan_presets(host, plan, rep, text_file_cls, sel, manifest):
    step = "CMakePresets.json"
    path = host / "CMakePresets.json"
    tf = _file(host, plan, text_file_cls, path)
    if tf is None:
        rep.warn(step, "没有 CMakePresets.json：跳过预设（切换存放方式用 cmake -DICBSP_FLASH_LAYOUT=…）")
        return
    try:
        data = json.loads(tf.text)
    except ValueError as e:
        rep.warn(step, f"不是合法 JSON（{e}），跳过；请手动加预设")
        return
    cfg = data.setdefault("configurePresets", [])
    bld = data.setdefault("buildPresets", [])
    have_cfg = {p.get("name") for p in cfg}
    have_bld = {p.get("name") for p in bld}
    base = "default" if "default" in have_cfg else None
    added = []

    def add(name, inherits, cache):
        if name not in have_cfg:
            p = {"name": name, "inherits": inherits}
            if cache:
                p["cacheVariables"] = cache
            cfg.append(p)
            have_cfg.add(name)
            added.append(name)
        if name not in have_bld:
            bld.append({"name": name, "configurePreset": name})
            have_bld.add(name)

    if base and "RelWithDebInfo" not in have_cfg:
        add("RelWithDebInfo", base, {"CMAKE_BUILD_TYPE": "RelWithDebInfo"})
    ef = sel.get("external_flash") or {}
    if ef.get("enabled"):
        for bt in manifest.get("options", {}).get("build_types", []):
            if bt not in have_cfg:
                continue
            for layout, suffix in LAYOUT_SUFFIX.items():
                add(f"{bt}-{suffix}", bt, {"ICBSP_FLASH_LAYOUT": layout})
    if not added:
        rep.skip(step, "预设已齐全")
        return
    indent = "\t" if "\n\t" in tf.text else 4
    tf.text = json.dumps(data, ensure_ascii=False, indent=indent) + "\n"
    plan.edit(tf)
    rep.ok(step, "加预设 " + "、".join(added))


# ---------------------------------------------------------------- 外置 Flash 文件


def plan_external_flash(host, plan, rep, text_file_cls, ioc, manifest, sel, board_dir, info, render, flash_scripts=True):
    """icbsp_flash_config.h、quadspi.c 的 USER CODE、Flash/daplink_qspi.cfg、旧版烧录脚本升级"""
    step = "外置 Flash"
    ef = sel.get("external_flash") or {}
    if not ef.get("enabled"):
        return
    chip = flash.chip_for(manifest, ef.get("chip"))
    a = flash.analyze(ioc, None, manifest)
    if chip is None or not a["supported"] or a["bank"] is None:
        rep.error(step, a["note"] or "外置 Flash 配置不完整（芯片 / QUADSPI Bank）")
        return
    quadspi_c = host / "Core/Src/quadspi.c"
    try:
        pins = flash.pin_afs(ioc, quadspi_c)
    except ValueError as e:
        rep.error(step, str(e))
        return
    if len(pins) != 6:
        rep.error(step, f".ioc 里 QUADSPI 引脚是 {len(pins)} 个，单线 / 双线 / 双 Flash 模式暂不支持（要 CLK + NCS + IO0~IO3）")
        return

    # 1. icbsp_flash_config.h（整文件生成）
    header = flash.config_header(chip, a["bank"], pins, int(ef.get("prescaler", 2)))
    _write_generated(host, plan, rep, text_file_cls, host / board_dir / "icbsp_flash_config.h", header, step)

    # 2. Flash/daplink_qspi.cfg（整文件生成）
    cfg = render(Path(__file__).resolve().parents[2] / "templates/Flash/daplink_qspi.cfg",
                 {**info, "QSPI_CHIP": chip["name"], "QSPI_BANK": a["bank"],
                  "QSPI_INIT": flash.openocd_qspi_init(chip, a["bank"], pins)})
    _write_generated(host, plan, rep, text_file_cls, host / "Flash/daplink_qspi.cfg", cfg, step)

    # 3. quadspi.c：方案 B 不能重新初始化 QUADSPI
    if not quadspi_c.exists():
        rep.warn(step, "没有 Core/Src/quadspi.c：方案 B 要保证应用不初始化 QUADSPI")
    else:
        tf = _file(host, plan, text_file_cls, quadspi_c)
        span = _user_code(tf.text, "QUADSPI_Init 0")
        if span is None:
            rep.error(step, "quadspi.c 没有 USER CODE QUADSPI_Init 0 段")
        else:
            cur = tf.text[span[0] : span[1]]
            if "ICBSP_FLASH_LAYOUT_QSPI_XIP" in cur:
                rep.skip(step, "quadspi.c 已有方案 B 的提前返回")
            elif cur.strip():
                rep.error(step, "quadspi.c 的 USER CODE QUADSPI_Init 0 已有自定义内容：请手动把 templates 里的方案 B 提前返回加在最前面")
            else:
                tf.text = tf.text[: span[0]] + QSPI_GUARD + "\n" + tf.text[span[1] :]
                plan.edit(tf)
                rep.ok(step, "quadspi.c：方案 B 时 MX_QUADSPI_Init() 直接返回")

    # 4. 旧版烧录脚本不认外置 Flash：换成新模板（不生成 Flash/ 时不动）
    for name in ("OpenOCD_flash.sh", "OpenOCD_flash.bat", "JLink_flash.sh", "JLink_flash.bat") if flash_scripts else ():
        dst = host / "Flash" / name
        if not dst.exists() or dst in plan.new_files:
            continue
        tf = _file(host, plan, text_file_cls, dst)
        if "ICBSP_FLASH_LAYOUT" in tf.text:
            continue
        tf.text = render(Path(__file__).resolve().parents[2] / "templates/Flash" / name, info)
        plan.edit(tf)
        rep.ok(step, f"Flash/{name} 换成认外置 Flash 的新版")

    mhz = 240 // (int(ef.get("prescaler", 2)) + 1)
    rep.ok(step, f"{chip['name']}，QUADSPI Bank {a['bank']}，应用运行时 QSPI {mhz} MHz，默认存放方式 {ef.get('layout')}")


def _write_generated(host, plan, rep, text_file_cls, path, text, step):
    if not path.exists():
        plan.create(path, text)
        rep.ok(step, f"新建 {_rel(path, host)}")
        return
    tf = _file(host, plan, text_file_cls, path)
    if tf.text == text:
        rep.skip(step, f"{_rel(path, host)} 已是最新")
        return
    tf.text = text
    plan.edit(tf)
    rep.ok(step, f"重新生成 {_rel(path, host)}")


def _user_code(text, name):
    begin = f"/* USER CODE BEGIN {name} */"
    end = f"/* USER CODE END {name} */"
    if text.count(begin) != 1 or text.count(end) != 1:
        return None
    start = text.index("\n", text.index(begin)) + 1
    stop = text.rfind("\n", 0, text.index(end)) + 1
    return start, stop


# ---------------------------------------------------------------- .gitignore


def gitignore_template():
    return (Path(__file__).resolve().parents[2] / "templates/gitignore").read_text(encoding="utf-8")


def plan_gitignore(host, plan, rep, text_file_cls):
    step = ".gitignore"
    path = host / ".gitignore"
    tpl = gitignore_template()
    if not path.exists():
        plan.create(path, tpl)
        rep.ok(step, "复制示例 .gitignore（templates/gitignore）")
        return
    tf = _file(host, plan, text_file_cls, path)
    have = {l.strip() for l in tf.text.split("\n")}
    want = [l for l in tpl.split("\n") if l.strip() and not l.lstrip().startswith("#")]
    missing = [l for l in want if l.strip() not in have]
    if not missing:
        rep.skip(step, ".gitignore 已包含示例里的全部条目")
        return
    if GITIGNORE_BEGIN in tf.text and GITIGNORE_END in tf.text:
        before, rest = tf.text.split(GITIGNORE_BEGIN, 1)
        inside, after = rest.split(GITIGNORE_END, 1)
        inside = inside.rstrip("\n") + "\n" + "\n".join(missing) + "\n"
        tf.text = before + GITIGNORE_BEGIN + inside + GITIGNORE_END + after
    else:
        tf.text = tf.text.rstrip("\n") + "\n\n" + GITIGNORE_BEGIN + "\n" + "\n".join(missing) + "\n" + GITIGNORE_END + "\n"
    plan.edit(tf)
    rep.ok(step, f"追加 {len(missing)} 条：{'、'.join(missing)}")


# ---------------------------------------------------------------- 选项来源（普通模式）


def cli_overrides(args, sel, manifest):
    """把 --external-flash / --flash-chip / --qspi-prescaler / --opt / --vendor-os / --no-opt / --flash-scripts 合进选择；返回是否有改动"""
    changed = False
    ef = sel["external_flash"]
    if args.external_flash:
        if args.external_flash == "off":
            changed |= ef.get("enabled") is not False
            ef["enabled"] = False
        else:
            changed |= not ef.get("enabled") or ef.get("layout") != args.external_flash
            ef["enabled"] = True
            ef["layout"] = args.external_flash
    if args.flash_chip:
        changed |= ef.get("chip") != args.flash_chip
        ef["chip"] = args.flash_chip
    if args.qspi_prescaler is not None:
        changed |= ef.get("prescaler") != args.qspi_prescaler
        ef["prescaler"] = args.qspi_prescaler
    op = sel["optimization"]
    for item in args.opt or []:
        for part in item.split(","):
            if not part.strip():
                continue
            bt, _, lv = part.partition("=")
            bt, lv = bt.strip(), lv.strip()
            if bt not in manifest["options"]["build_types"] or lv not in manifest["options"]["opt_levels"]:
                raise ValueError(f"--opt {part}：格式 类型=等级，类型 {'/'.join(manifest['options']['build_types'])}，"
                                 f"等级 {' '.join(manifest['options']['opt_levels'])}")
            changed |= op.get(bt) != lv or not op.get("enabled")
            op[bt] = lv
            op["enabled"] = True
    if args.vendor_os:
        changed |= not op.get("vendor_os") or not op.get("enabled")
        op["vendor_os"] = True
        op["enabled"] = True
    if args.no_opt:
        changed |= bool(op.get("enabled"))
        op["enabled"] = False
    if args.flash_scripts:
        want = args.flash_scripts == "on"
        changed |= sel.get("flash_scripts", True) != want
        sel["flash_scripts"] = want
    return changed
