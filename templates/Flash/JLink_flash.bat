@echo off
REM ------------------------------------------------------------------
REM J-Link 烧录脚本（Windows）—— 经 OpenOCD 的 jlink 适配器驱动探针
REM   自动递归在 build\ 下找 .elf，不用传路径
REM   对应 VS Code 任务：JLink_flash
REM 用法：Flash\JLink_flash.bat [BUILD_TYPE]
REM ------------------------------------------------------------------
setlocal enabledelayedexpansion

for %%i in ("%~dp0\..") do (
    set "PROJECT_DIR=%%~fi"
    set "FOLDER_NAME=%%~nxi"
)

set "PROJECT_NAME={{PROJECT}}"
if "%PROJECT_NAME%"=="" set "PROJECT_NAME=%FOLDER_NAME%"

set "BUILD_TYPE=%~1"
if "%BUILD_TYPE%"=="" set "BUILD_TYPE={{PRESET}}"

cd /d "%PROJECT_DIR%" || exit /b 1

set "ELF_FILE=build\%BUILD_TYPE%\%PROJECT_NAME%.elf"
if not exist "!ELF_FILE!" set "ELF_FILE=build\Debug\%PROJECT_NAME%.elf"
if not exist "!ELF_FILE!" set "ELF_FILE=build\Release\%PROJECT_NAME%.elf"
if not exist "!ELF_FILE!" (
    for /f "delims=" %%f in ('dir /b /s build\*.elf 2^>nul') do (
        set "ELF_FILE=%%f"
        goto :found
    )
)
:found

if not exist "!ELF_FILE!" (
    echo Error: ELF not found under build\ - build first ^(task: Build^)
    exit /b 1
)

REM OpenOCD 建议 0.12 及以上（实测用 0.12）；另装了新版时用环境变量指定：set OPENOCD=C:\path\to\openocd.exe
if not defined OPENOCD set "OPENOCD=openocd"
where "!OPENOCD!" >nul 2>nul
if errorlevel 1 if not exist "!OPENOCD!" (
    echo Error: openocd not found; this script drives J-Link through OpenOCD's jlink adapter.
    exit /b 1
)

echo Project: !PROJECT_NAME! ^(!BUILD_TYPE!^)
echo ELF    : !ELF_FILE!
echo Flashing via OpenOCD jlink adapter ^(SWD 4 MHz^)...

"!OPENOCD!" -f Flash/jlink.cfg ^
    -c "gdb_port disabled" ^
    -c "tcl_port disabled" ^
    -c "telnet_port disabled" ^
    -c "program \"!ELF_FILE!\" verify reset exit"
if errorlevel 1 exit /b %errorlevel%

echo Done.
exit /b 0
