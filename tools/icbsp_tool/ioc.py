"""
CubeMX .ioc 的读取、查询与修改。

.ioc 是 Java properties：键值用 `=` 分隔；键里的空格写成 `\\ `，值里的冒号写成 `\\:`。CubeMX 不写默认值，
所以“键不存在”= CubeMX 的默认值（默认值取自 STM32CubeMX/db/mcu/IP/*.xml，见各函数注释）。

修改时保持原文件的行与顺序，只改动到的行；新键按字母序插到相邻位置（CubeMX 自己也按字母序保存）。
"""

import re
from pathlib import Path

# H7：APB2 上的定时器（其余在 APB1）。时钟键名见 RCC-STM32H723_rcc_v1_0_Modes.xml：
#   Tim1OutputFreq_Value = TIM2~7、12~14（APB1），Tim2OutputFreq_Value = TIM1、8、15~17（APB2）
H7_APB2_TIMERS = {1, 8, 15, 16, 17}
F4_APB2_TIMERS = {1, 8, 9, 10, 11}
CUBEMX_DEFAULT_TIM_CLOCK = 96000000  # H7 RCC xml 里两项的 DefaultValue
FDCAN_MSGRAM_WORDS = 2560


def _unescape(s):
    out, i = [], 0
    while i < len(s):
        c = s[i]
        if c == "\\" and i + 1 < len(s):
            out.append(s[i + 1])
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def _escape_key(s):
    return s.replace("\\", "\\\\").replace(" ", "\\ ").replace(":", "\\:").replace("=", "\\=")


def _escape_value(s):
    return s.replace("\\", "\\\\").replace(":", "\\:")


def eval_int(expr):
    """CubeMX 写的 PSC / ARR 形如 24-1、2000-1、10000、0xFFFF"""
    if expr is None:
        return None
    expr = str(expr).strip()
    if re.fullmatch(r"0[xX][0-9a-fA-F]+", expr):
        return int(expr, 16)
    if not re.fullmatch(r"[\d\s+\-]+", expr):
        return None
    total = 0
    for sign, num in re.findall(r"([+-]?)\s*(\d+)", expr):
        total += -int(num) if sign == "-" else int(num)
    return total


class Ioc:
    def __init__(self, path):
        self.path = Path(path)
        raw = self.path.read_bytes()
        self.crlf = b"\r\n" in raw
        self.lines = raw.decode("utf-8").replace("\r\n", "\n").split("\n")
        self.original = list(self.lines)
        self._index()

    # ---------- 读写 ----------

    def _index(self):
        self.values = {}
        self.pos = {}
        for i, line in enumerate(self.lines):
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = self._split(line)
            self.values[k] = v
            self.pos[k] = i

    @staticmethod
    def _split(line):
        # 找第一个没被转义的 '='
        i = 0
        while i < len(line):
            if line[i] == "\\":
                i += 2
                continue
            if line[i] == "=":
                return _unescape(line[:i]), _unescape(line[i + 1 :])
            i += 1
        return _unescape(line), ""

    def get(self, key, default=None):
        return self.values.get(key, default)

    def set(self, key, value):
        """改一个键（没有就按字母序插入）；返回 (旧值, 新值)"""
        old = self.values.get(key)
        line = f"{_escape_key(key)}={_escape_value(str(value))}"
        if key in self.pos:
            self.lines[self.pos[key]] = line
        else:
            keys = sorted(self.pos, key=lambda k: self.pos[k])
            at = len(self.lines)
            for k in keys:
                if k > key:
                    at = self.pos[k]
                    break
            else:
                # 放在最后一个键之后（文件末尾可能有空行）
                at = (self.pos[keys[-1]] + 1) if keys else len(self.lines)
            self.lines.insert(at, line)
        self._index()
        return old, str(value)

    def changed(self):
        return self.lines != self.original

    def text(self):
        return "\n".join(self.lines)

    def to_bytes(self):
        t = self.text()
        return (t.replace("\n", "\r\n") if self.crlf else t).encode("utf-8")

    # ---------- 基本信息 ----------

    @property
    def mcu(self):
        return self.get("Mcu.UserName") or self.get("Mcu.Name") or ""

    @property
    def series(self):
        fam = (self.get("Mcu.Family") or self.mcu).upper()
        if "STM32H7" in fam:
            return "h7"
        if "STM32F4" in fam:
            return "f4"
        return None

    def ips(self):
        out = []
        for k, v in self.values.items():
            if re.fullmatch(r"Mcu\.IP\d+", k):
                out.append(v)
        return sorted(set(out), key=_natural)

    # ---------- 中断 / DMA / 引脚 ----------

    def nvic(self, irq):
        """返回 (是否打开, 抢占优先级)；键不存在 = 没打开"""
        v = self.get(f"NVIC.{irq}")
        if v is None:
            return False, None
        f = v.split(":")
        try:
            return f[0] == "true", int(f[1])
        except (IndexError, ValueError):
            return f[0] == "true", None

    def dma_requests(self):
        return {v for k, v in self.values.items() if re.fullmatch(r"Dma\.Request\d+", k)}

    def dma_param(self, request, param):
        for k, v in self.values.items():
            m = re.fullmatch(rf"Dma\.{re.escape(request)}\.\d+\.{re.escape(param)}", k)
            if m:
                return v
        return None

    def labeled_pins(self):
        """带 GPIO_Label 的引脚：{label: {pin, signal}}"""
        out = {}
        for k, v in self.values.items():
            if k.endswith(".GPIO_Label"):
                pin = k[: -len(".GPIO_Label")]
                out[v] = {"pin": pin, "signal": self.get(f"{pin}.Signal", "")}
        return out

    def gpio_outputs(self):
        return sorted(l for l, p in self.labeled_pins().items() if p["signal"] == "GPIO_Output")

    def gpio_exti(self):
        return sorted(l for l, p in self.labeled_pins().items() if re.match(r"GP(IO_EXTI|XTI)\d+", p["signal"]))

    # ---------- 外设视图 ----------

    def can_instances(self):
        """H7 FDCANn / F4 CANn。默认值来自 FDCAN-fdcan1_v1_0_Cube_Modes.xml"""
        out = []
        pat = r"FDCAN(\d+)" if self.series == "h7" else r"CAN(\d+)"
        for ip in self.ips():
            m = re.fullmatch(pat, ip)
            if not m:
                continue
            n = int(m.group(1))
            g = lambda p, d: self.get(f"{ip}.{p}", d)
            out.append(
                {
                    "ip": ip,
                    "n": n,
                    "frame_format": g("FrameFormat", "FDCAN_FRAME_CLASSIC"),
                    "msgram_offset": int(g("MessageRAMOffset", "0")),
                    "std_filters": int(g("StdFiltersNbr", "0")),
                    "ext_filters": int(g("ExtFiltersNbr", "0")),
                    "rx_fifo0": int(g("RxFifo0ElmtsNbr", "0")),
                    "rx_fifo0_size": g("RxFifo0ElmtSize", "FDCAN_DATA_BYTES_8"),
                    "rx_fifo1": int(g("RxFifo1ElmtsNbr", "0")),
                    "rx_fifo1_size": g("RxFifo1ElmtSize", "FDCAN_DATA_BYTES_8"),
                    "rx_buffers": int(g("RxBuffersNbr", "0")),
                    "rx_buffer_size": g("RxBufferSize", "FDCAN_DATA_BYTES_8"),
                    "tx_events": int(g("TxEventsNbr", "0")),
                    "tx_buffers": int(g("TxBuffersNbr", "0")),
                    "tx_fifo_queue": int(g("TxFifoQueueElmtsNbr", "0")),
                    "tx_size": g("TxElmtSize", "FDCAN_DATA_BYTES_8"),
                    "tx_mode": g("TxFifoQueueMode", "FDCAN_TX_FIFO_OPERATION"),
                    "nvic_it0": self.nvic(f"{ip}_IT0_IRQn")[0] if self.series == "h7" else self.nvic(f"{ip}_RX0_IRQn")[0],
                }
            )
        return out

    def uart_instances(self):
        out = []
        reqs = self.dma_requests()
        for ip in self.ips():
            m = re.fullmatch(r"(USART|UART|LPUART)(\d+)", ip)
            if not m:
                continue
            kind = "lpuart" if m.group(1) == "LPUART" else "uart"
            n = int(m.group(2))
            rs485 = any(k.startswith(f"{ip}.VirtualMode-Hardware Flow Control (RS485)") for k in self.values)
            out.append(
                {
                    "ip": ip,
                    "n": n,
                    "kind": kind,
                    "baudrate": int(self.get(f"{ip}.BaudRate", "115200")),
                    "rs485": rs485,
                    "dma_rx": f"{ip}_RX" in reqs,
                    "dma_tx": f"{ip}_TX" in reqs,
                    "dma_rx_width": self.dma_param(f"{ip}_RX", "MemDataAlignment"),
                    "dma_tx_width": self.dma_param(f"{ip}_TX", "MemDataAlignment"),
                    "nvic": self.nvic(f"{ip}_IRQn")[0],
                    "bdma": kind == "lpuart",
                }
            )
        return out

    def spi_instances(self):
        out = []
        reqs = self.dma_requests()
        for ip in self.ips():
            m = re.fullmatch(r"SPI(\d+)", ip)
            if not m:
                continue
            n = int(m.group(1))
            out.append(
                {
                    "ip": ip,
                    "n": n,
                    "mode": self.get(f"{ip}.Mode", "SPI_MODE_MASTER" if self.get(f"{ip}.VirtualType") == "VM_MASTER" else None),
                    "direction": self.get(f"{ip}.Direction", "SPI_DIRECTION_2LINES"),
                    "nss": self.get(f"{ip}.NSS", "SPI_NSS_SOFT"),
                    "virtual_nss": self.get(f"{ip}.VirtualNSS", ""),
                    "keep_io": self.get(f"{ip}.MasterKeepIOState", "SPI_MASTER_KEEP_IO_STATE_DISABLE"),
                    "dma_rx": f"{ip}_RX" in reqs,
                    "dma_tx": f"{ip}_TX" in reqs,
                    "nvic": self.nvic(f"{ip}_IRQn")[0],
                    "bdma": self.series == "h7" and n == 6,
                }
            )
        return out

    def timer_clock(self, n):
        """定时器 n 的输入时钟（Hz）；键缺省时用 CubeMX 默认值，第二项为 False 表示是猜的"""
        if self.series == "f4":
            key = "RCC.APB2TimFreq_Value" if n in F4_APB2_TIMERS else "RCC.APB1TimFreq_Value"
            default = 16000000
        else:
            key = "RCC.Tim2OutputFreq_Value" if n in H7_APB2_TIMERS else "RCC.Tim1OutputFreq_Value"
            default = CUBEMX_DEFAULT_TIM_CLOCK
        v = self.get(key)
        if v is None:
            return default, False
        return int(float(v)), True

    def timebase_tim(self):
        return self.get("NVIC.TimeBaseIP")

    def pwm_channels(self):
        """[(tim, ch)]：TIMn.Channel-PWM GenerationK CHk"""
        out = []
        for k in self.values:
            m = re.fullmatch(r"TIM(\d+)\.Channel-PWM Generation(\d) CH\d", k)
            if m:
                out.append((int(m.group(1)), int(m.group(2))))
        return sorted(set(out))

    def encoder_timers(self):
        """SH.S_TIMn_CHk.0=…,Encoder_Interface"""
        out = set()
        for k, v in self.values.items():
            m = re.fullmatch(r"SH\.S_TIM(\d+)_CH\d\.\d+", k)
            if m and "Encoder_Interface" in v:
                out.add(int(m.group(1)))
        return sorted(out)

    def tim_param(self, n, param, default):
        return self.get(f"TIM{n}.{param}", default)

    def freertos(self):
        tasks = self.get("FREERTOS.Tasks01", "")
        default_task = None
        for t in tasks.split(";"):
            f = t.split(",")
            if f and f[0] == "defaultTask" and len(f) >= 5:
                default_task = f[4]
        return {
            "enabled": "FREERTOS" in self.ips(),
            "cmsis_v2": self.get("VP_FREERTOS_VS_CMSIS_V2.Mode") == "CMSIS_V2",
            "tick": int(self.get("FREERTOS.configTICK_RATE_HZ", "1000")),
            "stack_check": int(self.get("FREERTOS.configCHECK_FOR_STACK_OVERFLOW", "0")),
            "malloc_hook": int(self.get("FREERTOS.configUSE_MALLOC_FAILED_HOOK", "0")),
            "default_task": default_task,
            "tasks_value": tasks,
        }


def fdcan_words(c):
    """一路 FDCAN 在 Message RAM 里占的字数（RM0468 Message RAM 一节；元素头 2 字 + 数据字数）"""

    def elem(size):
        m = re.search(r"(\d+)$", size or "")
        data = int(m.group(1)) if m else 8
        return 2 + (data + 3) // 4

    return (
        c["std_filters"] * 1
        + c["ext_filters"] * 2
        + c["rx_fifo0"] * elem(c["rx_fifo0_size"])
        + c["rx_fifo1"] * elem(c["rx_fifo1_size"])
        + c["rx_buffers"] * elem(c["rx_buffer_size"])
        + c["tx_events"] * 2
        + (c["tx_buffers"] + c["tx_fifo_queue"]) * elem(c["tx_size"])
    )


def _natural(s):
    return [int(t) if t.isdigit() else t for t in re.split(r"(\d+)", s)]
