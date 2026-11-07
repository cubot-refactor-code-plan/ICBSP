"""
按选择生成宿主板级文件里各“生成区”的内容（只算文本，不写盘；写盘见 regions.py / integrate.py）。

    文件                     生成区
    board_cfg.hpp            includes、peripherals、devices
    board_instances.hpp      instances
    board_cfg.cpp            devices、init、callbacks
    hardware_config.h        switches

片段全部来自 tools/manifest.json。外设按种类（can、uart、spi、pwm、encoder）再按编号排序，设备按选择里的顺序；
init() 先外设、后设备（设备要用已 init 的外设）。
"""

from . import selection as S

KIND_ORDER = ("can", "uart", "spi", "pwm", "encoder")
HEADER = "// 由 tools/integrate.py 按 .ioc 与接入选择生成：要改就改选择后重新运行；手改本区会被检测到、拒绝覆盖"


def _lit(v, spec=None):
    """参数值 → C++ 字面量（片段里的后缀 U / UL / f 由清单写好）"""
    if isinstance(v, bool):
        return "true" if v else "false"
    t = (spec or {}).get("type")
    if t == "float":
        x = float(v)
        return repr(x) if x != int(x) else f"{int(x)}.0"
    if t == "int_list":
        return ", ".join(str(int(e)) for e in v)
    if t == "int" and (spec or {}).get("hex"):
        return f"0x{int(v):02X}"
    return str(v)


class Generator:
    def __init__(self, ioc, manifest, sel):
        self.ioc = ioc
        self.m = manifest
        self.sel = sel
        self.series = ioc.series
        self.cands = {c["ip"]: c for c in S.candidates(ioc, manifest)}

    # ---------- 外设 ----------

    def periphs(self):
        """选中的外设，按种类与编号排序：[(候选, 参数)]"""
        out = []
        for ip, p in self.sel["peripherals"].items():
            if p["enabled"] and ip in self.cands:
                out.append((self.cands[ip], p["params"]))
        return sorted(out, key=lambda e: (KIND_ORDER.index(e[0]["kind"]), e[0]["n"], e[0]["ch"] or 0))

    def periph_fmt(self, c, params):
        spec = self.m["peripherals"][c["kind"]]
        pspec = {p["key"]: p for p in spec["params"]}
        f = {k: _lit(v, pspec.get(k)) for k, v in params.items()}
        f.update(instance=c["instance"], handle=c["handle"], label=c["label"], n=c["n"], ch=c["ch"], kind=c["ukind"] or "")
        dma = False
        for k, v in (spec.get("dma_buffer_when") or {}).items():
            dma = params.get(k) == v
        f["dma_attr"] = f"ICBSP_DMA_BUFFER({c['instance']}) " if dma and self.series == "h7" else ""
        return f

    def instance_of(self, ip):
        c = self.cands.get(ip)
        return c["instance"] if c else ip

    # ---------- 各生成区 ----------

    def includes(self):
        kinds = []
        for c, _ in self.periphs():
            if c["kind"] not in kinds:
                kinds.append(c["kind"])
        per = self.m["peripherals"]
        cube = sorted({per[k]["cubemx_header"][self.series] for k in kinds})
        lib = sorted({per[k]["include"] for k in kinds} | {"bsp/bsp_dwt.hpp"} | {self.m["devices"][d["type"]]["include"] for d in self.devices()})
        lines = [HEADER, '#include "bsp/bsp_series.hpp" // IWYU pragma: keep', '#include "main.h"         // IWYU pragma: keep（GPIO 标签宏）']
        lines += [f'#include "{h}"' for h in cube]
        if any(self.periph_fmt(c, p)["dma_attr"] for c, p in self.periphs()):
            lib = sorted(set(lib) | {"bsp/bsp_dma_buffer.hpp"})
        lines += [f'#include "{h}"' for h in lib]
        return "\n".join(lines)

    def peripherals(self):
        per = self.m["peripherals"]
        lines = [HEADER]
        for c, params in self.periphs():
            f = self.periph_fmt(c, params)
            extra = f"（{c['reason']}）" if c["reason"] else ""
            lines.append(f"/// {c['ip']}：{per[c['kind']]['title']}{extra}")
            lines.append(per[c["kind"]]["declare"][self.series].format(**f))
        dbg = self.sel.get("debug_uart")
        lines.append("")
        if dbg:
            lines += [
                f"/// 调试打印串口：{dbg}",
                "#define ICBSP_UART_PRINT_ENABLE 1",
                f"#define print_uart(...) {self.instance_of(dbg)}.printf(__VA_ARGS__)",
            ]
        else:
            lines += ["/// 没有选调试打印串口", "#define ICBSP_UART_PRINT_ENABLE 0", "#define print_uart(...) ((void)0)"]
        return "\n".join(lines)

    def instances(self):
        lists = {}
        for c, _ in self.periphs():
            macro = self.m["peripherals"][c["kind"]].get("list_macro")
            if macro:
                lists.setdefault(macro, []).append(c["instance"])
        lines = [HEADER]
        for macro in ("ICBSP_CAN_INSTANCES", "ICBSP_UART_INSTANCES", "ICBSP_SPI_INSTANCES"):
            names = lists.get(macro, [])
            if not names:
                lines.append(f"#define {macro}(X) /* 无 */")
                continue
            lines.append(f"#define {macro}(X) \\")
            for i, n in enumerate(names):
                lines.append(f"  X({n})" + (" \\" if i < len(names) - 1 else ""))
        return "\n".join(lines)

    def init(self):
        lines = [HEADER]
        for c, params in self.periphs():
            f = self.periph_fmt(c, params)
            lines += [l.format(**f) for l in self.m["peripherals"][c["kind"]]["init"]]
        lines += self.device_init()
        return "\n".join("  " + l if l else "" for l in lines)

    def switches(self):
        lines = [HEADER]
        for sid, sw in self.m["switches"].items():
            lines.append(f"#define {sid} {int(self.sel['switches'].get(sid, sw['default']))} ///< {sw['title']}")
        return "\n".join(lines)

    # ---------- 设备（阶段 5.6 补全） ----------

    def devices(self):
        return [d for d in self.sel.get("devices", []) if self.m["devices"].get(d.get("type"), {}).get("status") != "planned"]

    def device_init(self):
        return []

    def device_externs(self):
        return HEADER

    def device_defines(self):
        return HEADER

    def callbacks(self):
        return HEADER

    # ---------- 汇总 ----------

    def regions(self):
        """{文件名: {生成区名: 内容}}"""
        return {
            "board_cfg.hpp": {"includes": self.includes(), "peripherals": self.peripherals(), "devices": self.device_externs()},
            "board_instances.hpp": {"instances": self.instances()},
            "board_cfg.cpp": {"devices": self.device_defines(), "init": self.init(), "callbacks": self.callbacks()},
            "hardware_config.h": {"switches": self.switches()},
        }
