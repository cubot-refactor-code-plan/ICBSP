"""
把 generate.py 算出的生成区内容同步进宿主文件（只算修改，写盘由 integrate.py 的 Plan 统一做）。

规则：
  - 只改生成区，区外一律不动；内容和现在一样的区不算修改（可以重复运行）
  - 文件不存在：从库模板新建（模板里带生成区），再填生成区
  - 文件存在但缺某个生成区（手写的文件）：报错停下，提示把模板里的标记加进去
  - 生成区被手改过（结束标记里的哈希对不上）：报错停下；加 --overwrite-edited 才覆盖
  - 区外的冲突：宿主别处已经定义了要生成的 HAL_GPIO_EXTI_Callback、或区外手写了同名实例：报错停下
"""

import json
import re
from pathlib import Path

from . import regions as R

TEMPLATES = Path(__file__).resolve().parent.parent.parent / "templates" / "user" / "board"
STEP = "生成"
SELECTION_FILE = "icbsp_selection.json"


def saved_selection(host, board_dir):
    p = host / board_dir / SELECTION_FILE
    try:
        return json.loads(p.read_text(encoding="utf-8")) if p.is_file() else None
    except ValueError:
        return None


def target_files(host, board_dir):
    board = host / board_dir
    hw = board / "hardware_config.h"
    legacy = host / "Core/Inc/hardware_config.h"
    if not hw.exists() and legacy.exists():
        hw = legacy
    return {
        "board_cfg.hpp": board / "board_cfg.hpp",
        "board_cfg.cpp": board / "board_cfg.cpp",
        "board_instances.hpp": board / "board_instances.hpp",
        "hardware_config.h": hw,
    }


def _strip_regions(text):
    """去掉所有生成区（用来查区外的内容）"""
    regs, _ = R.parse(text)
    lines = text.split("\n")
    drop = set()
    for r in regs.values():
        drop.update(range(r["begin"], r["end"] + 1))
    return "\n".join(l for i, l in enumerate(lines) if i not in drop)


def plan(host, board_dir, plan_obj, rep, gen, text_file_cls, overwrite_edited=False):
    """把生成结果登记进 plan_obj；返回是否成功（有错误时 rep 里已记 error）"""
    files = target_files(host, board_dir)
    contents = gen.regions()
    ok = True
    changed, created = [], []
    pending = {}  # path -> (TextFile 或 None, 文本)
    for fname, path in files.items():
        if path in plan_obj.new_files:
            text = plan_obj.new_files[path]
            text = text.decode("utf-8") if isinstance(text, bytes) else text
            tf = None
        elif path.exists():
            tf = text_file_cls(path)
            text = tf.text
        else:
            text = (TEMPLATES / fname).read_text(encoding="utf-8").replace("\r\n", "\n")
            tf = None
            created.append(path)
        regs, bad = R.parse(text)
        rel = path.relative_to(host).as_posix()
        if bad:
            rep.error(STEP, f"{rel}：生成区标记不完整（{'；'.join(bad)}）")
            ok = False
            continue
        missing = [r for r in contents[fname] if r not in regs]
        if missing:
            rep.error(STEP, f"{rel} 没有生成区 {', '.join(missing)}（手写的文件）：把 templates/user/board/{fname} 里对应的 "
                      f"`// <ICBSP-GEN …>` 标记加进来，或把文件改名后让 integrate.py 重新生成")
            ok = False
            continue
        edited = [r for r in contents[fname] if regs[r]["recorded"] != regs[r]["actual"]]
        if edited and not overwrite_edited:
            rep.error(STEP, f"{rel} 的生成区 {', '.join(edited)} 被手改过：把改动挪到生成区外（或删掉改动）后重新运行；确认要丢掉改动就加 --overwrite-edited")
            ok = False
            continue
        new = text
        for r, body in contents[fname].items():
            new = R.replace(new, r, body)
        if new != text:
            changed.append(rel)
        pending[path] = (tf, new)

    if not ok:
        return False

    # 区外冲突
    outside = {p: _strip_regions(t) for p, (_, t) in pending.items()}
    gen_names = set(gen.generated_names())
    for p, t in outside.items():
        code = re.sub(r"//[^\n]*|/\*.*?\*/", "", t, flags=re.S)
        for name in sorted(gen_names):
            # 实例是对象，只会以 name.xxx / (name) 出现；区外出现“name(”就是又定义了一次
            if re.search(rf"(?<![.\w]){re.escape(name)}\s*\(", code):
                rep.error(STEP, f"{p.relative_to(host).as_posix()} 的生成区外已经定义了 {name}：和生成的实例重名，删掉区外的定义或在选择里改名")
                ok = False
    if contents["board_cfg.cpp"]["callbacks"].count("HAL_GPIO_EXTI_Callback"):
        skip = {"build", "third_party", "Drivers", "Middlewares", ".git", "cmake"}
        srcs = {p for p in host.rglob("*.c") if not skip & set(p.relative_to(host).parts)}
        srcs |= {p for p in host.rglob("*.cpp") if not skip & set(p.relative_to(host).parts)}
        for src in sorted(srcs | set(pending)):
            if src in pending:
                text = outside[src]
            elif src.is_file():
                text = src.read_text(encoding="utf-8", errors="replace")
            else:
                continue
            code = re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)
            if re.search(r"(?<!__weak )\bvoid\s+HAL_GPIO_EXTI_Callback\s*\([^;]*\)\s*\{", code) and not re.search(r"__weak\s+void\s+HAL_GPIO_EXTI_Callback", code):
                rep.error(STEP, f"{src.relative_to(host).as_posix()} 已经定义了 HAL_GPIO_EXTI_Callback：生成的设备要用 EXTI 转调，"
                          f"把那份删掉，或者把它的内容并进 board_cfg.cpp 的 callbacks 生成区之外、并不让 integrate.py 生成转调（不选设备的 INT 引脚）")
                ok = False
    if not ok:
        return False

    # 选择本身也存进宿主：生成区的“源”，随工程提交；下次扫描 / 打开 Web UI 从它开始
    sel_path = host / board_dir / SELECTION_FILE
    sel_text = json.dumps(gen.sel, ensure_ascii=False, indent=2) + "\n"
    if sel_path in plan_obj.new_files:
        plan_obj.create(sel_path, sel_text)
    elif not sel_path.exists():
        plan_obj.create(sel_path, sel_text)
        created.append(sel_path)
    else:
        stf = text_file_cls(sel_path)
        if stf.text != sel_text:
            stf.text = sel_text
            plan_obj.edit(stf)
            changed.append(sel_path.relative_to(host).as_posix())
    for path, (tf, new) in pending.items():
        if path in plan_obj.new_files or tf is None:
            plan_obj.create(path, new)
        else:
            tf.text = new
            plan_obj.edit(tf)
    if changed or created:
        rep.ok(STEP, f"生成区：{'、'.join(changed) or '无改动'}" + (f"；新建 {', '.join(p.relative_to(host).as_posix() for p in created)}" if created else ""))
    else:
        rep.skip(STEP, "生成区与选择一致，无需修改")
    for n in gen.notes():
        rep.warn(STEP, n)
    return True
