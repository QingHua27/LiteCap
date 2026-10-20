// recorder.h - 抓屏 + H.264 编码 + 录制线程
#pragma once
#include "common.h"
#include <atomic>
#include <cstdint>
#include <thread>

// 录制线程 -> UI 线程的消息
#define WM_REC_TICK  (WM_APP + 1)
#define WM_REC_DONE  (WM_APP + 2)
#define WM_REC_FAIL  (WM_APP + 3)

struct RecEvent {
    int         frames = 0;
    double      elapsed = 0.0;
    std::wstring path;
    std::wstring error;
    bool        fallback = false;   // 窗口捕获没建成，这次退回了 GDI 区域抓屏
};

// ---------------- GDI 抓屏 ----------------
class ScreenCapture {
public:
    bool Init(int x, int y, int w, int h);
    void Shutdown();
    bool Grab();
    const BYTE* Bits() const { return bits_; }
    int Width()  const { return w_; }
    int Height() const { return h_; }

private:
    HDC      screenDC_ = nullptr;
    HDC      memDC_    = nullptr;
    HBITMAP  bmp_      = nullptr;
    HGDIOBJ  oldBmp_   = nullptr;
    BITMAPINFO bmi_    = {};
    BYTE*    bits_     = nullptr;
    int      x_ = 0, y_ = 0, w_ = 0, h_ = 0;
};

// ---------------- MP4 (H.264) 编码 ----------------
// 编码器选择。Auto 优先硬件、失败回退软件；Software/Hardware 用于强制指定
// （settings.ini 里的 encoder 键，也方便对照测试）。
enum class EncMode { Auto, Hardware, Software };

class Mp4Encoder {
public:
    bool Open(const std::wstring& path, int w, int h, int fps, int bitrate,
              EncMode mode = EncMode::Auto, bool audio = false);
    bool WriteFrame(const BYTE* bgra, LONGLONG rtStart, LONGLONG rtDuration);
    bool WriteAudio(const int16_t* stereoPcm, DWORD frames,
                    LONGLONG rtStart, LONGLONG rtDuration);
    bool Close(); // True only when the MP4 was finalized successfully.

    // 这次实际用的是硬件编码器还是软件编码器（Open 成功之后才有意义）
    bool Hardware() const { return hardware_; }

private:
    bool OpenInternal(const std::wstring& path, int w, int h, int fps,
                      int bitrate, bool allowHardware, bool audio);

    struct Impl;
    Impl* impl_ = nullptr;
    bool  hardware_ = false;
};

// ---------------- 录制目标 ----------------
// 两种来源二选一：
//   Screen —— GDI 从屏幕 DC 抠一块矩形，会被别的窗口遮挡
//   Window —— WGC 直接订阅窗口自己的合成流，遮挡无关
// 尺寸也来自这里：Screen 用 region 算，Window 由窗口自己决定。
struct RecTarget {
    bool window = false;
    RECT region = {};
    HWND hwnd   = nullptr;
};

// ---------------- 录制线程 ----------------
class Recorder {
public:
    ~Recorder() { Stop(); }

    bool Start(HWND notify, const RecTarget& target, int fps, const std::string& quality,
               const std::wstring& outPath, bool systemAudio, bool microphoneAudio);
    void Stop();
    bool Running() const { return running_.load(); }
    const std::wstring& OutputPath() const { return outPath_; }

    // 本次录制是否用上了硬件编码器（未开始录制时无意义）
    bool LastHardware() const { return hw_; }

    // 本来选了「窗口」，但 WGC 建不起来，这次实际是 GDI 抓那块矩形。
    // 界面要据此提示用户：这种录法会把遮挡窗口也录进去。
    bool RegionFallback() const { return fallback_.load(); }

    // "auto" / "hardware" / "software"，来自 settings.ini 的 encoder 键
    void SetEncoderMode(const std::string& m) { encMode_ = m; }

private:
    void ThreadMain();

    std::thread       thread_;
    std::atomic<bool> stop_{ false };
    std::atomic<bool> running_{ false };
    std::atomic<bool> hw_{ false };
    std::atomic<bool> fallback_{ false };
    std::atomic<bool> systemAudio_{ false };
    std::atomic<bool> microphoneAudio_{ false };
    HWND              notify_ = nullptr;
    RecTarget         target_ = {};
    int               fps_ = 30;
    std::string       quality_ = "high";
    std::string       encMode_ = "auto";
    std::wstring      outPath_;
};

// 质量档 -> 码率系数 (bits per pixel per frame)
double QualityFactor(const std::string& quality);
