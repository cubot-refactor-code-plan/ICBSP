#!/usr/bin/env python3
"""
stm32_framework 宿主工程自动接入：按 docs/接入指南.md 把 CubeMX（CMake）工程改成能用本库的样子。

用法（在宿主工程根目录，或用 --host 指定）：

    python3 third_party/stm32-framework/tools/integrate.py --dry-run   # 只检查并打印将要做的修改
    python3 third_party/stm32-framework/tools/integrate.py             # 执行

做的事（每一步都先判断是否已完成，已完成就跳过，可重复运行）：
  1. 检查 CubeMX 前提（docs/接入指南.md 第 1 节）：STM32H7、CMake 工具链、FreeRTOS CMSIS_V2、tick 1 kHz、栈溢出检查 2、分配失败钩子
  2. 链接脚本加 .dma_buffer / .noinit（templates/linker_sections.ld）
  3. FreeRTOSConfig.h 的 USER CODE 1：configASSERT → fault_on_assert（templates/FreeRTOSConfig_user_code.h）
  4. freertos.c 的 Includes / 4 / 5 / Init 段：钩子记录现场、调 app_init()（templates/freertos_user_code.c）
  5. 复制 user/board/board_cfg.*、user/app/app_main.*、user/app/app_task.hpp、user/app/task/demo_task.cpp（已存在的不覆盖）
  6. 核对模板用到的句柄与参数；业务任务写在 user/app/task/，示例不自动创建
  7. 复制 hardware_config.h 到 user/board/（已存在的不覆盖）
  8. 根 CMakeLists.txt 末尾接入库与 user/board、user/app、user/module 包含路径；user/ 下的 .c / .cpp 自动收集
  9. .vscode/settings.json / tasks.json / launch.json（templates/vscode）：没有就生成，有就只补缺失的键、
     任务与调试配置（原有内容与注释都保留）。工程名取 CMAKE_PROJECT_NAME，芯片与 OpenOCD target 取 .ioc
     的 Mcu.Name；宿主 Flash/ 下有 SVD 才写 svdFile，没有就不写并提示一次
 10. 复制 Flash/ 烧录脚本（templates/Flash）：脚本运行时自己在 build/ 下递归找 .elf，不需要传路径
 11. 生成 .clang-format（沿用库根目录那一份，Language: Cpp 只作用于 C++）与 .clangd
     （templates/clang：CompilationDatabase 指向该预设的构建目录 + 本机交叉工具链的 query-driver）。
     两者都已存在时不覆盖，只校对内容是否还对得上

所有片段都从 templates/ 读取，与模板保持一致。只改内容仍是 CubeMX 默认的 USER CODE 段；遇到已有自定义内容
就报错并说明要手动合并的位置。任何一步报错都不写任何文件。保持各文件原有的行尾（CRLF / LF）。
"""

import argparse
import difflib
import glob
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

LIB_ROOT = Path(__file__).resolve().parent.parent
TEMPLATES = LIB_ROOT / "templates"
BOARD_TEMPLATES = ("board_cfg.hpp", "board_cfg.cpp", "board_instances.hpp")
APP_TEMPLATES = ("app_main.h", "app_main.cpp", "app_task.hpp")
TASK_TEMPLATES = ("demo_task.cpp",)


# ==================== 报告 ====================


class Report:
    def __init__(self):
        self.items = []

    def add(self, level, step, msg):
        self.items.append((level, step, msg))

    def ok(self, step, msg):
        self.add("完成", step, msg)

    def skip(self, step, msg):
        self.add("跳过", step, msg)

    def warn(self, step, msg):
        self.add("注意", step, msg)

    def error(self, step, msg):
        self.add("错误", step, msg)

    @property
    def has_error(self):
        return any(level == "错误" for level, _, _ in self.items)

    def print(self):
        for level, step, msg in self.items:
            print(f"[{level}] {step}: {msg}")


# ==================== 文件读写（保持行尾） ====================


class TextFile:
    """读入时统一成 \\n 处理，写回时恢复原行尾"""

    def __init__(self, path):
        self.path = path
        raw = path.read_bytes()
        self.crlf = b"\r\n" in raw
        self.original = raw.decode("utf-8").replace("\r\n", "\n")
        self.text = self.original

    def to_bytes(self):
        text = self.text.replace("\n", "\r\n") if self.crlf else self.text
        return text.encode("utf-8")


class Plan:
    """先在内存里算出所有修改，确认无错误后再一次写盘"""

    def __init__(self):
        self.edits = {}  # path -> TextFile
        self.new_files = {}  # path -> str
        self.modes = {}  # path -> 权限位（.sh 要为 0755）

    def edit(self, tf):
        self.edits[tf.path] = tf

    def create(self, path, text):
        """登记一个新文件：text 为 str 时按 UTF-8 写入，为 bytes 时原样写（保留行尾）"""
        self.new_files[path] = text

    def chmod(self, path, mode):
        self.modes[path] = mode

    def empty(self):
        return not any(tf.text != tf.original for tf in self.edits.values()) and not self.new_files and not self.modes

    def show(self, host):
        for path, tf in self.edits.items():
            if tf.text == tf.original:
                continue
            rel = path.relative_to(host).as_posix()
            sys.stdout.writelines(
                difflib.unified_diff(
                    tf.original.splitlines(keepends=True),
                    tf.text.splitlines(keepends=True),
                    f"a/{rel}",
                    f"b/{rel}",
                )
            )
        for path in self.new_files:
            print(f"+++ 新建 {path.relative_to(host).as_posix()}")

    def apply(self):
        for tf in self.edits.values():
            if tf.text != tf.original:
                tf.path.write_bytes(tf.to_bytes())
        for path, text in self.new_files.items():
            path.parent.mkdir(parents=True, exist_ok=True)
            if isinstance(text, bytes):
                path.write_bytes(text)
            else:
                path.write_text(text, encoding="utf-8")
        for path, mode in self.modes.items():
            path.chmod(mode)


# ==================== 文本工具 ====================


def user_code(text, name):
    """返回 /* USER CODE BEGIN name */ 与 END 之间内容的 (起, 止)：从 BEGIN 下一行开始，到 END 所在行行首为止"""
    begin = f"/* USER CODE BEGIN {name} */"
    end = f"/* USER CODE END {name} */"
    if text.count(begin) != 1 or text.count(end) != 1:
        return None
    start = text.index("\n", text.index(begin)) + 1
    stop = text.rfind("\n", 0, text.index(end)) + 1
    return start, stop


def section_text(text, name):
    span = user_code(text, name)
    return None if span is None else text[span[0] : span[1]]


def replace_section(text, name, content):
    start, stop = user_code(text, name)
    return text[:start] + content + text[stop:]


def strip_comments(code):
    code = re.sub(r"/\*.*?\*/", "", code, flags=re.S)
    return re.sub(r"//[^\n]*", "", code)


def compact(code):
    """去注释、去空白，用于判断一段代码是不是 CubeMX 默认内容"""
    return re.sub(r"\s+", "", strip_comments(code))


def read_template(name):
    return (TEMPLATES / name).read_text(encoding="utf-8").replace("\r\n", "\n")


def eval_int(expr):
    """CubeMX 写的 PSC / ARR 形如 24-1、2000-1、10000"""
    expr = expr.strip()
    if not re.fullmatch(r"[\d\s+\-]+", expr):
        return None
    total = 0
    for sign, num in re.findall(r"([+-]?)\s*(\d+)", expr):
        total += -int(num) if sign == "-" else int(num)
    return total


# ==================== 1. 前提检查 ====================


def check_prerequisites(host, rep):
    step = "前提"
    ok = True
    if not (host / "cmake/stm32cubemx/CMakeLists.txt").is_file():
        rep.error(step, "没有 cmake/stm32cubemx/CMakeLists.txt：CubeMX 的 Project Manager → Toolchain 要选 CMake")
        ok = False
    root_cmake = host / "CMakeLists.txt"
    if not root_cmake.is_file() or not re.search(
        r"add_subdirectory\(\s*cmake/stm32cubemx\s*\)", root_cmake.read_text(encoding="utf-8")
    ):
        rep.error(step, "根 CMakeLists.txt 里没有 add_subdirectory(cmake/stm32cubemx)")
        ok = False
    if not (host / "Core/Inc/stm32h7xx_hal_conf.h").is_file():
        rep.error(step, "没有 Core/Inc/stm32h7xx_hal_conf.h：本库只支持 STM32H7")
        ok = False

    cfg = host / "Core/Inc/FreeRTOSConfig.h"
    rtos = host / "Core/Src/freertos.c"
    if not cfg.is_file() or not rtos.is_file():
        rep.error(step, "没有 Core/Inc/FreeRTOSConfig.h 或 Core/Src/freertos.c：CubeMX 里要启用 FreeRTOS（CMSIS_V2）")
        return False
    cfg_text = cfg.read_text(encoding="utf-8")

    def define(name):
        m = re.search(rf"^\s*#define\s+{name}\s+(.+?)\s*$", cfg_text, re.M)
        return m.group(1) if m else None

    tick = define("configTICK_RATE_HZ")
    if tick is None or not re.search(r"\b1000\b", tick):
        rep.error(step, f"configTICK_RATE_HZ = {tick}：库要求 1000（CubeMX FreeRTOS → TICK_RATE_HZ）")
        ok = False
    if define("configCHECK_FOR_STACK_OVERFLOW") != "2":
        rep.error(step, "configCHECK_FOR_STACK_OVERFLOW 不是 2：CubeMX FreeRTOS → CHECK_FOR_STACK_OVERFLOW = Option2")
        ok = False
    if define("configUSE_MALLOC_FAILED_HOOK") != "1":
        rep.error(step, "configUSE_MALLOC_FAILED_HOOK 不是 1：CubeMX FreeRTOS → USE_MALLOC_FAILED_HOOK = Enabled")
        ok = False
    if '#include "cmsis_os2.h"' not in rtos.read_text(encoding="utf-8"):
        rep.error(step, "freertos.c 没有包含 cmsis_os2.h：FreeRTOS 接口要选 CMSIS_V2")
        ok = False

    if not (host / "Core/Src/stm32h7xx_hal_timebase_tim.c").is_file():
        rep.warn(step, "没找到 stm32h7xx_hal_timebase_tim.c：HAL 时基应选一个空闲定时器（SysTick 留给 FreeRTOS）")
    main_c = host / "Core/Src/main.c"
    if main_c.is_file() and "SCB_EnableDCache" in main_c.read_text(encoding="utf-8"):
        rep.warn(step, "D-Cache 已开启：库假定 D-Cache 关闭，否则 .dma_buffer 要用 MPU 设成非缓存区")
    if ok:
        rep.ok(step, "CubeMX 配置满足 docs/接入指南.md 第 1 节的要求")
    return ok


# ==================== 2. 链接脚本 ====================

AXI_SRAM_ORIGIN = 0x24000000                                     # 所有 STM32H7 的 AXI SRAM（D1 域）起始地址
DMA_UNREACHABLE = ((0x00000000, 0x00040000), (0x20000000, 0x20020000))  # ITCM（最大 256K）/ DTCM：DMA1 / DMA2 访问不到


def find_linker_script(host, rep, explicit):
    if explicit:
        path = (host / explicit).resolve()
        if not path.is_file():
            rep.error("链接脚本", f"{explicit} 不存在")
            return None
        return path
    found = set()
    for cmake in sorted((host / "cmake").glob("*.cmake")):
        for m in re.finditer(r'-T\s*\\?"?([^"\s\\]+\.ld)', cmake.read_text(encoding="utf-8")):
            p = m.group(1).replace("${CMAKE_SOURCE_DIR}", str(host)).replace("${CMAKE_CURRENT_SOURCE_DIR}", str(host))
            found.add(Path(p).resolve())
    if not found:
        found = {p.resolve() for p in host.glob("*.ld")}
    if len(found) != 1:
        rep.error("链接脚本", f"无法确定链接脚本（找到 {len(found)} 个），用 --linker-script 指定")
        return None
    return found.pop()


def plan_linker(host, plan, rep, explicit, region):
    step = "链接脚本"
    path = find_linker_script(host, rep, explicit)
    if path is None:
        return
    tf = TextFile(path)
    rel = path.relative_to(host).as_posix()
    missing = [s for s in ("dma_buffer", "noinit") if not re.search(rf"\.{s}\s*(\(NOLOAD\))?\s*:", tf.text)]
    if "dma_buffer" not in missing:
        # .dma_buffer 必须是可加载段：constexpr 实例的初值要从 Flash 拷进 RAM_D1
        dma_block = re.search(r"^[ \t]*\.dma_buffer[^\n]*:\s*\{.*?\}\s*>[^\n]*", tf.text, re.M | re.S)
        block_text = dma_block.group(0) if dma_block else ""
        if "(NOLOAD)" in block_text:
            rep.error(step, f"{rel} 的 .dma_buffer 是 NOLOAD，实例初值会丢；改成 `.dma_buffer : {{ ... }} >RAM_D1 AT> FLASH`（见 templates/linker_sections.ld）")
            return
        if "AT>" not in block_text:
            rep.error(step, f"{rel} 的 .dma_buffer 缺少 AT> FLASH 加载区；按 templates/linker_sections.ld 手动补上")
            return
    if not missing:
        rep.skip(step, f"{rel} 已有可加载的 .dma_buffer 与 .noinit")
        return

    regions = {
        m.group(1): int(m.group(2), 0)
        for m in re.finditer(r"^\s*([A-Za-z_]\w*)\s*\([rwxaRWXA!]+\)\s*:\s*ORIGIN\s*=\s*(0[xX][0-9A-Fa-f]+|\d+)", tf.text, re.M)
    }
    names = ", ".join(f"{n}@0x{o:08X}" for n, o in regions.items())
    if region is None:
        # 按地址认 AXI SRAM，不按名字：各型号 CubeMX 链接脚本的叫法不同（H723 RAM_D1，H743 / H750 RAM）
        region = next((n for n, o in regions.items() if o == AXI_SRAM_ORIGIN), None)
        region = region or next((r for r in ("RAM_D1", "AXI_SRAM") if r in regions), None)
        if region is None:
            rep.error(step, f"MEMORY 里没有起始于 0x{AXI_SRAM_ORIGIN:08X} 的 AXI SRAM（有 {names}），用 --ram-region 指定 DMA 可达的区")
            return
    elif region not in regions:
        rep.error(step, f"MEMORY 里没有 {region}（有 {names}）")
        return
    if any(lo <= regions[region] < hi for lo, hi in DMA_UNREACHABLE):
        rep.error(step, f"{region}@0x{regions[region]:08X} 是 ITCM / DTCM，DMA1 / DMA2 访问不到，换一个区（有 {names}）")
        return

    template = re.sub(r"^\s*/\*.*?\*/\s*\n", "", read_template("linker_sections.ld"), count=1, flags=re.S)
    blocks = {}
    # 连同 AT> FLASH 与 __dma_buffer_load = LOADADDR(...) 一起取，不能只截到第一个 >region
    for m in re.finditer(r"^[ \t]*\.(\w+)[^\n]*:\s*\{.*?\}\s*>[^\n]*(?:\n[ \t]*__\w+_load\s*=[^\n]*)?", template, re.M | re.S):
        blocks[m.group(1)] = re.sub(r"\}\s*>\s*\w+", f"}} >{region}", m.group(0), count=1)

    lines = tf.text.split("\n")
    data = next((i for i, l in enumerate(lines) if re.match(r"\s*\.data\s*:", l)), None)
    if data is None:
        rep.error(step, f"{rel} 里没找到 .data 段")
        return
    # 从 .data 往回跳过空行、注释和 _sidata = LOADADDR(.data)，落到上一个输出段的结尾 "} >FLASH"
    j = data - 1
    while j >= 0:
        s = lines[j].strip()
        if s == "" or s.startswith(("/*", "*")) or s.endswith("*/") or "LOADADDR(.data)" in s:
            j -= 1
        else:
            break
    if j < 0 or "}" not in lines[j]:
        rep.error(step, f"{rel} 里 .data 之前的结构不认识，请按 templates/linker_sections.ld 手动添加")
        return

    insert = ["", "  /* stm32_framework：见 templates/linker_sections.ld（.dma_buffer 可加载，放 BspUart / BspSpi 实例；.noinit 放 fault_record） */"]
    for i, name in enumerate(missing):
        if i:
            insert.append("")
        insert.extend(blocks[name].split("\n"))
    lines[j + 1 : j + 1] = insert
    tf.text = "\n".join(lines)
    plan.edit(tf)
    rep.ok(step, f"{rel} 加入 {' / '.join('.' + s for s in missing)}（>{region}，0x{regions[region]:08X}）")


# ==================== 3. FreeRTOSConfig.h ====================

DEFAULT_ASSERT = compact("#define configASSERT( x ) if ((x) == 0) {taskDISABLE_INTERRUPTS(); for( ;; );}")


def plan_freertos_config(host, plan, rep):
    step = "FreeRTOSConfig.h"
    tf = TextFile(host / "Core/Inc/FreeRTOSConfig.h")
    if "fault_on_assert" in tf.text:
        rep.skip(step, "configASSERT 已接到 fault_on_assert")
        return
    current = section_text(tf.text, "1")
    if current is None:
        rep.error(step, "没找到唯一的 USER CODE 1 段")
        return
    if compact(current) not in ("", DEFAULT_ASSERT):
        rep.error(step, "USER CODE 1 已有自定义内容，请手动合并 templates/FreeRTOSConfig_user_code.h")
        return
    tf.text = replace_section(tf.text, "1", section_text(read_template("FreeRTOSConfig_user_code.h"), "1"))
    plan.edit(tf)
    rep.ok(step, "USER CODE 1：configASSERT → fault_on_assert()")


# ==================== 4. freertos.c ====================


def plan_freertos_c(host, plan, rep):
    step = "freertos.c"
    tf = TextFile(host / "Core/Src/freertos.c")
    tpl = read_template("freertos_user_code.c")
    done = []

    for name in ("Includes", "4", "5", "Init"):
        if section_text(tf.text, name) is None:
            rep.error(step, f"没找到唯一的 USER CODE {name} 段")
            return

    # Includes / Init：没有就追加（保留段里已有的内容）
    for name, marker in (("Includes", '#include "app_main.h"'), ("Init", "app_init(")):
        current = section_text(tf.text, name)
        if marker in current:
            continue
        add = section_text(tpl, name)
        tf.text = replace_section(tf.text, name, add if current.strip() == "" else current + add)
        done.append(name)

    # 4 / 5：钩子函数体仍是 CubeMX 生成的空函数（只有注释）才替换函数体，保留原签名
    for name, func, marker in (
        ("4", "vApplicationStackOverflowHook", "fault_on_stack_overflow"),
        ("5", "vApplicationMallocFailedHook", "fault_on_malloc_failed"),
    ):
        current = section_text(tf.text, name)
        if marker in current:
            continue
        pattern = rf"(void\s+{func}\s*\([^)]*\))\s*\{{(.*?)\n\}}"
        m = re.search(pattern, current, re.S)
        if m is None or compact(m.group(2)) != "":
            rep.error(step, f"USER CODE {name} 里的 {func}() 不是 CubeMX 默认的空函数，请手动加入 {marker}()")
            return
        body = re.search(pattern, section_text(tpl, name), re.S).group(2)
        current = current[: m.start()] + m.group(1) + "\n{" + body + "\n}" + current[m.end() :]
        tf.text = replace_section(tf.text, name, current)
        done.append(name)

    if done:
        plan.edit(tf)
        rep.ok(step, f"USER CODE {' / '.join(done)}：钩子调 fault_on_*()，MX_FREERTOS_Init() 调 app_init()")
    else:
        rep.skip(step, "钩子与 app_init() 已接好")


# ==================== 5. 应用模板 ====================


def host_handles(host):
    handles = {}
    for src in (host / "Core/Src").glob("*.c"):
        for m in re.finditer(r"^\s*\w+_HandleTypeDef\s+(h\w+)\s*;", src.read_text(encoding="utf-8"), re.M):
            handles[m.group(1)] = src
    return handles


def check_board_cfg(host, rep, board_cfg_text, app_rel):
    """模板 board_cfg.hpp 写的是 DM-MC02 的句柄与参数，对照本工程 CubeMX 生成的代码核对"""
    step = "board_cfg"
    handles = host_handles(host)
    used = sorted(set(re.findall(r"&(h\w+)", strip_comments(board_cfg_text))))
    missing = [h for h in used if h not in handles]
    if missing:
        rep.warn(step, f"模板用到 {', '.join(missing)}，本工程没有：在 {app_rel}/board_cfg.* 里删掉或换成实际句柄（连同 #include）")

    for h, baud in re.findall(r"BspUartT<[^>]+>\s+\w+\(\s*\{\s*\.huart\s*=\s*&(huart\d+)[^}]*\.baudrate\s*=\s*(\d+)U?", board_cfg_text):
        if h in handles:
            m = re.search(rf"{h}\.Init\.BaudRate\s*=\s*(\d+)\s*;", handles[h].read_text(encoding="utf-8"))
            if m and m.group(1) != baud:
                rep.warn(step, f"{h} 模板波特率 {baud}，CubeMX 是 {m.group(1)}：改 {app_rel}/board_cfg.hpp（必须一致）")

    # 指定初始化器写法：.htim / .timer_clk_hz / .prescaler / .period，顺序按 board_cfg.hpp
    pwm = r"BspPwm\s+\w+\(\s*\{\s*\.htim\s*=\s*&(htim\d+)[^}]*?\.timer_clk_hz\s*=\s*(\d+)UL?[^}]*?\.prescaler\s*=\s*(\d+)U?[^}]*?\.period\s*=\s*(\d+)U?"
    for h, clk, psc, arr in re.findall(pwm, board_cfg_text):
        if h not in handles:
            continue
        src = handles[h].read_text(encoding="utf-8")
        for field, want in (("Prescaler", psc), ("Period", arr)):
            m = re.search(rf"{h}\.Init\.{field}\s*=\s*([^;]+);", src)
            have = eval_int(m.group(1)) if m else None
            if have is not None and have != int(want):
                rep.warn(step, f"{h} 模板 {field} = {want}，CubeMX 是 {have}：改 {app_rel}/board_cfg.hpp")
        rep.warn(step, f"{h} 的定时器时钟模板写的是 {clk} Hz（DM-MC02），按本工程时钟树核对")


def plan_app(host, plan, rep, board_dir, app_dir):
    step = "应用模板"
    board = host / board_dir
    app = host / app_dir
    task = app / "task"
    board_rel = board.relative_to(host).as_posix()
    app_rel = app.relative_to(host).as_posix()
    copied = []
    for name in BOARD_TEMPLATES:
        if not (board / name).exists():
            plan.create(board / name, (TEMPLATES / "user" / "board" / name).read_text(encoding="utf-8"))
            copied.append(f"{board_rel}/{name}")
    for name in APP_TEMPLATES:
        if not (app / name).exists():
            plan.create(app / name, (TEMPLATES / "user" / "app" / name).read_text(encoding="utf-8"))
            copied.append(f"{app_rel}/{name}")
    for name in TASK_TEMPLATES:
        if not (task / name).exists():
            plan.create(task / name, (TEMPLATES / "user" / "app" / "task" / name).read_text(encoding="utf-8"))
            copied.append(f"{app_rel}/task/{name}")
    if copied:
        rep.ok(step, f"复制 {', '.join(copied)}")
    else:
        rep.skip(step, "board_cfg / board_instances / app_main / app_task 已存在")
    cfg_parts = []
    for name in ("board_cfg.hpp", "board_cfg.cpp"):
        cfg_path = board / name
        if cfg_path.exists():
            cfg_parts.append(cfg_path.read_text(encoding="utf-8"))
    if cfg_parts:
        check_board_cfg(host, rep, "\n".join(cfg_parts), board_rel)
    app_main = app / "app_main.cpp"
    if app_main.exists() and "user_init(" in strip_comments(app_main.read_text(encoding="utf-8")):
        rep.warn(step, f"{app_rel}/app_main.cpp 仍在调 user_init()：当前模板已改为 system_init/board_init/system_start")


# ==================== 6. hardware_config.h ====================


def plan_hardware_config(host, plan, rep, board_dir):
    step = "hardware_config.h"
    dst = host / board_dir / "hardware_config.h"
    legacy = host / "Core/Inc/hardware_config.h"
    if dst.exists():
        text = dst.read_text(encoding="utf-8")
        if "CAN_RX_ENABLE" not in text:
            rep.warn(step, f"{board_dir}/hardware_config.h 是旧版（缺少 CAN_RX_ENABLE 等开关）：请按 templates/user/board/hardware_config.h 更新")
        else:
            rep.skip(step, f"{board_dir}/hardware_config.h 已存在")
        if legacy.exists():
            rep.warn(step, "Core/Inc/hardware_config.h 仍在旧位置：请删掉，否则两份配置会同时被包含")
        return
    if legacy.exists():
        rep.warn(step, "检测到旧位置 Core/Inc/hardware_config.h：请手动移到 user/board/，脚本不自动复制以免产生两份配置")
        return
    plan.create(dst, (TEMPLATES / "user" / "board" / "hardware_config.h").read_text(encoding="utf-8"))
    rep.ok(step, f"复制完整配置到 {board_dir}/（所有宏实际定义，按需修改）")


# ==================== 7. 根 CMakeLists.txt ====================


def plan_cmake(host, plan, rep, board_dir, app_dir, user_dir):
    step = "CMakeLists.txt"
    tf = TextFile(host / "CMakeLists.txt")
    lib_rel = Path(os.path.relpath(LIB_ROOT, host)).as_posix()
    board_rel = Path(os.path.relpath(host / board_dir, host)).as_posix()
    app_rel = Path(os.path.relpath(host / app_dir, host)).as_posix()
    user_rel = Path(os.path.relpath(host / user_dir, host)).as_posix()
    added = []
    if re.search(r"add_subdirectory\(\s*" + re.escape(lib_rel) + r"\b", tf.text):
        rep.skip(step, f"已有 add_subdirectory({lib_rel})")
    else:
        tf.text = tf.text.rstrip("\n") + "\n" + lib_block(lib_rel, board_rel, app_rel, user_rel)
        added.append(f"{lib_rel} 与 {user_rel}/ 的包含路径")
    # 库自身也要看到 user/board/hardware_config.h：已接入库的老工程再运行时补上
    if re.search(r"target_include_directories\(\s*stm32_framework\b[^)]*" + re.escape(board_rel) + r"(?=[\s)])", tf.text):
        rep.skip(step, f"stm32_framework 已有 {board_rel} 包含路径")
    else:
        tf.text = tf.text.rstrip("\n") + (
            f"\n# hardware_config.h 在 {board_rel}/：库自身也要看到\n"
            f"target_include_directories(stm32_framework PRIVATE {board_rel})\n"
        )
        added.append(f"stm32_framework 的 {board_rel} 包含路径")
    # user/ 单独判断：已接入库的老工程再运行时也会补上
    if re.search(r"\$\{CMAKE_CURRENT_SOURCE_DIR\}/" + re.escape(user_rel) + r"/\*\.cpp", tf.text):
        rep.skip(step, f"已自动收集 {user_rel}/ 的源文件")
    else:
        tf.text = tf.text.rstrip("\n") + "\n" + user_block(user_rel)
        added.append(f"{user_rel}/ 的源文件（GLOB）")
    if added:
        plan.edit(tf)
        rep.ok(step, f"末尾接入 {'、'.join(added)}")


def lib_block(lib_rel, board_rel, app_rel, user_rel):
    return (
        f"\n# stm32_framework：必须在 add_subdirectory(cmake/stm32cubemx) 之后接入（见 {lib_rel}/README.md 第 1 节）\n"
        f"add_subdirectory({lib_rel})\n"
        f"target_include_directories(stm32_framework PRIVATE {board_rel})  # 库自身也要看到 hardware_config.h\n"
        "target_link_libraries(${CMAKE_PROJECT_NAME} stm32_framework)\n"
        "target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_20)\n"
        f"target_include_directories(${{CMAKE_PROJECT_NAME}} PRIVATE {board_rel} {app_rel} {user_rel}/module)\n"
        "target_link_options(${CMAKE_PROJECT_NAME} PRIVATE -u _printf_float)  # BspUartT::printf 打印浮点需要（nano.specs）\n"
    )


def user_block(user_rel):
    return (
        f"\n# 业务代码：{user_rel}/ 下的 .c / .cpp 自动收集（CONFIGURE_DEPENDS：增删业务文件后直接构建，不用改这里）\n"
        f"file(GLOB_RECURSE USER_SOURCES CONFIGURE_DEPENDS ${{CMAKE_CURRENT_SOURCE_DIR}}/{user_rel}/*.c ${{CMAKE_CURRENT_SOURCE_DIR}}/{user_rel}/*.cpp)\n"
        "target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${USER_SOURCES})\n"
        f"target_include_directories(${{CMAKE_PROJECT_NAME}} PRIVATE {user_rel})\n"
    )


# ==================== 8. VS Code 与 Flash ====================


def preset_binary_dir(host, preset):
    """按 CMakePresets.json 解析 binaryDir（含 inherits），换成 VS Code 的 ${workspaceFolder}"""
    fallback = f"${{workspaceFolder}}/build/{preset}"
    try:
        presets = json.loads((host / "CMakePresets.json").read_text(encoding="utf-8"))["configurePresets"]
    except (OSError, ValueError, KeyError):
        return fallback
    by_name = {p.get("name"): p for p in presets}
    p = by_name.get(preset)
    while p is not None and "binaryDir" not in p:
        parent = p.get("inherits")
        p = by_name.get(parent[0] if isinstance(parent, list) else parent)
    if p is None:
        return fallback
    return p["binaryDir"].replace("${sourceDir}", "${workspaceFolder}").replace("${presetName}", preset)


VSCODE = TEMPLATES / "vscode"
FLASH = TEMPLATES / "Flash"
FLASH_FILES = (
    "daplink.cfg",
    "jlink.cfg",
    "OpenOCD_flash.sh",
    "OpenOCD_flash.bat",
    "JLink_flash.sh",
    "JLink_flash.bat",
    "README.md",
)
JSONC_ARRAYS = {"tasks": "label", "configurations": "name"}
# 本机可能已经装好的 SVD（CubeIDE / Keil DFP / cortex-debug 的第三方包），只在宿主没有时用来提醒
SVD_SYSTEM_GLOBS = (
    "~/.vscode/extensions/*/data/*.svd",
    "~/.vscode/extensions/*/svd/*.svd",
    "~/.vscode-server/extensions/*/data/*.svd",
    "~/.cache/arm/packs/*/*/CMSIS/SVD/*.svd",
    "~/AppData/Local/Arm/Packs/*/*/CMSIS/SVD/*.svd",
    "/opt/st/*/plugins/com.st.stm32cube.ide.mcu.debug*/resources/svd/*.svd",
    "~/st/*/plugins/com.st.stm32cube.ide.mcu.debug*/resources/svd/*.svd",
    "~/stm32cubeide*/plugins/com.st.stm32cube.ide.mcu.debug*/resources/svd/*.svd",
)


# ---------- JSONC：只补缺失项，保留宿主原有的键、任务、调试配置与注释 ----------


def _string_end(text, i, end):
    """text[i] 是双引号，返回字符串结束后（引号之后）的下标"""
    j = i + 1
    while j < end:
        if text[j] == "\\":
            j += 2
            continue
        if text[j] == '"':
            return j + 1
        j += 1
    return end


def _skip_ws(text, i, end):
    """跳过空白、// 与 /* */ 注释"""
    while i < end:
        c = text[i]
        if c in " \t\r\n":
            i += 1
        elif c == "/" and i + 1 < end and text[i + 1] == "/":
            j = text.find("\n", i, end)
            i = end if j < 0 else j + 1
        elif c == "/" and i + 1 < end and text[i + 1] == "*":
            j = text.find("*/", i, end)
            i = end if j < 0 else j + 2
        else:
            break
    return i


def _value_end(text, i, end):
    """i 指向值的第一个字符，返回值之后（不含逗号）的下标"""
    c = text[i]
    if c == '"':
        return _string_end(text, i, end)
    if c in "[{":
        close = "]" if c == "[" else "}"
        depth = 0
        j = i
        while j < end:
            ch = text[j]
            if ch == '"':
                j = _string_end(text, j, end)
                continue
            if ch == "/" and j + 1 < end and text[j + 1] == "/":
                k = text.find("\n", j, end)
                j = end if k < 0 else k
                continue
            if ch == "/" and j + 1 < end and text[j + 1] == "*":
                k = text.find("*/", j, end)
                j = end if k < 0 else k + 2
                continue
            if ch == c:
                depth += 1
            elif ch == close:
                depth -= 1
                if depth == 0:
                    return j + 1
            j += 1
        return end
    j = i
    while j < end and text[j] not in ",]}\n\r":
        j += 1
    return j


def _line_indent(text, pos):
    start = text.rfind("\n", 0, pos) + 1
    return re.match(r"[ \t]*", text[start:]).group(0)


def _sig_positions(text, start, end):
    """start..end 之间不在注释里的非空白字符下标（字符串只记首尾）"""
    out = []
    i = start
    while i < end:
        c = text[i]
        if c == '"':
            j = _string_end(text, i, end)
            out.extend((i, j - 1))
            i = j
        elif c == "/" and i + 1 < end and text[i + 1] == "/":
            j = text.find("\n", i, end)
            i = end if j < 0 else j
        elif c == "/" and i + 1 < end and text[i + 1] == "*":
            j = text.find("*/", i, end)
            i = end if j < 0 else j + 2
        else:
            if not c.isspace():
                out.append(i)
            i += 1
    return out


def _root_span(text):
    i = _skip_ws(text, 0, len(text))
    if i >= len(text) or text[i] != "{":
        return None
    return i, _value_end(text, i, len(text))


def _array_element_spans(text, start, end):
    """start / end 是 [ ... ] 的下标，返回各元素的 (起, 止)"""
    spans = []
    i = _skip_ws(text, start + 1, end)
    while i < end and text[i] != "]":
        stop = _value_end(text, i, end)
        spans.append((i, stop))
        i = _skip_ws(text, stop, end)
        if i < end and text[i] == ",":
            i = _skip_ws(text, i + 1, end)
    return spans


def _object_members(text, start, end):
    """start / end 是 { ... } 的下标，返回 [(键, 键起, 值起, 值止)]"""
    members = []
    i = _skip_ws(text, start + 1, end)
    while i < end and text[i] == '"':
        kstop = _string_end(text, i, end)
        key = text[i + 1 : kstop - 1]
        j = _skip_ws(text, kstop, end)
        if j >= end or text[j] != ":":
            break
        vstart = _skip_ws(text, j + 1, end)
        vstop = _value_end(text, vstart, end)
        members.append((key, i, vstart, vstop))
        i = _skip_ws(text, vstop, end)
        if i < end and text[i] == ",":
            i = _skip_ws(text, i + 1, end)
    return members


def _append_members(text, span, chunks):
    """在对象 / 数组 span 的最后一个成员之后插入 chunks（已序列化的 JSON 片段）"""
    start, end = span
    sig = _sig_positions(text, start + 1, end - 1)
    base = _line_indent(text, start)
    if not sig:
        indent = base + "    "
        body = ",\n".join(indent + c.replace("\n", "\n" + indent) for c in chunks)
        return text[: start + 1] + "\n" + body + "\n" + base + text[end - 1 :]
    indent = _line_indent(text, sig[0])
    out = []
    for i, chunk in enumerate(chunks):
        sep = "," if (i > 0 or text[sig[-1]] != ",") else ""
        out.append(f"{sep}\n{indent}{chunk.replace(chr(10), chr(10) + indent)}")
    return text[: sig[-1] + 1] + "".join(out) + text[sig[-1] + 1 :]


def strip_jsonc(text):
    """去掉注释与 ] } 前的多余逗号（字符串内部不动）"""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '"':
            j = _string_end(text, i, n)
            out.append(text[i:j])
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i, n)
            i = n if j < 0 else j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i, n)
            i = n if j < 0 else j + 2
        elif c == ",":
            j = _skip_ws(text, i + 1, n)
            if j < n and text[j] in "]}":
                i += 1
            else:
                out.append(c)
                i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def merge_root_keys(text, want):
    """补根对象里缺的键（如 settings.json 的各项设置）"""
    try:
        have = json.loads(strip_jsonc(text))
    except ValueError:
        return text, []
    extra = {k: v for k, v in want.items() if k not in have and k not in JSONC_ARRAYS}
    span = _root_span(text)
    if not extra or span is None:
        return text, []
    chunks = [f"{json.dumps(k, ensure_ascii=False)}: {json.dumps(v, indent=4, ensure_ascii=False)}" for k, v in extra.items()]
    return _append_members(text, span, chunks), [f"补 {'、'.join(extra)}"]


def _find_member_span(text, key):
    """根对象里 key 对应的值 span（对象 / 数组的括号范围）"""
    root = _root_span(text)
    if root is None:
        return None
    for name, _, vstart, vend in _object_members(text, *root):
        if name == key:
            return vstart, vend
    return None


def merge_one_nested(text, key, id_key, want_items):
    """给已经存在的任务 / 调试配置补上缺的键（返回新文本与说明，没有缺的就返回 None）"""
    span = _find_member_span(text, key)
    if span is None:
        return None
    for start, end in _array_element_spans(text, *span):
        try:
            item = json.loads(strip_jsonc(text[start:end]))
        except ValueError:
            continue
        if not isinstance(item, dict):
            continue
        target = next((w for w in want_items if w.get(id_key) == item.get(id_key)), None)
        if target is None:
            continue
        extra = {k: v for k, v in target.items() if k not in item}
        if extra:
            chunks = [f"{json.dumps(k, ensure_ascii=False)}: {json.dumps(v, indent=4, ensure_ascii=False)}" for k, v in extra.items()]
            return _append_members(text, (start, end), chunks), f"{item.get(id_key)} 补 {'、'.join(extra)}"
    return None


def merge_array_tail(text, key, id_key, want_items):
    """把宿主还没有的任务 / 调试配置追加到数组末尾"""
    span = _find_member_span(text, key)
    if span is None:
        return text, []
    try:
        have = json.loads(strip_jsonc(text[span[0] : span[1]]))
    except ValueError:
        return text, []
    names = {i.get(id_key) for i in have if isinstance(i, dict)} if isinstance(have, list) else set()
    missing = [w for w in want_items if w.get(id_key) not in names]
    if not missing:
        return text, []
    chunks = [json.dumps(m, indent=4, ensure_ascii=False) for m in missing]
    return _append_members(text, span, chunks), [f"新增 {', '.join(str(m.get(id_key)) for m in missing)}"]


# ---------- 工程信息：工程名 / 芯片 / OpenOCD target / SVD ----------


# 同一份参考手册的 H7 子系列：外设寄存器布局一致，SVD 可以互相借用
H7_SVD_GROUPS = (
    ("H742", "H743", "H750", "H753"),
    ("H723", "H725", "H730", "H733", "H735"),
    ("H745", "H747", "H755", "H757"),
    ("H7A0", "H7A3", "H7B0", "H7B3"),
)
# 双核 H7：OpenOCD 还要 set DUAL_CORE 1
H7_DUAL_CORE = ("H745", "H747", "H755", "H757")


def svd_names(group, family):
    """SVD 匹配顺序：先本型号，再同参考手册的兄弟型号"""
    for members in H7_SVD_GROUPS:
        if group in members:
            return [family] + [f"STM32{member}" for member in members if member != group]
    return [family]


def pick_svd(paths, prefixes):
    """按 prefixes 的顺序找 SVD，返回 (路径, 命中的前缀) 或 None"""
    files = list(map(Path, paths))
    for prefix in prefixes:
        hits = [p for p in files if prefix and p.stem.upper().startswith(prefix)]
        if hits:
            # 名字最短的最贴近家族级 SVD（STM32H723.svd 优于 STM32H723VGTx.svd 之类）
            return sorted(hits, key=lambda p: (len(p.stem), p.as_posix()))[0], prefix
    return None


def detect_project(host, rep, preset):
    """从 CMakeLists.txt / .ioc / CMakePresets.json 推出模板要用的占位符"""
    step = "工程信息"
    info = {
        "PRESET": preset,
        "PROJECT": host.name,
        "MCU": "",
        "DEVICE": "STM32H7xx",
        "OPENOCD_TARGET": "stm32h7x.cfg",
        "SVD": "",
        "COMPILE_COMMANDS_DIR": preset_binary_dir(host, preset).replace("${workspaceFolder}/", ""),
        "QUERY_DRIVER": "**/arm-none-eabi-*",
    }

    cmake = (host / "CMakeLists.txt").read_text(encoding="utf-8")
    m = re.search(r"set\(\s*CMAKE_PROJECT_NAME\s+([\w.\-]+)\s*\)", cmake) or re.search(r"^\s*project\(\s*([\w.\-]+)", cmake, re.M)
    if m:
        info["PROJECT"] = m.group(1)
    else:
        rep.warn(step, f"CMakeLists.txt 里没有 CMAKE_PROJECT_NAME / project()，工程名用目录名 {host.name}")

    # 不解析符号链接：CMake 写进编译数据库的是 PATH 里的路径，query-driver 要与之匹配
    gcc = shutil.which("arm-none-eabi-gcc")
    if gcc:
        info["QUERY_DRIVER"] = str(Path(gcc).parent / "arm-none-eabi-*")
    else:
        rep.warn(step, "PATH 里没有 arm-none-eabi-gcc：settings.json 的 query-driver 用通配，装好工具链后重跑本脚本")

    mcu = None
    for ioc in sorted(host.glob("*.ioc")):
        mm = re.search(r"^Mcu\.Name=(\S+)", ioc.read_text(encoding="utf-8", errors="ignore"), re.M)
        if mm:
            mcu = mm.group(1)
            break
    family = group = ""
    if mcu:
        info["MCU"] = mcu
        # 型号位：STM32 + 子系列 4 位 + 引脚 1 位 + Flash 1 位 + 封装 1 位 + 温度 1 位
        #   STM32H723VGTx → STM32H723VG（J-Link device）
        #   STM32H743VITx → STM32H743VI
        #   STM32H750VBTx → STM32H750VB
        name = mcu.upper()
        if name.endswith("X"):
            name = name[:-1]
        info["DEVICE"] = name[:11]
        family = name[:9]
        group = name[5:9]
        if group in H7_DUAL_CORE:
            rep.warn(step, f"{family} 是双核：OpenOCD 还要在 launch.json 的 openOCDLaunchCommands 里加 set DUAL_CORE 1；J-Link 用 device 即可")
        if not group.startswith("H7"):
            rep.warn(step, f"{family} 不是 STM32H7：本库只支持 H7，OpenOCD target 用 stm32h7x.cfg 兜底，需手动改")
        # 2 MB 的 H7（Flash 代号 I）是双 bank，OpenOCD 要换 target
        if name[10:11] == "I":
            info["OPENOCD_TARGET"] = "stm32h7x_dual_bank.cfg"
    else:
        rep.warn(step, "没找到 .ioc 的 Mcu.Name：launch.json 的 device 留成 STM32H7xx，需要手动改成实际型号")

    names = svd_names(group, family) if family else []
    hit = pick_svd(glob.glob(str(host / "Flash/*.svd")), names)
    if hit:
        path, prefix = hit
        info["SVD"] = path.name
        if prefix == family:
            rep.ok("SVD", f"宿主有 Flash/{path.name}：launch.json 写 svdFile，外设寄存器面板可用")
        else:
            rep.warn("SVD", f"宿主没有 {family}.svd，用 Flash/{path.name} 代替（同参考手册，寄存器布局一致）：launch.json 写 svdFile")
    elif names:
        elsewhere = [p for pat in ("*.svd", ".vscode/*.svd", "Docs/**/*.svd") for p in glob.glob(str(host / pat), recursive=True)]
        local_hit = pick_svd(elsewhere, names)
        system_hit = None if local_hit else pick_svd([p for pat in SVD_SYSTEM_GLOBS for p in glob.glob(os.path.expanduser(pat))], names)
        if local_hit:
            rep.warn("SVD", f"宿主有 {local_hit[0].relative_to(host).as_posix()}，但不在 Flash/ 下：移到 Flash/ 后重跑即可启用外设寄存器面板")
        elif system_hit:
            rep.warn("SVD", f"宿主 Flash/ 下没有 SVD；本机有 {system_hit[0]}，复制到 Flash/ 后重跑即可（不复制也不影响调试）")
        else:
            rep.warn("SVD", "宿主 Flash/ 下没有 SVD：launch.json 不写 svdFile，外设寄存器面板不可用（断点、变量、内存、反汇编不受影响）")
    return info


def render_template(path, info):
    text = path.read_text(encoding="utf-8").replace("\r\n", "\n")
    for key, value in info.items():
        text = text.replace("{{" + key + "}}", str(value))
    return text


def drop_svd(text):
    """宿主没有 SVD：去掉 launch.json 里 svdFile 那一行和它上面的 // SVD: 说明"""
    return "\n".join(l for l in text.split("\n") if not l.strip().startswith(("// SVD:", '"svdFile"')))


def plan_vscode(host, plan, rep, info):
    """宿主没有就生成；有就只补缺失的键 / 任务 / 调试配置，原有内容与注释都保留"""
    step = ".vscode"
    for name in ("settings.json", "tasks.json", "launch.json"):
        template = render_template(VSCODE / name, info)
        if not info["SVD"]:
            template = drop_svd(template)
        dst = host / ".vscode" / name
        if not dst.exists():
            plan.create(dst, template)
            rep.ok(step, f"新建 .vscode/{name}")
            continue
        tf = TextFile(dst)
        try:
            want = json.loads(strip_jsonc(template))
            json.loads(strip_jsonc(tf.text))
        except ValueError:
            rep.warn(step, f".vscode/{name} 不是能识别的 JSONC，跳过；请手动对照 templates/vscode/{name} 合并")
            continue
        text = tf.text
        text, added = merge_root_keys(text, want)
        for key, id_key in JSONC_ARRAYS.items():
            for _ in range(50):
                result = merge_one_nested(text, key, id_key, want.get(key) or [])
                if result is None:
                    break
                text, msg = result
                added.append(msg)
            text, tail = merge_array_tail(text, key, id_key, want.get(key) or [])
            added += tail
        if text == tf.text:
            rep.skip(step, f".vscode/{name} 已包含模板内容")
            continue
        try:
            json.loads(strip_jsonc(text))
        except ValueError:
            rep.warn(step, f".vscode/{name} 合并后不是合法 JSON，已跳过；请手动对照 templates/vscode/{name} 合并")
            continue
        tf.text = text
        plan.edit(tf)
        rep.ok(step, f".vscode/{name}：{'；'.join(added)}")

    final = plan.new_files.get(host / ".vscode" / "settings.json")
    if final is None and (host / ".vscode" / "settings.json") in plan.edits:
        final = plan.edits[host / ".vscode" / "settings.json"].text
    if final is not None and "--compile-commands-dir" not in final:
        rep.warn("clangd", f".vscode/settings.json 没有 --compile-commands-dir：clangd 默认只找 build/，请在 clangd.arguments 里加 --compile-commands-dir={info['COMPILE_COMMANDS_DIR']}")

    legacy = ("OpenOCD_flash_linux", "OpenOCD_flash_win", "JLink_flash_linux", "JLink_flash_win")
    tasks = plan.new_files.get(host / ".vscode" / "tasks.json")
    if tasks is None:
        tf = plan.edits.get(host / ".vscode" / "tasks.json")
        tasks = tf.text if tf is not None else ((host / ".vscode" / "tasks.json").read_text(encoding="utf-8") if (host / ".vscode" / "tasks.json").exists() else "")
    if any(f'"{name}"' in tasks for name in legacy):
        rep.warn(step, "tasks.json 里还有旧的 *_linux / *_win 烧录任务：新的 OpenOCD_flash / JLink_flash 会按平台自动选 .sh 或 .bat，旧任务可以删掉")


def plan_flash(host, plan, rep, info):
    """复制烧录脚本（已存在的不覆盖）；脚本运行时自己递归找 build/ 下的 .elf"""
    step = "Flash"
    copied = []
    for name in FLASH_FILES:
        dst = host / "Flash" / name
        if dst.exists():
            if name.endswith(".sh") and not os.access(dst, os.X_OK):
                plan.chmod(dst, 0o755)
                rep.ok(step, f"Flash/{name} 补可执行权限")
            continue
        plan.create(dst, render_template(FLASH / name, info))
        if name.endswith(".sh"):
            plan.chmod(dst, 0o755)
        copied.append(name)
    if copied:
        rep.ok(step, f"复制 {', '.join('Flash/' + n for n in copied)}（运行时自动在 build/ 下递归找 .elf）")
    else:
        rep.skip(step, "Flash/ 脚本已存在")


# ==================== 9. clangd：.clang-format / .clangd ====================


CLANGD = TEMPLATES / "clang"
CLANGD_FILE = CLANGD / ".clangd"
CLANGD_FLAGS = CLANGD / "compile_flags.yaml"
CLANG_FORMAT = LIB_ROOT / ".clang-format"


def plan_clang(host, plan, rep, info):
    """
    生成 .clang-format 与 .clangd（已存在的不覆盖，但会校对 / 补缺）

    - `.clang-format` 直接用库根目录那一份：唯一来源，不在 templates 里再存一份，避免两处不同步。
      它带 `Language: Cpp`，只格式 C++，CubeMX 生成的 Core/Src/*.c 不会被改。
    - `.clangd` 按 CMakePresets 的 binaryDir 填 CompilationDatabase，按本机工具链位置填
      --query-driver；缺了这两项 clangd 就会把 Cortex-M 工程标成一片红。
      宿主已经有手写 `.clangd` 时**不覆盖**：没有 CompileFlags 就把这一段补在最前面，
      已有 CompileFlags 就用 [注意] 提示手动改（避免动别人写好的配置）。
    """
    step = "clangd"

    dst = host / ".clang-format"
    if dst.exists():
        if dst.read_bytes() == CLANG_FORMAT.read_bytes():
            rep.skip(step, ".clang-format 已存在（与库风格一致）")
        else:
            rep.warn(step, ".clang-format 已存在但与库风格不同：保留宿主的；想要库风格就先删掉再重跑")
    else:
        # 按字节复制：库那份是 CRLF，经文本读写会被换成 LF
        plan.create(dst, CLANG_FORMAT.read_bytes())
        rep.ok(step, "新建 .clang-format（与库一致；Language: Cpp，不碰 CubeMX 生成的 .c）")

    dst = host / ".clangd"
    if not dst.exists():
        plan.create(dst, render_template(CLANGD_FILE, info))
        rep.ok(step, f"新建 .clangd（编译数据库 {info['COMPILE_COMMANDS_DIR']}，query-driver {info['QUERY_DRIVER']}）")
        return

    tf = TextFile(dst)
    if info["COMPILE_COMMANDS_DIR"] in tf.text:
        rep.skip(step, f".clangd 已存在（编译数据库 {info['COMPILE_COMMANDS_DIR']}）")
        return
    if re.search(r"^CompileFlags\s*:", tf.text, re.M):
        rep.warn(
            step,
            f".clangd 已有 CompileFlags 但没指向 {info['COMPILE_COMMANDS_DIR']}："
            "把 CompilationDatabase 改成该目录（换过预设时一定要改，否则索引的是旧构建）",
        )
        return
    # 宿主的手写配置里没有 CompileFlags：把这一段补在最前面，其余保持原样
    block = render_template(CLANGD_FLAGS, info)
    tf.text = block + "\n" + tf.text.lstrip("\n")
    plan.edit(tf)
    rep.ok(
        step,
        f"在已有 .clangd 顶部补 CompileFlags（编译数据库 {info['COMPILE_COMMANDS_DIR']}，"
        f"query-driver {info['QUERY_DRIVER']}），原有配置未改动",
    )


# ==================== 入口 ====================



def default_host():
    out = subprocess.run(
        ["git", "-C", str(LIB_ROOT), "rev-parse", "--show-superproject-working-tree"],
        capture_output=True,
        text=True,
    ).stdout.strip()
    return Path(out) if out else Path.cwd()


def main():
    ap = argparse.ArgumentParser(description="把 stm32_framework 接入 STM32H7 CubeMX（CMake）宿主工程（详见 docs/接入指南.md）")
    ap.add_argument("--host", type=Path, help="宿主工程根目录（默认：本库作为 submodule 所在的工程，否则当前目录）")
    ap.add_argument("--dry-run", action="store_true", help="只检查并打印将要做的修改，不写文件")
    ap.add_argument("--board-dir", default="user/board", help="板级配置目录，相对宿主根（默认 user/board）")
    ap.add_argument("--app-dir", default="user/app", help="应用代码目录，相对宿主根（默认 user/app）")
    ap.add_argument("--user-dir", default="user", help="业务代码根目录，相对宿主根（默认 user）")
    ap.add_argument("--linker-script", help="链接脚本，相对宿主根（默认从 cmake/*.cmake 的 -T 参数找）")
    ap.add_argument("--ram-region", help="放 .dma_buffer / .noinit 的内存区（默认按地址找 0x24000000 起的 AXI SRAM，不论叫 RAM_D1 还是 RAM）")
    ap.add_argument("--preset", default="Debug", help="用哪个 CMake 预设的编译数据库与构建目录（默认 Debug）")
    ap.add_argument("--no-hardware-config", action="store_true", help="不复制 hardware_config.h")
    ap.add_argument("--no-vscode", action="store_true", help="不处理 .vscode/settings.json、tasks.json、launch.json")
    ap.add_argument("--no-flash", action="store_true", help="不复制 Flash/ 烧录脚本")
    ap.add_argument("--no-clang", action="store_true", help="不生成 .clang-format / .clangd")
    args = ap.parse_args()

    host = (args.host or default_host()).resolve()
    if host == LIB_ROOT or not (host / "CMakeLists.txt").is_file():
        sys.exit(f"{host} 不是宿主工程根目录：在宿主工程根目录运行，或用 --host 指定")
    print(f"宿主工程：{host}\n库：{LIB_ROOT}\n")

    rep = Report()
    plan = Plan()
    if check_prerequisites(host, rep):
        plan_linker(host, plan, rep, args.linker_script, args.ram_region)
        plan_freertos_config(host, plan, rep)
        plan_freertos_c(host, plan, rep)
        plan_app(host, plan, rep, args.board_dir, args.app_dir)
        if not args.no_hardware_config:
            plan_hardware_config(host, plan, rep, args.board_dir)
        plan_cmake(host, plan, rep, args.board_dir, args.app_dir, args.user_dir)
        if not args.no_vscode or not args.no_flash or not args.no_clang:
            info = detect_project(host, rep, args.preset)
            if not args.no_vscode:
                plan_vscode(host, plan, rep, info)
            if not args.no_flash:
                plan_flash(host, plan, rep, info)
            if not args.no_clang:
                plan_clang(host, plan, rep, info)
    rep.print()

    if rep.has_error:
        print("\n有错误，未修改任何文件。")
        return 1
    if plan.empty():
        print("\n已全部接入，无需修改。")
        return 0
    if args.dry_run:
        print("\n将要做的修改（--dry-run，未写入）：\n")
        plan.show(host)
        return 0
    plan.apply()
    print(
        "\n已写入。下一步：\n"
        f"  1. 按上面的 [注意] 核对 {args.board_dir}/board_cfg.*（句柄、波特率、PWM 参数）\n"
        f"  2. 业务任务写在 {args.app_dir}/task/；示例 heartbeat/monitor 不自动创建，需要时在 StartDefaultTask() 里 xTaskCreate\n"
        f"  3. cmake --preset {args.preset} && cmake --build --preset {args.preset}\n"
        f"  4. 烧录：bash Flash/OpenOCD_flash.sh 或 bash Flash/JLink_flash.sh（自动在 build/ 下找 .elf）\n"
        "  5. VS Code 里执行 clangd: Restart language server（.clangd 已指向该预设的编译数据库）"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
