#!/usr/bin/env python3
"""icbsp：外置 QSPI Flash 镜像工具（构建后由 cmake/icbsp_flash.cmake 自动调用）

镜像在 QSPI 里的布局（两种方案相同）：

    0x90000000  镜像头（32 字节有效，补 0xFF 到 1 KB）
    0x90000400  载荷：方案 A 是要拷进 AXI SRAM 的程序，方案 B 是就地执行的程序（向量表在开头）

镜像头（小端，全部 uint32）：

    0  magic          'ICBI'（0x49424349）
    4  version        1
    8  layout         1 = QSPI_RAM（方案 A），2 = QSPI_XIP（方案 B）
    12 load_addr      载荷要放到的地址（A：AXI SRAM 起点；B：0x90000400）
    16 vector_addr    向量表地址（= load_addr）
    20 size           载荷字节数（4 字节对齐）
    24 payload_crc    载荷的 CRC-32（zlib 算法，与 bootloader 里 CRC 外设的配置一致）
    28 header_crc     前 28 字节的 CRC-32

方案 A：ELF 的运行地址和加载地址都在 AXI SRAM（调试器可直接下载进 RAM），本工具另外生成
        <名字>_qspi.bin / .hex（头 + 载荷，地址 0x90000000）用于写 QSPI。
方案 B：链接脚本在 0x90000000 预留了 .icbsp_image_header 段，本工具把算好的头写回 ELF，
        所以调试器直接下载 ELF 也能得到有效镜像；同样生成 _qspi.bin / .hex。

只用标准库；用法见 --help。
"""
from __future__ import annotations

import argparse
import struct
import sys
import zlib
from pathlib import Path

MAGIC = 0x49424349  # b"ICBI"
VERSION = 1
LAYOUTS = {"ram": 1, "xip": 2}
QSPI_BASE = 0x90000000
HEADER_SIZE = 0x400
HEADER_FMT = "<8I"
TAIL_GUARD = 0x1000  # 芯片最后 4 KB：bootloader 用 MPU 禁止访问（QUADSPI 预取越过芯片末尾会挂死总线）
PT_LOAD = 1


class ImageError(Exception):
    pass


# ---------------------------------------------------------------- ELF


SHT_NOBITS = 8
SHF_ALLOC = 0x2


def load_segments(elf: bytes) -> list[tuple[int, int, bytes]]:
    """返回 [(加载地址 LMA, 文件偏移, 数据)]（ELF32 小端）。

    按“有内容的 ALLOC 段（section）”取，再用所在 PT_LOAD 换算加载地址：ld 有时把 ELF 头本身映射进
    第一个 PT_LOAD（例如 0x20000000 处只有 .bss 的段），按 segment 取会把这些头字节当成程序。
    """
    if elf[:4] != b"\x7fELF":
        raise ImageError("不是 ELF 文件")
    if elf[4] != 1 or elf[5] != 1:
        raise ImageError("只支持 32 位小端 ELF")
    (e_phoff, e_shoff) = struct.unpack_from("<II", elf, 28)
    e_phentsize, e_phnum, e_shentsize, e_shnum = struct.unpack_from("<HHHH", elf, 42)
    phdrs = []
    for i in range(e_phnum):
        p_type, p_offset, _vaddr, p_paddr, p_filesz, _memsz, _flags, _align = struct.unpack_from(
            "<8I", elf, e_phoff + i * e_phentsize
        )
        if p_type == PT_LOAD and p_filesz > 0:
            phdrs.append((p_offset, p_filesz, p_paddr))
    segs = []
    for i in range(e_shnum):
        _name, sh_type, sh_flags, _addr, sh_offset, sh_size = struct.unpack_from("<6I", elf, e_shoff + i * e_shentsize)
        if not (sh_flags & SHF_ALLOC) or sh_type == SHT_NOBITS or sh_size == 0:
            continue
        for p_offset, p_filesz, p_paddr in phdrs:
            if p_offset <= sh_offset and sh_offset + sh_size <= p_offset + p_filesz:
                segs.append((p_paddr + (sh_offset - p_offset), sh_offset, elf[sh_offset : sh_offset + sh_size]))
                break
        else:
            raise ImageError(f"有内容的段（文件偏移 0x{sh_offset:X}）不在任何 PT_LOAD 里")
    if not segs:
        raise ImageError("ELF 里没有可加载的段")
    return sorted(segs)


def flatten(segs, start: int, fill: int) -> bytes:
    """把 [start, 最高地址) 范围内的段拼成连续字节，空隙用 fill 填充。"""
    end = max(addr + len(data) for addr, _, data in segs)
    out = bytearray([fill]) * (end - start)
    for addr, _, data in segs:
        if addr < start:
            raise ImageError(f"段地址 0x{addr:08X} 低于载荷起点 0x{start:08X}")
        out[addr - start : addr - start + len(data)] = data
    while len(out) % 4:
        out.append(fill)
    return bytes(out)


# ---------------------------------------------------------------- 头


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def make_header(layout: int, load_addr: int, payload: bytes) -> bytes:
    head = struct.pack("<7I", MAGIC, VERSION, layout, load_addr, load_addr, len(payload), crc32(payload))
    return head + struct.pack("<I", crc32(head))


def parse_header(raw: bytes) -> dict:
    fields = struct.unpack_from(HEADER_FMT, raw, 0)
    keys = ("magic", "version", "layout", "load_addr", "vector_addr", "size", "payload_crc", "header_crc")
    h = dict(zip(keys, fields))
    h["header_ok"] = h["magic"] == MAGIC and crc32(raw[:28]) == h["header_crc"]
    return h


# ---------------------------------------------------------------- 输出


def intel_hex(data: bytes, base: int) -> str:
    lines = []

    def rec(rtype: int, addr: int, payload: bytes) -> str:
        body = bytes([len(payload), (addr >> 8) & 0xFF, addr & 0xFF, rtype]) + payload
        return ":" + body.hex().upper() + f"{(-sum(body)) & 0xFF:02X}"

    upper = None
    for off in range(0, len(data), 16):
        addr = base + off
        if addr >> 16 != upper:
            upper = addr >> 16
            lines.append(rec(4, 0, struct.pack(">H", upper)))
        lines.append(rec(0, addr & 0xFFFF, data[off : off + 16]))
    lines.append(rec(1, 0, b""))
    return "\n".join(lines) + "\n"


def build(elf_path: Path, layout_name: str, out_dir: Path, qspi_size: int, ram_base: int, ram_size: int) -> dict:
    elf = bytearray(elf_path.read_bytes())
    segs = load_segments(bytes(elf))
    layout = LAYOUTS[layout_name]

    if layout_name == "ram":
        lo = min(addr for addr, _, _ in segs)
        for addr, _, data in segs:
            if not (ram_base <= addr and addr + len(data) <= ram_base + ram_size):
                raise ImageError(
                    f"方案 A 的 ELF 里有不在 AXI SRAM（0x{ram_base:08X}+{ram_size // 1024}K）的可加载段："
                    f"0x{addr:08X}，长度 {len(data)}"
                )
        if lo != ram_base:
            raise ImageError(f"方案 A 的向量表应在 0x{ram_base:08X}，实际最低加载地址 0x{lo:08X}")
        load_addr = ram_base
        payload = flatten(segs, load_addr, 0x00)
        if len(payload) > qspi_size - HEADER_SIZE - TAIL_GUARD:
            raise ImageError(f"镜像 {len(payload)} 字节超出 QSPI 可用容量（末尾 4 KB 由 MPU 禁止访问）")
        header = make_header(layout, load_addr, payload)
    else:
        load_addr = QSPI_BASE + HEADER_SIZE
        head_seg = [s for s in segs if s[0] <= QSPI_BASE < s[0] + len(s[2])]
        if not head_seg:
            raise ImageError("方案 B 的 ELF 在 0x90000000 没有 .icbsp_image_header 段（链接脚本不对？）")
        for addr, _, data in segs:
            if addr < QSPI_BASE or addr + len(data) > QSPI_BASE + qspi_size:
                raise ImageError(f"方案 B 的 ELF 里有不在 QSPI 的可加载段：0x{addr:08X}，长度 {len(data)}")
        # 从 0x90000000 拼成整块，前 1 KB 是镜像头（链接脚本里补到了 0x400），其后是载荷
        flat = flatten(segs, QSPI_BASE, 0xFF)
        if len(flat) <= HEADER_SIZE:
            raise ImageError("方案 B 的 ELF 在镜像头之后没有内容")
        payload = flat[HEADER_SIZE:]
        if len(payload) > qspi_size - HEADER_SIZE - TAIL_GUARD:
            raise ImageError(f"镜像 {len(payload)} 字节超出 QSPI 可用容量（末尾 4 KB 由 MPU 禁止访问）")
        header = make_header(layout, load_addr, payload)
        paddr, off, _ = head_seg[0]
        pos = off + (QSPI_BASE - paddr)
        old = parse_header(bytes(elf[pos : pos + 32]))
        if old["magic"] != MAGIC:
            raise ImageError("0x90000000 处不是 icbsp 镜像头（magic 不对）")
        elf[pos : pos + 32] = header
        elf_path.write_bytes(bytes(elf))

    image = header + b"\xff" * (HEADER_SIZE - len(header)) + payload
    stem = elf_path.stem + "_qspi"
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / f"{stem}.bin").write_bytes(image)
    (out_dir / f"{stem}.hex").write_text(intel_hex(image, QSPI_BASE), encoding="ascii")
    h = parse_header(header)
    h.update(bin=str(out_dir / f"{stem}.bin"), hex=str(out_dir / f"{stem}.hex"), layout_name=layout_name)
    return h


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="生成 icbsp 外置 QSPI Flash 镜像（头 + 载荷）")
    ap.add_argument("--elf", required=True, type=Path)
    ap.add_argument("--layout", required=True, choices=sorted(LAYOUTS))
    ap.add_argument("--out-dir", type=Path, help="输出目录（默认与 ELF 相同）")
    ap.add_argument("--qspi-size", type=lambda s: int(s, 0), default=16 << 20)
    ap.add_argument("--ram-base", type=lambda s: int(s, 0), default=0x24000000)
    ap.add_argument("--ram-size", type=lambda s: int(s, 0), default=512 << 10)
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args(argv)
    try:
        h = build(a.elf, a.layout, a.out_dir or a.elf.parent, a.qspi_size, a.ram_base, a.ram_size)
    except (ImageError, OSError) as e:
        print(f"qspi_image: 错误：{e}", file=sys.stderr)
        return 1
    if not a.quiet:
        name = "QSPI_RAM（方案 A）" if h["layout"] == 1 else "QSPI_XIP（方案 B）"
        print(
            f"qspi_image: {name} 载荷 {h['size']} 字节 → 0x{h['load_addr']:08X}，"
            f"CRC 0x{h['payload_crc']:08X}；{h['hex']}"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
