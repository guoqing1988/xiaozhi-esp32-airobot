@echo off
chcp 65001 >nul
rem ============================================================
rem  MecanumRobot 编译/烧录脚本 (arduino-cli)
rem  用法:
rem    build_arduino.bat          仅编译
rem    build_arduino.bat COM5     编译 + 烧录到 COM5
rem ============================================================

setlocal
cd /d "%~dp0"

rem 定位 arduino-cli: 默认安装路径优先, 其次系统 PATH
set "CLI="
if exist "D:\Program Files\Arduino CLI\arduino-cli.exe" set "CLI=D:\Program Files\Arduino CLI\arduino-cli.exe"
if not defined CLI (where arduino-cli >nul 2>nul && set "CLI=arduino-cli")
if not defined CLI (
    echo [错误] 找不到 arduino-cli, 请检查安装或 PATH
    exit /b 1
)

rem 编译 (RX 缓冲 256B, 防 AI 长指令序列丢指令; 该宏必须在编译时传入)
echo [编译中] ...
"%CLI%" compile --fqbn arduino:avr:uno --build-property "compiler.cpp.extra_flags=-DSERIAL_RX_BUFFER_SIZE=256" "%~dp0MecanumRobot.ino"
if errorlevel 1 (
    echo [错误] 编译失败
    exit /b 1
)

rem 可选烧录
if not "%~1"=="" (
    echo [烧录中] 端口 %~1 ...
    "%CLI%" upload -p %~1 --fqbn arduino:avr:uno "%~dp0MecanumRobot.ino"
    if errorlevel 1 (
        echo [错误] 烧录失败
        exit /b 1
    )
    echo [完成] 烧录成功
) else (
    echo [完成] 编译成功
)
endlocal
