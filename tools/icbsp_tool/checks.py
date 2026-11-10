"""
.ioc 校验：按 tools/manifest.json 的规则（级别、标题、修改方法）逐条判断，能自动修改的给出要改的 .ioc 键值。

规则分两类：全局规则（FreeRTOS、工具链、时基、D-Cache）对整个工程判断；外设规则只对用户选中的实例判断
（选中的实例由 generate.py 的选择决定，没给时按 default_selection()）。

每条结果是一个 dict：
    id / level / title / target（如 "FDCAN2"，全局规则为 ""）/ ok / detail / fix / auto_fix /
    changes：[{"key", "old", "new"}]——修复要写进 .ioc 的键值（ioc_fix.py 应用；auto_fix 为 False 时为空）
"""

from . import flash
from .ioc import FDCAN_MSGRAM_WORDS, eval_int, fdcan_words

NVIC_ON = "true:5:0:false:false:true:true:true:true:true"  # CubeMX 打开一个外设中断时写的值（优先级 5）
H7_TIM32 = {2, 5, 23, 24}
F4_TIM32 = {2, 5}


def _nvic_on(ioc, irq):
    v = ioc.get(f"NVIC.{irq}")
    if v is None:
        return NVIC_ON
    f = v.split(":")
    f[0] = "true"
    return ":".join(f)


def _ip_param(ioc, ip, param, value):
    """改 IP 参数时同步 <IP>.IPParameters（CubeMX 用它记哪些参数是用户改过的）"""
    changes = [(f"{ip}.{param}", value)]
    plist = ioc.get(f"{ip}.IPParameters")
    names = plist.split(",") if plist else []
    if param not in names:
        names.append(param)
        changes.append((f"{ip}.IPParameters", ",".join(names)))
    return changes


class Checker:
    def __init__(self, ioc, manifest):
        self.ioc = ioc
        self.rules = manifest["rules"]
        self.results = []

    def add(self, rid, target, ok, detail="", changes=None):
        r = self.rules[rid]
        auto = bool(r.get("auto_fix")) and not ok and bool(changes)
        self.results.append(
            {
                "id": rid,
                "level": r["level"],
                "title": r["title"],
                "target": target,
                "ok": ok,
                "detail": detail,
                "fix": r["fix"],
                "auto_fix": auto,
                "changes": [{"key": k, "old": self.ioc.get(k), "new": v} for k, v in (changes or [])] if auto else [],
            }
        )

    # ---------- 全局 ----------

    def check_global(self):
        ioc = self.ioc
        rt = ioc.freertos()
        self.add("toolchain_cmake", "", ioc.get("ProjectManager.TargetToolchain") == "CMake",
                 f"当前 {ioc.get('ProjectManager.TargetToolchain')}", [("ProjectManager.TargetToolchain", "CMake")])
        if not rt["enabled"]:
            self.add("rtos_cmsis_v2", "", False, "没有启用 FreeRTOS")
            return
        self.add("rtos_cmsis_v2", "", rt["cmsis_v2"], "" if rt["cmsis_v2"] else "接口不是 CMSIS_V2")
        self.add("rtos_tick", "", rt["tick"] == 1000, f"当前 {rt['tick']} Hz", _ip_param(ioc, "FREERTOS", "configTICK_RATE_HZ", "1000"))
        self.add("rtos_stack_check", "", rt["stack_check"] == 2, f"当前 {rt['stack_check']}",
                 _ip_param(ioc, "FREERTOS", "configCHECK_FOR_STACK_OVERFLOW", "2"))
        self.add("rtos_malloc_hook", "", rt["malloc_hook"] == 1, f"当前 {rt['malloc_hook']}",
                 _ip_param(ioc, "FREERTOS", "configUSE_MALLOC_FAILED_HOOK", "1"))
        dt = rt["default_task"]
        fix = []
        if dt is not None and dt != "As weak":
            tasks = []
            for t in rt["tasks_value"].split(";"):
                f = t.split(",")
                if f and f[0] == "defaultTask" and len(f) >= 5:
                    f[4] = "As weak"
                tasks.append(",".join(f))
            fix = [("FREERTOS.Tasks01", ";".join(tasks))]
        self.add("rtos_default_task", "", dt in (None, "As weak"), f"当前 {dt}", fix)
        tb = ioc.timebase_tim()
        self.add("timebase_tim", "", bool(tb), f"当前 {tb or 'SysTick'}")
        if ioc.series == "h7":
            dc = ioc.get("CORTEX_M7.CPU_DCache", "Disabled")
            self.add("dcache_off", "", dc != "Enabled", f"当前 {dc}", _ip_param(ioc, "CORTEX_M7", "CPU_DCache", "Disabled"))

    # ---------- 外设 ----------

    def check_can(self, selected):
        ioc = self.ioc
        cans = [c for c in ioc.can_instances() if c["ip"] in selected]
        if ioc.series != "h7":
            return  # F4 bxCAN 的检查留给 integrate.py 的前提检查（没有 Message RAM / 帧格式）
        for c in cans:
            ip = c["ip"]
            self.add("can_classic", ip, c["frame_format"] == "FDCAN_FRAME_CLASSIC", f"当前 {c['frame_format']}",
                     _ip_param(ioc, ip, "FrameFormat", "FDCAN_FRAME_CLASSIC"))
            fix = []
            if c["rx_fifo0"] == 0:
                fix += _ip_param(ioc, ip, "RxFifo0ElmtsNbr", "16")
            if c["tx_fifo_queue"] + c["tx_buffers"] == 0:
                fix += _ip_param(ioc, ip, "TxFifoQueueElmtsNbr", "16")
            self.add("can_fifo", ip, not fix, f"RX FIFO0 = {c['rx_fifo0']}，TX = {c['tx_fifo_queue'] + c['tx_buffers']}", fix)
            self.add("can_nvic", ip, c["nvic_it0"], "", [(f"NVIC.{ip}_IT0_IRQn", _nvic_on(ioc, f"{ip}_IT0_IRQn"))])
            self.add("can_std_filters", ip, c["std_filters"] >= 1, f"当前 {c['std_filters']}", _ip_param(ioc, ip, "StdFiltersNbr", "1"))
        # Message RAM：所有 FDCAN（不只是选中的）都占 RAM；按“其他规则都修好之后”的元素数算
        allc = ioc.can_instances()
        if not allc:
            return
        spans, overlap, offset, fix = [], [], 0, []
        for c in allc:
            if c["rx_fifo0"] == 0:
                c = dict(c, rx_fifo0=16)
            if c["tx_fifo_queue"] + c["tx_buffers"] == 0:
                c = dict(c, tx_fifo_queue=16)
            if c["std_filters"] < 1:
                c = dict(c, std_filters=1)  # 按 can_std_filters 也修好之后算；不修的话只是空出 1 字，不会重叠
            w = fdcan_words(c)
            spans.append((c["ip"], c["msgram_offset"], c["msgram_offset"] + w))
            if c["msgram_offset"] != offset:
                fix += _ip_param(ioc, c["ip"], "MessageRAMOffset", str(offset))
            offset += w
        for i, (a, s0, e0) in enumerate(spans):
            for b, s1, e1 in spans[i + 1 :]:
                if s0 < e1 and s1 < e0:
                    overlap.append(f"{a}[{s0},{e0}) 与 {b}[{s1},{e1})")
        too_big = max(e for _, _, e in spans) > FDCAN_MSGRAM_WORDS or offset > FDCAN_MSGRAM_WORDS
        detail = "；".join(overlap) or "、".join(f"{a}[{s},{e})" for a, s, e in spans)
        if too_big:
            detail += f"；合计超过 {FDCAN_MSGRAM_WORDS} 字，要减少元素数（不能自动修改）"
            fix = []
        self.add("can_msgram", "FDCAN", not overlap and not too_big, detail, fix)

    def check_uart(self, selected):
        ioc = self.ioc
        for u in ioc.uart_instances():
            ip = u["ip"]
            if ip not in selected:
                continue
            mode = selected[ip].get("mode", "dma")
            self.add("uart_nvic", ip, u["nvic"], "", [(f"NVIC.{ip}_IRQn", _nvic_on(ioc, f"{ip}_IRQn"))])
            if u["bdma"]:
                self.add("uart_bdma", ip, mode != "dma", "LPUART 由 BDMA 服务，访问不到 .dma_buffer（AXI SRAM）")
            if mode == "dma":
                miss = [d for d, ok in (("RX", u["dma_rx"]), ("TX", u["dma_tx"])) if not ok]
                width = [w for w in (u["dma_rx_width"], u["dma_tx_width"]) if w and "BYTE" not in w]
                detail = ("没配 " + "、".join(miss) + " DMA" if miss else "") + ("；数据宽度不是 Byte" if width else "")
                self.add("uart_dma", ip, not miss and not width, detail.strip("；") or "")

    def check_spi(self, selected):
        ioc = self.ioc
        for s in ioc.spi_instances():
            ip = s["ip"]
            if ip not in selected:
                continue
            ok = s["mode"] == "SPI_MODE_MASTER" and s["direction"] == "SPI_DIRECTION_2LINES" and "HARD" not in (s["nss"] + s["virtual_nss"])
            self.add("spi_master", ip, ok, f"Mode {s['mode']}，Direction {s['direction']}，NSS {s['nss']}")
            if ioc.series == "h7":
                self.add("spi_keep_io", ip, s["keep_io"] == "SPI_MASTER_KEEP_IO_STATE_ENABLE", f"当前 {s['keep_io']}",
                         _ip_param(ioc, ip, "MasterKeepIOState", "SPI_MASTER_KEEP_IO_STATE_ENABLE"))
            if selected[ip].get("use_dma", s["dma_rx"] and s["dma_tx"]):
                self.add("spi_nvic", ip, s["nvic"], "", [(f"NVIC.{ip}_IRQn", _nvic_on(ioc, f"{ip}_IRQn"))])

    def check_encoder(self, selected):
        ioc = self.ioc
        wide = H7_TIM32 if ioc.series == "h7" else F4_TIM32
        for n in ioc.encoder_timers():
            ip = f"TIM{n}"
            if ip not in selected:
                continue
            want = 0xFFFFFFFF if n in wide else 0xFFFF
            have = eval_int(ioc.tim_param(n, "Period", str(0xFFFFFFFF if n in wide else 0xFFFF)))
            self.add("encoder_period", ip, have == want, f"当前 {have}，要求 {want}", _ip_param(ioc, ip, "Period", str(want)))


    def check_external_flash(self, ef, manifest):
        """外置 Flash 启用时：QUADSPI 参数与芯片一致、引脚速度 Very High（只影响应用自己用 QSPI，bootloader 不读）"""
        ioc = self.ioc
        if not ef or not ef.get("enabled") or "QUADSPI" not in ioc.ips():
            return
        chip = flash.chip_for(manifest, ef.get("chip"))
        if chip is None:
            return
        # 键不存在 = CubeMX 默认值（QUADSPI 的 IP xml）
        want = {
            "FlashSize": (str(chip["size"].bit_length() - 2), "1"),
            "ClockPrescaler": (str(int(ef.get("prescaler", 2))), "255"),
            "SampleShifting": ("QSPI_SAMPLE_SHIFTING_HALFCYCLE", "QSPI_SAMPLE_SHIFTING_NONE"),
            "ChipSelectHighTime": ("QSPI_CS_HIGH_TIME_6_CYCLE", "QSPI_CS_HIGH_TIME_1_CYCLE"),
        }
        fix, diff = [], []
        params = (ioc.get("QUADSPI.IPParameters") or "").split(",")
        params = [x for x in params if x]
        for k, (v, default) in want.items():
            have = ioc.get(f"QUADSPI.{k}", default)
            if have != v:
                diff.append(f"{k} {have}→{v}")
                fix.append((f"QUADSPI.{k}", v))
                if k not in params:
                    params.append(k)
        if fix:
            fix.append(("QUADSPI.IPParameters", ",".join(params)))
        for p in flash.qspi_signals(ioc):
            pin = p["pin"]
            if ioc.get(f"{pin}.GPIO_Speed") == "GPIO_SPEED_FREQ_VERY_HIGH":
                continue
            diff.append(f"{pin} 速度")
            fix.append((f"{pin}.GPIO_Speed", "GPIO_SPEED_FREQ_VERY_HIGH"))
            names = [x for x in (ioc.get(f"{pin}.GPIOParameters") or "").split(",") if x]
            if "GPIO_Speed" not in names:
                fix.append((f"{pin}.GPIOParameters", ",".join(names + ["GPIO_Speed"])))
        self.add("qspi_params", "QUADSPI", not diff, "；".join(diff), fix)


def usable(ioc):
    """各外设实例能不能用作库的实例：{ip: (能用, 原因)}"""
    out = {}
    for s in ioc.spi_instances():
        if s["mode"] != "SPI_MODE_MASTER":
            out[s["ip"]] = (False, "不是主机模式")
        elif s["direction"] != "SPI_DIRECTION_2LINES":
            out[s["ip"]] = (False, f"不是全双工（{s['direction']}）")
        elif s["bdma"]:
            out[s["ip"]] = (True, "SPI6 由 BDMA 服务：只能轮询（use_dma = false）")
        else:
            out[s["ip"]] = (True, "")
    tb = ioc.timebase_tim()
    for n, ch in ioc.pwm_channels():
        if f"TIM{n}" == tb:
            out[f"TIM{n}_CH{ch}"] = (False, "是 HAL 时基")
    return out


def run(ioc, manifest, selection, external_flash=None):
    """selection：{"can": {ip: {...}}, "uart": {...}, "spi": {...}, "encoder": {...}}（只看键）；
    external_flash：完整选择里的 external_flash（启用时检查 QUADSPI 参数）"""
    c = Checker(ioc, manifest)
    c.check_external_flash(external_flash, manifest)
    c.check_global()
    c.check_can(selection.get("can", {}))
    c.check_uart(selection.get("uart", {}))
    c.check_spi(selection.get("spi", {}))
    c.check_encoder(selection.get("encoder", {}))
    return c.results
