#!/usr/bin/env python3
"""一次性补丁：把 tools/integrate.py 换成模板驱动的 .vscode / Flash 接入。

编辑器写盘此刻不生效（编辑工具报成功但磁盘没变），所以用这个脚本落盘。跑完即可删除。
"""

import sys
from pathlib import Path

PATH = Path(__file__).resolve().parent / "integrate.py"

NEW_BLOCK = r'''VSCODE = TEMPLATES / "vscode"
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


def pick_svd(paths, family):
    hits = [p for p in map(Path, paths) if family and p.stem.upper().startswith(family)]
    # 名字最短的最贴近家族级 SVD（STM32H723.svd 优于 STM32H723xx.svd 之类）
    return sorted(hits, key=lambda p: (len(p.stem), p.as_posix()))[0] if hits else None


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
    if mcu:
        info["MCU"] = mcu
        # STM32H723VGTx → STM32H723VG：J-Link 的 device 名去掉封装与温度代号
        name = mcu.upper()
        if name.endswith("X"):
            name = name[:-1]
        info["DEVICE"] = name[:11]
        family = name[:9]
    else:
        family = ""
        rep.warn(step, "没找到 .ioc 的 Mcu.Name：launch.json 的 device 留成 STM32H7xx，需要手动改成实际型号")

    hit = pick_svd(glob.glob(str(host / "Flash/*.svd")), family)
    if hit:
        info["SVD"] = hit.name
        rep.ok("SVD", f"宿主有 Flash/{hit.name}：launch.json 写 svdFile，外设寄存器面板可用")
    elif family:
        elsewhere = [p for pat in ("*.svd", ".vscode/*.svd", "Docs/**/*.svd") for p in glob.glob(str(host / pat), recursive=True)]
        local_hit = pick_svd(elsewhere, family)
        system_hit = None if local_hit else pick_svd([p for pat in SVD_SYSTEM_GLOBS for p in glob.glob(os.path.expanduser(pat))], family)
        if local_hit:
            rep.warn("SVD", f"宿主有 {local_hit.relative_to(host).as_posix()}，但不在 Flash/ 下：移到 Flash/ 后重跑即可启用外设寄存器面板")
        elif system_hit:
            rep.warn("SVD", f"宿主 Flash/ 下没有 SVD；本机有 {system_hit}，复制到 Flash/ 后重跑即可（不复制也不影响调试）")
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


# ==================== 入口 ====================
'''


def sub_once(text, old, new):
    count = text.count(old)
    if count != 1:
        sys.exit(f"锚点异常（出现 {count} 次）：{old[:70]!r}")
    return text.replace(old, new, 1)


def main():
    raw = PATH.read_bytes()
    crlf = b"\r\n" in raw
    text = raw.decode("utf-8").replace("\r\n", "\n")

    # 1. import glob
    text = sub_once(text, "import difflib\nimport json\n", "import difflib\nimport glob\nimport json\n")

    # 2. Plan 加 modes
    text = sub_once(
        text,
        "        self.new_files = {}  # path -> str\n",
        "        self.new_files = {}  # path -> str\n        self.modes = {}  # path -> 权限位（.sh 要为 0755）\n",
    )
    text = sub_once(
        text,
        "    def create(self, path, text):\n        self.new_files[path] = text\n",
        "    def create(self, path, text):\n        self.new_files[path] = text\n\n    def chmod(self, path, mode):\n        self.modes[path] = mode\n",
    )
    text = sub_once(
        text,
        "        return not any(tf.text != tf.original for tf in self.edits.values()) and not self.new_files\n",
        "        return not any(tf.text != tf.original for tf in self.edits.values()) and not self.new_files and not self.modes\n",
    )
    text = sub_once(
        text,
        '            path.write_text(text, encoding="utf-8")\n',
        '            path.write_text(text, encoding="utf-8")\n        for path, mode in self.modes.items():\n            path.chmod(mode)\n',
    )

    # 3. 段标题 + 用新实现替换旧的 clangd 版 plan_vscode
    text = sub_once(text, "# ==================== 8. clangd ====================", "# ==================== 8. VS Code 与 Flash ====================")
    start = text.index("def plan_vscode(host, plan, rep, preset):")
    end = text.index("# ==================== 入口 ====================")
    text = text[:start] + NEW_BLOCK + "\n" + text[end + len("# ==================== 入口 ====================\n") :]

    # 4. 命令行参数
    text = sub_once(
        text,
        '    ap.add_argument("--preset", default="Debug", help="clangd 使用哪个 CMake 预设的编译数据库（默认 Debug）")\n',
        '    ap.add_argument("--preset", default="Debug", help="用哪个 CMake 预设的编译数据库与构建目录（默认 Debug）")\n',
    )
    text = sub_once(
        text,
        '    ap.add_argument("--no-vscode", action="store_true", help="不改 .vscode/settings.json")\n',
        '    ap.add_argument("--no-vscode", action="store_true", help="不处理 .vscode/settings.json、tasks.json、launch.json")\n'
        '    ap.add_argument("--no-flash", action="store_true", help="不复制 Flash/ 烧录脚本")\n',
    )

    # 5. 主流程接线
    text = sub_once(
        text,
        "        if not args.no_vscode:\n            plan_vscode(host, plan, rep, args.preset)\n",
        "        if not args.no_vscode or not args.no_flash:\n"
        "            info = detect_project(host, rep, args.preset)\n"
        "            if not args.no_vscode:\n"
        "                plan_vscode(host, plan, rep, info)\n"
        "            if not args.no_flash:\n"
        "                plan_flash(host, plan, rep, info)\n",
    )

    # 6. 结束提示
    text = sub_once(
        text,
        '        "  4. VS Code 里 clangd: Restart language server"\n',
        '        f"  4. 烧录：bash Flash/OpenOCD_flash.sh 或 bash Flash/JLink_flash.sh（自动在 build/ 下找 .elf）\\n"\n'
        '        "  5. VS Code 里 clangd: Restart language server"\n',
    )

    # 7. 顶部说明
    text = sub_once(
        text,
        "  9. .vscode/settings.json 配置 clangd 的编译数据库目录与 query-driver\n",
        "  9. .vscode/settings.json / tasks.json / launch.json（templates/vscode）：没有就生成，有就只补缺失的键、\n"
        "     任务与调试配置（原有内容与注释都保留）。工程名取 CMAKE_PROJECT_NAME，芯片与 OpenOCD target 取 .ioc\n"
        "     的 Mcu.Name；宿主 Flash/ 下有 SVD 才写 svdFile，没有就不写并提示一次\n"
        " 10. 复制 Flash/ 烧录脚本（templates/Flash）：脚本运行时自己在 build/ 下递归找 .elf，不需要传路径\n",
    )

    out = text.replace("\n", "\r\n") if crlf else text
    PATH.write_bytes(out.encode("utf-8"))
    print(f"已更新 {PATH}（{len(out.encode('utf-8'))} 字节）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
