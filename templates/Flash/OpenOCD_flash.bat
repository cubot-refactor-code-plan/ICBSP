@echo off
REM ------------------------------------------------------------------
REM OpenOCD + CMSIS-DAP（DAPLink）烧录脚本（Windows）
REM   自动递归在 build\ 下找 .elf，不用传路径
REM   按构建目录 CMakeCache.txt 的 ICBSP_FLASH_LAYOUT 决定烧法：
REM     INTERNAL  烧 .elf 到片内 Flash（daplink.cfg）
REM     QSPI_RAM / QSPI_XIP  片内烧 bootloader（与板上不同时才烧），QSPI 烧 <工程名>_qspi.bin（daplink_qspi.cfg）
REM   对应 VS Code 任务：OpenOCD_flash
REM 用法：Flash\OpenOCD_flash.bat [预设名] [--boot] [--ram]
REM   --boot  强制重烧 bootloader；--ram  只用于 QSPI_RAM：下载进 AXI SRAM 运行，不写 Flash
REM ------------------------------------------------------------------
setlocal enabledelayedexpansion

for %%i in ("%~dp0\..") do (
    set "PROJECT_DIR=%%~fi"
    set "FOLDER_NAME=%%~nxi"
)

set "PROJECT_NAME={{PROJECT}}"
if "%PROJECT_NAME%"=="" set "PROJECT_NAME=%FOLDER_NAME%"

set "BUILD_TYPE={{PRESET}}"
set "FORCE_BOOT=0"
set "RAM_ONLY=0"
:args
if "%~1"=="" goto :args_done
if /i "%~1"=="--boot" (set "FORCE_BOOT=1") else if /i "%~1"=="--ram" (set "RAM_ONLY=1") else (set "BUILD_TYPE=%~1")
shift
goto :args
:args_done

cd /d "%PROJECT_DIR%" || exit /b 1

set "ELF_FILE=build\%BUILD_TYPE%\%PROJECT_NAME%.elf"
if not exist "!ELF_FILE!" set "ELF_FILE=build\Debug\%PROJECT_NAME%.elf"
if not exist "!ELF_FILE!" set "ELF_FILE=build\Release\%PROJECT_NAME%.elf"
if not exist "!ELF_FILE!" (
    for /f "delims=" %%f in ('dir /b /s build\*.elf 2^>nul ^| findstr /v /i "icbsp_boot"') do (
        set "ELF_FILE=%%f"
        goto :found
    )
)
:found

if not exist "!ELF_FILE!" (
    echo Error: ELF not found under build\ - build first ^(task: Build^)
    exit /b 1
)
for %%d in ("!ELF_FILE!") do set "BUILD_DIR=%%~dpd"
set "LAYOUT=INTERNAL"
if exist "!BUILD_DIR!CMakeCache.txt" (
    for /f "tokens=2 delims==" %%l in ('findstr /b /c:"ICBSP_FLASH_LAYOUT:" "!BUILD_DIR!CMakeCache.txt"') do set "LAYOUT=%%l"
)

REM OpenOCD 建议 0.12 及以上（实测用 0.12）；另装了新版时用环境变量指定：set OPENOCD=C:\path\to\openocd.exe
if not defined OPENOCD set "OPENOCD=openocd"
where "!OPENOCD!" >nul 2>nul
if errorlevel 1 if not exist "!OPENOCD!" (
    echo Error: openocd not found. Install it, or add it to PATH.
    exit /b 1
)

echo Project: !PROJECT_NAME!
echo ELF    : !ELF_FILE!
echo Layout : !LAYOUT!

if /i "!LAYOUT!"=="INTERNAL" (
    if "!RAM_ONLY!"=="1" (
        echo Error: --ram is only for QSPI_RAM
        exit /b 1
    )
    echo Flashing via OpenOCD ^(CMSIS-DAP, internal flash^)...
    "!OPENOCD!" -f Flash/daplink.cfg ^
        -c "gdb_port disabled" -c "tcl_port disabled" -c "telnet_port disabled" ^
        -c "program \"!ELF_FILE:\=/!\" verify reset exit"
    if errorlevel 1 exit /b !errorlevel!
    echo Done.
    exit /b 0
)

if not exist "Flash\daplink_qspi.cfg" (
    echo Error: Flash\daplink_qspi.cfg missing - enable external flash in integrate.py / Web UI
    exit /b 1
)
set "ELF_FWD=!ELF_FILE:\=/!"

if "!RAM_ONLY!"=="1" (
    if /i not "!LAYOUT!"=="QSPI_RAM" (
        echo Error: --ram is only for QSPI_RAM, current layout is !LAYOUT!
        exit /b 1
    )
    echo Loading into AXI SRAM ^(no flash write^)...
    "!OPENOCD!" -f Flash/daplink.cfg ^
        -c "gdb_port disabled" -c "tcl_port disabled" -c "telnet_port disabled" ^
        -c "init" -c "reset halt" -c "load_image \"!ELF_FWD!\"" ^
        -c "set sp [read_memory 0x24000000 32 1]; set pc [read_memory 0x24000004 32 1]" ^
        -c "reg msp $sp; reg xpsr 0x01000000; resume [expr {$pc & ~1}]" -c "shutdown"
    if errorlevel 1 exit /b !errorlevel!
    echo Running from RAM.
    exit /b 0
)

set "BOOT_ELF=!BUILD_DIR!icbsp_boot\icbsp_boot.elf"
set "IMAGE=!ELF_FILE:~0,-4!_qspi.bin"
if not exist "!BOOT_ELF!" (
    echo Error: !BOOT_ELF! not found - build first
    exit /b 1
)
if not exist "!IMAGE!" (
    echo Error: !IMAGE! not found - build first
    exit /b 1
)
set "BOOT_FWD=!BOOT_ELF:\=/!"
set "IMAGE_FWD=!IMAGE:\=/!"
set "BOOT_CMD=if {[catch {verify_image \"!BOOT_FWD!\"}]} { flash write_image erase \"!BOOT_FWD!\"; verify_image \"!BOOT_FWD!\" }"
if "!FORCE_BOOT!"=="1" set "BOOT_CMD=flash write_image erase \"!BOOT_FWD!\"; verify_image \"!BOOT_FWD!\""

echo Boot   : !BOOT_ELF!
echo Image  : !IMAGE!
echo Flashing via OpenOCD ^(bootloader to internal flash, image to QSPI 0x90000000^)...
"!OPENOCD!" -f Flash/daplink_qspi.cfg ^
    -c "gdb_port disabled" -c "tcl_port disabled" -c "telnet_port disabled" ^
    -c "init" -c "reset init" -c "!BOOT_CMD!" ^
    -c "flash write_image erase \"!IMAGE_FWD!\" 0x90000000 bin" ^
    -c "verify_image \"!IMAGE_FWD!\" 0x90000000 bin" ^
    -c "reset run" -c "shutdown"
if errorlevel 1 exit /b !errorlevel!

echo Done.
exit /b 0
