#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfplay.h>
#include <cstdio>

static const wchar_t* kClass = L"MFPlayProbeWnd";
static HWND g_video = nullptr;
static IMFPMediaPlayer* g_player = nullptr;

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

int main() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    printf("CoInitializeEx 0x%08lX\n", (unsigned long)hr);
    hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    printf("MFStartup       0x%08lX\n", (unsigned long)hr);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass; wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassExW(&wc);

    HWND host = CreateWindowExW(0, kClass, L"probe", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 640, 480, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(host, SW_SHOW);

    RECT rc; GetClientRect(host, &rc);
    g_video = CreateWindowExW(0, kClass, L"", WS_CHILD | WS_VISIBLE,
        0, 0, rc.right, rc.bottom, host, nullptr, wc.hInstance, nullptr);

    hr = MFPCreateMediaPlayer(L"test.mp4", FALSE, MFP_OPTION_NONE, nullptr,
                              g_video, &g_player);
    printf("MFPCreateMediaPlayer 0x%08lX  player=%p\n", (unsigned long)hr, (void*)g_player);

    if (SUCCEEDED(hr) && g_player) {
        IMFPMediaItem* item = nullptr;
        hr = g_player->GetMediaItem(&item);
        printf("GetMediaItem 0x%08lX item=%p\n", (unsigned long)hr, (void*)item);
        if (item) {
            PROPVARIANT pvd; PropVariantInit(&pvd);
            HRESULT d = item->GetDuration(MFP_POSITIONTYPE_100NS, &pvd);
            printf("  item GetDuration 0x%08lX -> %.2fs\n", (unsigned long)d,
                   (pvd.vt == VT_I8) ? pvd.hVal.QuadPart / 1e7 : -1.0);
            PropVariantClear(&pvd);
            item->Release();
        }
        hr = g_player->Play();
        printf("Play 0x%08lX\n", (unsigned long)hr);

        for (int i = 0; i < 40; i++) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg); DispatchMessageW(&msg);
            }
            if (i == 20) {
                PROPVARIANT pv; PropVariantInit(&pv);
                pv.vt = VT_I8; pv.hVal.QuadPart = 5000000; // 0.5s
                HRESULT s = g_player->SetPosition(MFP_POSITIONTYPE_100NS, &pv);
                printf("SetPosition 0x%08lX\n", (unsigned long)s);
                PropVariantClear(&pv);
            }
            if (i == 30) {
                PROPVARIANT pvd; PropVariantInit(&pvd);
                PROPVARIANT pvp; PropVariantInit(&pvp);
                g_player->GetDuration(MFP_POSITIONTYPE_100NS, &pvd);
                g_player->GetPosition(MFP_POSITIONTYPE_100NS, &pvp);
                LONGLONG dur = (pvd.vt == VT_I8) ? pvd.hVal.QuadPart : 0;
                LONGLONG cur = (pvp.vt == VT_I8) ? pvp.hVal.QuadPart : 0;
                PropVariantClear(&pvd); PropVariantClear(&pvp);
                MFP_MEDIAPLAYER_STATE st = MFP_MEDIAPLAYER_STATE_EMPTY;
                g_player->GetState(&st);
                printf("state=%d duration=%.2fs pos=%.2fs\n",
                       (int)st, dur / 1e7, cur / 1e7);
            }
            Sleep(50);
        }
        g_player->Shutdown();
        g_player->Release();
        printf("PLAYBACK PROBE OK\n");
    } else {
        printf("PLAYBACK PROBE FAILED\n");
    }
    MFShutdown();
    return 0;
}
