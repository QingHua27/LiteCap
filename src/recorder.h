// recorder.h - 抓屏 + H.264 编码 + 录制线程
#pragma once
#include "common.h"
#include <atomic>
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
class Mp4Encoder {
public:
    bool Open(const std::wstring& path, int w, int h, int fps, int bitrate);
    bool WriteFrame(const BYTE* bgra, LONGLONG rtStart, LONGLONG rtDuration);
    void Close();

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

// ---------------- 录制线程 ----------------
class Recorder {
public:
    ~Recorder() { Stop(); }

    bool Start(HWND notify, const RECT& region, int fps, const std::string& quality,
               const std::wstring& outPath);
    void Stop();
    bool Running() const { return running_.load(); }
    const std::wstring& OutputPath() const { return outPath_; }

private:
    void ThreadMain();

    std::thread       thread_;
    std::atomic<bool> stop_{ false };
    std::atomic<bool> running_{ false };
    HWND              notify_ = nullptr;
    RECT              region_ = {};
    int               fps_ = 30;
    std::string       quality_ = "high";
    std::wstring      outPath_;
};

// 质量档 -> 码率系数 (bits per pixel per frame)
double QualityFactor(const std::string& quality);
