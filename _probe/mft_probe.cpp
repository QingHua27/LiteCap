// mft_probe.cpp —— 枚举系统里的 H.264 编码器 MFT，区分硬件 / 软件
//
// 用来回答「录制能不能用 N 卡加速」：Media Foundation 的 SinkWriter 默认只用
// 软件编码器，除非显式打开 MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS，
// 而且机器上得真的注册了硬件编码 MFT。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mftransform.h>
#include <codecapi.h>
#include <fcntl.h>
#include <io.h>
#include <cstdio>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

static const wchar_t* HwTag(BOOL hw) { return hw ? L"硬件" : L"软件"; }

int wmain() {
    _setmode(_fileno(stdout), _O_U8TEXT);

    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(hr)) { wprintf(L"[x] MFStartup 失败 0x%08X\n", (unsigned)hr); return 1; }

    const struct { UINT32 flag; const wchar_t* name; } groups[] = {
        { MFT_ENUM_FLAG_HARDWARE, L"硬件编码器" },
        { MFT_ENUM_FLAG_SYNCMFT,  L"同步 MFT（软件）" },
    };

    for (const auto& g : groups) {
        IMFActivate** acts = nullptr;
        UINT32 n = 0;
        MFT_REGISTER_TYPE_INFO out = { MFMediaType_Video, MFVideoFormat_H264 };
        hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                       g.flag | MFT_ENUM_FLAG_SORTANDFILTER,
                       nullptr, &out, &acts, &n);
        wprintf(L"\n=== %s ===  (flag=0x%X, hr=0x%08X)\n",
                g.name, (unsigned)g.flag, (unsigned)hr);
        if (FAILED(hr) || n == 0) {
            wprintf(L"   (无)\n");
            continue;
        }
        for (UINT32 i = 0; i < n; ++i) {
            LPWSTR nm = nullptr;
            UINT32 cch = 0;
            if (SUCCEEDED(acts[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute,
                                                      &nm, &cch))) {
                // MFT_ENUM_HARDWARE_URL_Attribute 是字符串（设备路径），不是 UINT32，
                // 用 GetUINT32 读会失败并把标志位留在 0，把所有东西都判成软件。
                // 正确做法是看这个字符串属性存不存在。
                LPWSTR url = nullptr;
                UINT32 cchUrl = 0;
                BOOL hw = SUCCEEDED(acts[i]->GetAllocatedString(
                              MFT_ENUM_HARDWARE_URL_Attribute, &url, &cchUrl));
                if (url) CoTaskMemFree(url);
                wprintf(L"   [%u] %s  (%s)\n", i, nm, HwTag(hw));
                CoTaskMemFree(nm);
            }
            acts[i]->Release();
        }
        CoTaskMemFree(acts);
    }

    // 真正试着激活一个硬件编码器，看能不能创建成功
    wprintf(L"\n=== 尝试激活硬件 H.264 编码器 ===\n");
    {
        IMFActivate** acts = nullptr;
        UINT32 n = 0;
        MFT_REGISTER_TYPE_INFO out = { MFMediaType_Video, MFVideoFormat_H264 };
        hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                       MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                       nullptr, &out, &acts, &n);
        if (SUCCEEDED(hr) && n > 0) {
            IMFTransform* mft = nullptr;
            hr = acts[0]->ActivateObject(__uuidof(IMFTransform), (void**)&mft);
            wprintf(L"   ActivateObject -> 0x%08X %s\n", (unsigned)hr,
                    SUCCEEDED(hr) ? L"(成功)" : L"(失败)");
            if (SUCCEEDED(hr) && mft) {
                // 看看它支持哪些码率控制模式
                IMFAttributes* a = nullptr;
                if (SUCCEEDED(mft->GetAttributes(&a)) && a) {
                    UINT32 nctrl = 0;
                    if (SUCCEEDED(a->GetUINT32(CODECAPI_AVEncCommonRateControlMode,
                                               &nctrl)))
                        wprintf(L"   默认码率控制模式 = %u\n", nctrl);
                    a->Release();
                }
                mft->Release();
            }
            for (UINT32 i = 0; i < n; ++i) acts[i]->Release();
            CoTaskMemFree(acts);
        } else {
            wprintf(L"   没有可用的硬件编码器\n");
        }
    }

    // 顺带报告显卡
    wprintf(L"\n=== 显示适配器 ===\n");
    DISPLAY_DEVICEW dd = { sizeof(dd) };
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i) {
        if (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE)
            wprintf(L"   [主] %s  |  %s\n", dd.DeviceString, dd.DeviceID);
        dd.cb = sizeof(dd);
    }

    MFShutdown();
    return 0;
}
