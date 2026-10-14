# 更新日志

本项目遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

## [1.1.0] - 2026-10-15

首个版本。

### C++ 版（主推）

纯 Win32 + Media Foundation 实现，单文件约 415 KiB，静态链接运行库，无需安装。

**录制**

- 录制目标：全屏 / 指定显示器 / 自定义区域 / 指定窗口；窗口走 Windows Graphics Capture，遮挡时仍能录目标窗口
- 帧率 10 / 15 / 20 / 24 / 30 / 60 / 120 fps
- 画质 高 / 中 / 低，码率随分辨率与帧率自适应
- 编码器可选自动 / 硬件 / 软件：自动优先硬件并回退；按钮显示本次实际使用的 GPU 或 CPU 编码
- 音频可独立开关系统声音与麦克风；默认系统声音开、麦克风关，两项设置记忆；WASAPI 采集并混合为 AAC-LC
- 默认输出 `D:\Recordings`，D 盘不存在时退回用户「视频」目录
- 输出 H.264 / MP4

**界面**

- 录制记录列表：文件名 / 时长 / 大小，列表可收起并在展开时显示于右侧
- 软件内播放：双击列表项直接播放，支持播放暂停、进度条拖拽、`Esc` 关闭
- 右键菜单：在软件中播放 / 打开所在目录 / 复制完整路径 / 从列表移除 / 删除文件
- 悬浮窗模式：圆角置顶小条，实时计时，位置记忆
- 中英双语，`lang/` 下加文件即加语言
- 关闭快捷键录制窗口时正确恢复主窗口输入，避免 UI 锁死无法操作或退出
- 程序图标：深色圆角方块 + 白色屏幕描边 + 红色录制点，9 个尺寸（16~256）

### Python 版（存档）

PySide6 + mss + PyAV + numpy 实现，源码位于 `python/`。功能与 C++ 版基本一致，
另附 `--self-check` 离线自检。打包后体积 84 MB。

### 实现要点

- `MFVideoFormat_ARGB32` 的输入缓冲区按自上而下排列，GDI 与 WGC 帧原样复制给 SinkWriter，避免画面倒置
- 自动编码器模式优先硬件，120fps 等高帧率下自动切换软件；硬件 / 软件也可在界面强制选择
- 窗口捕获使用 Windows.Graphics.Capture + D3D11 staging texture；无边框窗口无法创建 WGC 会话时回退为 GDI 区域抓屏，并标示捕获模式
- WASAPI loopback / 默认麦克风以 QPC 时间戳对齐，混合为 48kHz AAC-LC 音轨；两路可独立开关
- SinkWriter 收到重复 PTS 会直接返回 `EINVAL`，加了单调递增保护
- Windows 默认调度精度约 15.6 ms，不足以支撑 60/120fps 的帧间隔。
  录制线程调用 `timeBeginPeriod(1)` 并把优先级提到 `ABOVE_NORMAL`，
  实测 120 fps 可达 **119.77 fps**
- 高帧率码率按 `sqrt(30 / fps)` 衰减，避免 120fps 码率相对 30fps 线性膨胀 4 倍。
  同一段画面实测 30fps 2.42 Mbps → 120fps 4.53 Mbps，仅涨 1.87 倍
- MFPlay 的 `GetDuration` 对部分文件返回 `0xC00D36D5`，改用 Source Reader
  读取容器 `MF_PD_DURATION`，进度条与总时长才准确
- 嵌入 comctl32 v6 manifest。缺它时控件退回经典外观，且 `CB_SETMINVISIBLE`
  会失败，表现为**下拉列表展开后高度为 0、什么都看不到**

### 说明

本程序不包含任何网络代码：没有 socket、没有 HTTP、没有更新检查、没有遥测。

[1.1.0]: https://github.com/QingHua27/LiteCap/releases/tag/v1.1.0
