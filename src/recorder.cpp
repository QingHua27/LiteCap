// recorder.cpp
#include "recorder.h"
#include "wincap.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <mmsystem.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>

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
    DWORD          audioStream = 0;
    bool           hasAudio = false;
    int            w = 0, h = 0;
    bool           began = false;
};

template <class T> static void SafeRelease(T** p) {
    if (*p) { (*p)->Release(); *p = nullptr; }
}

bool Mp4Encoder::OpenInternal(const std::wstring& path, int w, int h, int fps,
                              int bitrate, bool allowHardware, bool audio) {
    Close();
    impl_ = new Impl();
    impl_->w = w; impl_->h = h;

    // 打开硬件变换，让 MF 优先挑 GPU 编码器（NVENC / QSV / AMF）。
    // 失败不致命：调用方会去掉这个属性再来一次，退回软件编码。
    IMFAttributes* attrs = nullptr;
    if (SUCCEEDED(MFCreateAttributes(&attrs, 2)) && attrs && allowHardware)
        attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);

    HRESULT hr = MFCreateSinkWriterFromURL(path.c_str(), nullptr, attrs, &impl_->writer);
    SafeRelease(&attrs);
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
    // Both capture sources produce top-down BGRA. Explicit positive stride
    // prevents software RGB converters from assuming a bottom-up bitmap.
    if (SUCCEEDED(hr)) hr = inType->SetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32)w * 4);
    if (SUCCEEDED(hr)) hr = impl_->writer->SetInputMediaType(impl_->stream, inType, nullptr);
    SafeRelease(&inType);

    // 可选 AAC 音轨。WASAPI 统一转成 48kHz / 16-bit / stereo PCM，
    // SinkWriter 再封装成 MP4 支持最广的 AAC-LC。
    if (SUCCEEDED(hr) && audio) {
        IMFMediaType* aout = nullptr;
        hr = MFCreateMediaType(&aout);
        if (SUCCEEDED(hr)) hr = aout->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        if (SUCCEEDED(hr)) hr = aout->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        if (SUCCEEDED(hr)) hr = aout->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        if (SUCCEEDED(hr)) hr = aout->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
        if (SUCCEEDED(hr)) hr = aout->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        if (SUCCEEDED(hr)) hr = aout->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 16000); // 128 kbps
        if (SUCCEEDED(hr)) hr = aout->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
        if (SUCCEEDED(hr)) hr = aout->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29); // AAC-LC
        if (SUCCEEDED(hr)) hr = impl_->writer->AddStream(aout, &impl_->audioStream);
        SafeRelease(&aout);

        IMFMediaType* ain = nullptr;
        if (SUCCEEDED(hr)) hr = MFCreateMediaType(&ain);
        if (SUCCEEDED(hr)) hr = ain->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        if (SUCCEEDED(hr)) hr = ain->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        if (SUCCEEDED(hr)) hr = ain->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        if (SUCCEEDED(hr)) hr = ain->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
        if (SUCCEEDED(hr)) hr = ain->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        if (SUCCEEDED(hr)) hr = ain->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
        if (SUCCEEDED(hr)) hr = ain->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 192000);
        if (SUCCEEDED(hr)) hr = ain->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
        if (SUCCEEDED(hr)) hr = impl_->writer->SetInputMediaType(impl_->audioStream, ain, nullptr);
        SafeRelease(&ain);
        if (SUCCEEDED(hr)) impl_->hasAudio = true;
    }

    if (SUCCEEDED(hr)) hr = impl_->writer->BeginWriting();
    if (FAILED(hr)) { Close(); return false; }
    impl_->began = true;
    return true;
}

bool Mp4Encoder::Open(const std::wstring& path, int w, int h, int fps, int bitrate,
                      EncMode mode, bool audio) {
    // Auto：优先硬件编码（省 CPU），拿不到就退回软件。
    // 硬件 MFT 不是每台机器都有，个别分辨率/码率组合也可能不支持，
    // 所以软件这条兜底的路不能省。
    if (mode != EncMode::Software) {
        if (OpenInternal(path, w, h, fps, bitrate, /*allowHardware=*/true, audio)) {
            hardware_ = true;
            return true;
        }
        if (mode == EncMode::Hardware) return false;   // 强制硬件但失败
    }
    if (OpenInternal(path, w, h, fps, bitrate, /*allowHardware=*/false, audio)) {
        hardware_ = false;
        return true;
    }
    return false;
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

    // Input media type explicitly declares top-down stride for both software
    // and hardware paths. Copy the captured rows without reversing them.
    memcpy(dst, bgra, bytes);

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

bool Mp4Encoder::WriteAudio(const int16_t* stereoPcm, DWORD frames,
                            LONGLONG rtStart, LONGLONG rtDuration) {
    if (!impl_ || !impl_->writer || !impl_->hasAudio || !stereoPcm || !frames) return false;
    const DWORD bytes = frames * 2u * sizeof(int16_t);
    IMFMediaBuffer* buf = nullptr;
    if (FAILED(MFCreateMemoryBuffer(bytes, &buf))) return false;

    BYTE* dst = nullptr;
    DWORD maxLen = 0;
    if (FAILED(buf->Lock(&dst, &maxLen, nullptr))) { buf->Release(); return false; }
    if (maxLen < bytes) { buf->Unlock(); buf->Release(); return false; }
    memcpy(dst, stereoPcm, bytes);
    buf->Unlock();
    buf->SetCurrentLength(bytes);

    IMFSample* sample = nullptr;
    if (FAILED(MFCreateSample(&sample))) { buf->Release(); return false; }
    sample->AddBuffer(buf);
    sample->SetSampleTime(rtStart);
    sample->SetSampleDuration(rtDuration);
    HRESULT hr = impl_->writer->WriteSample(impl_->audioStream, sample);
    sample->Release();
    buf->Release();
    return SUCCEEDED(hr);
}

bool Mp4Encoder::Close() {
    if (!impl_) return true;
    bool finalized = false;
    if (impl_->writer) {
        if (impl_->began) finalized = SUCCEEDED(impl_->writer->Finalize());
        impl_->writer->Release();
        impl_->writer = nullptr;
    }
    delete impl_;
    impl_ = nullptr;
    return finalized;
}

// ================= WASAPI 音频采集 =================

namespace {

constexpr UINT32 kAudioRate = 48000;
constexpr UINT32 kAudioChannels = 2;
constexpr UINT32 kAudioFramesPerWrite = 480; // 10ms @ 48kHz
constexpr size_t kAudioRingFrames = kAudioRate * 4;
constexpr LONGLONG kAudioLeadHns = 1000000;  // 留 100ms 给 WASAPI 把包送到队列

struct AudioChunk {
    LONGLONG qpcHns = 0;            // IAudioCaptureClient::GetBuffer 的 100ns QPC 时间戳
    UINT32 frames = 0;
    std::vector<int16_t> pcm;       // 48kHz / stereo / signed 16-bit
};

LONGLONG QpcNowHns() {
    LARGE_INTEGER qpc = {}, freq = {};
    QueryPerformanceCounter(&qpc);
    QueryPerformanceFrequency(&freq);
    if (!freq.QuadPart) return 0;
    return (qpc.QuadPart / freq.QuadPart) * 10000000LL +
           (qpc.QuadPart % freq.QuadPart) * 10000000LL / freq.QuadPart;
}

float ReadWasapiSample(const BYTE* data, UINT32 frame, UINT32 channel,
                       UINT32 channels, UINT16 containerBits, UINT16 validBits, bool isFloat) {
    if (!channels) return 0.0f;
    if (channel >= channels) channel = channels - 1;
    const size_t bytesPerSample = containerBits / 8;
    const BYTE* p = data + ((size_t)frame * channels + channel) * bytesPerSample;
    if (isFloat && containerBits == 32) {
        float v = 0.0f;
        memcpy(&v, p, sizeof(v));
        if (!std::isfinite(v)) return 0.0f;
        return (std::max)(-1.0f, (std::min)(1.0f, v));
    }
    if (containerBits == 16) {
        int16_t v = 0; memcpy(&v, p, sizeof(v));
        if (validBits < 16) v = (int16_t)(v >> (16 - validBits));
        return (float)((double)v / (double)(1LL << (validBits - 1)));
    }
    if (containerBits == 24) {
        int32_t v = (int32_t)p[0] | ((int32_t)p[1] << 8) | ((int32_t)p[2] << 16);
        if (v & 0x800000) v |= (int32_t)0xFF000000;
        if (validBits < 24) v >>= (24 - validBits);
        return (float)((double)v / (double)(1LL << (validBits - 1)));
    }
    if (containerBits == 32) {
        int32_t v = 0; memcpy(&v, p, sizeof(v));
        if (validBits < 32) v >>= (32 - validBits);
        return (float)((double)v / (double)(1LL << (validBits - 1)));
    }
    return 0.0f;
}

int16_t FloatToPcm16(float v) {
    v = (std::max)(-1.0f, (std::min)(1.0f, v));
    const int n = (int)std::lrint(v * (v < 0.0f ? 32768.0f : 32767.0f));
    return (int16_t)(std::max)(-32768, (std::min)(32767, n));
}

class WasapiCapture {
public:
    explicit WasapiCapture(bool loopback) : loopback_(loopback) {}
    ~WasapiCapture() { Stop(); }
    WasapiCapture(const WasapiCapture&) = delete;
    WasapiCapture& operator=(const WasapiCapture&) = delete;

    bool Start(HRESULT& error) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            ready_ = false;
            error_ = S_OK;
        }
        stop_ = false;
        thread_ = std::thread(&WasapiCapture::ThreadMain, this);
        std::unique_lock<std::mutex> lk(mtx_);
        if (!cv_.wait_for(lk, std::chrono::seconds(8), [&] { return ready_; })) {
            error = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            lk.unlock();
            Stop();
            return false;
        }
        error = error_;
        return SUCCEEDED(error_);
    }

    void Stop() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
    }

    std::deque<AudioChunk> TakeChunks() {
        std::lock_guard<std::mutex> lk(mtx_);
        std::deque<AudioChunk> out;
        out.swap(chunks_);
        return out;
    }

private:
    void SignalReady(HRESULT hr) {
        std::lock_guard<std::mutex> lk(mtx_);
        error_ = hr;
        ready_ = true;
        cv_.notify_all();
    }

    void ThreadMain() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool uninit = SUCCEEDED(hr);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) { SignalReady(hr); return; }

        IMMDeviceEnumerator* enumerator = nullptr;
        IMMDevice* device = nullptr;
        IAudioClient* client = nullptr;
        IAudioCaptureClient* capture = nullptr;
        WAVEFORMATEX* mix = nullptr;
        auto cleanup = [&]() {
            if (client) client->Stop();
            if (capture) capture->Release();
            if (client) client->Release();
            if (device) device->Release();
            if (enumerator) enumerator->Release();
            if (mix) CoTaskMemFree(mix);
            if (uninit) CoUninitialize();
        };

        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
        if (SUCCEEDED(hr))
            hr = enumerator->GetDefaultAudioEndpoint(loopback_ ? eRender : eCapture,
                                                     eConsole, &device);
        if (SUCCEEDED(hr))
            hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client);
        if (SUCCEEDED(hr)) hr = client->GetMixFormat(&mix);

        UINT16 containerBits = mix ? mix->wBitsPerSample : 0;
        UINT16 validBits = containerBits;
        bool isFloat = mix && mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        if (mix && mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
            mix->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
            const auto* ext = (const WAVEFORMATEXTENSIBLE*)mix;
            isFloat = ext->SubFormat.Data1 == 3;   // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
            if (ext->Samples.wValidBitsPerSample) validBits = ext->Samples.wValidBitsPerSample;
        }
        if (SUCCEEDED(hr) && (!mix || mix->nSamplesPerSec == 0 || mix->nChannels == 0 ||
            (containerBits != 16 && containerBits != 24 && containerBits != 32) ||
            validBits == 0 || validBits > containerBits || (isFloat && containerBits != 32)))
            hr = AUDCLNT_E_UNSUPPORTED_FORMAT;

        const DWORD flags = loopback_ ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
        // Shared mode, 1s endpoint buffer. Polling avoids endpoint-specific event quirks
        // (notably older Windows loopback event-callback behavior).
        if (SUCCEEDED(hr)) hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags,
                                                   10000000LL, 0, mix, nullptr);
        if (SUCCEEDED(hr)) hr = client->GetService(__uuidof(IAudioCaptureClient), (void**)&capture);
        if (SUCCEEDED(hr)) hr = client->Start();
        if (FAILED(hr)) { SignalReady(hr); cleanup(); return; }
        SignalReady(S_OK);

        while (!stop_.load()) {
            UINT32 packetFrames = 0;
            hr = capture->GetNextPacketSize(&packetFrames);
            if (FAILED(hr)) break;
            if (!packetFrames) {
                std::this_thread::sleep_for(std::chrono::milliseconds(3));
                continue;
            }

            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD bufferFlags = 0;
            UINT64 devicePos = 0, qpcPos = 0;
            hr = capture->GetBuffer(&data, &frames, &bufferFlags, &devicePos, &qpcPos);
            if (FAILED(hr)) break;
            if (frames) {
                AudioChunk chunk;
                chunk.qpcHns = (bufferFlags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)
                    ? QpcNowHns() : (LONGLONG)qpcPos;
                chunk.frames = (UINT32)(((UINT64)frames * kAudioRate + mix->nSamplesPerSec / 2) /
                                        mix->nSamplesPerSec);
                if (!chunk.frames) chunk.frames = 1;
                chunk.pcm.resize((size_t)chunk.frames * 2);
                for (UINT32 out = 0; out < chunk.frames; ++out) {
                    const UINT32 srcFrame = (UINT32)((UINT64)out * mix->nSamplesPerSec / kAudioRate);
                    const UINT32 f = (std::min)(srcFrame, frames - 1);
                    float left = 0.0f, right = 0.0f;
                    if (!(bufferFlags & AUDCLNT_BUFFERFLAGS_SILENT) && data) {
                        if (mix->nChannels == 1) {
                            left = right = ReadWasapiSample(data, f, 0, mix->nChannels,
                                                           containerBits, validBits, isFloat);
                        } else {
                            left = ReadWasapiSample(data, f, 0, mix->nChannels,
                                                    containerBits, validBits, isFloat);
                            right = ReadWasapiSample(data, f, 1, mix->nChannels,
                                                     containerBits, validBits, isFloat);
                        }
                    }
                    chunk.pcm[(size_t)out * 2] = FloatToPcm16(left);
                    chunk.pcm[(size_t)out * 2 + 1] = FloatToPcm16(right);
                }
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    if (chunks_.size() >= 256) chunks_.pop_front();
                    chunks_.push_back(std::move(chunk));
                }
            }
            capture->ReleaseBuffer(frames);
        }

        cleanup();
    }

    bool loopback_ = false;
    std::atomic<bool> stop_{ false };
    std::thread thread_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<AudioChunk> chunks_;
    bool ready_ = false;
    HRESULT error_ = S_OK;
};

class AudioRing {
public:
    AudioRing() : pcm_(kAudioRingFrames * 2), tags_(kAudioRingFrames, (std::numeric_limits<LONGLONG>::min)()) {}

    void Add(const AudioChunk& chunk, LONGLONG originHns) {
        const LONGLONG delta = chunk.qpcHns - originHns;
        const LONGLONG start = delta * (LONGLONG)kAudioRate / 10000000LL;
        for (UINT32 i = 0; i < chunk.frames; ++i) {
            const LONGLONG index = start + i;
            const size_t slot = Slot(index);
            tags_[slot] = index;
            pcm_[slot * 2] = chunk.pcm[(size_t)i * 2];
            pcm_[slot * 2 + 1] = chunk.pcm[(size_t)i * 2 + 1];
        }
    }

    void Mix(LONGLONG start, UINT32 frames, bool add, std::vector<int16_t>& out) const {
        out.resize((size_t)frames * 2);
        for (UINT32 i = 0; i < frames; ++i) {
            const LONGLONG index = start + i;
            int left = 0, right = 0;
            if (add) {
                const size_t slot = Slot(index);
                if (tags_[slot] == index) { left = pcm_[slot * 2]; right = pcm_[slot * 2 + 1]; }
            }
            const size_t o = (size_t)i * 2;
            const int l0 = out[o], r0 = out[o + 1];
            out[o] = (int16_t)(std::max)(-32768, (std::min)(32767, l0 + left));
            out[o + 1] = (int16_t)(std::max)(-32768, (std::min)(32767, r0 + right));
        }
    }

private:
    static size_t Slot(LONGLONG index) {
        const LONGLONG cap = (LONGLONG)kAudioRingFrames;
        LONGLONG r = index % cap;
        if (r < 0) r += cap;
        return (size_t)r;
    }
    std::vector<int16_t> pcm_;
    std::vector<LONGLONG> tags_;
};

void AddCaptureChunks(WasapiCapture* capture, AudioRing& ring, LONGLONG originHns) {
    if (!capture) return;
    auto chunks = capture->TakeChunks();
    for (const AudioChunk& chunk : chunks) ring.Add(chunk, originHns);
}

bool WriteAudioUntil(Mp4Encoder& encoder, AudioRing& sysRing, AudioRing& micRing,
                     bool hasSys, bool hasMic, LONGLONG originHns,
                     LONGLONG endSample, LONGLONG& nextSample,
                     std::vector<int16_t>& mix) {
    while (nextSample < endSample) {
        const UINT32 n = (UINT32)(std::min)((LONGLONG)kAudioFramesPerWrite, endSample - nextSample);
        mix.assign((size_t)n * 2, 0);
        sysRing.Mix(nextSample, n, hasSys, mix);
        micRing.Mix(nextSample, n, hasMic, mix);
        const LONGLONG start = nextSample * 10000000LL / kAudioRate;
        const LONGLONG end = (nextSample + n) * 10000000LL / kAudioRate;
        if (!encoder.WriteAudio(mix.data(), n, start, end - start)) return false;
        nextSample += n;
    }
    return true;
}

} // namespace

// ================= 录制线程 =================

bool Recorder::Start(HWND notify, const RecTarget& target, int fps, const std::string& quality,
                     const std::wstring& outPath, bool systemAudio, bool microphoneAudio) {
    if (running_.load()) return false;
    // A worker can finish or fail on its own. Its std::thread still needs
    // joining before it is replaced by the next recording.
    if (thread_.joinable()) thread_.join();

    if (target.window) {
        if (!target.hwnd || !IsWindow(target.hwnd)) return false;
    } else {
        int w = target.region.right - target.region.left;
        int h = target.region.bottom - target.region.top;
        w -= w % 2;                     // H.264 要求偶数
        h -= h % 2;
        if (w < 16 || h < 16) return false;
    }

    target_ = target;
    fps_ = fps < 1 ? 1 : (fps > 120 ? 120 : fps);
    quality_ = quality;
    outPath_ = outPath;
    systemAudio_ = systemAudio;
    microphoneAudio_ = microphoneAudio;
    notify_ = notify;
    stop_ = false;
    running_ = true;
    thread_ = std::thread(&Recorder::ThreadMain, this);
    return true;
}

void Recorder::Stop() {
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

    // 失败时把 HRESULT 一起带出去。
    // 窗口捕获的两个典型失败对用户来说处置方式完全不同：
    //   0x80070005  目标窗口提权了（UIPI），只能换个窗口或让 LiteCap 也提权
    //   0x80004005  当前系统/会话不支持 WGC（老 Win10、某些远程桌面）
    // 只说一句「初始化失败」，用户除了重试没别的可做。
    auto fail = [&](const wchar_t* msg, HRESULT hr = S_OK) {
        RecEvent* e = new RecEvent();
        e->error = msg;
        if (hr != S_OK) {
            wchar_t buf[32];
            swprintf(buf, 32, L"  (0x%08X)", (unsigned)hr);
            e->error += buf;
        }
        PostMessageW(notify_, WM_REC_FAIL, 0, (LPARAM)e);
        timeEndPeriod(1);
        running_ = false;
    };

    // ---- 建捕获源 ----
    // 两条路走的是完全不同的 API，但对外都是「Grab 一次拿一帧自上而下的 BGRA」，
    // 所以下面的录制循环可以共用。
    bool useWindow = target_.window;
    fallback_ = false;
    ScreenCapture sc;
    WindowCapture wc;

    int w = 0, h = 0;
    if (useWindow) {
        if (!wc.Init(target_.hwnd)) {
            // Some targets or sessions reject WGC. Borderless style alone is
            // not a restriction: supported fullscreen renderers stay on WGC.
            // If initialization fails, explicitly fall back to this window's
            // screen rectangle; the UI labels the occlusion-sensitive mode.
            RECT wr = {};
            GetWindowRect(target_.hwnd, &wr);
            int rw = wr.right - wr.left;
            int rh = wr.bottom - wr.top;
            rw -= rw % 2;
            rh -= rh % 2;
            if (rw >= 16 && rh >= 16 && sc.Init(wr.left, wr.top, rw, rh)) {
                useWindow = false;
                fallback_ = true;
                w = rw;
                h = rh;
            } else {
                fail(L"window capture init failed", wc.LastError());
                return;
            }
        } else {
            w = wc.Width();
            h = wc.Height();
        }
    } else {
        w = target_.region.right - target_.region.left;
        h = target_.region.bottom - target_.region.top;
        w -= w % 2;
        h -= h % 2;
        if (w < 16 || h < 16) { fail(L"region too small"); return; }
        if (!sc.Init(target_.region.left, target_.region.top, w, h)) {
            fail(L"capture init failed");
            return;
        }
    }
    if (w < 16 || h < 16) { fail(L"capture size too small"); return; }

    auto grabFrame = [&]() -> const BYTE* {
        if (useWindow) return wc.Grab() ? wc.Bits() : nullptr;
        return sc.Grab() ? sc.Bits() : nullptr;
    };

    // 码率 = 像素率 × 每像素比特数。高帧率下相邻帧更相似，用 sqrt 衰减系数，
    // 避免 120fps 的码率相对 30fps 线性膨胀 4 倍。
    double factor = QualityFactor(quality_);
    if (fps_ > 30) factor *= std::sqrt(30.0 / (double)fps_);
    const int bitrate = (int)((double)w * h * fps_ * factor);
    const LONGLONG frameDur = 10000000LL / fps_;      // 100ns 单位

    const bool useSystemAudio = systemAudio_.load();
    const bool useMicrophone = microphoneAudio_.load();
    std::unique_ptr<WasapiCapture> sysCapture;
    std::unique_ptr<WasapiCapture> micCapture;
    HRESULT audioHr = S_OK;
    if (useSystemAudio) {
        sysCapture.reset(new WasapiCapture(/*loopback=*/true));
        if (!sysCapture->Start(audioHr)) {
            fail(L"system audio capture init failed", audioHr);
            return;
        }
    }
    if (useMicrophone) {
        micCapture.reset(new WasapiCapture(/*loopback=*/false));
        if (!micCapture->Start(audioHr)) {
            fail(L"microphone capture init failed", audioHr);
            return;
        }
    }
    const bool audioEnabled = useSystemAudio || useMicrophone;

    Mp4Encoder enc;
    RecEvent* ev = nullptr;

    EncMode mode = EncMode::Auto;
    if (encMode_ == "software")      mode = EncMode::Software;
    else if (encMode_ == "hardware") mode = EncMode::Hardware;

    // Auto 在 120fps 下强制走软件编码。
    // 实测（1920×1080，同一段画面，各 6 秒）：
    //   30fps  软件 29.93 / 硬件 29.92   —— 都达标
    //   60fps  软件 59.85 / 硬件 59.91   —— 都达标
    //   120fps 软件 119.75 / 硬件 82.38  —— 硬件明显追不上
    // 原因不是 NVENC 慢，而是 MF 为了喂硬件编码器要插一个 CPU 上的
    // ARGB32→NV12 色彩转换；1080p120 是每秒 2.5 亿像素，省下的 CPU 又还回去了。
    // 等 WGC 那条路把色彩转换搬到 GPU 之后，这个阈值可以重新评估。
    const int kHardwareMaxFps = 60;
    if (mode == EncMode::Auto && fps_ > kHardwareMaxFps) mode = EncMode::Software;

    if (!enc.Open(outPath_, w, h, fps_, bitrate, mode, audioEnabled)) {
        fail(L"encoder open failed");
        return;
    }
    hw_ = enc.Hardware();   // 记下这次走的是硬件还是软件编码

    const auto t0 = clock::now();
    const LONGLONG audioOriginHns = QpcNowHns();
    AudioRing sysRing, micRing;
    LONGLONG nextAudioSample = 0;
    std::vector<int16_t> audioMix;
    bool audioWriteFailed = false;
    bool videoWriteFailed = false;
    const double frameInterval = 1.0 / fps_;
    auto nextDue = t0;
    auto lastTick = t0;
    int frames = 0;

    while (!stop_.load()) {
        // 录的那个窗口被关掉了：正常收尾。已经写好的文件仍然是完整可播的，
        // 没必要把这一段丢掉。
        // 回退到 GDI 之后 wc 已经关掉了，得直接看窗口还在不在。
        if (useWindow) {
            if (wc.TargetClosed()) break;
        } else if (fallback_ && !IsWindow(target_.hwnd)) {
            break;
        }

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

        const BYTE* px = grabFrame();
        if (!px) continue;

        const double elapsed = std::chrono::duration<double>(clock::now() - t0).count();
        LONGLONG pts = (LONGLONG)(elapsed * 10000000.0);
        // 时间戳必须严格递增，否则封装器直接失败
        static thread_local LONGLONG lastPts = -1;
        if (pts <= lastPts) pts = lastPts + 1;
        lastPts = pts;

        if (!enc.WriteFrame(px, pts, frameDur)) {
            videoWriteFailed = true;
            break;
        }
        ++frames;

        if (audioEnabled) {
            AddCaptureChunks(sysCapture.get(), sysRing, audioOriginHns);
            AddCaptureChunks(micCapture.get(), micRing, audioOriginHns);
            const LONGLONG elapsedHns =
                (LONGLONG)(std::chrono::duration<double>(clock::now() - t0).count() * 10000000.0);
            const LONGLONG safeHns = (std::max)(0LL, elapsedHns - kAudioLeadHns);
            const LONGLONG audioEnd = safeHns * kAudioRate / 10000000LL;
            if (!WriteAudioUntil(enc, sysRing, micRing, useSystemAudio, useMicrophone,
                                 audioOriginHns, audioEnd, nextAudioSample, audioMix)) {
                audioWriteFailed = true;
                break;
            }
        }

        if (frames == 1 || std::chrono::duration<double>(clock::now() - lastTick).count() >= 0.25) {
            lastTick = clock::now();
            ev = new RecEvent();
            ev->frames = frames;
            ev->elapsed = std::chrono::duration<double>(clock::now() - t0).count();
            ev->fallback = fallback_.load();
            PostMessageW(notify_, WM_REC_TICK, 0, (LPARAM)ev);
        }
    }

    if (sysCapture) sysCapture->Stop();
    if (micCapture) micCapture->Stop();
    if (audioEnabled && !audioWriteFailed) {
        AddCaptureChunks(sysCapture.get(), sysRing, audioOriginHns);
        AddCaptureChunks(micCapture.get(), micRing, audioOriginHns);
        const LONGLONG elapsedHns =
            (LONGLONG)(std::chrono::duration<double>(clock::now() - t0).count() * 10000000.0);
        const LONGLONG audioEnd = elapsedHns * kAudioRate / 10000000LL;
        if (!WriteAudioUntil(enc, sysRing, micRing, useSystemAudio, useMicrophone,
                             audioOriginHns, audioEnd, nextAudioSample, audioMix))
            audioWriteFailed = true;
    }

    const bool finalized = enc.Close();
    sc.Shutdown();
    wc.Shutdown();
    timeEndPeriod(1);

    if (audioWriteFailed || videoWriteFailed || !finalized || frames == 0) {
        RecEvent* error = new RecEvent();
        if (audioWriteFailed) error->error = L"audio sample write failed";
        else if (videoWriteFailed) error->error = L"video sample write failed";
        else if (!finalized) error->error = L"video finalization failed";
        else error->error = L"no video frames captured";
        PostMessageW(notify_, WM_REC_FAIL, 0, (LPARAM)error);
        running_ = false;
        return;
    }

    const double elapsed = std::chrono::duration<double>(clock::now() - t0).count();
    ev = new RecEvent();
    ev->frames = frames;
    ev->elapsed = elapsed;
    ev->path = outPath_;
    PostMessageW(notify_, WM_REC_DONE, 0, (LPARAM)ev);
    running_ = false;
}
