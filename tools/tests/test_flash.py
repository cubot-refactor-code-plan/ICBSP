"""
外置 QSPI Flash / 编译优化 / .gitignore 的主机侧测试（不需要工具链和硬件）。

    cd tools && python3 -m unittest discover -s tests -v

夹具 fixtures/h750_qspi.ioc：H750_FULL_TEST 回归宿主的 .ioc（STM32H750VBTx，W25Q128 在 QUADSPI Bank 2，实板验证过）。
"""

import json
import re
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

import integrate  # noqa: E402
import qspi_image  # noqa: E402
from icbsp_tool import checks, flash, host_options, manifest, regions, selection  # noqa: E402
from icbsp_tool.ioc import Ioc  # noqa: E402

MAN = manifest.load()
W25Q128 = flash.chip_for(MAN, "W25Q128")


def load(name):
    return Ioc(HERE / "fixtures" / name)


class FlashAnalyzeTest(unittest.TestCase):
    def test_flash_size_code(self):
        self.assertEqual(flash.internal_flash_kb("STM32H750VBTx"), 128)
        self.assertEqual(flash.internal_flash_kb("STM32H723VGTx"), 1024)
        self.assertEqual(flash.internal_flash_kb("STM32H743VITx"), 2048)
        self.assertEqual(flash.internal_flash_kb("STM32F407VGTx"), 1024)
        self.assertIsNone(flash.internal_flash_kb("nonsense"))

    def test_h750_suggest(self):
        a = flash.analyze(load("h750_qspi.ioc"), None, MAN)
        self.assertEqual((a["peripheral"], a["supported"], a["bank"], a["internal_kb"]), ("QUADSPI", True, 2, 128))
        self.assertTrue(a["suggest"])
        self.assertEqual([p["signal"] for p in a["pins"]], ["CLK", "NCS", "IO0", "IO1", "IO2", "IO3"])
        self.assertEqual(a["chip_guess"], "W25Q128")  # QUADSPI.FlashSize = 23

    def test_octospi_not_supported(self):
        a = flash.analyze(load("mc02_h723.ioc"), None, MAN)
        self.assertFalse(a["supported"])
        self.assertTrue(a["peripheral"].startswith("OCTOSPI"))
        self.assertIn("暂不支持", a["note"])
        self.assertFalse(a["suggest"])

    def test_pin_af_table(self):
        pins = flash.pin_afs(load("h750_qspi.ioc"), None)  # 没有 quadspi.c 时用内置表
        self.assertEqual([(p[0], p[3]) for p in pins],
                         [("PB2", 9), ("PC11", 9), ("PE7", 10), ("PE8", 10), ("PE9", 10), ("PE10", 10)])

    def test_af_from_msp(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "quadspi.c"
            p.write_text("    GPIO_InitStruct.Pin = GPIO_PIN_7|GPIO_PIN_8;\n    GPIO_InitStruct.Alternate = GPIO_AF10_QUADSPI;\n"
                         "    HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);\n", encoding="utf-8")
            self.assertEqual(flash.af_from_msp(p), {"PE7": 10, "PE8": 10})

    def test_map_usage_skips_noload(self):
        with tempfile.TemporaryDirectory() as d:
            m = Path(d) / "x.map"
            m.write_text("Linker script and memory map\n\n"
                         ".text           0x08000000     0x1000\n"
                         ".data           0x20000000      0x100 load address 0x08001000\n"
                         ".bss            0x20000100     0x7000 load address 0x08001100\n"
                         "._user_heap_stack 0x20007100  0x600 load address 0x08001100\n", encoding="utf-8")
            self.assertEqual(flash._map_flash_bytes(m), 0x1100)


class FlashGenerateTest(unittest.TestCase):
    def setUp(self):
        self.pins = flash.pin_afs(load("h750_qspi.ioc"), None)

    def test_config_header(self):
        h = flash.config_header(W25Q128, 2, self.pins, 2)
        # cmake/icbsp_flash.cmake 用正则读这两个整数宏
        for name, want in (("ICBSP_QSPI_FLASH_SIZE", 16 << 20), ("ICBSP_AXI_SRAM_SIZE", 512 << 10)):
            m = re.search(rf"#define {name} \(?(0[xX][0-9a-fA-F]+|[0-9]+)[uU]?", h)
            self.assertEqual(int(m.group(1), 0), want)
        self.assertIn("#define ICBSP_QSPI_BANK 2", h)
        self.assertIn("X(E, 10, 10)", h)
        self.assertNotRegex(h, r"[ \t]+\n", "行尾不应有空格")

    def test_openocd_init(self):
        tcl = flash.openocd_qspi_init(W25Q128, 2, self.pins)
        self.assertIn("mww 0x52005000 0x01000390", tcl)  # PRESCALER=1、FTHRES=3、FSEL、SSHIFT
        self.assertIn("mww 0x52005004 0x00170500", tcl)  # FSIZE=23、CSHT=5
        self.assertIn("mww 0x52005014 0x0D002503", tcl)  # 单线内存映射读 0x03
        self.assertIn("mmw 0x58021024 0x00000AAA 0x00000FFF", tcl)  # PE8~PE10 AF10


class OptionsSelectionTest(unittest.TestCase):
    def test_defaults(self):
        s = selection.default_selection(load("h750_qspi.ioc"), MAN)
        self.assertFalse(s["external_flash"]["enabled"])
        self.assertEqual(s["external_flash"]["chip"], "W25Q128")
        self.assertFalse(s["optimization"]["enabled"])
        self.assertTrue(s["gitignore"])

    def test_merge_and_validate(self):
        ioc = load("h750_qspi.ioc")
        s = selection.merge(ioc, MAN, {"external_flash": {"enabled": True, "prescaler": 0}})
        bad = selection.validate_options(ioc, MAN, s)
        self.assertTrue(any(w == "external_flash.prescaler" and lv == "error" for lv, w, _ in bad))  # 240 MHz > 133
        s = selection.merge(ioc, MAN, {"external_flash": {"enabled": True, "prescaler": 2}})
        self.assertEqual([b for b in selection.validate_options(ioc, MAN, s) if b[0] == "error"], [])
        s = selection.merge(ioc, MAN, {"optimization": {"enabled": True, "Debug": "-O7"}})
        self.assertTrue(any(w == "optimization.Debug" for _, w, _ in selection.validate_options(ioc, MAN, s)))

    def test_octospi_enabled_is_error(self):
        ioc = load("mc02_h723.ioc")
        s = selection.merge(ioc, MAN, {"external_flash": {"enabled": True, "chip": "W25Q64"}})
        self.assertTrue(any(lv == "error" and w == "external_flash" for lv, w, _ in selection.validate_options(ioc, MAN, s)))

    def test_qspi_params_rule(self):
        ioc = load("h750_qspi.ioc")
        ef = {"enabled": True, "chip": "W25Q128", "prescaler": 2}
        r = [x for x in checks.run(ioc, MAN, {}, ef) if x["id"] == "qspi_params"]
        self.assertEqual(len(r), 1)
        self.assertTrue(r[0]["ok"], r[0]["detail"])  # 夹具已经按芯片改好
        ef["prescaler"] = 3
        r = [x for x in checks.run(ioc, MAN, {}, ef) if x["id"] == "qspi_params"][0]
        self.assertFalse(r["ok"])
        self.assertIn({"key": "QUADSPI.ClockPrescaler", "old": "2", "new": "3"}, r["changes"])
        self.assertEqual([x for x in checks.run(ioc, MAN, {}, None) if x["id"] == "qspi_params"], [])


class HashRegionTest(unittest.TestCase):
    def test_roundtrip(self):
        t = "a\n" + regions.block("x", "foo", "#") + "\nb"
        r = regions.parse(t, "#")[0]["x"]
        self.assertEqual(r["recorded"], r["actual"])
        self.assertEqual(regions.parse(t)[0], {})  # // 标记不认 #
        t2 = regions.replace(t, "x", "bar", "#")
        self.assertEqual(regions.parse(t2, "#")[0]["x"]["body"], "bar")
        self.assertEqual(regions.edited(t2.replace("bar", "baz"), "#"), ["x"])


def fake_host(tmp):
    host = Path(tmp)
    (host / "CMakeLists.txt").write_text(
        "project(demo)\nadd_subdirectory(cmake/stm32cubemx)\n\n"
        "# icbsp：必须在 add_subdirectory(cmake/stm32cubemx) 之后接入\nadd_subdirectory(third_party/icbsp)\n", encoding="utf-8")
    (host / "CMakePresets.json").write_text(json.dumps({
        "version": 3,
        "configurePresets": [{"name": "default", "hidden": True}, {"name": "Debug", "inherits": "default"},
                             {"name": "Release", "inherits": "default"}],
        "buildPresets": [{"name": "Debug", "configurePreset": "Debug"}, {"name": "Release", "configurePreset": "Release"}],
    }, indent=4), encoding="utf-8")
    return host


class HostOptionsTest(unittest.TestCase):
    def sel(self, **kw):
        s = selection.default_selection(load("h750_qspi.ioc"), MAN)
        selection.merge_options(s, kw)
        return s

    def test_cmake_region(self):
        with tempfile.TemporaryDirectory() as d:
            host = fake_host(d)
            plan, rep = integrate.Plan(), integrate.Report()
            s = self.sel(external_flash={"enabled": True, "layout": "QSPI_XIP"},
                         optimization={"enabled": True, "Debug": "-Og", "vendor_os": True})
            host_options.plan_cmake_region(host, plan, rep, integrate.TextFile, s, "third_party/icbsp", "user/board", MAN, False)
            self.assertFalse(rep.has_error, rep.items)
            text = plan.edits[host / "CMakeLists.txt"].text
            self.assertLess(text.index("<ICBSP-GEN icbsp-options>"), text.index("# icbsp：必须在"))
            self.assertIn("DEFAULT QSPI_XIP", text)
            self.assertIn("icbsp_optimization(TARGET ${CMAKE_PROJECT_NAME} Debug -Og RelWithDebInfo -O2 Release -Os VENDOR_OS)", text)
            # 再跑一次：内容一致就跳过；关掉两项就清空生成区
            (host / "CMakeLists.txt").write_text(text, encoding="utf-8")
            plan, rep = integrate.Plan(), integrate.Report()
            host_options.plan_cmake_region(host, plan, rep, integrate.TextFile, s, "third_party/icbsp", "user/board", MAN, False)
            self.assertTrue(plan.empty())
            off = self.sel()
            host_options.plan_cmake_region(host, plan, rep, integrate.TextFile, off, "third_party/icbsp", "user/board", MAN, False)
            self.assertNotIn("icbsp_flash_layout", plan.edits[host / "CMakeLists.txt"].text)

    def test_cmake_region_edited(self):
        with tempfile.TemporaryDirectory() as d:
            host = fake_host(d)
            text = (host / "CMakeLists.txt").read_text(encoding="utf-8")
            text = text.replace("# icbsp：必须在", regions.block("icbsp-options", "set(X 1)", "#").replace("set(X 1)", "set(X 2)") + "\n# icbsp：必须在")
            (host / "CMakeLists.txt").write_text(text, encoding="utf-8")
            plan, rep = integrate.Plan(), integrate.Report()
            s = self.sel(optimization={"enabled": True})
            host_options.plan_cmake_region(host, plan, rep, integrate.TextFile, s, "third_party/icbsp", "user/board", MAN, False)
            self.assertTrue(rep.has_error)

    def test_presets(self):
        with tempfile.TemporaryDirectory() as d:
            host = fake_host(d)
            plan, rep = integrate.Plan(), integrate.Report()
            host_options.plan_presets(host, plan, rep, integrate.TextFile, self.sel(external_flash={"enabled": True}), MAN)
            data = json.loads(plan.edits[host / "CMakePresets.json"].text)
            names = [p["name"] for p in data["configurePresets"]]
            for n in ("RelWithDebInfo", "Debug-Internal", "Debug-QspiRam", "Debug-QspiXip", "Release-QspiXip", "RelWithDebInfo-QspiRam"):
                self.assertIn(n, names)
            p = next(p for p in data["configurePresets"] if p["name"] == "Debug-QspiXip")
            self.assertEqual(p, {"name": "Debug-QspiXip", "inherits": "Debug", "cacheVariables": {"ICBSP_FLASH_LAYOUT": "QSPI_XIP"}})
            self.assertEqual(sorted(names), sorted({p["configurePreset"] for p in data["buildPresets"]} | {"default"}))

    def test_gitignore(self):
        with tempfile.TemporaryDirectory() as d:
            host = Path(d)
            plan, rep = integrate.Plan(), integrate.Report()
            host_options.plan_gitignore(host, plan, rep, integrate.TextFile)
            self.assertIn("build/", plan.new_files[host / ".gitignore"])
            (host / ".gitignore").write_text("build/\n*.su\nmy_secret.txt\n", encoding="utf-8")
            plan, rep = integrate.Plan(), integrate.Report()
            host_options.plan_gitignore(host, plan, rep, integrate.TextFile)
            text = plan.edits[host / ".gitignore"].text
            self.assertTrue(text.startswith("build/\n*.su\nmy_secret.txt\n"))
            self.assertIn(host_options.GITIGNORE_BEGIN, text)
            self.assertEqual(text.count("build/"), 1)
            (host / ".gitignore").write_text(text, encoding="utf-8")
            plan, rep = integrate.Plan(), integrate.Report()
            host_options.plan_gitignore(host, plan, rep, integrate.TextFile)
            self.assertTrue(plan.empty())


# ---------------------------------------------------------------- qspi_image


def make_elf(sections):
    """最小的 ELF32 小端：sections = [(lma, vma, data or size, nobits)]；每段一个 PT_LOAD"""
    ehsize, phentsize, shentsize = 52, 32, 40
    phnum = len(sections)
    off = ehsize + phnum * phentsize
    blobs, phdrs, shdrs = b"", [], [struct.pack("<10I", *([0] * 10))]
    for lma, vma, data, nobits in sections:
        size = data if nobits else len(data)
        phdrs.append(struct.pack("<8I", 1, off, vma, lma, 0 if nobits else size, size, 6, 4))
        shdrs.append(struct.pack("<10I", 0, 8 if nobits else 1, 2, vma, off, size, 0, 0, 4, 0))
        if not nobits:
            blobs += data
            off += size
    shoff = off
    eh = b"\x7fELF" + bytes([1, 1, 1]) + bytes(9)
    eh += struct.pack("<HHIIIIIHHHHHH", 2, 40, 1, 0, ehsize, shoff, 0, ehsize, phentsize, phnum, shentsize, len(shdrs), 0)
    return eh + b"".join(phdrs) + blobs + b"".join(shdrs)


class QspiImageTest(unittest.TestCase):
    def test_header_roundtrip(self):
        payload = bytes(range(256)) * 4
        h = qspi_image.make_header(1, 0x24000000, payload)
        p = qspi_image.parse_header(h)
        self.assertTrue(p["header_ok"])
        self.assertEqual((p["size"], p["payload_crc"]), (1024, zlib.crc32(payload)))

    def test_intel_hex(self):
        text = qspi_image.intel_hex(b"\x01\x02", 0x90000000)
        lines = text.split()
        self.assertEqual(lines[0], ":020000049000" + "6A")
        for line in lines:
            body = bytes.fromhex(line[1:])
            self.assertEqual(sum(body) & 0xFF, 0)
        self.assertEqual(lines[-1], ":00000001FF")

    def test_ram_image(self):
        vec = struct.pack("<II", 0x20020000, 0x24000101) + b"\x00" * 0x100
        elf = make_elf([(0x24000000, 0x24000000, vec, False),
                        (0x24000108, 0x20000000, b"DATA", False),     # .data：运行在 DTCM，初值跟在代码后面
                        (0x20000004, 0x20000004, 0x100, True)])       # .bss
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "a.elf"
            p.write_bytes(elf)
            h = qspi_image.build(p, "ram", Path(d), 16 << 20, 0x24000000, 512 << 10)
            img = (Path(d) / "a_qspi.bin").read_bytes()
            self.assertEqual(h["load_addr"], 0x24000000)
            self.assertEqual(img[0x400 : 0x400 + 8], vec[:8])
            self.assertEqual(img[0x400 + 0x108 : 0x400 + 0x10C], b"DATA")
            self.assertEqual(h["size"] % 4, 0)

    def test_ram_rejects_outside(self):
        elf = make_elf([(0x24000000, 0x24000000, b"\x00" * 8, False), (0x08000000, 0x08000000, b"x" * 4, False)])
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "a.elf"
            p.write_bytes(elf)
            with self.assertRaises(qspi_image.ImageError):
                qspi_image.build(p, "ram", Path(d), 16 << 20, 0x24000000, 512 << 10)

    def test_xip_patches_elf(self):
        head = struct.pack("<8I", qspi_image.MAGIC, 1, 2, 0x90000400, 0x90000400, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF)
        head += b"\xff" * (0x400 - len(head))
        code = struct.pack("<II", 0x20020000, 0x90000401) + b"\xaa" * 24
        elf = make_elf([(0x90000000, 0x90000000, head + code, False)])
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "b.elf"
            p.write_bytes(elf)
            h = qspi_image.build(p, "xip", Path(d), 16 << 20, 0x24000000, 512 << 10)
            self.assertEqual((h["size"], h["payload_crc"]), (len(code), zlib.crc32(code)))
            patched = p.read_bytes()
            i = patched.index(struct.pack("<I", qspi_image.MAGIC))
            self.assertTrue(qspi_image.parse_header(patched[i : i + 32])["header_ok"])
            img = (Path(d) / "b_qspi.bin").read_bytes()
            self.assertEqual(img[:32], patched[i : i + 32])


if __name__ == "__main__":
    unittest.main()
