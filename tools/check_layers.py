#!/usr/bin/env python3
"""
分层检查：code/ 下每个文件只能包含本层或更低层的库头文件。

    python3 tools/check_layers.py        # 有违规时退出码 1

层次（低 → 高）：algorithm、bsp → service → device → tasks。
例外：`service/system/status_code.hpp`（纯状态码，不依赖 RTOS / HAL）各层都能包含。
宿主文件（hardware_config.h、board_instances.hpp）和 HAL / FreeRTOS 头不在检查范围内。
"""

import pathlib
import re
import sys

CODE = pathlib.Path(__file__).resolve().parent.parent / "code"

# 每层允许包含的库目录
ALLOWED = {
    "algorithm": {"algorithm"},
    "bsp": {"bsp"},
    "service": {"service", "bsp", "algorithm"},
    "device": {"device", "service", "bsp", "algorithm"},
    "tasks": {"tasks", "service", "bsp"},
}
SHARED = {"service/system/status_code.hpp"}
INC = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)


def main() -> int:
    bad = []
    for path in sorted(CODE.rglob("*")):
        if path.suffix not in {".hpp", ".cpp", ".h"}:
            continue
        layer = path.relative_to(CODE).parts[0]
        for target in INC.findall(path.read_text(encoding="utf-8")):
            top = target.split("/", 1)[0]
            if top not in ALLOWED or target in SHARED:
                continue  # 宿主 / 第三方头文件，或公共状态码
            if top not in ALLOWED[layer]:
                bad.append(f"{path.relative_to(CODE)}: {layer}/ 不能包含 {target}")
            elif not (CODE / target).is_file():
                bad.append(f"{path.relative_to(CODE)}: 找不到 {target}")
    for line in bad:
        print(line)
    print(f"check_layers: {'OK' if not bad else f'{len(bad)} 处违规'}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
