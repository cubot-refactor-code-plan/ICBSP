"""
外置 QSPI Flash：是否建议启用、引脚 / 芯片参数，以及生成 icbsp_flash_config.h 与 OpenOCD 的 QSPI 初始化脚本。

三种程序存放方式（cmake/icbsp_flash.cmake）：
    INTERNAL  片内 Flash
    QSPI_RAM  方案 A：程序存 QSPI，bootloader 拷进 AXI SRAM 运行
    QSPI_XIP  方案 B：程序在 QSPI 上就地执行

目前只支持 QUADSPI（H742/H743/H750/H753 等）；OCTOSPI（H723/H730/H7B0 等）识别出来但标为暂不支持。
"""

import re
import subprocess
from pathlib import Path

LAYOUTS = ("INTERNAL", "QSPI_RAM", "QSPI_XIP")
H7_AXI_SRAM_SIZE = 512 * 1024
BOOT_STATUS_ADDR = 0x3800FFE0
SUGGEST_USAGE = 0.85  # 片内 Flash 占用超过这个比例就建议外置 Flash

# 料号第 11 位（STM32H750VBT6 的 B）= 片内 Flash 容量
FLASH_SIZE_CODE = {"8": 64, "B": 128, "Z": 192, "C": 256, "E": 512, "G": 1024, "I": 2048}

# STM32H7 的 QUADSPI 引脚复用功能号（数据手册 Alternate function 表）。
# 优先从 CubeMX 生成的 quadspi.c 读（GPIO_AFn_QUADSPI），读不到才用这张表。
H7_QUADSPI_AF = {
    "PB2": 9, "PF10": 9,                                   # CLK
    "PB6": 10, "PG6": 10,                                  # BK1_NCS
    "PF8": 10, "PC9": 9, "PD11": 9,                        # BK1_IO0
    "PF9": 10, "PC10": 9, "PD12": 9,                       # BK1_IO1
    "PF7": 9, "PE2": 9,                                    # BK1_IO2
    "PF6": 9, "PA1": 9, "PD13": 9,                         # BK1_IO3
    "PC11": 9,                                             # BK2_NCS
    "PE7": 10, "PH2": 9,                                   # BK2_IO0
    "PE8": 10, "PH3": 9,                                   # BK2_IO1
    "PE9": 10, "PG9": 9,                                   # BK2_IO2
    "PE10": 10, "PG14": 9,                                 # BK2_IO3
}
GPIO_BASE = 0x58020000  # GPIOA；每个端口 0x400
RCC_AHB4ENR = 0x580244E0
RCC_AHB3ENR = 0x580244D4
QUADSPI_BASE = 0x52005000
SIGNAL_ORDER = ("CLK", "NCS", "IO0", "IO1", "IO2", "IO3")


def internal_flash_kb(mcu):
    """料号推片内 Flash 容量（KB）；认不出返回 None"""
    m = re.match(r"STM32[A-Z]\d{3}[A-Z0-9]([0-9A-Z])", (mcu or "").upper())
    return FLASH_SIZE_CODE.get(m.group(1)) if m else None


def qspi_signals(ioc):
    """.ioc 里 QUADSPI 引脚：[{pin, port, num, signal, bank}]（signal 为 CLK / NCS / IO0~3）"""
    out = []
    for k, v in ioc.values.items():
        if not k.endswith(".Signal") or not v.startswith("QUADSPI_"):
            continue
        pin = k[: -len(".Signal")]
        m = re.fullmatch(r"P([A-K])(\d+)", pin)
        if not m:
            continue
        sig = v[len("QUADSPI_") :]
        bank = None
        b = re.match(r"BK(\d)_(.+)", sig)
        if b:
            bank, sig = int(b.group(1)), b.group(2)
        out.append({"pin": pin, "port": m.group(1), "num": int(m.group(2)), "signal": sig, "bank": bank})
    out.sort(key=lambda p: SIGNAL_ORDER.index(p["signal"]) if p["signal"] in SIGNAL_ORDER else 99)
    return out


def af_from_msp(quadspi_c):
    """从 CubeMX 生成的 quadspi.c 读每个引脚的复用功能号：{"PB2": 9, ...}"""
    if not quadspi_c or not Path(quadspi_c).exists():
        return {}
    text = Path(quadspi_c).read_text(encoding="utf-8", errors="replace")
    out = {}
    blocks = re.findall(
        r"GPIO_InitStruct\.Pin\s*=\s*([^;]+);.*?GPIO_InitStruct\.Alternate\s*=\s*GPIO_AF(\d+)_QUADSPI\s*;\s*HAL_GPIO_Init\(GPIO([A-K])",
        text,
        re.S,
    )
    for pins, af, port in blocks:
        for n in re.findall(r"GPIO_PIN_(\d+)", pins):
            out[f"P{port}{n}"] = int(af)
    return out


def chip_for(manifest, name):
    for c in manifest.get("flash_chips", []):
        if c["name"] == name:
            return c
    return None


def chip_by_size(manifest, size_log2):
    """按 .ioc 的 QUADSPI.FlashSize（容量 = 2^(n+1)）猜芯片"""
    for c in manifest.get("flash_chips", []):
        if c["size"] == 1 << (size_log2 + 1):
            return c
    return None


NOLOAD_SECTION = re.compile(r"bss|noinit|heap|stack", re.I)


def _map_flash_bytes(map_path):
    """从 GNU ld 的 .map 统计放进片内 Flash（0x08000000 起）的字节：输出段起始地址或加载地址在 Flash 里的都算。
    链接溢出时 ld 不产出 ELF，但 .map 仍会写出来，所以溢出时也能估算。"""
    text = Path(map_path).read_text(encoding="utf-8", errors="replace")
    text = text[text.find("Linker script and memory map") :]
    used = 0
    for m in re.finditer(r"^(\.[\w.]+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)(?:\s+load address 0x([0-9a-f]+))?", text, re.M):
        if NOLOAD_SECTION.search(m.group(1)):
            continue  # NOLOAD 段在 .map 里也印 load address，但不占 Flash
        addr = int(m.group(4) or m.group(2), 16)
        if 0x08000000 <= addr < 0x08200000:
            used += int(m.group(3), 16)
    return used


def elf_usage(host, flash_kb):
    """宿主 build/ 下最新一次构建的片内 Flash 占用：返回 (比例, 字节, 文件路径) 或 None。
    优先用最新的 ELF（arm-none-eabi-size）；.map 比 ELF 新（上次链接失败）时用 .map。"""
    build = Path(host) / "build"
    if not build.is_dir() or not flash_kb:
        return None
    skip = lambda p: "icbsp_boot" in p.parts or "CMakeFiles" in p.parts
    elfs = [p for p in build.rglob("*.elf") if not skip(p)]
    maps = [p for p in build.rglob("*.map") if not skip(p)]
    elf = max(elfs, key=lambda p: p.stat().st_mtime) if elfs else None
    mp = max(maps, key=lambda p: p.stat().st_mtime) if maps else None
    used, src = 0, None
    if mp is not None and (elf is None or mp.stat().st_mtime > elf.stat().st_mtime + 1):
        used, src = _map_flash_bytes(mp), mp
    elif elf is not None:
        try:
            r = subprocess.run(["arm-none-eabi-size", "-A", str(elf)], capture_output=True, text=True, timeout=10)
        except (OSError, subprocess.TimeoutExpired):
            return None
        for line in r.stdout.splitlines():
            f = line.split()
            if len(f) == 3 and f[1].isdigit() and f[2].isdigit() and 0x08000000 <= int(f[2]) < 0x08200000:
                used += int(f[1])
        src = elf
    if not used:
        return None
    return used / (flash_kb * 1024), used, str(src)


def analyze(ioc, host=None, manifest=None):
    """外置 Flash 分析：是否支持、是否建议、引脚、芯片猜测"""
    ips = ioc.ips()
    mcu = ioc.mcu
    kb = internal_flash_kb(mcu)
    res = {
        "peripheral": None,
        "supported": False,
        "suggest": False,
        "reasons": [],
        "internal_kb": kb,
        "bank": None,
        "pins": [],
        "chip_guess": None,
        "usage": None,
        "note": "",
    }
    if ioc.series != "h7":
        res["note"] = "外置 Flash 方案只支持 STM32H7"
        return res
    if "QUADSPI" in ips:
        res["peripheral"] = "QUADSPI"
        res["supported"] = True
        res["reasons"].append(".ioc 启用了 QUADSPI")
        pins = qspi_signals(ioc)
        res["pins"] = pins
        banks = sorted({p["bank"] for p in pins if p["bank"]})
        res["bank"] = banks[0] if len(banks) == 1 else None
        if len(banks) > 1:
            res["note"] = "QUADSPI 用了两个 Bank（双 Flash 模式）：暂不支持"
            res["supported"] = False
        fs = ioc.get("QUADSPI.FlashSize")
        if fs and fs.isdigit() and manifest and int(fs) > 1:
            c = chip_by_size(manifest, int(fs))
            res["chip_guess"] = c["name"] if c else None
    elif any(ip.startswith("OCTOSPI") for ip in ips):
        res["peripheral"] = next(ip for ip in ips if ip.startswith("OCTOSPI"))
        res["note"] = f"{res['peripheral']} 外置 Flash 暂不支持（只支持 QUADSPI）"
    if kb is not None and kb <= 128:
        res["reasons"].append(f"{mcu} 片内 Flash 只有 {kb} KB")
    if host:
        u = elf_usage(host, kb)
        if u:
            ratio, used, elf = u
            res["usage"] = {"ratio": round(ratio, 3), "bytes": used, "elf": elf}
            if ratio >= SUGGEST_USAGE:
                res["reasons"].append(f"最近一次构建占片内 Flash {ratio * 100:.0f}%（{used // 1024} KB，{Path(elf).name}）")
    res["suggest"] = res["supported"] and len(res["reasons"]) >= 2  # 有 QUADSPI，且容量小或占用高
    return res


# ---------------------------------------------------------------- 生成


def pin_afs(ioc, quadspi_c):
    """[(pin, port, num, af, signal)]；拿不到复用功能号时抛 ValueError"""
    from_msp = af_from_msp(quadspi_c)
    out = []
    for p in qspi_signals(ioc):
        af = from_msp.get(p["pin"], H7_QUADSPI_AF.get(p["pin"]))
        if af is None:
            raise ValueError(f"不知道 {p['pin']}（QUADSPI_{p['signal']}）的复用功能号")
        out.append((p["pin"], p["port"], p["num"], af, p["signal"]))
    return out


def config_header(chip, bank, pins, prescaler, sample_shift=True, cs_high=6):
    """icbsp_flash_config.h 的全文（CMake 用正则读其中的整数宏，只写整数常量）"""
    rows = []
    for i, (_pin, port, num, af, sig) in enumerate(pins):
        label = sig if sig == "CLK" else f"BK{bank}_{sig}"
        cont = " \\" if i < len(pins) - 1 else ""
        rows.append((f"  X({port}, {num}, {af})".ljust(16) + f"/* {label} */".ljust(14) + cont).rstrip())
    lines = "\n".join(rows) + "\n"
    mhz = 240 // (prescaler + 1)
    return f"""/**
 * @file icbsp_flash_config.h
 * @brief icbsp 外置 QSPI Flash 配置（bootloader 与 cmake/icbsp_flash.cmake 共用）
 *
 * 由 integrate.py / Web UI 的“外置 Flash”生成：引脚取自 .ioc，芯片参数取自 manifest.json 的 flash_chips。
 * CMake 用正则读其中的整数宏，所以这里只能写整数常量（不要写表达式）。重新运行 integrate.py 会覆盖本文件。
 */
#ifndef ICBSP_FLASH_CONFIG_H
#define ICBSP_FLASH_CONFIG_H

/* 芯片：{chip['name']}（{chip['size'] >> 20} MB） */
#define ICBSP_QSPI_FLASH_SIZE 0x{chip['size']:08X}u
#define ICBSP_QSPI_ADDR_BYTES {chip['addr_bytes']}
#define ICBSP_QSPI_JEDEC_MANUFACTURER 0x{chip['jedec_manufacturer']:02X}u
#define ICBSP_QSPI_JEDEC_CAPACITY 0x{chip['jedec_capacity']:02X}u

/* QUADSPI：Bank {bank}；时钟 = HCLK3 / (PRESCALER + 1)，应用 HCLK3 = 240 MHz 时为 {mhz} MHz（芯片上限 {chip['max_mhz']} MHz） */
#define ICBSP_QSPI_BANK {bank}
#define ICBSP_QSPI_PRESCALER {prescaler}
#define ICBSP_QSPI_SAMPLE_SHIFT {1 if sample_shift else 0}
#define ICBSP_QSPI_CS_HIGH_CYCLES {cs_high}

/* 引脚：X(端口, 引脚号, 复用功能号) */
#define ICBSP_QSPI_PINS(X) \\
{lines}
/* 片上存储 */
#define ICBSP_AXI_SRAM_SIZE 0x{H7_AXI_SRAM_SIZE:08X}u
#define ICBSP_BOOT_STATUS_ADDR 0x{BOOT_STATUS_ADDR:08X}u

#endif
"""


def openocd_qspi_init(chip, bank, pins):
    """OpenOCD 的 TCL：开时钟、配引脚、QUADSPI 进单线内存映射读（stmqspi 驱动要求），HSI 64 MHz / 2 = 32 MHz"""
    out = [
        f"\tmmw 0x{RCC_AHB4ENR:08X} 0x000007FF 0\t\t\t;# RCC_AHB4ENR：GPIOA~K 时钟",
        f"\tmmw 0x{RCC_AHB3ENR:08X} 0x00004000 0\t\t\t;# RCC_AHB3ENR：QSPIEN",
        "\tsleep 1",
    ]
    ports = {}
    for _pin, port, num, af, _sig in pins:
        ports.setdefault(port, []).append((num, af))
    for port, lst in sorted(ports.items()):
        base = GPIO_BASE + 0x400 * (ord(port) - ord("A"))
        moder_set = moder_clr = ospeed = afrl_set = afrl_clr = afrh_set = afrh_clr = pupd_clr = 0
        for num, af in lst:
            moder_set |= 2 << (num * 2)
            moder_clr |= 3 << (num * 2)
            ospeed |= 3 << (num * 2)
            pupd_clr |= 3 << (num * 2)
            if num < 8:
                afrl_set |= af << (num * 4)
                afrl_clr |= 0xF << (num * 4)
            else:
                afrh_set |= af << ((num - 8) * 4)
                afrh_clr |= 0xF << ((num - 8) * 4)
        names = ", ".join(f"P{port}{n}:AF{a}" for n, a in lst)
        out.append(f"\t# 端口 {port}：{names}")
        out.append(f"\tmmw 0x{base:08X} 0x{moder_set:08X} 0x{moder_clr:08X}\t;# MODER")
        out.append(f"\tmmw 0x{base + 0x08:08X} 0x{ospeed:08X} 0x00000000\t;# OSPEEDR")
        out.append(f"\tmmw 0x{base + 0x0C:08X} 0x00000000 0x{pupd_clr:08X}\t;# PUPDR")
        if afrl_clr:
            out.append(f"\tmmw 0x{base + 0x20:08X} 0x{afrl_set:08X} 0x{afrl_clr:08X}\t;# AFRL")
        if afrh_clr:
            out.append(f"\tmmw 0x{base + 0x24:08X} 0x{afrh_set:08X} 0x{afrh_clr:08X}\t;# AFRH")
    size_log2 = chip["size"].bit_length() - 1
    cr = (1 << 24) | (3 << 8) | (1 << 4) | ((1 << 7) if bank == 2 else 0)  # PRESCALER=1、FTHRES=3、SSHIFT、FSEL
    dcr = ((size_log2 - 1) << 16) | (5 << 8)  # FSIZE、CSHT=6 个周期
    adsize = chip["addr_bytes"] - 1
    read_cmd = 0x13 if chip["addr_bytes"] == 4 else 0x03
    ccr = (3 << 26) | (1 << 24) | (adsize << 12) | (1 << 10) | (1 << 8) | read_cmd  # 单线内存映射读
    out += [
        f"\tmww 0x{QUADSPI_BASE:08X} 0x{cr:08X}\t\t\t;# QUADSPI_CR：PRESCALER=1、FTHRES=3、SSHIFT{'、FSEL=Bank2' if bank == 2 else ''}",
        f"\tmww 0x{QUADSPI_BASE + 4:08X} 0x{dcr:08X}\t\t\t;# QUADSPI_DCR：FSIZE={size_log2 - 1}、CSHT=5",
        f"\tmww 0x{QUADSPI_BASE + 0x14:08X} 0x{ccr:08X}\t\t\t;# QUADSPI_CCR：单线内存映射读 0x{read_cmd:02X}",
        f"\tmmw 0x{QUADSPI_BASE:08X} 0x00000001 0\t\t\t;# QUADSPI_CR：EN=1",
    ]
    if chip["addr_bytes"] == 4:
        out += [
            f"\tmmw 0x{QUADSPI_BASE:08X} 0x00000002 0\t\t\t;# ABORT",
            f"\tmww 0x{QUADSPI_BASE + 0x14:08X} 0x000001B7\t\t\t;# 0xB7：进 4 字节地址模式",
            "\tsleep 1",
            f"\tmmw 0x{QUADSPI_BASE:08X} 0x00000002 0\t\t\t;# ABORT",
            f"\tmww 0x{QUADSPI_BASE + 0x14:08X} 0x{ccr:08X}\t\t\t;# 回到内存映射读",
        ]
    return "\n".join(out)
