"""
从 .ioc 列出可做成库实例的外设（候选），算出每个参数的默认值；默认选择与选择的校验。

选择（selection）的格式，也是 `integrate.py --apply <选择.json>` 读的格式：

    {
      "peripherals": {
        "FDCAN1":    {"kind": "can",  "enabled": true, "params": {"rx_depth": 16, "tx_depth": 16}},
        "USART1":    {"kind": "uart", "enabled": true, "params": {"mode": "dma", "dir": "TxRx", ...}},
        "TIM12_CH2": {"kind": "pwm",  "enabled": true, "params": {...}}
      },
      "devices": [ {"type": "dji_motor", "name": "chassis_lf", "needs": {"bus": "FDCAN1"}, "params": {"type": "M3508", "id": 1}} ],
      "switches": {"ICBSP_CAN_RX_ENABLE": 1, ...},
      "debug_uart": "USART1",
      "external_flash": {"enabled": true, "layout": "QSPI_RAM", "chip": "W25Q128", "prescaler": 2},
      "optimization": {"enabled": true, "Debug": "-Og", "RelWithDebInfo": "-O2", "Release": "-Os", "vendor_os": false},
      "gitignore": true
    }

external_flash：外置 QSPI Flash（tools/icbsp_tool/flash.py、cmake/icbsp_flash.cmake）。layout 是写进根 CMakeLists.txt 的
默认程序存放方式（INTERNAL / QSPI_RAM / QSPI_XIP），日常切换用 CMakePresets 的 *-Internal / *-QspiRam / *-QspiXip。
optimization：各构建类型的优化等级（cmake/icbsp_options.cmake）；vendor_os = CubeMX 生成的源码固定 -Os。
gitignore：把 templates/gitignore 合并进宿主 .gitignore（只追加缺的行）。

只出现部分键时按默认值补齐；PWM / 编码器默认不选（例如 DM-MC02 的 TIM3_CH4 是 BMI088 加热电阻，不能默认启动）。
"""

import re

from . import checks
from . import flash


def candidates(ioc, manifest):
    """[{ip, kind, instance, handle, label, n, ch, usable, reason, params(默认值), facts}]"""
    s = ioc.series
    per = manifest["peripherals"]
    use = checks.usable(ioc)
    out = []

    def mk(kind, ip, n, extra, defaults_from, label=None, ch=None, ukind=None):
        p = per[kind]
        fmt = {"n": n, "ch": ch, "kind": ukind or ""}
        ok, why = use.get(ip, (True, ""))
        params = {}
        for prm in p["params"]:
            if "default" in prm:
                params[prm["key"]] = prm["default"]
            else:
                params[prm["key"]] = defaults_from.get(prm["default_from"])
        out.append(
            {
                "ip": ip,
                "kind": kind,
                "instance": p["instance"].format(**fmt),
                "handle": p["handle"][s].format(**fmt),
                "label": label or ip,
                "n": n,
                "ch": ch,
                "ukind": ukind,
                "usable": ok,
                "reason": why,
                "params": params,
                "facts": extra,
            }
        )

    for c in ioc.can_instances():
        mk("can", c["ip"], c["n"], c, {}, label=f"CAN{c['n']}")
    for u in ioc.uart_instances():
        both = u["dma_rx"] and u["dma_tx"] and not u["bdma"]
        mk("uart", u["ip"], u["n"], u, {"uart_mode": "dma" if both else "it", "ioc_baudrate": u["baudrate"]}, ukind=u["kind"])
    for sp in ioc.spi_instances():
        mk("spi", sp["ip"], sp["n"], sp, {"spi_dma": bool(sp["dma_rx"] and sp["dma_tx"] and not sp["bdma"])})
    for n, ch in ioc.pwm_channels():
        clk, known = ioc.timer_clock(n)
        psc = checks.eval_int(ioc.tim_param(n, "Prescaler", "0"))
        arr = checks.eval_int(ioc.tim_param(n, "Period", "65535"))
        mk("pwm", f"TIM{n}_CH{ch}", n, {"clock_known": known}, {"ioc_timer_clock": clk, "ioc_prescaler": psc, "ioc_period": arr}, ch=ch)
    for n in ioc.encoder_timers():
        mk("encoder", f"TIM{n}", n, {}, {})
    return out


def default_selection(ioc, manifest):
    sel = {"peripherals": {}, "devices": [], "switches": {}, "debug_uart": None}
    for c in candidates(ioc, manifest):
        on = c["usable"] and c["kind"] in ("can", "uart", "spi")
        sel["peripherals"][c["ip"]] = {"kind": c["kind"], "enabled": on, "params": dict(c["params"])}
    uarts = [ip for ip, p in sel["peripherals"].items() if p["kind"] == "uart" and p["enabled"]]
    sel["debug_uart"] = "USART1" if "USART1" in uarts else (uarts[0] if uarts else None)
    sel["switches"] = default_switches(manifest, sel)
    sel.update(default_options(ioc, manifest))
    return sel


def default_options(ioc, manifest):
    """外置 Flash / 编译优化 / .gitignore 的默认值（默认都不改宿主的构建方式，.gitignore 默认合并）"""
    opt = manifest.get("options", {})
    guess = flash.analyze(ioc, None, manifest).get("chip_guess")
    return {
        "external_flash": {
            "enabled": False,
            "layout": "QSPI_RAM",
            "chip": guess,
            "prescaler": opt.get("qspi_prescaler_default", 2),
        },
        "optimization": {"enabled": False, **opt.get("opt_defaults", {}), "vendor_os": False},
        "gitignore": True,
    }


OPTION_KEYS = ("external_flash", "optimization", "gitignore")


def merge_options(sel, user):
    """把 user 里的选项键合并进 sel（字典逐键覆盖）"""
    for key in OPTION_KEYS:
        if key not in user:
            continue
        if isinstance(sel.get(key), dict) and isinstance(user[key], dict):
            sel[key].update(user[key])
        else:
            sel[key] = user[key]
    return sel


def default_switches(manifest, sel):
    kinds = {p["kind"] for p in sel["peripherals"].values() if p["enabled"]}
    out = {}
    for sid, sw in manifest["switches"].items():
        need = sw.get("needs_ip")
        out[sid] = sw["default"] if (need is None or need in kinds) else 0
    return out


def merge(ioc, manifest, user):
    """用户给的（可能不完整的）选择 + 默认值 → 完整选择"""
    sel = default_selection(ioc, manifest)
    for ip, p in (user.get("peripherals") or {}).items():
        if ip not in sel["peripherals"]:
            sel["peripherals"][ip] = {"kind": p.get("kind"), "enabled": bool(p.get("enabled")), "params": dict(p.get("params") or {}), "unknown": True}
            continue
        cur = sel["peripherals"][ip]
        if "enabled" in p:
            cur["enabled"] = bool(p["enabled"])
        cur["params"].update(p.get("params") or {})
    if "devices" in user:
        sel["devices"] = [dict(d) for d in user["devices"]]
    if "debug_uart" in user:
        sel["debug_uart"] = user["debug_uart"]
    sel["switches"] = default_switches(manifest, sel)
    sel["switches"].update(user.get("switches") or {})
    merge_options(sel, user)
    return sel


def by_kind(sel):
    """给 checks.run 用：{"can": {ip: params}, ...}（只含选中的）"""
    out = {}
    for ip, p in sel["peripherals"].items():
        if p["enabled"]:
            out.setdefault(p["kind"], {})[ip] = p["params"]
    return out


def validate(ioc, manifest, sel):
    """选择本身的问题（参数越界、设备依赖缺失、名字冲突等）：[(level, where, msg)]"""
    bad = []
    cands = {c["ip"]: c for c in candidates(ioc, manifest)}
    names = set()
    for ip, p in sel["peripherals"].items():
        if not p["enabled"]:
            continue
        c = cands.get(ip)
        if c is None:
            bad.append(("error", ip, ".ioc 里没有这个外设（或 CubeMX 没把它配成库能用的模式）"))
            continue
        if not c["usable"]:
            bad.append(("error", ip, f"不能用作库实例：{c['reason']}"))
        spec = {prm["key"]: prm for prm in manifest["peripherals"][c["kind"]]["params"]}
        for k, v in p["params"].items():
            if k in spec:
                bad += [("error", f"{ip}.{k}", m) for m in check_value(spec[k], v)]
        names.add(c["instance"])
        if c["kind"] == "uart" and "baudrate" in p["params"] and int(p["params"]["baudrate"]) != c["facts"]["baudrate"]:
            bad.append(("warn", f"{ip}.baudrate", f"{p['params']['baudrate']} 与 CubeMX 的 {c['facts']['baudrate']} 不一致：线路按 CubeMX 跑，库按这里估算卡死阈值——改 .ioc 或这里"))
    if sel.get("debug_uart") and not sel["peripherals"].get(sel["debug_uart"], {}).get("enabled"):
        bad.append(("error", "debug_uart", f"调试串口 {sel['debug_uart']} 没有选中"))
    devs = manifest["devices"]
    for i, d in enumerate(sel["devices"]):
        where = f"devices[{i}]"
        spec = devs.get(d.get("type"))
        if spec is None:
            bad.append(("error", where, f"不认识的设备类型 {d.get('type')}"))
            continue
        if spec["status"] == "planned":
            bad.append(("error", where, f"{spec['title']} 还没实现（计划中）"))
            continue
        name = d.get("name", "")
        if not re.fullmatch(r"[a-z_][a-z0-9_]*", name):
            bad.append(("error", where, f"实例名 {name!r} 不合法（小写字母、数字、下划线）"))
        elif name in names:
            bad.append(("error", where, f"实例名 {name} 重复"))
        names.add(name)
        for need in spec["needs"]:
            v = (d.get("needs") or {}).get(need["key"])
            if not v:
                if not need.get("optional"):
                    bad.append(("error", f"{where}.{need['key']}", f"缺 {need['title']}"))
                continue
            bad += [("error", f"{where}.{need['key']}", m) for m in check_need(ioc, sel, need, v)]
        pspec = {prm["key"]: prm for prm in spec["params"]}
        for k, v in (d.get("params") or {}).items():
            if k not in pspec:
                bad.append(("error", f"{where}.{k}", "不认识的参数"))
            else:
                bad += [("error", f"{where}.{k}", m) for m in check_value(pspec[k], v)]
        for sw in spec.get("switches", []):
            if not sel["switches"].get(sw):
                bad.append(("error", where, f"{spec['title']} 需要 {sw} = 1"))
        if d.get("type") == "dji_motor":
            prm = {**{p["key"]: p.get("default") for p in spec["params"]}, **(d.get("params") or {})}
            if prm.get("type") == "GM6020" and int(prm.get("id", 1)) > 7:
                bad.append(("error", f"{where}.id", "GM6020 的 ID 只能是 1~7"))
    # PWM 通道、编码器、GPIO 只能给一个设备用（总线可以多个设备共用）
    owner = {}
    for i, d in enumerate(sel["devices"]):
        spec = devs.get(d.get("type"))
        if not spec or spec["status"] == "planned":
            continue
        for need in spec["needs"]:
            v = (d.get("needs") or {}).get(need["key"])
            if v and need["kind"] in ("pwm", "encoder", "gpio_out", "gpio_exti"):
                if v in owner:
                    bad.append(("error", f"devices[{i}].{need['key']}", f"{v} 已经给 {owner[v]} 用了"))
                else:
                    owner[v] = f"{d.get('name')}.{need['key']}"
    for sid, v in sel["switches"].items():
        sw = manifest["switches"].get(sid)
        if sw is None:
            bad.append(("error", sid, "不认识的开关"))
            continue
        if v not in (0, 1):
            bad.append(("error", sid, "只能是 0 或 1"))
        for dep in sw.get("requires", []):
            if v and not sel["switches"].get(dep):
                bad.append(("error", sid, f"需要 {dep} = 1"))
    bad += validate_options(ioc, manifest, sel)
    return bad


def validate_options(ioc, manifest, sel):
    bad = []
    opt = manifest.get("options", {})
    ef = sel.get("external_flash") or {}
    if ef.get("layout") not in flash.LAYOUTS:
        bad.append(("error", "external_flash.layout", f"只能是 {'/'.join(flash.LAYOUTS)}"))
    if ef.get("enabled"):
        a = flash.analyze(ioc, None, manifest)
        if not a["supported"]:
            bad.append(("error", "external_flash", a["note"] or ".ioc 没有启用 QUADSPI（外置 Flash 只支持 QUADSPI）"))
        chip = flash.chip_for(manifest, ef.get("chip"))
        if chip is None:
            names = "、".join(c["name"] for c in manifest.get("flash_chips", []))
            bad.append(("error", "external_flash.chip", f"选一个芯片：{names}"))
        try:
            pre = int(ef.get("prescaler"))
        except (TypeError, ValueError):
            pre = -1
        if not 0 <= pre <= 255:
            bad.append(("error", "external_flash.prescaler", "预分频 0~255"))
        elif chip is not None and 240 / (pre + 1) > chip["max_mhz"]:
            bad.append(("error", "external_flash.prescaler",
                        f"QSPI 时钟 {240 / (pre + 1):.0f} MHz 超过 {chip['name']} 的 {chip['max_mhz']} MHz（按应用 HCLK3 = 240 MHz 算）"))
        elif chip is not None and chip.get("status") != "tested":
            bad.append(("warn", "external_flash.chip", f"{chip['name']} 只做过编译验证，没有上板实测"))
    op = sel.get("optimization") or {}
    for bt in opt.get("build_types", []):
        if op.get(bt) not in opt.get("opt_levels", []):
            bad.append(("error", f"optimization.{bt}", f"只能是 {' '.join(opt.get('opt_levels', []))}"))
    if not isinstance(sel.get("gitignore", True), bool):
        bad.append(("error", "gitignore", "只能是 true / false"))
    return bad


def check_value(spec, v):
    t = spec["type"]
    try:
        if t == "int":
            x = int(v)
        elif t == "float":
            x = float(v)
        elif t == "bool":
            return [] if isinstance(v, bool) else ["要填 true / false"]
        elif t == "enum":
            return [] if v in spec["values"] else [f"只能是 {', '.join(spec['values'])}"]
        elif t == "int_list":
            xs = [int(e) for e in v]
            if not xs:
                return ["至少一个"]
            return [f"{e} 超出 {spec['min']}~{spec['max']}" for e in xs if not spec["min"] <= e <= spec["max"]]
        else:
            return [f"不认识的类型 {t}"]
    except (TypeError, ValueError):
        return [f"不是合法的 {t}"]
    if "min" in spec and x < spec["min"] or "max" in spec and x > spec["max"]:
        return [f"{x} 超出 {spec['min']}~{spec['max']}"]
    return []


def check_need(ioc, sel, need, v):
    kind = need["kind"]
    if kind == "gpio_out":
        return [] if v in ioc.gpio_outputs() else [f"{v} 不是带标签的 GPIO 输出（CubeMX 里给引脚配 GPIO_Output 并起标签）"]
    if kind == "gpio_exti":
        return [] if v in ioc.gpio_exti() else [f"{v} 不是带标签的 EXTI 引脚"]
    p = sel["peripherals"].get(v)
    if p is None or p["kind"] != kind:
        return [f"{v} 不是 {kind} 外设"]
    if not p["enabled"]:
        return [f"{v} 没有选中"]
    return []


def device_catalog(ioc, manifest, sel):
    """可挂设备：[{type, title, status, status_note, available, reason, needs: [{key, kind, title, optional, options}], params}]
    设备只在它依赖的外设（已选中）与功能开关都满足时 available；计划中的设备列出但不可选。"""
    enabled = {}
    cands = {c["ip"]: c for c in candidates(ioc, manifest)}
    for ip, p in sel["peripherals"].items():
        if p["enabled"] and ip in cands:
            enabled.setdefault(p["kind"], []).append(ip)
    out = []
    for dtype, spec in manifest["devices"].items():
        item = {"type": dtype, "title": spec["title"], "status": spec["status"], "status_note": spec.get("status_note", ""),
                "params": spec.get("params", []), "needs": [], "available": True, "reason": ""}
        why = []
        for need in spec.get("needs", []):
            kind = need["kind"]
            if kind == "gpio_out":
                opts = ioc.gpio_outputs()
            elif kind == "gpio_exti":
                opts = ioc.gpio_exti()
            else:
                opts = enabled.get(kind, [])
            item["needs"].append({**need, "options": opts})
            if not opts and not need.get("optional"):
                why.append(f"没有可用的{need['title']}")
        if spec["status"] == "planned":
            why.insert(0, "计划中，还没实现")
        for sw in spec.get("switches", []):
            if not sel["switches"].get(sw):
                why.append(f"需要 {sw} = 1")
        if why:
            item["available"] = False
            item["reason"] = "；".join(why)
        out.append(item)
    return out
