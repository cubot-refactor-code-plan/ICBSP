"""
.ioc 自动修改与 CubeMX 无界面重新生成。

修改：checks.py 给出的、能自动修改的失败项（changes）逐条写进 .ioc；写前把原文件备份成 <名字>.ioc.bak-<时间>。
重新生成：用 STM32CubeMX 自带的 Java 跑命令行脚本（DM-MC02 工程上验证过的做法）：

    cd <CubeMX 目录> && ./jre/bin/java -jar STM32CubeMX -q <脚本>
    脚本：config load <.ioc 的绝对路径> / project generate / exit

注意：桌面上开着 CubeMX 界面时它会抢锁，命令行会一直等（这里有超时）。生成后看 git status：
CubeMX 可能改链接脚本的几行行尾、把 Drivers/CMSIS/DSP 拷回来（见宿主 CLAUDE.md）。
"""

import os
import shutil
import subprocess
import tempfile
import time
from pathlib import Path

DEFAULT_DIRS = (Path.home() / "STM32CubeMX", Path("/opt/st/stm32cubemx"), Path("C:/Program Files/STMicroelectronics/STM32Cube/STM32CubeMX"))
TIMEOUT_S = 600


def apply_fixes(ioc, results, ids):
    """把 results 里 id 在 ids（"all" = 全部）中、能自动修改的失败项写进 ioc 对象；返回实际改了的 [(规则, 目标, 键, 旧, 新)]"""
    done = []
    for r in results:
        if r["ok"] or not r["auto_fix"]:
            continue
        if ids != "all" and r["id"] not in ids:
            continue
        for c in r["changes"]:
            old, new = ioc.set(c["key"], c["new"])
            done.append((r["id"], r["target"], c["key"], old, new))
    return done


def backup(path):
    stamp = time.strftime("%Y%m%d-%H%M%S")
    dst = path.with_name(f"{path.name}.bak-{stamp}")
    shutil.copy2(path, dst)
    return dst


def find_cubemx(explicit=None):
    """返回 (java, jar) 或 None"""
    cands = []
    if explicit:
        cands.append(Path(explicit))
    if os.environ.get("ICBSP_CUBEMX"):
        cands.append(Path(os.environ["ICBSP_CUBEMX"]))
    cands += list(DEFAULT_DIRS)
    for c in cands:
        d = c.parent if c.is_file() else c
        jar = d / "STM32CubeMX"
        if not jar.is_file():
            jar = d / "STM32CubeMX.exe"
        java = d / "jre" / "bin" / ("java.exe" if os.name == "nt" else "java")
        if jar.is_file() and java.is_file():
            return java, jar
    return None


def regenerate(ioc_path, cubemx=None, timeout=TIMEOUT_S):
    """无界面重新生成；返回 (成功, 说明, 输出尾部)"""
    found = find_cubemx(cubemx)
    if found is None:
        return False, "找不到 STM32CubeMX（--cubemx 指定目录，或设环境变量 ICBSP_CUBEMX）：.ioc 已改好，请用 CubeMX 打开后点 Generate Code", ""
    java, jar = found
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False, encoding="utf-8") as f:
        f.write(f"config load {Path(ioc_path).resolve()}\nproject generate\nexit\n")
        script = f.name
    try:
        p = subprocess.run([str(java), "-jar", str(jar), "-q", script], cwd=str(jar.parent), capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return False, f"CubeMX {timeout} s 没有结束：桌面上是否开着 CubeMX 界面（会抢锁）？关掉后重试", ""
    finally:
        os.unlink(script)
    tail = "\n".join((p.stdout + p.stderr).splitlines()[-15:])
    if p.returncode != 0:
        return False, f"CubeMX 退出码 {p.returncode}", tail
    return True, "CubeMX 已重新生成工程（看一下 git status：链接脚本行尾、Drivers/CMSIS/DSP 等）", tail
