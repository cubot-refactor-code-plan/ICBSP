#!/usr/bin/env python3
"""一次性补丁：README 目录树换成 code/，文档表加 docs/code/ 与架构图；编辑器配置插入图1。跑完即可删除。"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

OLD_TREE = """ICBSP/
├─ bsp/          CAN、UART、SPI、PWM、GPIO、Key、Encoder、DWT
├─ service/      Status / EventState、Online、CAN 收发节点、fault_record、system
├─ tasks/        sys_task、can_rx_task、can_tx_task
├─ device/       DjiMotor、DeviceMotor、Servo、R9DS、Emm_V5、BMI088、Buzzer
├─ protocol/     emmv5_protocol（帧编解码）
├─ algorithm/    PID、QuaternionPID、VQF、Quaternion、Matrix、RLS
├─ templates/    宿主接入模板（board、app、链接脚本、FreeRTOS 钩子、.vscode、Flash）
├─ tools/        integrate.py：自动接入 CubeMX 工程
├─ docs/         接入指南、常用接口、编码规范、器件手册、历史测试记录
└─ CMakeLists.txt"""

NEW_TREE = """ICBSP/
├─ code/         库源码（分层见 docs/code/）
│  ├─ bsp/       CAN、UART、SPI、PWM、GPIO、Key、Encoder、DWT
│  ├─ service/   Status / EventState、Online、CAN 收发节点、fault_record、system
│  ├─ device/    DjiMotor、DeviceMotor、Servo、R9DS、Emm_V5、BMI088、Buzzer
│  ├─ protocol/  emmv5_protocol（帧编解码）
│  ├─ algorithm/ PID、QuaternionPID、VQF、Quaternion、Matrix、RLS
│  └─ tasks/     sys_task、can_rx_task、can_tx_task
├─ templates/    宿主接入模板（board、app、链接脚本、FreeRTOS 钩子、.vscode、Flash）
├─ tools/        integrate.py（自动接入）、draw_diagrams.py（画架构图）
├─ docs/         接入指南、常用接口、编辑器配置、代码层说明、架构图、器件手册
└─ CMakeLists.txt"""

OLD_DOC_ROW = "| [docs/常用接口.md](docs/常用接口.md)       | UART / CAN / 电机 / PID / 系统状态的使用示例                       |\n"
NEW_DOC_ROW = (
    "| [docs/code/](docs/code/)                   | 代码层索引：分层依赖图、启动顺序图、依赖矩阵、六层文档             |\n"
    "| [docs/常用接口.md](docs/常用接口.md)       | UART / CAN / 电机 / PID / 系统状态的使用示例                       |\n"
)

OLD_EDITOR_HEAD = """`integrate.py` 会把 `templates/vscode/` 的三个文件生成 / 合并进宿主 `.vscode/`：宿主没有就整体生成，
已经有了就只补缺失的键和任务，原有内容与注释都保留。本文说明每一项是干什么的，重点讲 outline++ 的分区正则。
"""
NEW_EDITOR_HEAD = """`integrate.py` 会把 `templates/vscode/` 的三个文件生成 / 合并进宿主 `.vscode/`：宿主没有就整体生成，
已经有了就只补缺失的键和任务，原有内容与注释都保留。本文说明每一项是干什么的，重点讲 outline++ 的分区正则。

![工具链关系](img/toolchain.png)

> 图里 ① 输入 → ② integrate.py → ③ `.vscode` / ④ `Flash/` → ⑤ 构建产物 的关系；
> 矢量版 [img/toolchain.svg](img/toolchain.svg)。
"""


def patch(name, pairs):
    path = ROOT / name
    raw = path.read_bytes()
    crlf = b"\r\n" in raw
    text = raw.decode("utf-8").replace("\r\n", "\n")
    for old, new in pairs:
        if text.count(old) != 1:
            sys.exit(f"{name}: 锚点出现 {text.count(old)} 次 -> {old[:50]!r}")
        text = text.replace(old, new, 1)
    path.write_bytes((text.replace("\n", "\r\n") if crlf else text).encode("utf-8"))
    print(f"已更新 {name}")


patch("README.md", [(OLD_TREE, NEW_TREE), (OLD_DOC_ROW, NEW_DOC_ROW)])
patch("docs/编辑器配置.md", [(OLD_EDITOR_HEAD, NEW_EDITOR_HEAD)])
