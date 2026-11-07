#!/usr/bin/env python3
"""
生成结果的编译测试（阶段 5.9）：对每个用例 tests/cases/*.json，
  1. 把用例的 .ioc 放进空目录，用 STM32CubeMX 命令行生成一个全新工程（从没接入过本库）；
  2. 把本库拷进 third_party/icbsp；
  3. integrate.py --apply <用例的选择> 从零完整接入（链接脚本、FreeRTOS、模板、CMake……）并生成板级代码；
  4. cmake --preset Debug && cmake --build --preset Debug，要求 0 error、0 warning。

    python3 tools/tests/compile_test.py [工作目录] [--cubemx <目录>] [--case <名字>]

需要：arm-none-eabi-gcc（PATH 或 /opt/arm-none-eabi/bin）、cmake、ninja、STM32CubeMX（桌面上别开着它）。退出码 = 失败的用例数。
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
LIB = HERE.parent.parent
sys.path.insert(0, str(HERE.parent))
from icbsp_tool import cubemx  # noqa: E402


def sh(cmd, cwd, env=None):
    p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, env=env)
    return p.returncode, p.stdout + p.stderr


def run_case(case, work, cubemx_dir):
    c = json.loads(case.read_text(encoding="utf-8"))
    name = case.stem
    proj = work / name
    if proj.exists():
        shutil.rmtree(proj)
    proj.mkdir(parents=True)
    ioc = proj / f"{name}.ioc"
    text = (HERE / "fixtures" / c["ioc"]).read_text(encoding="utf-8")
    text = re.sub(r"^ProjectManager\.ProjectName=.*$", f"ProjectManager.ProjectName={name}", text, flags=re.M)
    text = re.sub(r"^ProjectManager\.ProjectFileName=.*$", f"ProjectManager.ProjectFileName={name}.ioc", text, flags=re.M)
    ioc.write_text(text, encoding="utf-8")
    ok, msg, tail = cubemx.regenerate(ioc, cubemx_dir)
    if not ok or not (proj / "CMakeLists.txt").exists():
        return False, f"CubeMX 生成失败：{msg}\n{tail}"
    shutil.copytree(LIB, proj / "third_party" / "icbsp", ignore=shutil.ignore_patterns(".git", "build", "__pycache__", "tests"))
    sel = proj / "selection.json"
    sel.write_text(json.dumps(c["selection"], ensure_ascii=False), encoding="utf-8")
    code, out = sh([sys.executable, "third_party/icbsp/tools/integrate.py", "--host", str(proj), "--apply", str(sel),
                    "--no-vscode", "--no-flash", "--no-clang"], proj)
    if code != 0:
        return False, f"integrate.py 失败（{code}）：\n{out[-3000:]}"
    env = dict(os.environ, PATH="/opt/arm-none-eabi/bin:" + os.environ.get("PATH", ""))
    code, out = sh(["cmake", "--preset", "Debug"], proj, env)
    if code != 0:
        return False, f"cmake 配置失败：\n{out[-3000:]}"
    code, out = sh(["cmake", "--build", "--preset", "Debug"], proj, env)
    warn = sorted({l for l in out.splitlines() if re.search(r"\b(warning|error):", l)})
    if code != 0 or warn:
        return False, f"编译{'失败' if code else '有警告'}：\n" + "\n".join(warn[:30]) + ("" if warn else out[-3000:])
    flash = re.search(r"FLASH:\s+(\d+ B)", out)
    return True, f"0 error / 0 warning，FLASH {flash.group(1) if flash else '?'}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("work", nargs="?", type=Path, default=Path(tempfile.gettempdir()) / f"icbsp_compile_{os.getpid()}")
    ap.add_argument("--cubemx")
    ap.add_argument("--case")
    a = ap.parse_args()
    cases = sorted((HERE / "cases").glob("*.json"))
    if a.case:
        cases = [c for c in cases if c.stem == a.case]
    bad = 0
    for case in cases:
        ok, msg = run_case(case, a.work.resolve(), a.cubemx)
        print(f"{'PASS' if ok else 'FAIL'} {case.stem}: {msg}", flush=True)
        bad += 0 if ok else 1
    print(f"工作目录：{a.work}")
    return bad


if __name__ == "__main__":
    sys.exit(main())
