"""
接入引擎（tools/icbsp_tool）的主机侧测试：不需要工具链和硬件，CI 里跑。

    cd tools && python3 -m unittest discover -s tests -v
    ICBSP_UPDATE_SNAPSHOTS=1 python3 -m unittest discover -s tests   # 生成结果有意改变后，更新快照

夹具：
    fixtures/mc02_h723.ioc       DM-MC02 回归宿主的 .ioc（实板验证过的配置）
    fixtures/defaults_h723.ioc   从上面删掉若干键（FDCAN 元素数 / 偏移 / 过滤器、SPI2 KeepIO、USART1 中断、栈溢出检查）
                                 = 新建工程里这些项保持 CubeMX 默认值的样子
用例 cases/*.json：{ioc, selection}；生成结果与 snapshots/<用例>/<文件>__<生成区>.txt 逐字比对。
编译验证（生成结果 0 warning）见 compile_test.sh，需要 ARM 工具链与 STM32CubeMX。
"""

import json
import os
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

from icbsp_tool import checks, generate, manifest, regions, selection  # noqa: E402
from icbsp_tool.ioc import Ioc, fdcan_words  # noqa: E402

TEMPLATES = HERE.parent.parent / "templates" / "user" / "board"
UPDATE = os.environ.get("ICBSP_UPDATE_SNAPSHOTS") == "1"


def load(name):
    return Ioc(HERE / "fixtures" / name)


class ManifestTest(unittest.TestCase):
    def test_valid(self):
        self.assertEqual(manifest.validate(manifest.load()), [])

    def test_templates_sealed(self):
        for f in ("board_cfg.hpp", "board_cfg.cpp", "board_instances.hpp", "hardware_config.h"):
            text = (TEMPLATES / f).read_text(encoding="utf-8")
            regs, bad = regions.parse(text)
            self.assertEqual(bad, [], f)
            self.assertTrue(regs, f"{f} 没有生成区")
            self.assertEqual(regions.edited(text), [], f"{f} 的生成区哈希不对（改模板后要重新 seal）")


class IocTest(unittest.TestCase):
    def test_mc02_parse(self):
        i = load("mc02_h723.ioc")
        self.assertEqual((i.mcu, i.series, i.timebase_tim()), ("STM32H723VGTx", "h7", "TIM8"))
        cans = i.can_instances()
        self.assertEqual([c["msgram_offset"] for c in cans], [0, 129, 258])
        self.assertEqual([fdcan_words(c) for c in cans], [129, 129, 129])
        self.assertEqual(i.timer_clock(12), (275000000, True))
        self.assertIn("GYRO_ACC_CS", i.gpio_outputs())
        self.assertIn("GYRO_GYRO_INT", i.gpio_exti())
        self.assertIn((12, 2), i.pwm_channels())

    def test_defaults(self):
        i = load("defaults_h723.ioc")
        c = i.can_instances()[0]
        self.assertEqual((c["rx_fifo0"], c["tx_fifo_queue"], c["std_filters"], c["msgram_offset"]), (0, 0, 0, 0))
        self.assertEqual(i.freertos()["stack_check"], 0)

    def test_set_roundtrip(self):
        i = load("mc02_h723.ioc")
        i.set("TIM1.Channel-PWM Generation1 CH1", "TIM_CHANNEL_1")  # 键里有空格
        self.assertFalse(i.changed())
        i.set("ZZZ.New", "a:b")
        self.assertIn("ZZZ.New=a\\:b", i.text())
        self.assertEqual(Ioc.__new__(Ioc).__class__, Ioc)


class ChecksTest(unittest.TestCase):
    def setUp(self):
        self.m = manifest.load()

    def run_checks(self, name):
        i = load(name)
        sel = selection.default_selection(i, self.m)
        return i, checks.run(i, self.m, selection.by_kind(sel))

    def test_mc02_all_pass(self):
        _, res = self.run_checks("mc02_h723.ioc")
        self.assertEqual([r for r in res if not r["ok"]], [])

    def test_defaults_fail_and_fix(self):
        i, res = self.run_checks("defaults_h723.ioc")
        failed = {(r["id"], r["target"]) for r in res if not r["ok"]}
        for want in [("can_fifo", "FDCAN1"), ("can_std_filters", "FDCAN2"), ("spi_keep_io", "SPI2"), ("uart_nvic", "USART1"),
                     ("rtos_stack_check", ""), ("can_msgram", "FDCAN")]:
            self.assertIn(want, failed)
        from icbsp_tool import cubemx

        cubemx.apply_fixes(i, res, "all")
        sel = selection.default_selection(i, self.m)
        again = checks.run(i, self.m, selection.by_kind(sel))
        self.assertEqual([(r["id"], r["target"]) for r in again if not r["ok"]], [])
        self.assertEqual([c["msgram_offset"] for c in i.can_instances()], [0, 129, 258])

    def test_usable(self):
        u = checks.usable(load("mc02_h723.ioc"))
        self.assertFalse(u["SPI1"][0])  # 只发送
        self.assertTrue(u["SPI2"][0])


class SelectionTest(unittest.TestCase):
    def setUp(self):
        self.m = manifest.load()
        self.i = load("mc02_h723.ioc")

    def errors(self, user):
        sel = selection.merge(self.i, self.m, user)
        return [w + " " + msg for _, w, msg in selection.validate(self.i, self.m, sel)]

    def test_default_has_no_pwm(self):
        sel = selection.default_selection(self.i, self.m)
        self.assertFalse(any(p["enabled"] for p in sel["peripherals"].values() if p["kind"] == "pwm"))

    def test_errors(self):
        dev = lambda **k: {"peripherals": {"TIM12_CH2": {"enabled": True}}, "devices": [k]}
        self.assertTrue(any("1~7" in e for e in self.errors(dev(type="dji_motor", name="m", needs={"bus": "FDCAN1"}, params={"type": "GM6020", "id": 8}))))
        self.assertTrue(any("计划中" in e for e in self.errors(dev(type="dr16", name="rc", needs={"bus": "USART1"}))))
        self.assertTrue(any("缺" in e for e in self.errors(dev(type="bmi088", name="imu", needs={"bus": "SPI2"}))))
        two = {"peripherals": {"TIM12_CH2": {"enabled": True}},
               "devices": [{"type": "buzzer", "name": "a", "needs": {"pwm": "TIM12_CH2"}}, {"type": "servo", "name": "b", "needs": {"pwm": "TIM12_CH2"}}]}
        self.assertTrue(any("已经给" in e for e in self.errors(two)))
        dup = {"devices": [{"type": "r9ds", "name": "bsp_uart1", "needs": {"bus": "USART1"}}]}
        self.assertTrue(any("重复" in e for e in self.errors(dup)))
        self.assertTrue(any("没有选中" in e for e in self.errors({"peripherals": {"FDCAN1": {"enabled": False}},
                                                                 "devices": [{"type": "dm_motor", "name": "x", "needs": {"bus": "FDCAN1"}}]})))
        self.assertTrue(any("需要 ICBSP_SYS_TASK_ENABLE" in e for e in self.errors({"switches": {"ICBSP_SYS_TASK_ENABLE": 0}})))

    def test_catalog(self):
        sel = selection.default_selection(self.i, self.m)
        cat = {d["type"]: d for d in selection.device_catalog(self.i, self.m, sel)}
        self.assertTrue(cat["bmi088"]["available"])
        self.assertFalse(cat["device_motor"]["available"])  # 没有编码器
        self.assertFalse(cat["dr16"]["available"])


class GenerateTest(unittest.TestCase):
    def test_snapshots(self):
        m = manifest.load()
        for case in sorted((HERE / "cases").glob("*.json")):
            c = json.loads(case.read_text(encoding="utf-8"))
            i = load(c["ioc"])
            sel = selection.merge(i, m, c["selection"])
            self.assertEqual([b for b in selection.validate(i, m, sel) if b[0] == "error"], [], case.name)
            out = generate.Generator(i, m, sel).regions()
            snap = HERE / "snapshots" / case.stem
            for f, regs in out.items():
                for r, body in regs.items():
                    p = snap / f"{f}__{r}.txt"
                    if UPDATE:
                        p.parent.mkdir(parents=True, exist_ok=True)
                        p.write_text(body + "\n", encoding="utf-8")
                    with self.subTest(case=case.stem, file=f, region=r):
                        self.assertTrue(p.exists(), f"缺快照 {p}（ICBSP_UPDATE_SNAPSHOTS=1 生成）")
                        self.assertEqual(p.read_text(encoding="utf-8"), body + "\n")

    def test_fill_templates_idempotent(self):
        m = manifest.load()
        i = load("mc02_h723.ioc")
        c = json.loads((HERE / "cases" / "mc02_full.json").read_text(encoding="utf-8"))
        gen = generate.Generator(i, m, selection.merge(i, m, c["selection"]))
        for f, regs in gen.regions().items():
            text = (TEMPLATES / f).read_text(encoding="utf-8")
            for r, body in regs.items():
                text = regions.replace(text, r, body)
            self.assertEqual(regions.edited(text), [])
            again = text
            for r, body in regs.items():
                again = regions.replace(again, r, body)
            self.assertEqual(again, text, f)
            hand = text.replace("ICBSP_CHECK(bsp_can1.init()", "ICBSP_CHECK(bsp_can9.init()") if f == "board_cfg.cpp" else None
            if hand:
                self.assertEqual(regions.edited(hand), ["init"])


if __name__ == "__main__":
    unittest.main()
