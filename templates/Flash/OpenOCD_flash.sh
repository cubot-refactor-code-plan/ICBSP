#!/usr/bin/env bash
# ------------------------------------------------------------------
# OpenOCD + CMSIS-DAP（DAPLink）烧录脚本（Linux）
#
#   - 自动定位工程根目录，递归在 build/ 下找 .elf（不用传路径）
#   - 按构建目录 CMakeCache.txt 里的 ICBSP_FLASH_LAYOUT 决定烧法（外置 Flash 方案，见 docs/接入指南.md）：
#       INTERNAL  烧 .elf 到片内 Flash（daplink.cfg）
#       QSPI_RAM  方案 A：片内烧 bootloader（与板上不同时才烧），QSPI 烧 <工程名>_qspi.bin（daplink_qspi.cfg）
#       QSPI_XIP  方案 B：同上
#   - 对应 VS Code 任务：OpenOCD_flash（.vscode/tasks.json）
#
# 用法：bash Flash/OpenOCD_flash.sh [预设名] [--boot] [--ram]
#   预设名      build/<预设名>/ 下的构建，例如 Debug、Debug-QspiRam；省略时依次尝试 {{PRESET}}、Debug、Release，
#               都没有就在 build/ 下递归找，找到多个会提示实际用的是哪个
#   --boot      外置方案：不比较，强制重烧 bootloader
#   --ram       只用于 QSPI_RAM：把程序直接下载进 AXI SRAM 运行，不写任何 Flash（调试迭代用，掉电即失）
# ------------------------------------------------------------------
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_DIR"

PROJECT_NAME="{{PROJECT}}"
[ -n "$PROJECT_NAME" ] || PROJECT_NAME="$(basename "$PROJECT_DIR")"
BUILD_TYPE="{{PRESET}}"
FORCE_BOOT=0
RAM_ONLY=0
for arg in "$@"; do
    case "$arg" in
        --boot) FORCE_BOOT=1 ;;
        --ram) RAM_ONLY=1 ;;
        -h|--help) sed -n '2,20p' "${BASH_SOURCE[0]}"; exit 0 ;;
        -*) echo "❌ 不认识的参数 $arg（可用 --boot / --ram）"; exit 1 ;;
        *) BUILD_TYPE="$arg" ;;
    esac
done

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
    all="$(find build -type f -name '*.elf' -not -path '*/icbsp_boot/*' 2>/dev/null | sort || true)"
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
BUILD_DIR="$(dirname "$ELF_FILE")"
LAYOUT="INTERNAL"
if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
    LAYOUT="$(sed -n 's/^ICBSP_FLASH_LAYOUT:[A-Z]*=//p' "$BUILD_DIR/CMakeCache.txt" | head -n 1)"
    [ -n "$LAYOUT" ] || LAYOUT="INTERNAL"
fi

# OpenOCD 建议 0.12 及以上（实测用的是 0.12；0.11 没验证过）。系统包管理器装的常常是 0.11，
# 另装了新版时用环境变量指定：OPENOCD=/usr/local/bin/openocd bash Flash/OpenOCD_flash.sh
OPENOCD="${OPENOCD:-openocd}"
if ! command -v "$OPENOCD" >/dev/null 2>&1; then
    echo "❌ 未找到 openocd，请先安装：sudo apt install openocd"
    exit 1
fi
OPENOCD_VER="$("$OPENOCD" --version 2>&1 | head -n 1 | grep -oE '[0-9]+\.[0-9]+' | head -n 1)"
case "$OPENOCD_VER" in
    0.[0-9]|0.1[01]) echo "⚠️  $OPENOCD 是 $OPENOCD_VER，建议用 0.12 及以上（用 OPENOCD=<路径> 指定新版）" ;;
esac

echo "🔧 项目：$PROJECT_NAME"
echo "📦 ELF ：$ELF_FILE"
echo "🗂  程序存放方式：$LAYOUT"

QUIET=(-c "gdb_port disabled" -c "tcl_port disabled" -c "telnet_port disabled")

if [ "$LAYOUT" = "INTERNAL" ]; then
    if [ "$RAM_ONLY" = 1 ]; then
        echo "❌ --ram 只用于 QSPI_RAM（方案 A）"
        exit 1
    fi
    echo "⚡ OpenOCD 烧录中（CMSIS-DAP，片内 Flash）..."
    "$OPENOCD" -f "$SCRIPT_DIR/daplink.cfg" "${QUIET[@]}" \
        -c "program \"$ELF_FILE\" verify reset exit"
    echo "✅ 烧录完成"
    exit 0
fi

if [ ! -f "$SCRIPT_DIR/daplink_qspi.cfg" ]; then
    echo "❌ 没有 Flash/daplink_qspi.cfg：用 integrate.py / Web UI 打开“外置 Flash”生成"
    exit 1
fi

if [ "$RAM_ONLY" = 1 ]; then
    if [ "$LAYOUT" != "QSPI_RAM" ]; then
        echo "❌ --ram 只用于 QSPI_RAM（方案 A），当前是 $LAYOUT"
        exit 1
    fi
    echo "⚡ 下载进 AXI SRAM 运行（不写 Flash，掉电即失）..."
    # 复位停住（bootloader 还没跑），下载 ELF，按 0x24000000 的向量表设栈和入口后运行。
    # 不做 verify_image：ld 把 ELF 头映射进了 0x20000000 的段（只有 .bss），OpenOCD 校验它会误报不一致
    "$OPENOCD" -f "$SCRIPT_DIR/daplink.cfg" "${QUIET[@]}" \
        -c "init" -c "reset halt" \
        -c "load_image \"$ELF_FILE\"" \
        -c "set sp [read_memory 0x24000000 32 1]; set pc [read_memory 0x24000004 32 1]" \
        -c "reg msp \$sp; reg xpsr 0x01000000; resume [expr {\$pc & ~1}]" \
        -c "shutdown"
    echo "✅ 已在 RAM 中运行"
    exit 0
fi

BOOT_ELF="$BUILD_DIR/icbsp_boot/icbsp_boot.elf"
IMAGE="${ELF_FILE%.elf}_qspi.bin"
for f in "$BOOT_ELF" "$IMAGE"; do
    if [ ! -f "$f" ]; then
        echo "❌ 找不到 $f，请先编译（外置 Flash 方案会同时生成 bootloader 和 QSPI 镜像）"
        exit 1
    fi
done
echo "📦 boot：$BOOT_ELF"
echo "📦 镜像：$IMAGE"
echo "⚡ OpenOCD 烧录中（CMSIS-DAP，bootloader → 片内 Flash，镜像 → QSPI 0x90000000）..."

# bootloader 与板上一致就不重烧（片内 Flash 只有一个 128 KB 扇区，每次擦写都要几秒）
BOOT_CMD="if {[catch {verify_image \"$BOOT_ELF\"}]} { echo \"bootloader 与板上不同，烧录\"; flash write_image erase \"$BOOT_ELF\"; verify_image \"$BOOT_ELF\" } else { echo \"bootloader 未变，跳过\" }"
if [ "$FORCE_BOOT" = 1 ]; then
    BOOT_CMD="flash write_image erase \"$BOOT_ELF\"; verify_image \"$BOOT_ELF\""
fi
"$OPENOCD" -f "$SCRIPT_DIR/daplink_qspi.cfg" "${QUIET[@]}" \
    -c "init" -c "reset init" \
    -c "$BOOT_CMD" \
    -c "flash write_image erase \"$IMAGE\" 0x90000000 bin" \
    -c "verify_image \"$IMAGE\" 0x90000000 bin" \
    -c "reset run" -c "shutdown"

echo "✅ 烧录完成"
