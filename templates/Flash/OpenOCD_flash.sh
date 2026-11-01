#!/usr/bin/env bash
# ------------------------------------------------------------------
# OpenOCD + CMSIS-DAP（DAPLink）烧录脚本（Linux）
#
#   - 自动定位工程根目录，递归在 build/ 下找 .elf（不用传路径）
#   - 对应 VS Code 任务：OpenOCD_flash（.vscode/tasks.json）
#
# 用法：bash Flash/OpenOCD_flash.sh [BUILD_TYPE]
#   省略 BUILD_TYPE 时依次尝试 build/{{PRESET}}/<工程名>.elf、build/Debug、build/Release，
#   都没有就在 build/ 下递归找，找到多个会提示实际用的是哪个。
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

if ! command -v openocd >/dev/null 2>&1; then
    echo "❌ 未找到 openocd，请先安装：sudo apt install openocd"
    exit 1
fi

echo "🔧 项目：$PROJECT_NAME"
echo "📦 ELF ：$ELF_FILE"
echo "⚡ OpenOCD 烧录中（CMSIS-DAP）..."

openocd -f "$SCRIPT_DIR/daplink.cfg" \
        -c "gdb_port disabled" \
        -c "tcl_port disabled" \
        -c "telnet_port disabled" \
        -c "program \"$ELF_FILE\" verify reset exit"

echo "✅ 烧录完成"
