"""
宿主文件里的“生成区”：

    // <ICBSP-GEN peripherals>
    ……由 integrate.py 生成的内容……
    // </ICBSP-GEN peripherals 1a2b3c4d>

结束标记里的 8 位十六进制是区内内容的 SHA-1 前缀（按 \\n 换行、去掉缩进前的原文计算）。重新生成时：
记录的哈希和区内实际内容对不上 = 有人手改过本区 → 停下来不覆盖（让用户把改动挪到区外，或删掉本区重新生成）。
区外的内容一律不动。

CMake、.gitignore 等用 `#` 注释的文件把 prefix 传成 "#"：

    # <ICBSP-GEN icbsp-options>
    ……
    # </ICBSP-GEN icbsp-options 1a2b3c4d>
"""

import hashlib
import re


def _patterns(prefix):
    p = re.escape(prefix)
    return (
        re.compile(rf"^([ \t]*){p} <ICBSP-GEN ([\w-]+)>[ \t]*$"),
        re.compile(rf"^[ \t]*{p} </ICBSP-GEN ([\w-]+) ([0-9a-f]{{8}})>[ \t]*$"),
    )


BEGIN, END = _patterns("//")


def digest(body):
    return hashlib.sha1(body.encode("utf-8")).hexdigest()[:8]


def parse(text, prefix="//"):
    """返回 ({名字: 区信息}, [问题])；区信息：begin、end（行号）、indent、body、recorded、actual"""
    begin_re, end_re = _patterns(prefix)
    lines = text.split("\n")
    found, bad, open_ = {}, [], None
    for i, line in enumerate(lines):
        mb = begin_re.match(line)
        me = end_re.match(line)
        if mb:
            if open_:
                bad.append(f"第 {i + 1} 行：生成区 {mb.group(2)} 开始时 {open_[1]} 还没结束")
            open_ = (i, mb.group(2), mb.group(1))
        elif me:
            if not open_ or open_[1] != me.group(1):
                bad.append(f"第 {i + 1} 行：结束标记 {me.group(1)} 没有对应的开始标记")
                open_ = None
                continue
            b, name, indent = open_
            body = "\n".join(lines[b + 1 : i])
            if name in found:
                bad.append(f"生成区 {name} 出现了两次")
            found[name] = {"begin": b, "end": i, "indent": indent, "body": body, "recorded": me.group(2), "actual": digest(body)}
            open_ = None
    if open_:
        bad.append(f"生成区 {open_[1]} 没有结束标记")
    return found, bad


def replace(text, name, body, prefix="//"):
    """把生成区 name 的内容换成 body（body 已带缩进），返回新文本"""
    regs, bad = parse(text, prefix)
    if bad:
        raise ValueError("；".join(bad))
    r = regs[name]
    lines = text.split("\n")
    new = [f"{r['indent']}{prefix} <ICBSP-GEN {name}>"] + (body.split("\n") if body else []) + [f"{r['indent']}{prefix} </ICBSP-GEN {name} {digest(body)}>"]
    return "\n".join(lines[: r["begin"]] + new + lines[r["end"] + 1 :])


def block(name, body, prefix="//"):
    """一个完整的生成区（开始标记 + 内容 + 带哈希的结束标记）"""
    return "\n".join([f"{prefix} <ICBSP-GEN {name}>"] + (body.split("\n") if body else []) + [f"{prefix} </ICBSP-GEN {name} {digest(body)}>"])


def seal(text, prefix="//"):
    """按区内现有内容重算所有结束标记的哈希（给模板用）"""
    regs, bad = parse(text, prefix)
    if bad:
        raise ValueError("；".join(bad))
    for name in regs:
        text = replace(text, name, parse(text, prefix)[0][name]["body"], prefix)
    return text


def edited(text, prefix="//"):
    """手改过的生成区名"""
    regs, _ = parse(text, prefix)
    return [n for n, r in regs.items() if r["recorded"] != r["actual"]]
