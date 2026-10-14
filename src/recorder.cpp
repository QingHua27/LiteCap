// recorder.cpp
#include "recorder.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <mmsystem.h>
#include <chrono>
#include <cmath>
#include <cstring>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "winmm.lib")

double QualityFactor(const std::string& q) {
    if (q == "high")   return 0.12;
    if (q == "medium") return 0.07;
    return 0.04;   // low
}

// ================= GDI 抓屏 =================

bool ScreenCapture::Init(int x, int y, int w, int h) {
    Shutdown();
    x_ = x; y_ = y; w_ = w; h_ = h;

    screenDC_ = GetDC(nullptr);
    if (!screenDC_) return false;

    memDC_ = CreateCompatibleDC(screenDC_);
    if (!memDC_) { Shutdown(); return false; }

    bmi_.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi_.bmiHeader.biWidth       = w;
    bmi_.bmiHeader.biHeight      = -h;      // 负值 = 自上而下
    bmi_.bmiHeader.biPlanes      = 1;
    bmi_.bmiHeader.biBitCount    = 32;
    bmi_.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    bmp_ = CreateDIBSection(screenDC_, &bmi_, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp_ || !bits) { Shutdown(); return false; }
    bits_ = (BYTE*)bits;
    oldBmp_ = SelectObject(memDC_, bmp_);
    return true;
}

void ScreenCapture::Shutdown() {
    if (memDC_ && oldBmp_) SelectObject(memDC_, oldBmp_);
    oldBmp_ = nullptr;
    if (bmp_) { DeleteObject(bmp_); bmp_ = nullptr; }
    if (memDC_) { DeleteDC(memDC_); memDC_ = nullptr; }
    if (screenDC_) { ReleaseDC(nullptr, screenDC_); screenDC_ = nullptr; }
    bits_ = nullptr;
}

bool ScreenCapture::Grab() {
    if (!memDC_ || !screenDC_) return false;
    // CAPTUREBLT 让分层窗口（如某些悬浮提示）也能被抓到
    return BitBlt(memDC_, 0, 0, w_, h_, screenDC_, x_, y_, SRCCOPY | CAPTUREBLT) != FALSE;
}

// ================= MP4 / H.264 =================

struct Mp4Encoder::Impl {
    IMFSinkWriter* writer = nullptr;
    DWORD          stream = 0;
    int            w = 0, h = 0;
    bool           began = false;
};

template <class T> static void SafeRelease(T** p) {
    if (*p) { (*p)->Release(); *p = nullptr; }
}

bool Mp4Encoder::Open(const std::wstring& path, int w, int h, int fps, int bitrate) {
    Close();
    impl_ = new Impl();
    impl_->w = w; impl_->h = h;

    HRESULT hr = MFCreateSinkWriterFromURL(path.c_str(), nullptr, nullptr, &impl_->writer);
    if (FAILED(hr)) { Close(); return false; }

    IMFMediaType* outType = nullptr;
    hr = MFCreateMediaType(&outType);
    if (SUCCEEDED(hr)) hr = outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    if (SUCCEEDED(hr)) hr = outType->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)bitrate);
    if (SUCCEEDED(hr)) hr = outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = MFSetAttributeSize(outType, MF_MT_FRAME_SIZE, w, h);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(outType, MF_MT_FRAME_RATE, fps, 1);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(outType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(hr)) hr = impl_->writer->AddStream(outType, &impl_->stream);
    SafeRelease(&outType);

    IMFMediaType* inType = nullptr;
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&inType);
    if (SUCCEEDED(hr)) hr = inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32);
    if (SUCCEEDED(hr)) hr = inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = MFSetAttributeSize(inType, MF_MT_FRAME_SIZE, w, h);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(inType, MF_MT_FRAME_RATE, fps, 1);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(inType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(hr)) hr = impl_->writer->SetInputMediaType(impl_->stream, inType, nullptr);
    SafeRelease(&inType);

    if (SUCCEEDED(hr)) hr = impl_->writer->BeginWriting();
    if (FAILED(hr)) { Close(); return false; }
    impl_->began = true;
    return true;
}

bool Mp4Encoder::WriteFrame(const BYTE* bgra, LONGLONG rtStart, LONGLONG rtDuration) {
    if (!impl_ || !impl_->writer) return false;
    const int w = impl_->w, h = impl_->h;
    const DWORD bytes = (DWORD)w * h * 4;

    IMFMediaBuffer* buf = nullptr;
    if (FAILED(MFCreateMemoryBuffer(bytes, &buf))) return false;

    BYTE* dst = nullptr;
    DWORD maxLen = 0;
    if (FAILED(buf->Lock(&dst, &maxLen, nullptr))) { buf->Release(); return false; }
    if (maxLen < bytes) { buf->Unlock(); buf->Release(); return false; }

    // MFVideoFormat_ARGB32 要求自下而上存放，抓屏缓冲区是自上而下，逐行倒序
    const DWORD stride = (DWORD)w * 4;
    for (int y = 0; y < h; ++y)
        memcpy(dst + (size_t)y * stride, bgra + (size_t)(h - 1 - y) * stride, stride);

    buf->Unlock();
    buf->SetCurrentLength(bytes);

    IMFSample* sample = nullptr;
    if (FAILED(MFCreateSample(&sample))) { buf->Release(); return false; }
    sample->AddBuffer(buf);
    sample->SetSampleTime(rtStart);
    sample->SetSampleDuration(rtDuration);
    HRESULT hr = impl_->writer->WriteSample(impl_->stream, sample);
    sample->Release();
    buf->Release();
    return SUCCEEDED(hr);
}

void Mp4Encoder::Close() {
    if (!impl_) return;
    if (impl_->writer) {
        if (impl_->began) impl_->writer->Finalize();
        impl_->writer->Release();
        impl_->writer = nullptr;
    }
    delete impl_;
    impl_ = nullptr;
}

// ================= 录制线程 =================

bool Recorder::Start(HWND notify, const RECT& region, int fps, const std::string& quality,
                     const std::wstring& outPath) {
    if (running_.load()) return false;

    int w = region.right - region.left;
    int h = region.bottom - region.top;
    w -= w % 2;                     // H.264 要求偶数
    h -= h % 2;
    if (w < 16 || h < 16) return false;

    region_.left = region.left;
    region_.top  = region.top;
    region_.right  = region.left + w;
    region_.bottom = region.top + h;
    fps_ = fps < 1 ? 1 : (fps > 120 ? 120 : fps);
    quality_ = quality;
    outPath_ = outPath;
    notify_ = notify;
    stop_ = false;
    running_ = true;
    thread_ = std::thread(&Recorder::ThreadMain, this);
    return true;
}

void Recorder::Stop() {
    if (!running_.load()) return;
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    running_ = false;
}

void Recorder::ThreadMain() {
    using clock = std::chrono::steady_clock;

    // 关键：Windows 默认调度精度约 15.6ms，60/120fps 的帧间隔是 16.7/8.3ms，
    // 不提升计时器精度根本追不上。timeBeginPeriod 只影响本进程，退出时归还。
    timeBeginPeriod(1);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    const int w = region_.right - region_.left;
    const int h = region_.bottom - region_.top;

    // 码率 = 像素率 × 每像素比特数。高帧率下相邻帧更相似，用 sqrt 衰减系数，
    // 避免 120fps 的码率相对 30fps 线性膨胀 4 倍。
    double factor = QualityFactor(quality_);
    if (fps_ > 30) factor *= std::sqrt(30.0 / (double)fps_);
    const int bitrate = (int)((double)w * h * fps_ * factor);
    const LONGLONG frameDur = 10000000LL / fps_;      // 100ns 单位

    ScreenCapture cap;
    Mp4Encoder enc;
    RecEvent* ev = nullptr;

    if (!cap.Init(region_.left, region_.top, w, h)) {
        ev = new RecEvent(); ev->error = L"capture init failed";
        PostMessageW(notify_, WM_REC_FAIL, 0, (LPARAM)ev);
        timeEndPeriod(1);
        running_ = false;
        return;
    }
    if (!enc.Open(outPath_, w, h, fps_, bitrate)) {
        cap.Shutdown();
        ev = new RecEvent(); ev->error = L"encoder open failed";
        PostMessageW(notify_, WM_REC_FAIL, 0, (LPARAM)ev);
        timeEndPeriod(1);
        running_ = false;
        return;
    }

    const auto t0 = clock::now();
    const double frameInterval = 1.0 / fps_;
    auto nextDue = t0;
    auto lastTick = t0;
    int frames = 0;

    while (!stop_.load()) {
        const auto now = clock::now();
        if (now < nextDue) {
            // 睡到接近下一帧时刻再让出，避免 120fps 下把 CPU 全烧在空转上
            long long remain = std::chrono::duration_cast<std::chrono::microseconds>(nextDue - now).count();
            if (remain > 2000) std::this_thread::sleep_for(std::chrono::microseconds(remain - 1000));
            else               std::this_thread::sleep_for(std::chrono::microseconds(200));
            continue;
        }
        nextDue += std::chrono::microseconds((long long)(frameInterval * 1e6));
        if (nextDue < now - std::chrono::microseconds((long long)(frameInterval * 1e6)))
            nextDue = now + std::chrono::microseconds((long long)(frameInterval * 1e6));

        if (!cap.Grab()) continue;

        const double elapsed = std::chrono::duration<double>(clock::now() - t0).count();
        LONGLONG pts = (LONGLONG)(elapsed * 10000000.0);
        // 时间戳必须严格递增，否则封装器直接失败
        static thread_local LONGLONG lastPts = -1;
        if (pts <= lastPts) pts = lastPts + 1;
        lastPts = pts;

        enc.WriteFrame(cap.Bits(), pts, frameDur);
        ++frames;

        if (std::chrono::duration<double>(clock::now() - lastTick).count() >= 0.25) {
            lastTick = clock::now();
            ev = new RecEvent();
            ev->frames = frames;
            ev->elapsed = std::chrono::duration<double>(clock::now() - t0).count();
            PostMessageW(notify_, WM_REC_TICK, 0, (LPARAM)ev);
        }
    }

    enc.Close();
    cap.Shutdown();
    timeEndPeriod(1);

    const double elapsed = std::chrono::duration<double>(clock::now() - t0).count();
    ev = new RecEvent();
    ev->frames = frames;
    ev->elapsed = elapsed;
    ev->path = outPath_;
    PostMessageW(notify_, WM_REC_DONE, 0, (LPARAM)ev);
    running_ = false;
}
