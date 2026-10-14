# 更新日志

本项目遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

## [1.0.0] - 2026-10-15

首个版本。

### C++ 版（主推）

纯 Win32 + Media Foundation 实现，单文件 325 KB，静态链接运行库，无需安装。

**录制**

- 录制目标：全屏 / 指定显示器 / 自定义区域（支持多显示器与负坐标）
- 帧率 10 / 15 / 20 / 24 / 30 / 60 / 120 fps
- 画质 高 / 中 / 低，码率随分辨率与帧率自适应
- 默认输出 `D:\Recordings`，D 盘不存在时退回用户「视频」目录
- 输出 H.264 / MP4

**界面**

- 录制记录列表：文件名 / 时长 / 大小 / 录制时间
- 软件内播放：双击列表项直接播放，支持播放暂停、进度条拖拽、`Esc` 关闭
- 右键菜单：在软件中播放 / 打开所在目录 / 复制完整路径 / 从列表移除 / 删除文件
- 悬浮窗模式：圆角置顶小条，实时计时，位置记忆
- 中英双语，`lang/` 下加文件即加语言

### Python 版（存档）

PySide6 + mss + PyAV + numpy 实现，源码位于 `python/`。功能与 C++ 版基本一致，
另附 `--self-check` 离线自检。打包后体积 84 MB。

### 实现要点

- `MFVideoFormat_ARGB32` 是 bottom-up 存储，写帧时需逐行倒序，否则画面上下翻转
- SinkWriter 收到重复 PTS 会直接返回 `EINVAL`，加了单调递增保护
- Windows 默认调度精度约 15.6 ms，不足以支撑 60/120fps 的帧间隔。
  录制线程调用 `timeBeginPeriod(1)` 并把优先级提到 `ABOVE_NORMAL`，
  实测 120 fps 可达 **119.62 fps**
- 高帧率码率按 `sqrt(30 / fps)` 衰减，避免 120fps 码率相对 30fps 线性膨胀 4 倍
- MFPlay 的 `GetDuration` 对部分文件返回 `0xC00D36D5`，改用 Source Reader
  读取容器 `MF_PD_DURATION`，进度条与总时长才准确
- 嵌入 comctl32 v6 manifest。缺它时控件退回经典外观，且 `CB_SETMINVISIBLE`
  会失败，表现为**下拉列表展开后高度为 0、什么都看不到**

### 说明

本程序不包含任何网络代码：没有 socket、没有 HTTP、没有更新检查、没有遥测。

[1.0.0]: https://github.com/QingHua27/LiteCap/releases/tag/v1.0.0
