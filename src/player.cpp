// player.cpp - MFPlay 播放窗口
#include "player.h"

#include <mfapi.h>
#include <mfplay.h>
#include <mfreadwrite.h>
#include <commctrl.h>
#include <cstdio>

#pragma comment(lib, "mfplay.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "comctl32.lib")

namespace {

const wchar_t* kPlayerClass = L"LiteCapPlayerWnd";

HWND              g_wnd = nullptr;
HWND              g_video = nullptr;
HWND              g_btnPlay = nullptr;
HWND              g_btnClose = nullptr;
HWND              g_slider = nullptr;
HWND              g_lblTime = nullptr;
IMFPMediaPlayer*  g_player = nullptr;
I18n*             g_i18n = nullptr;
bool              g_playing = false;
bool              g_dragging = false;
LONGLONG          g_duration = 0;      // 100ns
std::wstring      g_title;

const int ID_PLAY   = 2001;
const int ID_SLIDER = 2002;
const int ID_CLOSE  = 2003;
const int ID_TIMER  = 3001;
const int BAR_H     = 40;

std::wstring TimeText(LONGLONG hns100) {
    long long sec = (long long)(hns100 / 10000000);
    if (sec < 0) sec = 0;
    wchar_t buf[64];
    if (sec >= 3600)
        swprintf(buf, 64, L"%lld:%02lld:%02lld", sec / 3600, (sec / 60) % 60, sec % 60);
    else
        swprintf(buf, 64, L"%02lld:%02lld", sec / 60, sec % 60);
    return buf;
}

// MFPlay 的 IMFPMediaPlayer/IMFPMediaItem::GetDuration 对部分文件会返回
// 0xC00D36D5，拿不到时长。这里用 Source Reader 直接读容器的 MF_PD_DURATION，
// 播放本身不受影响，只是为了把进度条和总时长显示正确。
LONGLONG ProbeDuration(const std::wstring& path) {
    IMFSourceReader* reader = nullptr;
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader)) || !reader)
        return 0;
    LONGLONG dur = 0;
    PROPVARIANT pv; PropVariantInit(&pv);
    if (SUCCEEDED(reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE,
                                                   MF_PD_DURATION, &pv))) {
        if (pv.vt == VT_UI8)      dur = (LONGLONG)pv.uhVal.QuadPart;
        else if (pv.vt == VT_I8)  dur = pv.hVal.QuadPart;
    }
    PropVariantClear(&pv);
    reader->Release();
    return dur;
}

void UpdateTimeLabel() {
    LONGLONG cur = 0;
    if (g_player) {
        PROPVARIANT pv; PropVariantInit(&pv);
        if (SUCCEEDED(g_player->GetPosition(MFP_POSITIONTYPE_100NS, &pv)) && pv.vt == VT_I8)
            cur = pv.hVal.QuadPart;
        PropVariantClear(&pv);
    }
    std::wstring s = TimeText(cur) + L" / " + TimeText(g_duration);
    SetWindowTextW(g_lblTime, s.c_str());

    if (!g_dragging && g_duration > 0) {
        int pos = (int)((double)cur / (double)g_duration * 1000.0);
        SendMessageW(g_slider, TBM_SETPOS, TRUE, pos);
    }
}

void RefreshPlayButton() {
    const wchar_t* txt = g_playing ? L"\u23F8" : L"\u25B6";   // ⏸ / ▶
    SetWindowTextW(g_btnPlay, txt);
}

void Layout() {
    RECT rc;
    GetClientRect(g_wnd, &rc);
    int w = rc.right, h = rc.bottom;
    int barY = h - BAR_H;
    if (barY < 40) barY = 40;

    MoveWindow(g_video, 0, 0, w, barY, TRUE);

    int x = 8;
    MoveWindow(g_btnPlay, x, barY + 7, 34, 26, TRUE);  x += 40;
    int closeW = 34;
    int sliderW = w - x - closeW - 16 - 110;
    if (sliderW < 60) sliderW = 60;
    MoveWindow(g_slider, x, barY + 9, sliderW, 24, TRUE);
    MoveWindow(g_lblTime, x + sliderW + 8, barY + 11, 100, 20, TRUE);
    MoveWindow(g_btnClose, w - closeW - 8, barY + 7, closeW, 26, TRUE);
    if (g_player) g_player->UpdateVideo();
}

void DestroyPlayerWindow() {
    if (g_wnd) {
        KillTimer(g_wnd, ID_TIMER);
        DestroyWindow(g_wnd);
        g_wnd = nullptr;
    }
}

LRESULT CALLBACK VideoProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK PlayerProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_SIZE:
        Layout();
        return 0;

    case WM_TIMER:
        if (w == ID_TIMER) {
            UpdateTimeLabel();
            if (g_player) {
                MFP_MEDIAPLAYER_STATE st = MFP_MEDIAPLAYER_STATE_EMPTY;
                g_player->GetState(&st);
                bool playing = (st == MFP_MEDIAPLAYER_STATE_PLAYING);
                if (playing != g_playing) { g_playing = playing; RefreshPlayButton(); }
            }
        }
        return 0;

    case WM_HSCROLL:
        if ((HWND)l == g_slider) {
            UINT code = LOWORD(w);
            if (code == TB_THUMBTRACK || code == TB_THUMBPOSITION) g_dragging = true;
            if (code == TB_ENDTRACK) {
                g_dragging = false;
                int pos = (int)SendMessageW(g_slider, TBM_GETPOS, 0, 0);
                if (g_player && g_duration > 0) {
                    PROPVARIANT pv; PropVariantInit(&pv);
                    pv.vt = VT_I8;
                    pv.hVal.QuadPart = (LONGLONG)((double)pos / 1000.0 * (double)g_duration);
                    g_player->SetPosition(MFP_POSITIONTYPE_100NS, &pv);
                    PropVariantClear(&pv);
                }
            }
        }
        return 0;

    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == ID_PLAY && g_player) {
            if (g_playing) g_player->Pause();
            else           g_player->Play();
            return 0;
        }
        if (id == ID_CLOSE) { DestroyPlayerWindow(); return 0; }
        break;
    }

    case WM_KEYDOWN:
        if (w == VK_ESCAPE) { DestroyPlayerWindow(); return 0; }
        if (w == VK_SPACE && g_player) {
            if (g_playing) g_player->Pause(); else g_player->Play();
            return 0;
        }
        break;

    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)w;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(40, 44, 50));
        return (LRESULT)GetStockObject(NULL_BRUSH);
    }

    case WM_CLOSE:
        DestroyPlayerWindow();
        return 0;

    case WM_DESTROY:
        if (g_player) {
            g_player->Shutdown();
            g_player->Release();
            g_player = nullptr;
        }
        g_video = nullptr;
        g_wnd = nullptr;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

bool Player::IsOpen() { return g_wnd != nullptr; }

void Player::Close() { DestroyPlayerWindow(); }

bool Player::Open(HINSTANCE inst, const std::wstring& path, I18n* i18n) {
    if (g_wnd) DestroyPlayerWindow();
    g_i18n = i18n;
    g_duration = 0;      // 换文件时必须清掉上一部的时长

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = PlayerProc;
        wc.hInstance = inst;
        wc.lpszClassName = kPlayerClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.hIcon   = LoadAppIcon(inst, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
        wc.hIconSm = LoadAppIcon(inst, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
        RegisterClassExW(&wc);

        WNDCLASSEXW vc = { sizeof(vc) };
        vc.lpfnWndProc = VideoProc;
        vc.hInstance = inst;
        vc.lpszClassName = L"LiteCapVideoWnd";
        vc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        vc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        RegisterClassExW(&vc);
        registered = true;
    }

    g_title = path;
    size_t slash = g_title.find_last_of(L"\\/");
    if (slash != std::wstring::npos) g_title = g_title.substr(slash + 1);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int ww = sw * 3 / 4, wh = sh * 3 / 4;

    g_wnd = CreateWindowExW(0, kPlayerClass, g_title.c_str(),
                            WS_OVERLAPPEDWINDOW,
                            (sw - ww) / 2, (sh - wh) / 2, ww, wh,
                            nullptr, nullptr, inst, nullptr);
    if (!g_wnd) return false;

    RECT rc; GetClientRect(g_wnd, &rc);
    g_video = CreateWindowExW(0, L"LiteCapVideoWnd", L"", WS_CHILD | WS_VISIBLE,
                              0, 0, rc.right, rc.bottom - BAR_H,
                              g_wnd, nullptr, inst, nullptr);

    g_btnPlay = CreateWindowExW(0, L"BUTTON", L"\u25B6",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT,
        0, 0, 34, 26, g_wnd, (HMENU)(INT_PTR)ID_PLAY, inst, nullptr);
    g_slider = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
        WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
        0, 0, 100, 24, g_wnd, (HMENU)(INT_PTR)ID_SLIDER, inst, nullptr);
    SendMessageW(g_slider, TBM_SETRANGE, TRUE, MAKELONG(0, 1000));
    g_lblTime = CreateWindowExW(0, L"STATIC", L"00:00 / 00:00",
        WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
        0, 0, 100, 20, g_wnd, nullptr, inst, nullptr);
    g_btnClose = CreateWindowExW(0, L"BUTTON", L"\u2715",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT,
        0, 0, 34, 26, g_wnd, (HMENU)(INT_PTR)ID_CLOSE, inst, nullptr);

    HRESULT hr = MFPCreateMediaPlayer(path.c_str(), FALSE, MFP_OPTION_NONE,
                                      nullptr, g_video, &g_player);
    if (FAILED(hr) || !g_player) {
        MessageBoxW(g_wnd, (L"MFPCreateMediaPlayer failed: 0x" +
                            std::to_wstring((unsigned long)hr)).c_str(), L"LiteCap", MB_ICONERROR);
        DestroyPlayerWindow();
        return false;
    }

    g_duration = ProbeDuration(path);

    ShowWindow(g_wnd, SW_SHOW);
    Layout();
    SetTimer(g_wnd, ID_TIMER, 200, nullptr);

    g_player->Play();
    g_playing = true;
    RefreshPlayButton();
    return true;
}
