#!/usr/bin/env python3
"""生成 docs/img 下的架构图（同时输出 PNG 与 SVG）。

    python3 tools/draw_diagrams.py             # 输出到 docs/img/
    python3 tools/draw_diagrams.py --out DIR   # 输出到指定目录

设计要点：
- 同一份布局代码画两遍：一遍用 Pillow 出 PNG（超采样抗锯齿），一遍写 SVG（矢量、可放大）。
- 所有坐标都是「1x」单位常量，改图只改常量，不会像自动布局那样牵一发动全身。
- 文字宽度统一用 Pillow 度量，保证两种输出里方块的宽度完全一致。
"""

import argparse
import html
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

FONT_DIR = Path("/usr/share/fonts/opentype/noto")
FONT_REGULAR = FONT_DIR / "NotoSansCJK-Regular.ttc"
FONT_BOLD = FONT_DIR / "NotoSansCJK-Bold.ttc"
FONT_FAMILY = "Noto Sans CJK SC"
SS = 3  # 仅 PNG 用的超采样倍数

INK = "#24303F"
SUB = "#4A5568"
ARROW = "#5F6A7D"

FILL = {
    "in": "#F4F6F9",
    "int": "#E6EFFF",
    "vs": "#E9F6EC",
    "flash": "#FDF2DF",
    "build": "#F0EAFA",
    "host": "#FFF6E9",
    "lib": "#F2F6FB",
    "leaf": "#F3EEFB",
    "hal": "#EFEFF2",
    "note": "#FFFDF0",
}
LINE = {
    "in": "#8C96A6",
    "int": "#3F74C9",
    "vs": "#4C9A63",
    "flash": "#BD8A34",
    "build": "#7358B0",
    "host": "#C08A45",
    "lib": "#7A8CA6",
    "leaf": "#7A5FB5",
    "hal": "#8A8F99",
    "note": "#C8B96B",
}

_FONTS = {}


def measure(msg, size, bold=False):
    key = (size, bold)
    if key not in _FONTS:
        _FONTS[key] = ImageFont.truetype(str(FONT_BOLD if bold else FONT_REGULAR), 100)
    return _FONTS[key].getlength(msg) * size / 100


class BaseCanvas:
    """两种后端共用的绘制接口。锚点沿用 Pillow 的写法：mm / lm / rm / la / lb"""

    def __init__(self, w, h):
        self.w, self.h = w, h

    def rr(self, xy, radius, fill, outline, width): ...
    def line(self, x0, y0, x1, y1, color, width): ...
    def poly(self, pts, color): ...
    def text(self, x, y, msg, size, bold, color, anchor): ...
    def save(self, out_dir, name): ...


class PILCanvas(BaseCanvas):
    def __init__(self, w, h):
        super().__init__(w, h)
        self.img = Image.new("RGB", (w * SS, h * SS), "white")
        self.d = ImageDraw.Draw(self.img)

    def rr(self, xy, radius, fill, outline, width):
        # SVG 用 "none" 表示无填充，PIL 要 None
        fill = None if fill in (None, "none") else fill
        outline = None if outline in (None, "none") else outline
        self.d.rounded_rectangle([v * SS for v in xy], radius=radius * SS, fill=fill, outline=outline, width=max(1, int(width * SS)))

    def line(self, x0, y0, x1, y1, color, width):
        self.d.line([x0 * SS, y0 * SS, x1 * SS, y1 * SS], fill=color, width=max(1, int(width * SS)))

    def poly(self, pts, color):
        self.d.polygon([(x * SS, y * SS) for x, y in pts], fill=color)

    def text(self, x, y, msg, size, bold, color, anchor):
        self.d.text((x * SS, y * SS), msg, font=ImageFont.truetype(str(FONT_BOLD if bold else FONT_REGULAR), size * SS), fill=color, anchor=anchor)

    def save(self, out_dir, name):
        path = out_dir / f"{name}.png"
        self.img.resize((self.w, self.h), Image.LANCZOS).save(path)
        return path


SVG_ANCHOR = {"mm": ("middle", "central"), "lm": ("start", "central"), "rm": ("end", "central"), "la": ("start", "hanging"), "lb": ("start", "alphabetic")}


class SVGCanvas(BaseCanvas):
    def __init__(self, w, h):
        super().__init__(w, h)
        self.parts = [f'<rect width="{w}" height="{h}" fill="#ffffff"/>']

    def rr(self, xy, radius, fill, outline, width):
        x0, y0, x1, y1 = xy
        self.parts.append(
            f'<rect x="{x0}" y="{y0}" width="{x1 - x0}" height="{y1 - y0}" rx="{radius}" ry="{radius}" '
            f'fill="{fill}" stroke="{outline}" stroke-width="{width}"/>'
        )

    def line(self, x0, y0, x1, y1, color, width):
        self.parts.append(f'<line x1="{x0}" y1="{y0}" x2="{x1}" y2="{y1}" stroke="{color}" stroke-width="{width}"/>')

    def poly(self, pts, color):
        points = " ".join(f"{x},{y}" for x, y in pts)
        self.parts.append(f'<polygon points="{points}" fill="{color}"/>')

    def text(self, x, y, msg, size, bold, color, anchor):
        ta, db = SVG_ANCHOR[anchor]
        weight = ' font-weight="bold"' if bold else ""
        self.parts.append(
            f'<text x="{x}" y="{y}" font-family="{FONT_FAMILY}" font-size="{size}"{weight} fill="{color}" '
            f'text-anchor="{ta}" dominant-baseline="{db}" xml:space="preserve">{html.escape(msg)}</text>'
        )

    def save(self, out_dir, name):
        path = out_dir / f"{name}.svg"
        body = "\n".join(self.parts)
        path.write_text(
            f'<svg xmlns="http://www.w3.org/2000/svg" width="{self.w}" height="{self.h}" viewBox="0 0 {self.w} {self.h}">\n{body}\n</svg>\n',
            encoding="utf-8",
        )
        return path


# ==================== 基础图元 ====================


def box(c, xy, fill, line, radius=10, width=1.3):
    c.rr(xy, radius, fill, line, width)


def panel(c, xy, title, key, dashed=False):
    x0, y0, x1, y1 = xy
    if dashed:
        dashed_rect(c, xy, LINE[key], 7)
    else:
        c.rr(xy, 12, "none", LINE[key], 1.2)
    if title:
        c.text(x0 + 14, y0 + 14, title, 14, True, LINE[key], "lm")


def dashed_rect(c, xy, color, dash):
    x0, y0, x1, y1 = xy
    for seg in ((x0, y0, x1, y0), (x1, y0, x1, y1), (x1, y1, x0, y1), (x0, y1, x0, y0)):
        dash_line(c, [(seg[0], seg[1]), (seg[2], seg[3])], color, 1.2, dash)


def dash_line(c, pts, color, width, dash=7):
    for i in range(len(pts) - 1):
        (x0, y0), (x1, y1) = pts[i], pts[i + 1]
        length = max(abs(x1 - x0), abs(y1 - y0))
        if length == 0:
            continue
        steps = int(length // dash) + 1
        for step in range(steps):
            t0 = min(1.0, (step * dash) / length)
            t1 = min(1.0, (step * dash + dash * 0.6) / length)
            c.line(x0 + (x1 - x0) * t0, y0 + (y1 - y0) * t0, x0 + (x1 - x0) * t1, y0 + (y1 - y0) * t1, color, width)


def arrow(c, pts, color=ARROW, dashed=False, width=1.4):
    if dashed:
        dash_line(c, pts, color, width)
    else:
        for i in range(len(pts) - 1):
            c.line(pts[i][0], pts[i][1], pts[i + 1][0], pts[i + 1][1], color, width)
    (x0, y0), (x1, y1) = pts[-2], pts[-1]
    if x0 == x1:
        s = 1 if y1 > y0 else -1
        head = [(x1, y1), (x1 - 5.5, y1 - 10 * s), (x1 + 5.5, y1 - 10 * s)]
    else:
        s = 1 if x1 > x0 else -1
        head = [(x1, y1), (x1 - 10 * s, y1 - 5.5), (x1 - 10 * s, y1 + 5.5)]
    c.poly(head, color)


def text(c, xy, lines, size=14, bold=False, fill=INK, gap=1.4, anchor="mm"):
    x, y = xy
    line_h = size * gap
    top = y - (line_h * len(lines)) / 2 + line_h / 2
    for i, line in enumerate(lines):
        c.text(x, top + i * line_h, line, size, bold, fill, anchor)


def label(c, xy, msg, color=SUB, size=12, anchor="lm"):
    c.text(xy[0], xy[1], msg, size, False, color, anchor)


def chip_row(c, x, y, w, h, items, key, size=12, gap=10, max_w=None):
    """一行自适应宽度的小方块：按文字宽度分配，超宽时整体等比压缩"""
    if not items:
        return
    widths = [measure(item, size) + 20 for item in items]
    if max_w:
        widths = [min(wd, max_w) for wd in widths]
    need = sum(widths) + gap * (len(items) - 1)
    if need > w:
        k = (w - gap * (len(items) - 1)) / sum(widths)
        widths = [wd * k for wd in widths]
    bx = x
    for item, wd in zip(items, widths):
        box(c, (bx, y, bx + wd, y + h), "#FFFFFF", LINE[key], radius=8, width=1.0)
        text(c, (bx + wd / 2, y + h / 2), [item], size=size)
        bx += wd + gap


# ==================== 图1：工具链关系 ====================


def fig_toolchain(c):
    panel(c, (120, 34, 1360, 196), "① 输入：CubeMX 生成的工程 + 源码", "in", dashed=True)
    xs = [150, 455, 760, 1065]
    for (title, l1, l2), x in zip(
        [
            ("源码注释", "// ---- 标题 ----", "/* ==== 标题 ==== */"),
            (".ioc", "Mcu.Name", ""),
            ("CMakeLists.txt", "CMAKE_PROJECT_NAME", ""),
            ("CMakePresets.json", "Debug / Release", ""),
        ],
        xs,
    ):
        box(c, (x, 78, x + 260, 176), FILL["in"], LINE["in"])
        text(c, (x + 130, 106), [title], size=15, bold=True)
        text(c, (x + 130, 136), [l1], size=12, fill=SUB)
        if l2:
            text(c, (x + 130, 158), [l2], size=12, fill=SUB)

    box(c, (580, 246, 940, 326), FILL["int"], LINE["int"], width=1.8)
    text(c, (760, 274), ["tools/integrate.py"], size=15, bold=True, fill=LINE["int"])
    text(c, (760, 304), ["探测 · 生成 · 合并"], size=12, fill=LINE["int"])

    for x in (585, 890, 1195):
        c.line(x, 176, x, 214, ARROW, 1.3)
    c.line(585, 214, 1195, 214, ARROW, 1.3)
    c.line(760, 214, 760, 246, ARROW, 1.3)
    c.poly([(760, 246), (754.5, 236), (765.5, 236)], ARROW)

    panel(c, (120, 396, 710, 812), "③ .vscode/", "vs")
    panel(c, (790, 396, 1360, 812), "④ Flash/", "flash")

    for i, (title, sub) in enumerate(
        [
            ("settings.json", ["clangd.arguments", "outlinePlus 正则"]),
            ("tasks.json", ["Build · Clean_Rebuild · Clean", "OpenOCD_flash · JLink_flash"]),
            ("launch.json", ["cmsis-dap-debug · jlink-debug", "device ← .ioc，elf ← 工程名"]),
        ]
    ):
        y = 434 + i * 128
        box(c, (150, y, 680, y + 104), FILL["vs"], LINE["vs"])
        text(c, (415, y + 32), [title], size=15, bold=True)
        text(c, (415, y + 66), sub, size=12, fill=SUB)

    for i, (title, sub) in enumerate(
        [
            ("OpenOCD_flash.sh  ·  JLink_flash.sh", ["参数：构建类型（默认 Debug）"]),
            ("daplink.cfg  ·  jlink.cfg", ["target/stm32h7x(_dual_bank).cfg"]),
            ("*.svd（宿主自带，可选）", ["有才写 svdFile，没有只提示一次"]),
        ]
    ):
        y = 434 + i * 128
        box(c, (820, y, 1330, y + 104), FILL["flash"], LINE["flash"])
        text(c, (1075, y + 32), [title], size=13, bold=True)
        text(c, (1075, y + 66), sub, size=12, fill=SUB)

    arrow(c, [(690, 326), (690, 360), (430, 360), (430, 396)], LINE["int"], dashed=True)
    label(c, (444, 344), "生成 / 合并", LINE["int"])
    arrow(c, [(830, 326), (830, 360), (1080, 360), (1080, 396)], LINE["int"], dashed=True)
    label(c, (1094, 344), "复制", LINE["int"])

    arrow(c, [(280, 176), (280, 434)], LINE["in"])
    label(c, (292, 300), "正则", LINE["in"])

    arrow(c, [(710, 748), (790, 748)], LINE["vs"])
    label(c, (750, 726), "cfg · svd", LINE["vs"], anchor="mm")

    panel(c, (330, 880, 1150, 1064), "⑤ 构建产物  build/<预设>/", "build")
    box(c, (360, 918, 730, 1024), FILL["build"], LINE["build"])
    text(c, (545, 952), ["compile_commands.json"], size=13, bold=True)
    text(c, (545, 984), ["clangd 的补全 / 跳转靠它"], size=12, fill=SUB)
    box(c, (760, 918, 1120, 1024), FILL["build"], LINE["build"], width=1.8)
    text(c, (940, 952), ["<工程名>.elf"], size=13, bold=True)
    text(c, (940, 984), ["调试与烧录都消费它"], size=12, fill=SUB)

    arrow(c, [(430, 812), (430, 880)], LINE["vs"])
    label(c, (444, 838), "cmake --preset / --build --preset", LINE["vs"])

    arrow(c, [(1080, 812), (1080, 852), (960, 852), (960, 880)], LINE["flash"])
    label(c, (1094, 830), "找 build/**/*.elf", LINE["flash"])

    box(c, (120, 1092, 1360, 1226), FILL["note"], LINE["note"], width=1.0)
    text(
        c,
        (740, 1150),
        [
            "① 里的三个文件只提供「叫什么 / 什么芯片 / 构建到哪」，integrate.py 用它们推出 launch.json 的 device、elf 路径与 clangd 的编译数据库目录",
            "⑤ 的产物由 cmake 与 Build 任务产生；.vscode 里的 clangd、调试配置和 Flash 脚本都只是「引用」它，不会自己编译",
            "outlinePlus 正则只作用于源码注释，与构建链路无关；宿主 Flash/ 下没有 *.svd 时不写 svdFile，调试本身不受影响",
        ],
        size=13,
        fill=SUB,
        gap=1.7,
    )


# ==================== 图2：分层依赖 ====================


def fig_layers(c):
    left, right = 90, 1000
    rows = [
        ("宿主 app/", ["app_main.cpp（app_init）", "StartDefaultTask", "task/*.cpp"], "host"),
        ("宿主 board/", ["board_cfg.hpp", "board_cfg.cpp（board_init）", "board_instances.hpp", "hardware_config.h"], "host"),
        ("库 device/", ["dji_motor", "device_motor", "servo", "r9ds", "emmv5_group", "bmi088", "buzzer"], "lib"),
        ("库 tasks/", ["sys_task（10 ms）", "can_rx_task（1 ms）", "can_tx_task（按需建）"], "lib"),
        ("库 service/", ["system", "status", "online_check", "can_rx_node", "can_tx_node", "fault_record"], "lib"),
        ("库 bsp/", ["can", "uart", "spi", "pwm", "gpio", "key", "encoder", "dwt"], "lib"),
    ]
    row_y = [60, 180, 300, 420, 540, 660]
    row_h = 92

    for (title, items, key), y in zip(rows, row_y):
        box(c, (left, y, right, y + row_h), FILL[key], LINE[key], width=1.0)
        text(c, (left + 92, y + row_h / 2), [title], size=14, bold=True)
        chip_row(c, left + 190, y + 22, right - left - 214, row_h - 44, items, key)

    box(c, (left, 782, right, 862), FILL["hal"], LINE["hal"], width=1.0)
    text(c, ((left + right) / 2, 822), ["HAL · FreeRTOS · CubeMX 生成的句柄（hfdcan1 / huart1 / htim12 …）"], size=13, fill=SUB)

    box(c, (1040, 300, 1390, 470), FILL["leaf"], LINE["leaf"], width=1.0)
    text(c, (1215, 322), ["algorithm/"], size=14, bold=True)
    chip_row(c, 1058, 340, 314, 42, ["pid", "quaternion_pid"], "leaf")
    chip_row(c, 1058, 396, 314, 42, ["vqf", "matrix", "rls"], "leaf")

    box(c, (1040, 492, 1390, 560), FILL["leaf"], LINE["leaf"], width=1.0)
    text(c, (1215, 512), ["protocol/"], size=14, bold=True)
    text(c, (1215, 538), ["emmv5_protocol（纯编解码）"], size=12, fill=SUB)

    notes = [
        "app_init() 调 board_init()",
        "board_cfg / board_instances（X-macro 清单）",
        "device 用 PID / 四元数 / VQF / 帧编解码",
        "—",
        "任务的在线状态、系统事件组",
        "service 收发都走 BspCan",
    ]
    for i, (y, msg) in enumerate(zip(row_y, notes)):
        y_next = row_y[i + 1] if i + 1 < len(row_y) else 782
        if msg != "—":
            arrow(c, [(520, y + row_h), (520, y_next)], ARROW)
            label(c, (536, (y + row_h + y_next) / 2), msg)

    arrow(c, [(right, 346), (1040, 346)], LINE["leaf"])
    label(c, (1032, 322), "用", LINE["leaf"], anchor="rm")

    box(c, (1040, 660, 1390, 862), FILL["note"], LINE["note"], width=1.0)
    text(c, (1215, 686), ["依赖只朝下（上层依赖下层）"], size=13, bold=True, fill=LINE["note"])
    for i, line in enumerate(
        [
            "跨层依赖：",
            "app → service（system_init / start）",
            "board → bsp（实例在 board_cfg 构造）",
            "device → bsp（初始化 / 收发）",
            "device → service（注册 CanRx / CanTx 节点）",
            "tasks → bsp（巡检 / Bus-Off 恢复）",
        ]
    ):
        c.text(1058, 718 + i * 21, line, 11, False, SUB, "la")

    dash_line(c, [(60, 226), (60, 706), (90, 706)], LINE["host"], 1.3, 7)
    label(c, (68, 452), "X-macro", LINE["host"], size=11)


# ==================== 图3：启动顺序与任务 ====================


def fig_startup(c):
    chain = [
        ("MX_FREERTOS_Init()", "freertos.c 的 USER CODE Init 段", 56, "in"),
        ("app_init()", "user/app/app_main.cpp", 176, "host"),
        ("① system_init()", "建 sys_state（EventState）", 296, "int"),
        ("② board_init()", "外设 init · 设备 init · 注册 CAN 节点", 416, "int"),
        ("③ system_start()", "按节点建 can_tx，置运行位", 536, "int"),
        ("业务任务", "wait_running() 之后才开始跑", 656, "host"),
    ]
    cx0, cx1, ch = 100, 480, 88
    rail_a, rail_b = 520, 540
    for title, sub, y, key in chain:
        box(c, (cx0, y, cx1, y + ch), FILL[key], LINE[key], width=1.7 if key == "int" else 1.3)
        text(c, ((cx0 + cx1) / 2, y + 30), [title], size=14, bold=True)
        text(c, ((cx0 + cx1) / 2, y + 62), [sub], size=11, fill=SUB)
    for i in range(len(chain) - 1):
        arrow(c, [(290, chain[i][2] + ch), (290, chain[i + 1][2])], ARROW)

    for y, msg in [
        (296, "调度器还没启动：这里只是「创建」sys_task 与 can_rx_task，它们要等 vTaskStartScheduler() 才跑"),
        (416, "任何一路 init() 返回非 OK 就 configASSERT 停机，fault_record 记下调用行号，print_uart 能打出来"),
        (536, "板级没有发送节点就不建 can_tx；随后置运行位，唤醒所有在 wait_running() 上等待的任务"),
    ]:
        box(c, (560, y, 1330, y + ch), FILL["note"], LINE["note"], width=1.0)
        c.text(584, y + ch / 2, msg, 12, False, SUB, "lm")
        arrow(c, [(cx1, y + ch / 2), (560, y + ch / 2)], LINE["note"], dashed=True, width=1.2)

    tw, gap, ty = 380, 40, 872
    for i, (name, lines) in enumerate(
        [
            ("sys_task", ["10 ms 周期 · DWT 计时", "CAN Bus-Off 恢复 · UART 巡检", "Online::update() · 每 2 s 打印"]),
            ("can_rx_task", ["1 ms 周期", "按 ID 分发", "未认领的进回退缓冲"]),
            ("can_tx_task", ["槽位聚合 · 保底心跳", "100 ms 指令有效期", "没有发送节点就不创建"]),
        ]
    ):
        x = 100 + i * (tw + gap)
        box(c, (x, ty, x + tw, ty + 116), FILL["lib"], LINE["lib"], width=1.3)
        text(c, (x + tw / 2, ty + 26), [name], size=14, bold=True)
        text(c, (x + tw / 2, ty + 74), lines, size=11, fill=SUB, gap=1.6)

    arrow(c, [(rail_a, 340), (rail_a, 812), (275, 812), (275, ty)], LINE["int"], dashed=True)
    arrow(c, [(rail_a, 812), (655, 812), (655, ty)], LINE["int"], dashed=True)
    label(c, (286, 802), "① system_init 创建 sys_task / can_rx_task", LINE["int"], size=11, anchor="lb")
    arrow(c, [(rail_b, 580), (rail_b, 838), (1035, 838), (1035, ty)], LINE["int"], dashed=True)
    label(c, (700, 846), "③ system_start 有节点才创建 can_tx_task", LINE["int"], size=11, anchor="la")

    label(c, (100, 852), "优先级 / 栈深在宿主 hardware_config.h：CAN_TASK_PRIORITY 最高", SUB, size=11, anchor="la")


# ==================== 入口 ====================

FIGURES = [
    ("toolchain", 1480, 1250, fig_toolchain),
    ("layers", 1440, 940, fig_layers),
    ("startup", 1400, 1010, fig_startup),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=Path, default=Path(__file__).resolve().parent.parent / "docs" / "img")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    for name, w, h, fn in FIGURES:
        paths = []
        for canvas_cls in (PILCanvas, SVGCanvas):
            canvas = canvas_cls(w, h)
            fn(canvas)
            paths.append(canvas.save(args.out, name))
        print("、".join(str(p) for p in paths))


if __name__ == "__main__":
    main()
