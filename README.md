# LiteCap

**极简离线录屏工具** —— 只做一件事：把屏幕录成 MP4。

没有账号、没有云同步、没有更新检查、没有遥测。整个程序不包含任何网络代码。

本仓库包含两个独立实现：

| 实现            | 位置                   | 说明                                          |
| ------------- | -------------------- | ------------------------------------------- |
| **C++ 版**（推荐） | 仓库根目录                | 纯 Win32 + Media Foundation，静态链接运行库，单文件约 415 KiB，无需安装 |
| Python 版（存档）  | [`python/`](python/) | PySide6 + PyAV，源码可直接跑，打包后 84 MB             |

<img src="res/app.ico" width="72" alt="LiteCap 图标">

---

## C++ 版

![主界面](docs/main_window.png)

### 功能

| 功能                                           | 说明                                      |
| -------------------------------------------- | --------------------------------------- |
| 全屏 / 指定显示器 / 自定义区域 / 指定窗口                   | 框选时背景轻度压暗、选区保持原亮度；支持多显示器与负坐标；窗口捕获可跟随 Flash / CEF 游戏的全屏渲染窗口 |
| 帧率 10 / 15 / 20 / 24 / 30 / 60 / **120** fps | 实测 120fps 能跑到 **119.8fps**              |
| 画质 高 / 中 / 低                                 | 码率随分辨率与帧率自适应，高帧率按 `sqrt` 衰减             |
| **编码器：自动 / 硬件 / 软件**                        | 自动优先硬件并回退；也可强制 GPU 或 CPU 编码，选项会记住       |
| **音频：系统声音 / 麦克风独立开关**                     | 默认录制系统声音、不录麦克风；两项可同时开启，设置会记住         |
| 输出到 `D:\Recordings`                          | D 盘不存在时自动退回用户「视频」目录                     |
| 录制记录列表                                       | 文件名 / 时长 / 大小；可收起，展开时位于右侧，存于 `history.tsv` |
| **软件内播放**                                    | 双击列表项直接播放，支持播放/暂停、进度条拖拽、`Esc` 关闭        |
| **右键菜单**                                     | 在软件中播放 · 打开所在目录 · 复制完整路径 · 从列表移除 · 删除文件 |
| 悬浮窗模式                                        | 圆角置顶小条，随时开始/停止，实时计时，位置记忆                |
| 自定义全局快捷键                                  | 支持 F8、字母、数字等单键及组合键，按一次开始、再按一次停止；Esc 取消设置，Backspace / Delete 清除 |
| 中英文界面                                        | 内置语言包 + `lang/*.ini` 覆盖，切换即时生效        |

<table>
<tr>
<td><img src="docs/main_recording.png" width="330" alt="录制中"></td>
<td><img src="docs/float_bar.png" width="330" alt="悬浮窗"></td>
</tr>
<tr>
<td><img src="docs/player.png" width="330" alt="软件内播放"></td>
<td><img src="docs/context_menu.png" width="330" alt="右键菜单"></td>
</tr>
</table>

### 编译

需要 Visual Studio 2022/2026 的 **C++ 桌面开发** 工作负载（或独立生成工具）。

```bat
build.bat
```

脚本会自动通过 `vswhere` 定位并调用 `vcvars64.bat`，产物为 `LiteCap.exe`。

<details>
<summary>手动编译命令</summary>

```bat
rc /nologo /fo build\app.res res\app.rc
cl /nologo /EHsc /std:c++20 /MT /O2 /utf-8 /DUNICODE /D_UNICODE ^
   /I src src\common.cpp src\recorder.cpp src\history.cpp src\player.cpp src\main.cpp src\wincap.cpp build\app.res ^
   /Fo:build\ /Fe:LiteCap.exe ^
   /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED ^
   mfplay.lib mfplat.lib mfreadwrite.lib mfuuid.lib ^
   ole32.lib oleaut32.lib user32.lib gdi32.lib comctl32.lib ^
   shlwapi.lib shell32.lib winmm.lib windowsapp.lib d3d11.lib dxgi.lib dwmapi.lib
```

- `/utf-8` —— 源码含中文注释，不加会被按 GBK 解读。
- `/MT` —— 静态链接运行库，exe 不依赖 VC++ Redistributable。
- `/MANIFEST:EMBED` —— 嵌入 comctl32 v6 manifest。**必须加**，否则控件退回经典外观，
  且 `CB_SETMINVISIBLE` 等 v6 消息会失败（表现为下拉列表展开后高度为 0、什么都看不到）。

> 手动编译时若报 `LNK1158: 无法运行 rc.exe`，说明 SDK 的 `bin\<ver>\x64` 不在 PATH 里
> （`/MANIFEST:EMBED` 会调用资源编译器）。用 `build.bat` 则不存在这个问题。

</details>

依赖的 DLL 全部是系统自带的：包括 `COMCTL32 / GDI32 / KERNEL32 / MFPlat / MFPlay / MFReadWrite / SHELL32 / USER32 / ole32`，以及 WGC 所需的 `d3d11 / dxgi / dwmapi / windowsapp`。

### 实现要点

窗口录制支持坦克风暴等 Flash / CEF 游戏切换全屏。录像尺寸在开始时确定；录制中进入/退出全屏或改变窗口大小时，按原始比例缩放到固定画布，必要时留黑边。希望以全屏分辨率输出时，可先将游戏全屏再开始录制。

#### 录制管线

```text
桌面 / 显示器 / 自定义区域 ── GDI BitBlt ── DIB Section (32bpp, 自上而下) ─┐
                                                                              ├─ 原样拷贝 BGRA
指定窗口 ── Windows.Graphics.Capture ── D3D11 staging texture (自上而下) ──────┘
        ↓
IMFSinkWriter  →  H.264 视频 ──────────────┐
                                            ├─ MP4
WASAPI loopback / microphone → 48kHz stereo PCM → AAC 音频 ─┘
```

GDI 与 WGC 两条输入都提供自上而下的 BGRA，编码输入显式声明正步幅，让软件与硬件转换路径按相同方向读取。帧数据直接复制给 SinkWriter；逐行倒序
会让成片上下颠倒。方向用四色块靶子录制，再由 FFmpeg 独立解码复核，避免拿同属
Media Foundation 的内置播放器自我验证。

#### 编码器选择

主界面的「编码器」下拉框提供三种模式，设置保存在 `settings.ini` 的 `encoder` 键：

| 选项 | 配置值 | 行为 |
|---|---|---|
| 自动（默认） | `auto` | 优先尝试硬件编码；无法启动时回退软件。超过 60fps 自动走软件，避免硬件编码跟不上 |
| 硬件编码 | `hardware` | 强制尝试 GPU 编码；失败时报告错误，不静默回退 |
| 软件编码 | `software` | 强制使用 CPU 编码，适用于硬件 MFT 不稳定或需要复现对照时 |

录制按钮会显示本次实际使用的「GPU 编码」或「CPU 编码」，所以自动模式是否成功启用硬件一眼可见。

#### 音频来源

主界面可独立勾选「录制系统声音」和「录制麦克风」，两项都可以同时开或同时关：

- 默认值：系统声音开启、麦克风关闭。
- 系统声音使用 WASAPI loopback 采集默认播放设备；麦克风使用 WASAPI 默认录音输入设备。
- 选择会立即写入 `settings.ini`（`audio_system=1|0`、`audio_microphone=1|0`），下次启动恢复上次状态。
- 勾选的音源按 WASAPI 时间戳对齐并混合成一个 AAC-LC 立体声音轨（48kHz）；两项都关闭时仅录制视频。
- 设备不可用或 Windows 隐私设置禁止麦克风时会提示错误及 HRESULT；请检查默认设备或麦克风权限。

#### 帧率精度

Windows 默认调度精度约 **15.6 ms**，而 60fps 的帧间隔是 16.7ms、120fps 只有 8.3ms。
不调用 `timeBeginPeriod(1)` 根本追不上目标帧率。录制线程进入时申请、退出时归还，
并把线程优先级提到 `THREAD_PRIORITY_ABOVE_NORMAL`。

实测（1920×1080）：

| 档位      | 标称             | 实测             | 码率（同一段画面）  |
| ------- | -------------- | -------------- | ---------- |
| 30 fps  | 30/1           | 29.92 fps      | 2.42 Mbps  |
| 120 fps | 10000000/83333 | **119.77 fps** | 4.53 Mbps  |

#### 时间戳必须严格递增

SinkWriter 收到重复 PTS 会直接返回 `EINVAL` 并中断录制。因为抓屏耗时抖动，
按墙钟算出的 PTS 可能重复，所以加了单调递增保护：

```cpp
if (pts <= lastPts) pts = lastPts + 1;
lastPts = pts;
```

#### 码率自适应

```
码率 = 宽 × 高 × 帧率 × 每像素比特数
fps > 30 时，系数再乘 sqrt(30 / fps)
```

高帧率下相邻帧高度相似，不需要线性增长的码率。120fps 时系数降到 0.5。

绝对码率随画面内容波动很大（静态桌面可以低到 1.5 Mbps，满屏滚动能到 8 Mbps），
所以看**倍数**才有意义。同一段画面实测：30fps 2.42 Mbps → 120fps 4.53 Mbps，
只涨了 **1.87 倍**，而线性推算应该是 4 倍。

#### 播放器时长

MFPlay 的 `IMFPMediaPlayer::GetDuration` / `IMFPMediaItem::GetDuration`
对部分文件返回 `0xC00D36D5` 拿不到时长（播放本身正常）。改用 Source Reader
直接读容器的 `MF_PD_DURATION`，进度条与总时长才准确。

### 目录结构

```
├─ LiteCap.exe          编译产物
├─ build.bat            一键构建
├─ settings.ini         运行期配置（自动生成）
├─ history.tsv          录制记录（自动生成）
├─ lang/
│  ├─ zh-CN.ini         简体中文（同时作为内置语言包编进 exe）
│  └─ en-US.ini         English（同上）
├─ res/
│  ├─ app.ico           程序图标（9 个尺寸，16~256）
│  └─ app.rc            图标 + 内置语言包资源脚本
├─ src/
│  ├─ common.h/.cpp     字符串 / 路径 / 格式化 / 配置 / 本地化 / 图标加载
│  ├─ recorder.h/.cpp   GDI / WGC 抓屏 + WASAPI 音频 + H.264/AAC 编码与录制线程
│  ├─ wincap.h/.cpp     窗口枚举与 Windows.Graphics.Capture 实现
│  ├─ history.h/.cpp    录制记录（TSV 持久化）
│  ├─ player.h/.cpp     软件内播放（Media Foundation MFPlay）
│  └─ main.cpp          主窗口 / 悬浮窗 / 区域框选 / 右键菜单
├─ _probe/
│  └─ fps_probe.cpp     读 MP4 真实参数的辅助工具
├─ _test/               自动化端到端测试脚本
├─ _tools/
│  ├─ make_icon.py      生成 res/app.ico
│  └─ verify_icon.py    从 exe 抽图标做验证
├─ docs/                界面截图
└─ python/              Python 版存档
```

### 图标

`res/app.ico` 由 `_tools/make_icon.py` 生成，**不依赖 Pillow** —— 在高分辨率画布上做
几何判定，再降采样得到抗锯齿，最后打包成多尺寸 ICO：

```bash
python _tools/make_icon.py --preview   # 生成 ico + 各尺寸预览图
```

设计是深色圆角方块 + 白色「屏幕」描边 + 中心红色录制点，配色与程序内的悬浮窗
（`#1A1C20`）和录制按钮（`#D63B3B`）一致。小尺寸（≤24px）会收细描边、缩小圆点，
否则 16px 下红点会贴住白框糊成一团。

窗口图标在 `common.cpp` 的 `LoadAppIcon` 里按实际尺寸加载（大图标给 Alt+Tab 与任务栏，
小图标给标题栏）。不按尺寸加载的话系统会把 32×32 硬缩到 16×16，边缘会糊。


### 自动化测试

`_test/` 下的脚本可在程序运行时驱动它做端到端验证（纯 ctypes，无第三方依赖）：

```bash
python _test/e2e.py         # 主流程：录制 → 悬浮窗 → 停止 → 双击播放 → 右键菜单
python _test/fps_test.py    # 帧率专项，默认 120fps：选档 → 录制 → 读真实帧率
python _test/audio_test.py  # WASAPI 默认值 / 持久化 / 系统声与麦克风 AAC 音轨
python _test/hotkey_close_test.py # 关闭快捷键录制窗后，主窗口可操作并可退出
python _test/single_key_hotkey_test.py # 单键设置、持久化、全局注册与录制触发（独立副本）
python _test/fps_test.py 4  # 指定档位索引（0=10 1=15 2=20 3=24 4=30 5=60 6=120）
python _test/combo_check.py # 检查各下拉列表展开高度
python _test/diag.py        # 窗口诊断
```

原生回归测试可在已初始化 MSVC x64 环境的命令提示符中编译运行（先创建 `build/`）：

```bat
cl /nologo /EHsc /std:c++20 /MT /O2 /utf-8 /I src src\wincap.cpp _test\fullscreen_capture_test.cpp /Fo:build\ /Fe:build\fullscreen_capture_test.exe /link ole32.lib user32.lib gdi32.lib dwmapi.lib
build\fullscreen_capture_test.exe
cl /nologo /EHsc /std:c++20 /MT /O2 /utf-8 /DUNICODE /D_UNICODE /I src src\common.cpp src\recorder.cpp src\history.cpp src\player.cpp src\wincap.cpp _test\region_picker_test.cpp build\app.res /Fo:build\ /Fe:build\region_picker_test.exe /link mfplay.lib mfplat.lib mfreadwrite.lib mfuuid.lib ole32.lib oleaut32.lib user32.lib gdi32.lib comctl32.lib shlwapi.lib shell32.lib winmm.lib dwmapi.lib
build\region_picker_test.exe
cl /nologo /EHsc /std:c++20 /MT /O2 /utf-8 /I src src\common.cpp src\recorder.cpp src\wincap.cpp _test\encoder_orientation_fixture.cpp /Fo:build\ /Fe:build\encoder_orientation_fixture.exe /link ole32.lib oleaut32.lib user32.lib gdi32.lib shell32.lib shlwapi.lib dwmapi.lib
python _test\encoder_orientation_test.py build\encoder_orientation_fixture.exe ffmpeg.exe build\encoder-orientation
```

全屏测试覆盖关联渲染窗口切换、退出全屏、调整尺寸和固定输出画布；框选测试覆盖背景可见性、选区亮度、坐标、取消及 GDI 资源清理。框选测试会短暂显示测试背景。

`_probe/fps_probe.cpp` 编译后可直接读 MP4：

```
$ fps_probe.exe D:\Recordings\LiteCap_20261015_043605.mp4
文件      : D:\Recordings\LiteCap_20261015_043605.mp4
分辨率    : 1920x1080
标称帧率  : 120.00 fps  (10000000/83333)
平均码率  : 4.53 Mbps
时长      : 4.98 秒
实际帧数  : 597
实测帧率  : 119.77 fps
```

<details>
<summary>测试踩到的坑（供参考）</summary>

- **`HWND` 在 ctypes 里必须显式声明 `restype`**，否则 64 位句柄被截断成 int32，
  `FindWindow` 永远返回 0。
- **`mouse_event` 已失效**，模拟鼠标必须用 `SendInput`。
- **`SendMessage` 合成的 `WM_LBUTTONDOWN` 不触发 `NM_DBLCLK`** ——
  ListView 的双击判定读的是消息队列的 `GetMessagePos`/`GetMessageTime`，合成消息拿不到。
- 测试进程要用 `AttachThreadInput` 绕过 Windows 前台锁定，才能把目标窗口提到最前。
- 后台启动的 GUI 进程会随 shell 命令结束被回收，**启动 + 测试 + 关闭要放在同一条命令里**。
- 抓弹出菜单不能用 `PrintWindow`（抓不到独立弹出窗口），要用屏幕 DC + `CAPTUREBLT`。

</details>

### 已知边界

- **音频设备使用 Windows 默认端点。** 系统声音与麦克风可分别开关；想录指定麦克风，请先在 Windows 声音设置中将它设为默认输入设备。
- **捕获后端按目标切换。** 指定窗口走 WGC，不受遮挡影响；桌面、显示器和自定义区域走 GDI。
  个别无边框窗口不满足 `CreateForWindow` 要求时会退回 GDI 区域捕获，并在录制按钮标明「区域模式」；
  这种回退会受遮挡影响。4K 高帧率也受限于 `BitBlt` 带宽，建议降帧率。
- **悬浮窗会被录进画面。** 这是屏幕录制的固有性质。想录出干净画面：把悬浮窗拖到录制
  区域之外，或切回主窗口操作。

---

## Python 版（存档）

见 [`python/`](python/)，保留完整源码与文档。

|                     | C++ 版      | Python 版                     |
| ------------------- | ---------- | ---------------------------- |
| 分发体积                | **415 KiB** | 84 MB（PyInstaller one-file）  |
| 运行依赖                | 无（静态链接）    | 无（已打包）                       |
| 启动速度                | 即时         | 需解压到临时目录，较慢                  |
| 录制记录 / 软件内播放 / 右键菜单 | ✅          | —                            |
| 悬浮窗模式               | ✅          | ✅                            |
| 中英双语                | ✅          | ✅                            |
| 依赖                  | MSVC（仅编译时） | PySide6 / mss / PyAV / numpy |

Python 版额外提供一个离线自检：

```
python python/LiteCap.py --self-check
```

它会扫描源码里是否存在网络相关导入并给出结论。

---

## 本地化

语言包分两层，**外置优先**：

| 层   | 位置                                              | 作用                        |
| --- | ----------------------------------------------- | ------------------------- |
| 内置  | 编译进 exe（`res/app.rc` 里的 RCDATA）                  | 保证只拿到一个 exe 也能正常显示界面      |
| 外置  | `lang/<code>.ini`（C++）/ `python/lang/<code>.json` | 覆盖内置的同名键，也可以新增语言          |

外置文件**不需要写全**：只写想改的那几行就够了，没提到的键自动沿用内置值。
新增语言时写个 `lang/ja-JP.ini` 丢进去，语言下拉框里立刻多一项 —— 不用重新编译。

`_name` 是下拉框里显示的语言名。键的查找顺序是：当前语言 → 英文 → 键名本身，不会出现空白。

> 内置这一层是必需的。发布版只给一个 exe，若文案全靠外置文件，
> 用户下载后看到的会是 `target_all` / `pick_region` / `col_name` 这类键名，等于不可用。

---

## 授权

本项目为独立实现，与任何商业录屏软件无代码或授权关系。
