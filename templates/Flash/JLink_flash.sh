#!/usr/bin/env bash
# ------------------------------------------------------------------
# J-Link 烧录脚本（Linux）—— 经 OpenOCD 的 jlink 适配器（libjaylink）驱动探针
#
#   - 自动定位工程根目录，递归在 build/ 下找 .elf（不用传路径）
#   - 对应 VS Code 任务：JLink_flash（.vscode/tasks.json）
#
# 用法：bash Flash/JLink_flash.sh [BUILD_TYPE]
# ------------------------------------------------------------------
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_DIR"

PROJECT_NAME="{{PROJECT}}"
[ -n "$PROJECT_NAME" ] || PROJECT_NAME="$(basename "$PROJECT_DIR")"
BUILD_TYPE="${1:-{{PRESET}}}"

find_elf() {
    local candidate all picked count
    for candidate in \
        "build/${BUILD_TYPE}/${PROJECT_NAME}.elf" \
        "build/Debug/${PROJECT_NAME}.elf" \
        "build/Release/${PROJECT_NAME}.elf"; do
        if [ -f "$candidate" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    all="$(find build -type f -name '*.elf' 2>/dev/null | sort || true)"
    [ -n "$all" ] || return 1
    picked="$(printf '%s\n' "$all" | grep -F "/${PROJECT_NAME}.elf" | head -n 1 || true)"
    [ -n "$picked" ] || picked="$(printf '%s\n' "$all" | head -n 1)"
    count="$(printf '%s\n' "$all" | wc -l)"
    if [ "$count" -gt 1 ]; then
        echo "⚠️  build/ 下有 $count 个 .elf，用 $picked（其余：）" >&2
        printf '%s\n' "$all" | sed 's/^/     /' >&2
    fi
    printf '%s\n' "$picked"
}

ELF_FILE="$(find_elf || true)"
if [ -z "$ELF_FILE" ] || [ ! -f "$ELF_FILE" ]; then
    echo "❌ 找不到 .elf，请先编译（任务：Build）"
    exit 1
fi
ELF_FILE="$(realpath "$ELF_FILE")"

if ! command -v openocd >/dev/null 2>&1; then
    echo "❌ 未找到 openocd；本脚本通过 OpenOCD 的 jlink 适配器驱动 J-Link"
    exit 1
fi

echo "🔧 项目：$PROJECT_NAME（$BUILD_TYPE）"
echo "📦 ELF ：$ELF_FILE"
echo "⚡ 经 OpenOCD jlink 适配器烧录中（SWD 4 MHz）..."

openocd -f "$SCRIPT_DIR/jlink.cfg" \
        -c "gdb_port disabled" \
        -c "tcl_port disabled" \
        -c "telnet_port disabled" \
        -c "program \"$ELF_FILE\" verify reset exit"

echo "✅ 烧录完成"
