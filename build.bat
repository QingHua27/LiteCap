@echo off
REM ============================================================
REM  LiteCap C++ 版构建脚本
REM  依赖：Visual Studio 的「使用 C++ 的桌面开发」工作负载
REM  用法：直接双击运行，或在任意命令行里执行 build.bat
REM ============================================================
setlocal enabledelayedexpansion
cd /d "%~dp0"

if not exist build mkdir build

REM ---- 1) 准备 VC 编译环境 ----
where cl.exe >nul 2>nul
if not errorlevel 1 goto :have_env

set "VCVARS="

REM 1a) 先扫常见安装路径。
REM     VS 的 Insiders / Preview 版常常没有注册到 vswhere，官方接口查不到，
REM     所以这里不能只依赖 vswhere。
for %%d in (
    "%ProgramFiles%\Microsoft Visual Studio\18\Insiders"
    "%ProgramFiles%\Microsoft Visual Studio\18\Preview"
    "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise"
    "%ProgramFiles%\Microsoft Visual Studio\2022\Professional"
    "%ProgramFiles%\Microsoft Visual Studio\2022\Community"
    "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools"
    "%ProgramFiles%\Microsoft Visual Studio\2022\Preview"
    "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Enterprise"
    "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Professional"
    "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Community"
    "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\BuildTools"
) do (
    if not defined VCVARS if exist "%%~d\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%~d\VC\Auxiliary\Build\vcvars64.bat"
)

REM 1b) 路径没扫到，再试官方 vswhere
if not defined VCVARS (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if exist "!VSWHERE!" (
        for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
        if defined VSPATH if exist "!VSPATH!\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=!VSPATH!\VC\Auxiliary\Build\vcvars64.bat"
    )
)

if not defined VCVARS (
    echo [x] 没有找到 Visual C++ 生成工具。
    echo     请安装 Visual Studio 的「使用 C++ 的桌面开发」工作负载，
    echo     或者改在「x64 Native Tools Command Prompt」里运行本脚本。
    exit /b 1
)

echo [*] 使用 VC 环境: !VCVARS!
call "!VCVARS!" >nul

:have_env

REM ---- 2) 确保 rc.exe 可用 ----
REM 嵌 manifest 和编图标都要用到资源编译器。vcvars64.bat 正常情况下会把它加进 PATH，
REM 但在某些环境（VS Insiders、精简安装、受限权限）下会漏掉，这里兜底找一次。
where rc.exe >nul 2>nul
if errorlevel 1 (
    set "SDKBIN="
    for /d %%v in ("%ProgramFiles(x86)%\Windows Kits\10\bin\10.*") do (
        if not defined SDKBIN if exist "%%~v\x64\rc.exe" set "SDKBIN=%%~v\x64"
    )
    if not defined SDKBIN if exist "%ProgramFiles(x86)%\Windows Kits\10\bin\x64\rc.exe" set "SDKBIN=%ProgramFiles(x86)%\Windows Kits\10\bin\x64"
    if defined SDKBIN (
        set "PATH=!SDKBIN!;!PATH!"
        echo [i] 已手动加入 SDK 工具目录: !SDKBIN!
    ) else (
        echo [!] 未找到 rc.exe，将跳过图标（程序仍可编译，只是没有自定义图标）。
    )
)

REM ---- 3) 图标资源（缺图标也能编）----
set "RES="
if exist res\app.ico (
    where rc.exe >nul 2>nul
    if not errorlevel 1 (
        echo [*] 编译图标资源...
        rc.exe /nologo /fo build\app.res res\app.rc
        if errorlevel 1 (
            echo [x] 资源编译失败。
            exit /b 1
        )
        set "RES=build\app.res"
    )
)

REM ---- 4) 编译链接 ----
REM /MT             = 静态链接运行库，生成的 exe 不依赖 VC++ Redistributable，拷走就能跑
REM /utf-8          = 源码含中文注释，不加会按 GBK 解读
REM /MANIFEST:EMBED = 把 comctl32 v6 manifest 嵌进 exe。必须加，否则控件退回经典外观，
REM                    且 CB_SETMINVISIBLE 等 v6 消息会失败（下拉列表展开后什么都看不到）
echo [*] 正在编译 LiteCap...
cl.exe /nologo /EHsc /std:c++17 /MT /O2 /utf-8 /DUNICODE /D_UNICODE ^
    /I src ^
    src\common.cpp src\recorder.cpp src\history.cpp src\player.cpp src\main.cpp %RES% ^
    /Fo:build\ /Fe:LiteCap.exe ^
    /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED ^
    mfplay.lib mfplat.lib mfreadwrite.lib mfuuid.lib ^
    ole32.lib oleaut32.lib user32.lib gdi32.lib comctl32.lib ^
    shlwapi.lib shell32.lib winmm.lib

if errorlevel 1 (
    echo.
    echo [x] 编译失败。
    exit /b 1
)

echo.
echo [ok] 构建完成: %cd%\LiteCap.exe
echo [i] 运行前请确认 lang\ 与 exe 在同一目录（语言包）。
endlocal
