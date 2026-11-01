#!/usr/bin/env python3
"""一次性补丁 2：plan_vscode 结尾加「旧烧录任务」提醒。跑完即可删除。"""

import sys
from pathlib import Path

PATH = Path(__file__).resolve().parent / "integrate.py"

OLD = "\n\ndef plan_flash(host, plan, rep, info):\n"
NEW = '''
    legacy = ("OpenOCD_flash_linux", "OpenOCD_flash_win", "JLink_flash_linux", "JLink_flash_win")
    tasks = plan.new_files.get(host / ".vscode" / "tasks.json")
    if tasks is None:
        tf = plan.edits.get(host / ".vscode" / "tasks.json")
        tasks = tf.text if tf is not None else ((host / ".vscode" / "tasks.json").read_text(encoding="utf-8") if (host / ".vscode" / "tasks.json").exists() else "")
    if any(f'"{name}"' in tasks for name in legacy):
        rep.warn(step, "tasks.json 里还有旧的 *_linux / *_win 烧录任务：新的 OpenOCD_flash / JLink_flash 会按平台自动选 .sh 或 .bat，旧任务可以删掉")


def plan_flash(host, plan, rep, info):
'''


def main():
    raw = PATH.read_bytes()
    crlf = b"\r\n" in raw
    text = raw.decode("utf-8").replace("\r\n", "\n")
    if text.count(OLD) != 1:
        sys.exit(f"锚点异常：出现 {text.count(OLD)} 次")
    text = text.replace(OLD, NEW, 1)
    if text.count("legacy = (") != 1:
        sys.exit("已经补过了")
    out = text.replace("\n", "\r\n") if crlf else text
    PATH.write_bytes(out.encode("utf-8"))
    print("已加入旧任务提醒")
    return 0


if __name__ == "__main__":
    sys.exit(main())
