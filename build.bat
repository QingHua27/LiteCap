@echo off
REM ============================================================
REM  LiteCap C++ 版构建脚本
REM  依赖：Visual Studio 2022/2026 生成工具（含 C++ 桌面开发）
REM  用法：在「x64 Native Tools Command Prompt」里运行 build.bat
REM        或在普通 cmd 里运行（脚本会自动找 vcvars64.bat）
REM ============================================================
setlocal enabledelayedexpansion
cd /d "%~dp0"

if not exist build mkdir build

REM ---- 若尚未进入 VS 环境，自动调用 vcvars64.bat ----
where cl.exe >nul 2>nul
if errorlevel 1 (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if not exist "!VSWHERE!" (
        echo [x] 找不到 vswhere.exe，请改用 "x64 Native Tools Command Prompt" 运行本脚本。
        exit /b 1
    )
    for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
    if not defined VSPATH (
        echo [x] 未检测到已安装的 Visual C++ 生成工具。
        exit /b 1
    )
    call "!VSPATH!\VC\Auxiliary\Build\vcvars64.bat" >nul
)

echo [*] 正在编译 LiteCap C++ 版...
REM /MT      = 静态链接运行库，生成的 exe 不依赖 VC++ Redistributable，拷走就能跑
REM /utf-8   = 源码含中文注释，不加会按 GBK 解读
REM MANIFEST:EMBED = 把 comctl32 v6 manifest 嵌进 exe（启用现代控件外观，
REM                  且 CB_SETMINVISIBLE 等 v6 消息才生效）
cl.exe /nologo /EHsc /std:c++17 /MT /O2 /utf-8 /DUNICODE /D_UNICODE ^
    /I src ^
    src\common.cpp src\recorder.cpp src\history.cpp src\player.cpp src\main.cpp ^
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
echo [ok] 构建完成： %cd%\LiteCap.exe
echo [i] 运行前请确认 lang\ 与 exe 在同一目录（语言包）。
endlocal
