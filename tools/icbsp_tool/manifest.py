"""
库侧清单 tools/manifest.json 的读取与自检。

清单是生成的唯一数据源：外设类型 → BSP 类与代码片段，设备目录 → 依赖、参数与片段，功能开关，校验规则。
片段里的 {xxx} 用 str.format 替换，{{ }} 是字面花括号。validate() 检查每个占位符都有来源，
改了库的构造函数 / Config 却忘了同步清单时能在这里（以及快照测试的编译）发现。
"""

import json
import string
from pathlib import Path

MANIFEST_PATH = Path(__file__).resolve().parent.parent / "manifest.json"
SERIES = ("h7", "f4")
PARAM_TYPES = {"int", "float", "bool", "enum", "int_list"}
NEED_KINDS = {"can", "uart", "spi", "pwm", "encoder", "gpio_out", "gpio_exti"}
DEVICE_STATUS = {"tested", "board_use", "compile_only", "planned"}
LEVELS = {"error", "warn"}

# 外设片段可用的内建占位符（由 generate.py 提供）
PERIPH_BUILTINS = {"instance", "handle", "label", "n", "ch", "kind", "dma_attr"}
# 设备片段可用的内建占位符
DEVICE_BUILTINS = {"name"}


def load(path=MANIFEST_PATH):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def _fields(fmt):
    """str.format 片段里的占位符名"""
    return {name for _, name, _, _ in string.Formatter().parse(fmt) if name}


def validate(m):
    """返回问题列表（空 = 通过）"""
    bad = []
    if m.get("manifest_version") != 1:
        bad.append("manifest_version 应为 1")
    rules = m.get("rules", {})
    for rid, r in rules.items():
        if r.get("level") not in LEVELS:
            bad.append(f"rules.{rid}: level 只能是 {sorted(LEVELS)}")
        for k in ("title", "fix"):
            if not r.get(k):
                bad.append(f"rules.{rid}: 缺 {k}")
    for kind, p in m.get("peripherals", {}).items():
        where = f"peripherals.{kind}"
        for k in ("title", "ioc_ip", "instance", "handle", "include", "declare", "init", "params"):
            if k not in p:
                bad.append(f"{where}: 缺 {k}")
        keys = set()
        for prm in p.get("params", []):
            bad += _check_param(f"{where}.params", prm)
            keys.add(prm.get("key"))
        allowed = keys | PERIPH_BUILTINS
        for s in SERIES:
            for k in ("ioc_ip", "handle", "declare"):
                if s not in p.get(k, {}):
                    bad.append(f"{where}.{k}: 缺 {s}")
            frag = p.get("declare", {}).get(s, "")
            for name in _fields(frag) - allowed:
                bad.append(f"{where}.declare.{s}: 占位符 {{{name}}} 没有来源")
        for frag in p.get("init", []):
            for name in _fields(frag) - allowed:
                bad.append(f"{where}.init: 占位符 {{{name}}} 没有来源")
        for rid in p.get("rules", []):
            if rid not in rules:
                bad.append(f"{where}.rules: 规则 {rid} 不存在")
    switches = m.get("switches", {})
    for sid, sw in switches.items():
        for dep in sw.get("requires", []):
            if dep not in switches:
                bad.append(f"switches.{sid}: 依赖的 {dep} 不存在")
    for dev, d in m.get("devices", {}).items():
        where = f"devices.{dev}"
        if d.get("status") not in DEVICE_STATUS:
            bad.append(f"{where}: status 只能是 {sorted(DEVICE_STATUS)}")
        for need in d.get("needs", []):
            if need.get("kind") not in NEED_KINDS:
                bad.append(f"{where}.needs: kind {need.get('kind')} 不认识")
        if d.get("status") == "planned":
            continue
        for k in ("include", "define", "extern", "init", "params"):
            if k not in d:
                bad.append(f"{where}: 缺 {k}")
        keys = {n["key"] for n in d.get("needs", [])}
        for prm in d.get("params", []):
            bad += _check_param(f"{where}.params", prm)
            keys.add(prm.get("key"))
        allowed = keys | DEVICE_BUILTINS
        frags = d.get("define", []) + d.get("extern", []) + d.get("init", []) + [e["call"] for e in d.get("exti", [])]
        for frag in frags + d.get("notes", []):
            for name in _fields(frag) - allowed:
                bad.append(f"{where}: 片段里的占位符 {{{name}}} 没有来源")
        for e in d.get("exti", []):
            if e.get("pin") not in keys:
                bad.append(f"{where}.exti: pin {e.get('pin')} 不是 needs 里的键")
        for sw in d.get("switches", []):
            if sw not in switches:
                bad.append(f"{where}.switches: {sw} 不是已知开关")
    bad += _check_flash(m)
    return bad


def _check_flash(m):
    """flash_chips / options（外置 Flash 与编译优化）"""
    bad = []
    names = set()
    for i, c in enumerate(m.get("flash_chips", [])):
        where = f"flash_chips[{i}]"
        for k in ("name", "size", "addr_bytes", "jedec_manufacturer", "jedec_capacity", "max_mhz", "status"):
            if k not in c:
                bad.append(f"{where}: 缺 {k}")
        size = c.get("size", 0)
        if not isinstance(size, int) or size <= 0 or size & (size - 1):
            bad.append(f"{where}: size 必须是 2 的幂（字节）")
        elif c.get("addr_bytes") == 3 and size > 16 << 20:
            bad.append(f"{where}: 超过 16 MB 要用 4 字节地址")
        if c.get("addr_bytes") not in (3, 4):
            bad.append(f"{where}: addr_bytes 只能是 3 或 4")
        if c.get("status") not in DEVICE_STATUS:
            bad.append(f"{where}: status 只能是 {sorted(DEVICE_STATUS)}")
        if c.get("name") in names:
            bad.append(f"{where}: 名字 {c.get('name')} 重复")
        names.add(c.get("name"))
    opt = m.get("options")
    if opt is not None:
        for k in ("flash_layouts", "build_types", "opt_levels", "opt_defaults", "qspi_prescaler_default"):
            if k not in opt:
                bad.append(f"options: 缺 {k}")
        if set(opt.get("flash_layouts", {})) != {"INTERNAL", "QSPI_RAM", "QSPI_XIP"}:
            bad.append("options.flash_layouts 必须正好是 INTERNAL / QSPI_RAM / QSPI_XIP")
        for bt, lv in opt.get("opt_defaults", {}).items():
            if bt not in opt.get("build_types", []) or lv not in opt.get("opt_levels", []):
                bad.append(f"options.opt_defaults.{bt}: 构建类型或等级不在列表里")
    return bad


def _check_param(where, prm):
    bad = []
    if not prm.get("key"):
        bad.append(f"{where}: 参数缺 key")
    if prm.get("type") not in PARAM_TYPES:
        bad.append(f"{where}.{prm.get('key')}: type 只能是 {sorted(PARAM_TYPES)}")
    if prm.get("type") == "enum" and not prm.get("values"):
        bad.append(f"{where}.{prm.get('key')}: enum 缺 values")
    if "default" not in prm and "default_from" not in prm:
        bad.append(f"{where}.{prm.get('key')}: 缺 default / default_from")
    if prm.get("type") == "enum" and "default" in prm and prm["default"] not in prm["values"]:
        bad.append(f"{where}.{prm.get('key')}: 默认值不在 values 里")
    return bad


if __name__ == "__main__":
    problems = validate(load())
    print("\n".join(problems) or "manifest: OK")
    raise SystemExit(1 if problems else 0)
