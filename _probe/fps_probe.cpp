// fps_probe.cpp - 读 MP4 的真实参数：分辨率 / 标称帧率 / 实际帧数 / 时长
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <cstdio>
#include <io.h>
#include <fcntl.h>

int wmain(int argc, wchar_t** argv) {
    // stdout 被重定向到管道时，wprintf 默认按 C locale 转换会失败、输出为空。
    // 切成 UTF-8 文本模式，宽字符才能正确写出去。
    _setmode(_fileno(stdout), _O_U8TEXT);
    if (argc < 2) { wprintf(L"用法: fps_probe.exe <file.mp4>\n"); return 1; }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);

    IMFSourceReader* r = nullptr;
    HRESULT hr = MFCreateSourceReaderFromURL(argv[1], nullptr, &r);
    if (FAILED(hr) || !r) { wprintf(L"打开失败 0x%08lX\n", (unsigned long)hr); return 1; }

    PROPVARIANT pv; PropVariantInit(&pv);
    LONGLONG dur = 0;
    if (SUCCEEDED(r->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &pv))) {
        if (pv.vt == VT_UI8) dur = (LONGLONG)pv.uhVal.QuadPart;
        else if (pv.vt == VT_I8) dur = pv.hVal.QuadPart;
    }
    PropVariantClear(&pv);

    IMFMediaType* mt = nullptr;
    UINT32 num = 0, den = 0, w = 0, h = 0, br = 0;
    if (SUCCEEDED(r->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &mt)) && mt) {
        MFGetAttributeRatio(mt, MF_MT_FRAME_RATE, &num, &den);
        MFGetAttributeSize(mt, MF_MT_FRAME_SIZE, &w, &h);
        mt->GetUINT32(MF_MT_AVG_BITRATE, &br);
        mt->Release();
    }

    // 数一遍真实帧数
    int frames = 0;
    for (;;) {
        DWORD flags = 0;
        IMFSample* s = nullptr;
        hr = r->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, nullptr, &s);
        if (FAILED(hr)) break;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { if (s) s->Release(); break; }
        if (s) { frames++; s->Release(); }
    }
    r->Release();

    double sec = dur / 1e7;
    wprintf(L"文件      : %s\n", argv[1]);
    wprintf(L"分辨率    : %ux%u\n", w, h);
    wprintf(L"标称帧率  : %.2f fps  (%u/%u)\n", den ? (double)num / den : 0.0, num, den);
    wprintf(L"平均码率  : %.2f Mbps\n", br / 1e6);
    wprintf(L"时长      : %.2f 秒\n", sec);
    wprintf(L"实际帧数  : %d\n", frames);
    wprintf(L"实测帧率  : %.2f fps\n", sec > 0 ? frames / sec : 0.0);

    MFShutdown();
    CoUninitialize();
    return 0;
}
