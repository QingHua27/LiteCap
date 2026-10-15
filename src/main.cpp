// main.cpp - LiteCap C++ 版主程序
// 主窗口 + 录制记录列表 + 悬浮窗 + 区域框选 + 软件内播放
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <mfapi.h>
#include <shlobj.h>
#include <shellapi.h>
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include "common.h"
#include "recorder.h"
#include "wincap.h"
#include "history.h"
#include "player.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

// 必须启用 comctl32 v6：没有它系统会退回 v5 经典外观，
// 而且 CB_SETMINVISIBLE 这类 v6 才有的消息会直接失败返回 0，
// 下拉列表就只能靠控件高度硬撑（见 LayoutMain 里的说明）。
#pragma comment(linker, "\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ---------------- 控件 ID ----------------
enum {
    IDC_TARGET = 1001, IDC_PICK, IDC_FPS, IDC_QUALITY, IDC_LANG,
    IDC_DIR, IDC_BROWSE, IDC_RECORD, IDC_FLOAT, IDC_LIST, IDC_HINT,
    IDC_HOTKEY_EDIT, IDC_HOTKEY_SET, IDC_PICK_WIN, IDC_HIST_TOGGLE,
    IDC_ENCODER, IDC_AUDIO_SYSTEM, IDC_AUDIO_MIC,
};
enum {
    IDM_PLAY = 4001, IDM_OPEN_DIR, IDM_COPY_PATH, IDM_REMOVE, IDM_DELETE_FILE,
};

// 可选帧率档位（单一来源，改这里即可全局生效）
static const int kFpsVals[] = { 10, 15, 20, 24, 30, 60, 120 };
static const int kFpsCount  = (int)(sizeof(kFpsVals) / sizeof(kFpsVals[0]));
static const int kFpsDefault = 4;   // 默认 30 fps

static const wchar_t* kMainClass = L"LiteCapMainWnd";
static const wchar_t* kBarClass  = L"LiteCapBarWnd";
static const wchar_t* kSelClass  = L"LiteCapSelWnd";
static const wchar_t* kHotkeyClass = L"LiteCapHotkeyWnd";

// 全局热键：id 只要在本进程内唯一即可
static const int kHotkeyId = 0xB001;

// ---------------- 全局状态 ----------------
static HINSTANCE   g_inst = nullptr;
static HWND        g_main = nullptr;
static HWND        g_bar = nullptr;
static HWND        g_list = nullptr;
static HFONT       g_font = nullptr;
static HFONT       g_fontBold = nullptr;
static UINT        g_dpi = 96;
static Config      g_cfg;
static I18n        g_i18n;
static History     g_hist;
static Recorder    g_rec;
static bool        g_floatMode = false;
static bool        g_encShown = false;   // 本次录制是否已在标题栏标出编码器

// 录制记录是否展开。界面是两栏：左边一列设置项，右边一块录制记录。
// 收起时右栏整块消失、窗口收窄回单栏宽度 —— 只藏控件不缩窗口的话，
// 底下会留一大块空白，看着像界面坏了。
static bool        g_histOpen = true;
static int         g_listH = 0;          // 列表高度，按窗口高度自适应并记住

// 两栏的宽度。左栏固定，右栏是展开时额外加出来的宽度。
static int         g_colW = 0;           // 左栏（设置项）的客户区宽度
static int         g_rightW = 0;         // 右栏（录制记录）额外占用的宽度

// 主窗口控件
static HWND g_lblTarget, g_cmbTarget, g_btnPick, g_btnPickWin;
static HWND g_lblFps, g_cmbFps;
static HWND g_lblQuality, g_cmbQuality, g_lblLang, g_cmbLang;
// 编码器三选一。值和 recorder.h 的 EncMode 对应：
//   0 = Auto     优先硬件，失败自动退软件
//   1 = Hardware 强制硬件，失败就失败（宁可录不出来也不偷偷降级）
//   2 = Software 强制软件
// 硬件编码在 120fps 下追不上（见 recorder.cpp 的 kHardwareMaxFps），
// 所以这个选项不是摆设：测试和排障都靠它固定变量。
static HWND g_lblEncoder, g_cmbEncoder;
static HWND g_chkSystemAudio, g_chkMicrophone;
static HWND g_lblDir, g_edtDir, g_btnBrowse, g_btnRecord, g_btnFloat;
static HWND g_lblHotkey, g_edtHotkey, g_btnHotkey;
static HWND g_lblHist, g_lblHint;

// 目标下拉里的窗口条目。下标和 g_cmbTarget 里「窗口段」一一对应，
// 选中时直接按下标取，不用再去解析字符串。
static std::vector<WinItem> g_targetWins;

// 悬浮窗状态
static int  g_barDragX = 0, g_barDragY = 0;
static bool g_barDragging = false;
static std::wstring g_barTime = L"00:00";

static int S(int v) { return MulDiv(v, (int)g_dpi, 96); }

// ---------------- 区域框选 ----------------
namespace {

struct SelState {
    HWND  wnd = nullptr;
    HDC   memDC = nullptr;
    HBITMAP bmp = nullptr;
    int   vx = 0, vy = 0, vw = 0, vh = 0;   // 虚拟屏幕
    bool  dragging = false;
    POINT start = {}, cur = {};
    RECT  result = {};
    bool  ok = false;
};

SelState g_sel;

void SelPaint() {
    HDC dc = GetDC(g_sel.wnd);
    BitBlt(dc, 0, 0, g_sel.vw, g_sel.vh, g_sel.memDC, 0, 0, SRCCOPY);

    // 压暗整屏
    RECT full = { 0, 0, g_sel.vw, g_sel.vh };
    HBRUSH dim = CreateSolidBrush(RGB(0, 0, 0));
    // 用半透明的方式不可行于 GDI，这里改成四块遮罩留下选区
    RECT sel = {};
    if (g_sel.dragging) {
        sel.left   = (std::min)(g_sel.start.x, g_sel.cur.x);
        sel.top    = (std::min)(g_sel.start.y, g_sel.cur.y);
        sel.right  = (std::max)(g_sel.start.x, g_sel.cur.x);
        sel.bottom = (std::max)(g_sel.start.y, g_sel.cur.y);
    }
    if (g_sel.dragging && sel.right > sel.left && sel.bottom > sel.top) {
        RECT parts[4] = {
            { full.left, full.top, full.right, sel.top },
            { full.left, sel.bottom, full.right, full.bottom },
            { full.left, sel.top, sel.left, sel.bottom },
            { sel.right, sel.top, full.right, sel.bottom },
        };
        for (int i = 0; i < 4; ++i) {
            if (parts[i].right > parts[i].left && parts[i].bottom > parts[i].top)
                FillRect(dc, &parts[i], (HBRUSH)GetStockObject(GRAY_BRUSH));
        }
        HBRUSH frame = CreateSolidBrush(RGB(56, 160, 255));
        FrameRect(dc, &sel, frame);
        DeleteObject(frame);

        wchar_t info[64];
        swprintf(info, 64, L"%d x %d", sel.right - sel.left, sel.bottom - sel.top);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        TextOutW(dc, sel.left + 6, (std::max)(4, (int)sel.top - 22), info, (int)wcslen(info));
    } else {
        FillRect(dc, &full, (HBRUSH)GetStockObject(GRAY_BRUSH));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        HFONT f = CreateFontW(S(18), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                              0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HGDIOBJ old = SelectObject(dc, f);
        std::wstring tip = g_i18n.T("sel_hint");
        DrawTextW(dc, tip.c_str(), -1, &full, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, old);
        DeleteObject(f);
    }
    DeleteObject(dim);
    ReleaseDC(g_sel.wnd, dc);
}

LRESULT CALLBACK SelProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        EndPaint(h, &ps);
        SelPaint();
        return 0;
    }
    case WM_LBUTTONDOWN:
        g_sel.dragging = true;
        g_sel.start = g_sel.cur = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        SetCapture(h);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_MOUSEMOVE:
        if (g_sel.dragging) {
            g_sel.cur = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP: {
        g_sel.dragging = false;
        ReleaseCapture();
        RECT sel;
        sel.left   = (std::min)(g_sel.start.x, g_sel.cur.x);
        sel.top    = (std::min)(g_sel.start.y, g_sel.cur.y);
        sel.right  = (std::max)(g_sel.start.x, g_sel.cur.x);
        sel.bottom = (std::max)(g_sel.start.y, g_sel.cur.y);
        if (sel.right - sel.left >= 8 && sel.bottom - sel.top >= 8) {
            g_sel.result.left   = sel.left   + g_sel.vx;
            g_sel.result.top    = sel.top    + g_sel.vy;
            g_sel.result.right  = sel.right  + g_sel.vx;
            g_sel.result.bottom = sel.bottom + g_sel.vy;
            g_sel.ok = true;
        }
        DestroyWindow(h);
        return 0;
    }
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) { g_sel.ok = false; DestroyWindow(h); return 0; }
        return 0;
    case WM_DESTROY:
        if (g_sel.bmp) { DeleteObject(g_sel.bmp); g_sel.bmp = nullptr; }
        if (g_sel.memDC) { DeleteDC(g_sel.memDC); g_sel.memDC = nullptr; }
        g_sel.wnd = nullptr;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// 返回 true 表示用户选了区域
bool PickRegion(HWND owner, RECT& out) {
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    HDC screen = GetDC(nullptr);
    g_sel.memDC = CreateCompatibleDC(screen);
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = vw;
    bmi.bmiHeader.biHeight = -vh;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    g_sel.bmp = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(g_sel.memDC, g_sel.bmp);
    BitBlt(g_sel.memDC, 0, 0, vw, vh, screen, vx, vy, SRCCOPY);
    SelectObject(g_sel.memDC, old);
    ReleaseDC(nullptr, screen);

    g_sel.vx = vx; g_sel.vy = vy; g_sel.vw = vw; g_sel.vh = vh;
    g_sel.dragging = false;
    g_sel.ok = false;

    g_sel.wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kSelClass, L"",
                                WS_POPUP, vx, vy, vw, vh, owner, nullptr, g_inst, nullptr);
    if (!g_sel.wnd) return false;
    ShowWindow(g_sel.wnd, SW_SHOW);
    SetForegroundWindow(g_sel.wnd);
    SetFocus(g_sel.wnd);

    MSG msg;
    while (g_sel.wnd && GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_sel.ok) { out = g_sel.result; return true; }
    return false;
}

}  // namespace

// ---------------- 悬浮窗 ----------------
namespace {

const int BAR_W = 196, BAR_H = 40;
const int BAR_BTN_W = 96;
const int BAR_RESTORE_W = 30;

RECT BarRecordRect()  { return { 6, 6, 6 + BAR_BTN_W, BAR_H - 6 }; }
RECT BarRestoreRect() { return { BAR_W - BAR_RESTORE_W - 6, 6, BAR_W - 6, BAR_H - 6 }; }

void BarPaint(HWND h) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);

    RECT rc; GetClientRect(h, &rc);
    HBRUSH bg = CreateSolidBrush(RGB(26, 28, 32));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);

    SetBkMode(dc, TRANSPARENT);

    // 录制按钮
    RECT rb = BarRecordRect();
    HBRUSH btn = CreateSolidBrush(g_rec.Running() ? RGB(58, 63, 71) : RGB(214, 59, 59));
    HBRUSH oldB = (HBRUSH)SelectObject(dc, btn);
    RoundRect(dc, rb.left, rb.top, rb.right, rb.bottom, S(12), S(12));
    SelectObject(dc, oldB);
    DeleteObject(btn);

    SetTextColor(dc, RGB(255, 255, 255));
    std::wstring txt = g_rec.Running() ? (L"\u25A0 " + g_i18n.T("bar_stop"))
                                       : (L"\u25CF " + g_i18n.T("bar_start"));
    HFONT f = CreateFontW(S(13), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                          0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HGDIOBJ oldF = SelectObject(dc, f);
    DrawTextW(dc, txt.c_str(), -1, &rb, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // 计时
    RECT tb = { rb.right + 2, 0, BarRestoreRect().left, BAR_H };
    SetTextColor(dc, RGB(232, 234, 237));
    DrawTextW(dc, g_barTime.c_str(), -1, &tb, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // 还原按钮
    RECT xb = BarRestoreRect();
    SetTextColor(dc, RGB(195, 200, 207));
    DrawTextW(dc, L"\u25A3", -1, &xb, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    SelectObject(dc, oldF);
    DeleteObject(f);
    EndPaint(h, &ps);
}

void BarSavePos() {
    if (!g_bar) return;
    RECT r; GetWindowRect(g_bar, &r);
    wchar_t buf[64];
    swprintf(buf, 64, L"%d,%d", r.left, r.top);
    g_cfg.Set("float_pos", buf);
    g_cfg.Save();
}

void BarApplyPos() {
    std::wstring pos = g_cfg.Get("float_pos");
    int x = -1, y = -1;
    if (!pos.empty()) swscanf_s(pos.c_str(), L"%d,%d", &x, &y);
    if (x < 0 || y < 0) {
        RECT wa = {};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        x = wa.right - S(BAR_W) - S(24);
        y = wa.bottom - S(BAR_H) - S(24);
    }
    SetWindowPos(g_bar, HWND_TOPMOST, x, y, S(BAR_W), S(BAR_H),
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

LRESULT CALLBACK BarProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT:
        BarPaint(h);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        RECT rb = BarRecordRect();
        RECT xb = BarRestoreRect();
        if (PtInRect(&rb, pt)) {
            PostMessageW(g_main, WM_COMMAND, IDC_RECORD, 0);
        } else if (PtInRect(&xb, pt)) {
            PostMessageW(g_main, WM_COMMAND, IDC_FLOAT, 0);
        } else {
            g_barDragging = true;
            RECT wr; GetWindowRect(h, &wr);
            g_barDragX = pt.x - wr.left;
            g_barDragY = pt.y - wr.top;
            SetCapture(h);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_barDragging) {
            POINT p; GetCursorPos(&p);
            SetWindowPos(h, HWND_TOPMOST, p.x - g_barDragX, p.y - g_barDragY,
                         0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_barDragging) { g_barDragging = false; ReleaseCapture(); BarSavePos(); }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void BarShow(bool show) {
    if (!g_bar) {
        g_bar = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kBarClass, L"LiteCap",
                                WS_POPUP, 0, 0, S(BAR_W), S(BAR_H),
                                nullptr, nullptr, g_inst, nullptr);
        if (!g_bar) return;
        HRGN rgn = CreateRoundRectRgn(0, 0, S(BAR_W) + 1, S(BAR_H) + 1, S(18), S(18));
        SetWindowRgn(g_bar, rgn, TRUE);
        BarApplyPos();
    }
    ShowWindow(g_bar, show ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (show) { InvalidateRect(g_bar, nullptr, TRUE); SetWindowPos(g_bar, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW); }
}

}  // namespace

// ---------------- 主窗口辅助 ----------------
namespace {

std::vector<RECT> g_monitors;

void RefreshMonitors() {
    g_monitors.clear();
    EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR, HDC, LPRECT r, LPARAM) -> BOOL {
            g_monitors.push_back(*r);
            return TRUE;
        }, 0);
}

// 目标下拉里「窗口段」之前的固定条目数：
//   0          全部屏幕
//   1..N       显示器 1..N
//   N+1        区域 W×H（只有框选过才有这一条）
// 之后的条目就是 g_targetWins[sel - TargetFixedCount()]。
int TargetFixedCount() {
    return 1 + (int)g_monitors.size() + (g_cfg.Get("region").empty() ? 0 : 1);
}

void RefreshTargetCombo() {
    const std::wstring t      = g_cfg.Get("target", L"all");
    const std::wstring wtitle = g_cfg.Get("window_title");
    const std::wstring wexe   = g_cfg.Get("window_exe");
    const bool hasRegion = !g_cfg.Get("region").empty();

    SendMessageW(g_cmbTarget, CB_RESETCONTENT, 0, 0);
    g_targetWins.clear();

    SendMessageW(g_cmbTarget, CB_ADDSTRING, 0, (LPARAM)g_i18n.T("target_all").c_str());
    for (size_t i = 0; i < g_monitors.size(); ++i) {
        wchar_t buf[128];
        const RECT& r = g_monitors[i];
        swprintf(buf, 128, L"%s %d  (%d\u00D7%d)",
                 g_i18n.T("target_monitor").c_str(), (int)i + 1,
                 r.right - r.left, r.bottom - r.top);
        SendMessageW(g_cmbTarget, CB_ADDSTRING, 0, (LPARAM)buf);
    }
    if (hasRegion)
        SendMessageW(g_cmbTarget, CB_ADDSTRING, 0, (LPARAM)g_i18n.T("target_region").c_str());

    const int fixed = TargetFixedCount();

    // 标题 + 进程名都一样的窗口合并成一条。有些程序（记事本多开、浏览器多窗口）
    // 会开一堆同标题窗口，全列出来只会把下拉撑爆，而它们录出来其实没区别。
    for (const WinItem& w : EnumCaptureWindows()) {
        bool dup = false;
        for (const WinItem& p : g_targetWins)
            if (p.title == w.title && p.exe == w.exe) { dup = true; break; }
        if (dup) continue;

        wchar_t buf[768];
        swprintf(buf, 768, L"%s: %s (%s)", g_i18n.T("target_window").c_str(),
                 w.title.c_str(), w.exe.c_str());
        SendMessageW(g_cmbTarget, CB_ADDSTRING, 0, (LPARAM)buf);
        g_targetWins.push_back(w);
    }

    int sel = 0;
    if (t == L"region" && hasRegion) {
        sel = (int)g_monitors.size() + 1;
    } else if (t.rfind(L"mon:", 0) == 0) {
        int idx = _wtoi(t.c_str() + 4);
        if (idx >= 1 && idx <= (int)g_monitors.size()) sel = idx;
    } else if (t == L"window" && !wtitle.empty()) {
        for (size_t i = 0; i < g_targetWins.size(); ++i) {
            if (g_targetWins[i].title == wtitle && g_targetWins[i].exe == wexe) {
                sel = fixed + (int)i;
                break;
            }
        }
        if (sel == 0) {
            // 上次录的那个窗口现在没开。补一条占位，别让选择悄悄跳回「全部屏幕」——
            // 那会导致用户以为还在录那个程序，实际录了整个屏幕。
            std::wstring label = g_i18n.T("target_window") + L": " + wtitle;
            if (!wexe.empty()) label += L" (" + wexe + L")";
            label += L" " + g_i18n.T("target_window_off");
            SendMessageW(g_cmbTarget, CB_ADDSTRING, 0, (LPARAM)label.c_str());
            WinItem ph;
            ph.title = wtitle;
            ph.exe = wexe;
            g_targetWins.push_back(ph);
            sel = fixed + (int)g_targetWins.size() - 1;
        }
    }
    SendMessageW(g_cmbTarget, CB_SETCURSEL, sel, 0);
}

void RefreshLangCombo() {
    SendMessageW(g_cmbLang, CB_RESETCONTENT, 0, 0);
    for (const auto& l : g_i18n.Languages())
        SendMessageW(g_cmbLang, CB_ADDSTRING, 0, (LPARAM)l.name.c_str());
    int sel = 0;
    const auto& langs = g_i18n.Languages();
    for (size_t i = 0; i < langs.size(); ++i)
        if (langs[i].code == g_i18n.Current()) sel = (int)i;
    SendMessageW(g_cmbLang, CB_SETCURSEL, sel, 0);
}

// LVITEMW 的一次性构造。
// 注意：C++20 起不允许对临时对象取地址（C2102），
// 所以不能再写 &(LVITEMW{...})，得先落到一个局部变量上。
LVITEMW MakeLvItem(int sub, LPWSTR text) {
    LVITEMW li = {};
    li.mask = LVIF_TEXT;
    li.iSubItem = sub;
    li.pszText = text;
    return li;
}

// 录制记录列表的列。
// 只有三列：文件名里本来就带时间戳（LiteCap_20261015_052526.mp4），
// 再单开一列「录制时间」纯属浪费 —— 界面收窄一半之后横向空间得省着用。
void ApplyListColumns() {
    if (!g_list) return;
    while (SendMessageW(g_list, LVM_DELETECOLUMN, 0, 0)) {}
    LVCOLUMNW col = {};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    const char* keys[3] = { "col_name", "col_duration", "col_size" };
    const int widths[3] = { 150, 58, 72 };      // 占位值，SizeListColumns 会按实际宽度重算
    for (int i = 0; i < 3; ++i) {
        std::wstring txt = g_i18n.T(keys[i]);
        col.iSubItem = i;
        col.pszText = (LPWSTR)txt.c_str();
        col.cx = S(widths[i]);
        SendMessageW(g_list, LVM_INSERTCOLUMNW, i, (LPARAM)&col);
    }
}

// 按列表控件的实际宽度分配列宽。用 LVM_SETCOLUMNWIDTH 而不是删了重建，
// 后者在拖窗口的时候会闪。
void SizeListColumns() {
    if (!g_list) return;
    RECT lr = {};
    if (!GetClientRect(g_list, &lr) || lr.right <= 0) return;
    const int avail = lr.right - S(6);          // 给竖向滚动条留一点
    int wName = avail * 56 / 100;
    int wDur  = avail * 20 / 100;
    int wSize = avail - wName - wDur;
    if (wName < S(60)) wName = S(60);
    SendMessageW(g_list, LVM_SETCOLUMNWIDTH, 0, wName);
    SendMessageW(g_list, LVM_SETCOLUMNWIDTH, 1, wDur);
    SendMessageW(g_list, LVM_SETCOLUMNWIDTH, 2, wSize);
}

void RefreshHistoryList() {
    SendMessageW(g_list, LVM_DELETEALLITEMS, 0, 0);
    const auto& items = g_hist.Items();
    for (size_t i = 0; i < items.size(); ++i) {
        const RecItem& it = items[i];
        LVITEMW lv = {};
        lv.mask = LVIF_TEXT;
        lv.iItem = (int)i;
        lv.pszText = (LPWSTR)it.name.c_str();
        int row = (int)SendMessageW(g_list, LVM_INSERTITEMW, 0, (LPARAM)&lv);

        std::wstring dur = FormatDuration(it.durationMs);
        std::wstring sz = FormatBytes(it.bytes);
        LVITEMW c1 = MakeLvItem(1, (LPWSTR)dur.c_str());
        LVITEMW c2 = MakeLvItem(2, (LPWSTR)sz.c_str());
        SendMessageW(g_list, LVM_SETITEMTEXTW, row, (LPARAM)&c1);
        SendMessageW(g_list, LVM_SETITEMTEXTW, row, (LPARAM)&c2);
    }
}

// ---------------- 全局热键 ----------------
static UINT g_hotkeyMod = MOD_CONTROL | MOD_ALT;
static UINT g_hotkeyVk  = 'R';

// 拼成 "Ctrl+Alt+R" 这样的可读文本
std::wstring FormatHotkey(UINT mod, UINT vk) {
    if (!vk) return g_i18n.T("hotkey_none");
    std::wstring s;
    if (mod & MOD_CONTROL) s += L"Ctrl+";
    if (mod & MOD_ALT)     s += L"Alt+";
    if (mod & MOD_SHIFT)   s += L"Shift+";
    if (mod & MOD_WIN)     s += L"Win+";

    // GetKeyNameText 要的是扫描码左移 16 位；小键盘和这几个键还得带上扩展位，
    // 否则 NumLock 会被念成 "Num 5" 之类的怪名字。
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LONG lp = (LONG)(sc << 16);
    switch (vk) {
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
    case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME:
    case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK:
        lp |= 1 << 24;
        break;
    default: break;
    }
    wchar_t name[64] = {};
    if (GetKeyNameTextW(lp, name, 64) > 0) {
        s += name;
    } else {
        wchar_t b[16];
        if (vk >= VK_F1 && vk <= VK_F24)       swprintf(b, 16, L"F%u", vk - VK_F1 + 1);
        else if ((vk >= '0' && vk <= '9') ||
                 (vk >= 'A' && vk <= 'Z'))     swprintf(b, 16, L"%c", (wchar_t)vk);
        else                                   swprintf(b, 16, L"0x%02X", vk);
        s += b;
    }
    return s;
}

void UpdateHotkeyText() {
    if (!g_edtHotkey) return;
    SetWindowTextW(g_edtHotkey, FormatHotkey(g_hotkeyMod, g_hotkeyVk).c_str());
}

// 注册失败基本都是组合已被别的程序占了，返回 false 交给调用方提示。
bool RegisterAppHotkey() {
    UnregisterHotKey(g_main, kHotkeyId);
    if (!g_hotkeyVk) return true;    // 空 = 用户主动清掉，不注册
    return RegisterHotKey(g_main, kHotkeyId, g_hotkeyMod | MOD_NOREPEAT,
                          g_hotkeyVk) != FALSE;
}

// ---------------- 快捷键录制窗口 ----------------
static UINT g_hkMod = 0, g_hkKey = 0;
static bool g_hkDone = false, g_hkOk = false;

LRESULT CALLBACK HotkeyProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        UINT vk = (UINT)w;
        // Alt+F4 是系统级关闭命令，不要误记录成「Alt+F4」热键。
        // 按常规窗口关闭路径收尾，让 PickHotkey 恢复 owner 并释放模态状态。
        if (m == WM_SYSKEYDOWN && vk == VK_F4 && (l & (1L << 29))) {
            SendMessageW(h, WM_CLOSE, 0, 0);
            return 0;
        }
        if (vk == VK_ESCAPE) { g_hkDone = true; g_hkOk = false; return 0; }
        if (vk == VK_BACK || vk == VK_DELETE) {
            g_hkMod = 0; g_hkKey = 0; g_hkDone = true; g_hkOk = true;
            return 0;
        }
        // 只按修饰键不算数，继续等真正的那个键
        if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU ||
            vk == VK_LWIN || vk == VK_RWIN) return 0;

        UINT mod = 0;
        if (GetKeyState(VK_CONTROL) & 0x8000) mod |= MOD_CONTROL;
        if (GetKeyState(VK_SHIFT)   & 0x8000) mod |= MOD_SHIFT;
        if (GetKeyState(VK_MENU)    & 0x8000) mod |= MOD_ALT;
        if ((GetKeyState(VK_LWIN) & 0x8000) ||
            (GetKeyState(VK_RWIN) & 0x8000))  mod |= MOD_WIN;
        g_hkMod = mod; g_hkKey = vk; g_hkDone = true; g_hkOk = true;
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));

        HGDIOBJ old = SelectObject(dc, g_fontBold);
        RECT r1 = rc; r1.top += S(26);
        std::wstring t = g_i18n.T("hotkey_capture_hint");
        DrawTextW(dc, t.c_str(), -1, &r1, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);

        SelectObject(dc, g_font);
        RECT r2 = rc; r2.top += S(62);
        std::wstring s = g_i18n.T("hotkey_capture_sub");
        DrawTextW(dc, s.c_str(), -1, &r2, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, old);
        EndPaint(h, &ps);
        return 0;
    }

    case WM_CLOSE:
        // 点标题栏 X / 按 Alt+F4 时，必须让模态循环收尾。
        // 仅走 DefWindowProc 会销毁对话框但 g_hkDone 仍为 false；
        // 主窗口会一直保持禁用，消息循环卡死，连退出按钮也点不了。
        g_hkOk = false;
        g_hkDone = true;
        return 0;

    case WM_DESTROY:
        // 兜住意外 DestroyWindow：不能把父窗口永久留在禁用状态。
        if (!g_hkDone) {
            g_hkOk = false;
            g_hkDone = true;
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// 模态地录一个单键或组合键；返回 false 表示用户取消
bool PickHotkey(HWND owner, UINT& mod, UINT& vk) {
    g_hkMod = 0; g_hkKey = 0; g_hkDone = false; g_hkOk = false;

    RECT orc; GetWindowRect(owner, &orc);
    int w = S(440), hh = S(150);
    int x = orc.left + ((orc.right - orc.left) - w) / 2;
    int y = orc.top + ((orc.bottom - orc.top) - hh) / 2;

    HWND dlg = CreateWindowExW(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME, kHotkeyClass, L"",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU,
                               x, y, w, hh, owner, nullptr, g_inst, nullptr);
    if (!dlg) return false;

    const bool ownerWasEnabled = IsWindowEnabled(owner) != FALSE;
    EnableWindow(owner, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetForegroundWindow(dlg);
    SetFocus(dlg);

    // 嵌套模态消息循环。WM_CLOSE 会由 HotkeyProc 标记完成，随后统一恢复父窗；
    // GetMessageW 返回 0/-1 时也必须走同一清理路径，否则父窗会一直禁用。
    MSG msg = {};
    int gm = 0;
    while (!g_hkDone && (gm = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (gm == 0) {
        // 不吞掉应用主消息循环的退出请求。
        PostQuitMessage((int)msg.wParam);
        g_hkOk = false;
    } else if (gm < 0) {
        g_hkOk = false;
    }

    if (IsWindow(dlg)) DestroyWindow(dlg);
    if (IsWindow(owner)) {
        EnableWindow(owner, ownerWasEnabled ? TRUE : FALSE);
        if (ownerWasEnabled) {
            SetActiveWindow(owner);
            SetForegroundWindow(owner);
            SetFocus(owner);
        }
    }

    if (g_hkOk) { mod = g_hkMod; vk = g_hkKey; }
    return g_hkOk;
}

void ApplyFont(HWND c) { SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE); }

// 主窗口标题。录制中会追加编码器类型，见 OnRecEvent 的 WM_REC_TICK 分支。
void SetMainTitle(const wchar_t* suffix = nullptr) {
    std::wstring title = std::wstring(L"LiteCap 1.0  \u00B7  ") + g_i18n.T("app_subtitle");
    if (suffix && *suffix) title += std::wstring(L"  \u2014  ") + suffix;
    SetWindowTextW(g_main, title.c_str());
}

// 录制记录标题栏上的小三角：◂ 展开 / ▸ 收起
//
// 原来是 ▾/▸（向下 / 向右）—— 那是列表在下方时的说法。
// 现在改成两栏、列表往右展开，箭头也得跟着横过来：
//   ▸ 表示「点一下会从右边长出来」，◂ 表示「点一下会折回左边」。
// 继续挂个向下的三角，会让人以为列表是往下弹的。
void UpdateHistoryHeader() {
    if (!g_lblHist) return;
    std::wstring t = g_histOpen ? L"\u25C2  " : L"\u25B8  ";
    t += g_i18n.T("history");
    SetWindowTextW(g_lblHist, t.c_str());
}

// 录制按钮上的文字。
// 编码器类型本来只挂在标题栏，但界面收窄之后标题被截成「LiteCap 1.0 · 极简离…」，
// 那行提示就白写了 —— 挪到按钮上，宽度足够，而且录制时视线本来就在这儿。
// 「区域模式」也挂这儿：那意味着这次没走窗口捕获、遮挡会进画面，
// 属于用户必须知道的事，不能偷偷降级。
std::wstring RecordButtonText() {
    if (!g_rec.Running()) return g_i18n.T("start");
    std::wstring s = g_i18n.T("stop");
    if (g_encShown)
        s += std::wstring(L"  \u00B7  ") +
             (g_rec.LastHardware() ? g_i18n.T("enc_gpu") : g_i18n.T("enc_cpu"));
    if (g_rec.RegionFallback())
        s += std::wstring(L"  \u00B7  ") + g_i18n.T("cap_region");
    return s;
}

void UpdateRecordButton() {
    if (!g_btnRecord) return;
    SetWindowTextW(g_btnRecord, RecordButtonText().c_str());
}

void Retranslate() {
    SetMainTitle();
    SetWindowTextW(g_lblTarget, g_i18n.T("target").c_str());
    SetWindowTextW(g_lblFps, g_i18n.T("fps").c_str());
    SetWindowTextW(g_lblQuality, g_i18n.T("quality").c_str());
    SetWindowTextW(g_lblEncoder, g_i18n.T("encoder").c_str());
    SetWindowTextW(g_chkSystemAudio, g_i18n.T("audio_system").c_str());
    SetWindowTextW(g_chkMicrophone, g_i18n.T("audio_microphone").c_str());
    SetWindowTextW(g_lblLang, g_i18n.T("language").c_str());
    SetWindowTextW(g_lblDir, g_i18n.T("output_dir").c_str());
    SetWindowTextW(g_lblHotkey, g_i18n.T("hotkey").c_str());
    SetWindowTextW(g_btnHotkey, g_i18n.T("hotkey_set").c_str());
    UpdateHistoryHeader();
    SetWindowTextW(g_lblHint, g_i18n.T("history_hint").c_str());
    SetWindowTextW(g_btnPick, g_i18n.T("pick_region").c_str());
    SetWindowTextW(g_btnPickWin, g_i18n.T("pick_window").c_str());
    SetWindowTextW(g_btnBrowse, g_i18n.T("browse").c_str());
    SetWindowTextW(g_btnFloat, g_i18n.T("float_mode").c_str());
    UpdateRecordButton();

    // 画质下拉
    int qs = (int)SendMessageW(g_cmbQuality, CB_GETCURSEL, 0, 0);
    SendMessageW(g_cmbQuality, CB_RESETCONTENT, 0, 0);
    SendMessageW(g_cmbQuality, CB_ADDSTRING, 0, (LPARAM)g_i18n.T("quality_high").c_str());
    SendMessageW(g_cmbQuality, CB_ADDSTRING, 0, (LPARAM)g_i18n.T("quality_medium").c_str());
    SendMessageW(g_cmbQuality, CB_ADDSTRING, 0, (LPARAM)g_i18n.T("quality_low").c_str());
    SendMessageW(g_cmbQuality, CB_SETCURSEL, qs < 0 ? 0 : qs, 0);

    // 编码器下拉。三项顺序必须和 EncMode 的枚举值一致：
    //   Auto / Hardware / Software —— recorder.cpp 里按下标取名。
    int es = (int)SendMessageW(g_cmbEncoder, CB_GETCURSEL, 0, 0);
    SendMessageW(g_cmbEncoder, CB_RESETCONTENT, 0, 0);
    SendMessageW(g_cmbEncoder, CB_ADDSTRING, 0, (LPARAM)g_i18n.T("enc_opt_auto").c_str());
    SendMessageW(g_cmbEncoder, CB_ADDSTRING, 0, (LPARAM)g_i18n.T("enc_opt_hardware").c_str());
    SendMessageW(g_cmbEncoder, CB_ADDSTRING, 0, (LPARAM)g_i18n.T("enc_opt_software").c_str());
    SendMessageW(g_cmbEncoder, CB_SETCURSEL, es < 0 ? 0 : es, 0);

    // 列：LVM_SETCOLUMNW 只能改已有列，创建列必须用 LVM_INSERTCOLUMNW。
    // 切换语言时要把旧列删掉重建，否则表头文字不会更新。
    ApplyListColumns();

    RefreshTargetCombo();
    RefreshLangCombo();
    UpdateHotkeyText();
    if (g_bar) InvalidateRect(g_bar, nullptr, TRUE);
}

// 排版。返回值是「当前状态下理想的客户区高度」——
// 收起/展开记录区的时候要靠它把窗口收到刚好，否则底下会留一块空白。
//
// 两栏结构：
//   左栏  设置项，宽度固定（g_colW），窗口再宽也不跟着拉伸
//   右栏  录制记录，占用展开时额外加出来的那部分宽度
// 左栏固定宽度是刻意的：控件宽度忽宽忽窄很难看，而且下拉、编辑框
// 在窄宽度下本来就够用了。多出来的空间给列表更划算。
//
// fromResize=true 表示这次是用户真的在拖窗口，可以按实际高度重新分配列表；
// false 表示这次只是为了算高度，列表高度必须用存下来的值，不能再从
// rc.bottom 反推 —— 那样算出来的高度依赖当前窗口尺寸，会自相矛盾。
int LayoutMain(bool fromResize) {
    RECT rc;
    GetClientRect(g_main, &rc);
    const int W = rc.right;
    const int pad = S(12);
    const int rowH = S(26);
    const int gap = S(8);
    const int lw = S(52);                 // 左栏标签列
    const int fx = pad + lw;
    const int colContentW = g_colW - pad * 2;
    const int fw = colContentW - lw;      // 左栏控件宽度
    const int hintH = S(34);              // 提示两行
    int y = S(12);

    auto place = [&](HWND c, int x, int yy, int w, int h) {
        MoveWindow(c, x, yy, w, h, TRUE);
    };

    // ---- 目标：下拉独占一行，下面一行放两个按钮 ----
    // ComboBox 的 height 参数指的是「下拉列表 + 文本区」的总高度，
    // 只给 rowH 会让列表部分≈0，展开后一项都看不到。这里留足列表空间。
    place(g_lblTarget, pad, y + S(4), lw, rowH);
    place(g_cmbTarget, fx, y, fw, rowH + S(180));
    y += rowH + S(4);

    const int halfW = (fw - gap) / 2;
    place(g_btnPick,    fx, y, halfW, rowH);
    place(g_btnPickWin, fx + halfW + gap, y, fw - halfW - gap, rowH);
    y += rowH + gap + S(2);

    // ---- 帧率 / 画质 / 语言 ----
    place(g_lblFps, pad, y + S(4), lw, rowH);
    place(g_cmbFps, fx, y, fw, rowH + S(180));
    y += rowH + gap;

    place(g_lblQuality, pad, y + S(4), lw, rowH);
    place(g_cmbQuality, fx, y, fw, rowH + S(180));
    y += rowH + gap;

    // 编码器放在画质后面：两者都是「录出来的文件长什么样」的参数，
    // 放一块儿才好理解。窗口右栏只放录制参数，所以这里只占一行。
    place(g_lblEncoder, pad, y + S(4), lw, rowH);
    place(g_cmbEncoder, fx, y, fw, rowH + S(180));
    y += rowH + gap;

    // 两个独立音源开关。默认开系统声音、关麦克风；不会强制要求二选一。
    const int audioHalf = fw / 2;
    place(g_chkSystemAudio, fx, y, audioHalf, rowH);
    place(g_chkMicrophone, fx + audioHalf, y, fw - audioHalf, rowH);
    y += rowH + gap;

    place(g_lblLang, pad, y + S(4), lw, rowH);
    place(g_cmbLang, fx, y, fw, rowH + S(180));
    y += rowH + gap + S(2);

    // ---- 输出目录 / 快捷键 ----
    // 这两行把「浏览…/设置…」放到标签行右侧，编辑框就能占满整行 ——
    // 宽度减半之后，路径框再被按钮挤掉 60px 就什么都看不见了。
    const int smW = S(58), smH = S(22);
    place(g_lblDir, pad, y, lw + S(70), S(20));
    place(g_btnBrowse, pad + colContentW - smW, y, smW, smH);
    y += S(24);
    place(g_edtDir, pad, y, colContentW, rowH);
    y += rowH + gap + S(2);

    place(g_lblHotkey, pad, y, lw + S(70), S(20));
    place(g_btnHotkey, pad + colContentW - smW, y, smW, smH);
    y += S(24);
    // 用 ES_READONLY 而不是禁用：禁用会变灰，看着像功能坏了
    place(g_edtHotkey, pad, y, colContentW, rowH);
    y += rowH + gap + S(4);

    // ---- 录制 / 悬浮窗 ----
    const int floatW = S(76);
    place(g_btnRecord, pad, y, colContentW - floatW - gap, S(38));
    place(g_btnFloat, pad + colContentW - floatW, y, floatW, S(38));
    y += S(38) + S(12);

    // ---- 录制记录的开关（留在左栏底部，位置固定，不跟着展开状态跳来跳去）----
    place(g_lblHist, pad, y, colContentW, S(22));
    y += S(24);
    const int leftNeed = y + pad;

    // 双保险：v6 下让下拉列表至少显示 12 项（v5 会失败，靠上面的高度兜底）
    HWND combos[] = { g_cmbTarget, g_cmbFps, g_cmbQuality, g_cmbEncoder, g_cmbLang };
    for (HWND c : combos) SendMessageW(c, CB_SETMINVISIBLE, 12, 0);

    // 下拉框宽度严格跟控件一致，确保列表不越过主窗口右边框；
    // 窗口标题太长时宁可截断，也不让下拉横跨出界。
    SendMessageW(g_cmbTarget, CB_SETDROPPEDWIDTH, fw, 0);

    // ---- 右栏 ----
    if (g_histOpen) {
        ShowWindow(g_list, SW_SHOW);
        ShowWindow(g_lblHint, SW_SHOW);
        const int rightX = pad + colContentW + S(14);
        const int rightW = W - rightX - pad;
        const int top = S(12);

        int listH = g_listH;
        if (fromResize) {
            int avail = rc.bottom - top - hintH - S(4) - pad;
            if (avail >= S(80)) listH = avail;
        }
        if (listH < S(80)) listH = S(80);
        g_listH = listH;

        place(g_list, rightX, top, rightW, listH);
        // 提示文案按窄宽度会折成两行，给一行的高度它就直接被切掉了
        place(g_lblHint, rightX, top + listH + S(4), rightW, hintH);
        SizeListColumns();
        return (std::max)(leftNeed, top + listH + S(4) + hintH + pad);
    }

    ShowWindow(g_list, SW_HIDE);
    ShowWindow(g_lblHint, SW_HIDE);
    return leftNeed;
}

// 窗口外框和客户区之间的差（标题栏 + 边框），换算窗口尺寸时要用
void GetChromeSize(int& cx, int& cy) {
    RECT wr = {}, cr = {};
    GetWindowRect(g_main, &wr);
    GetClientRect(g_main, &cr);
    cx = (wr.right - wr.left) - cr.right;
    cy = (wr.bottom - wr.top) - cr.bottom;
}

// 收起 / 展开录制记录。
// 展开时右栏是「额外加出来」的一块，所以宽度和高度都得跟着变：
// 收起 = 单栏窄窗口，展开 = 左栏 + 右栏列表。
void ToggleHistory() {
    g_histOpen = !g_histOpen;
    UpdateHistoryHeader();
    g_cfg.Set("history_open", g_histOpen ? L"1" : L"0");
    g_cfg.Save();

    int chromeW = 0, chromeH = 0;
    GetChromeSize(chromeW, chromeH);

    const int need = LayoutMain(false);   // 先按新状态排一遍，拿到理想客户区尺寸
    const int clientW = g_colW + (g_histOpen ? g_rightW : 0);

    RECT wr = {};
    GetWindowRect(g_main, &wr);
    SetWindowPos(g_main, nullptr, wr.left, wr.top, clientW + chromeW, need + chromeH,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    LayoutMain(true);                     // 尺寸变了，再排一次把列表撑到新高度
}

void ApplyRecordingUI(bool recording) {
    UpdateRecordButton();
    EnableWindow(g_btnRecord, TRUE);
    EnableWindow(g_cmbTarget, !recording);
    EnableWindow(g_cmbFps, !recording);
    EnableWindow(g_cmbQuality, !recording);
    EnableWindow(g_cmbEncoder, !recording);
    EnableWindow(g_chkSystemAudio, !recording);
    EnableWindow(g_chkMicrophone, !recording);
    EnableWindow(g_cmbLang, !recording);
    EnableWindow(g_edtDir, !recording);
    EnableWindow(g_btnBrowse, !recording);
    EnableWindow(g_btnPick, !recording);
    EnableWindow(g_btnPickWin, !recording);
    if (g_bar) InvalidateRect(g_bar, nullptr, TRUE);
}

void StartRecording() {
    // 保存当前设置
    const int sel = (int)SendMessageW(g_cmbTarget, CB_GETCURSEL, 0, 0);
    const int fixed = TargetFixedCount();

    RecTarget rt;
    std::wstring target = L"all";
    if (sel > 0 && sel < fixed) {
        if (sel <= (int)g_monitors.size()) {
            wchar_t buf[32]; swprintf(buf, 32, L"mon:%d", sel); target = buf;
            rt.region = g_monitors[sel - 1];
        } else {
            target = L"region";
            std::wstring rs = g_cfg.Get("region");
            int a = 0, b = 0, c = 0, d = 0;
            if (swscanf_s(rs.c_str(), L"%d,%d,%d,%d", &a, &b, &c, &d) != 4) {
                MessageBoxW(g_main, g_i18n.T("err_no_region").c_str(), L"LiteCap", MB_ICONWARNING);
                return;
            }
            rt.region = { a, b, a + c, b + d };
        }
    } else if (sel >= fixed) {
        const int wi = sel - fixed;
        if (wi < 0 || wi >= (int)g_targetWins.size()) return;
        const WinItem& w = g_targetWins[wi];

        // 配置里存的是标题 + 进程名，不是 hwnd —— hwnd 重启就变，存了也没用。
        HWND hw = FindWindowByTitleAndExe(w.title, w.exe);
        if (!hw) {
            MessageBoxW(g_main, g_i18n.T("err_no_window").c_str(), L"LiteCap", MB_ICONWARNING);
            return;
        }
        target = L"window";
        rt.window = true;
        rt.hwnd = hw;
        g_cfg.Set("window_title", w.title);
        g_cfg.Set("window_exe", w.exe);
    } else {
        rt.region.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
        rt.region.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
        rt.region.right = rt.region.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        rt.region.bottom = rt.region.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    }
    g_cfg.Set("target", target);

    wchar_t dir[1024] = {};
    GetWindowTextW(g_edtDir, dir, 1024);
    std::wstring outDir = dir[0] ? dir : DefaultOutputDir();
    g_cfg.Set("output_dir", outDir);

    int fpsSel = (int)SendMessageW(g_cmbFps, CB_GETCURSEL, 0, 0);
    int fps = kFpsVals[(fpsSel < 0 || fpsSel >= kFpsCount) ? kFpsDefault : fpsSel];
    g_cfg.Set("fps", std::to_wstring(fps));

    int q = (int)SendMessageW(g_cmbQuality, CB_GETCURSEL, 0, 0);
    const char* qk[] = { "high", "medium", "low" };
    std::string quality = qk[q < 0 ? 0 : q];
    g_cfg.Set("quality", U82W(quality));

    // 编码器：三项的顺序和 recorder.h 的 EncMode 一致。
    // 这是录屏质量/兼容性的关键开关：NVENC 在 120fps 下追不上，
    // 个别驱动上硬件编码还可能直接失败，所以必须能手动固定。
    int e = (int)SendMessageW(g_cmbEncoder, CB_GETCURSEL, 0, 0);
    const char* ek[] = { "auto", "hardware", "software" };
    std::string encMode = ek[e < 0 ? 0 : e];
    g_cfg.Set("encoder", U82W(encMode));
    const bool systemAudio = SendMessageW(g_chkSystemAudio, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool microphoneAudio = SendMessageW(g_chkMicrophone, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_cfg.Set("audio_system", systemAudio ? L"1" : L"0");
    g_cfg.Set("audio_microphone", microphoneAudio ? L"1" : L"0");
    g_cfg.Save();

    g_rec.SetEncoderMode(encMode);

    // 目录
    std::wstring mk = outDir;
    if (!CreateDirectoryW(mk.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        // 尝试逐级创建
        for (size_t i = 3; i <= mk.size(); ++i) {
            if (mk[i] == L'\\' || i == mk.size()) {
                std::wstring sub = mk.substr(0, i);
                CreateDirectoryW(sub.c_str(), nullptr);
            }
        }
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[128];
    swprintf(name, 128, L"LiteCap_%04d%02d%02d_%02d%02d%02d.mp4",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::wstring outPath = JoinPath(outDir, name);

    if (!g_rec.Start(g_main, rt, fps, quality, outPath, systemAudio, microphoneAudio)) {
        MessageBoxW(g_main, g_i18n.T("err_start").c_str(), L"LiteCap", MB_ICONERROR);
        return;
    }
    g_barTime = L"00:00";
    g_encShown = false;          // 新一次录制，编码器类型等第一帧再标
    ApplyRecordingUI(true);
}

void StopRecording() {
    EnableWindow(g_btnRecord, FALSE);
    if (g_bar) InvalidateRect(g_bar, nullptr, TRUE);
    g_rec.Stop();
}

void OnRecEvent(WPARAM wp, LPARAM lp) {
    RecEvent* ev = (RecEvent*)lp;
    if (!ev) return;
    switch (wp) {
    case WM_REC_TICK:
        g_barTime = FormatDuration((long long)(ev->elapsed * 1000));
        if (g_bar) InvalidateRect(g_bar, nullptr, TRUE);
        // 编码器是录制线程里异步打开的，到第一帧才有结果。顺便标出这次走的是
        // 硬件还是软件 —— 否则"自动回退"对用户来说就是看不见的魔法。
        // 提示放在录制按钮上而不是标题栏：窗口收窄之后标题会被截掉，看不见。
        if (!g_encShown) {
            g_encShown = true;
            SetMainTitle(g_rec.LastHardware() ? g_i18n.T("enc_gpu").c_str()
                                              : g_i18n.T("enc_cpu").c_str());
            UpdateRecordButton();
        }
        break;
    case WM_REC_DONE: {
        RecItem it;
        it.epoch = (long long)time(nullptr);
        it.durationMs = (long long)(ev->elapsed * 1000);
        it.bytes = FileSize(ev->path);
        it.path = ev->path;
        size_t slash = it.path.find_last_of(L"\\/");
        it.name = (slash == std::wstring::npos) ? it.path : it.path.substr(slash + 1);
        g_hist.Add(it);
        RefreshHistoryList();
        g_barTime = L"00:00";
        g_encShown = false;
        SetMainTitle();
        ApplyRecordingUI(false);
        break;
    }
    case WM_REC_FAIL: {
        std::wstring reason = ev->error;
        const wchar_t* sysFail = L"system audio capture init failed";
        const wchar_t* micFail = L"microphone capture init failed";
        if (reason.rfind(sysFail, 0) == 0)
            reason = g_i18n.T("err_audio_system") + reason.substr(wcslen(sysFail));
        else if (reason.rfind(micFail, 0) == 0)
            reason = g_i18n.T("err_audio_microphone") + reason.substr(wcslen(micFail));
        MessageBoxW(g_main, (g_i18n.T("failed") + L"\n" + reason).c_str(),
                    L"LiteCap", MB_ICONERROR);
        g_barTime = L"00:00";
        g_encShown = false;
        SetMainTitle();
        ApplyRecordingUI(false);
        break;
    }
    }
    delete ev;
}

// ---- 录制记录：右键菜单 ----
void ShowListMenu(HWND hwnd, int index) {
    if (index < 0 || index >= (int)g_hist.Count()) return;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_PLAY, g_i18n.T("menu_play").c_str());
    AppendMenuW(m, MF_STRING, IDM_OPEN_DIR, g_i18n.T("menu_open_dir").c_str());
    AppendMenuW(m, MF_STRING, IDM_COPY_PATH, g_i18n.T("menu_copy_path").c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_REMOVE, g_i18n.T("menu_remove").c_str());
    AppendMenuW(m, MF_STRING, IDM_DELETE_FILE, g_i18n.T("menu_delete_file").c_str());

    POINT pt; GetCursorPos(&pt);
    int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                  pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(m);
    if (!cmd) return;

    const RecItem item = g_hist.Items()[index];
    switch (cmd) {
    case IDM_PLAY:
        if (FileExists(item.path)) Player::Open(g_inst, item.path, &g_i18n);
        else MessageBoxW(hwnd, g_i18n.T("err_missing").c_str(), L"LiteCap", MB_ICONWARNING);
        break;
    case IDM_OPEN_DIR: {
        std::wstring arg = L"/select,\"" + item.path + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
        break;
    }
    case IDM_COPY_PATH:
        if (OpenClipboard(hwnd)) {
            EmptyClipboard();
            size_t bytes = (item.path.size() + 1) * sizeof(wchar_t);
            HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, bytes);
            if (hg) {
                void* p = GlobalLock(hg);
                memcpy(p, item.path.c_str(), bytes);
                GlobalUnlock(hg);
                SetClipboardData(CF_UNICODETEXT, hg);
            }
            CloseClipboard();
        }
        break;
    case IDM_REMOVE:
        g_hist.RemoveAt((size_t)index);
        RefreshHistoryList();
        break;
    case IDM_DELETE_FILE:
        if (MessageBoxW(hwnd, g_i18n.T("confirm_delete").c_str(), L"LiteCap",
                        MB_YESNO | MB_ICONWARNING) == IDYES) {
            DeleteFileW(item.path.c_str());
            g_hist.RemoveAt((size_t)index);
            RefreshHistoryList();
        }
        break;
    }
}

// ---- 悬浮窗模式 ----
void EnterFloatMode() {
    g_floatMode = true;
    BarShow(true);
    ShowWindow(g_main, SW_HIDE);
}

void ExitFloatMode() {
    g_floatMode = false;
    ShowWindow(g_main, SW_SHOW);
    SetForegroundWindow(g_main);
    BarShow(false);
}

}  // namespace

// ---------------- 主窗口过程 ----------------
static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE:
        return 0;

    case WM_SIZE:
        LayoutMain(true);
        return 0;

    case WM_ACTIVATE:
        // 从别的程序切回来时顺手刷新一下窗口列表 ——
        // 不然刚打开的记事本要等用户点「选择窗口…」才出现。
        // 下拉正开着的时候绝不能重建内容，会把列表直接抽掉。
        if (LOWORD(w) != WA_INACTIVE && g_cmbTarget && !g_rec.Running() &&
            !SendMessageW(g_cmbTarget, CB_GETDROPPEDSTATE, 0, 0))
            RefreshTargetCombo();
        break;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)l;
        int chromeW = 0, chromeH = 0;
        GetChromeSize(chromeW, chromeH);
        // 最小宽度跟着展开状态走：展开时不允许拖到两栏互相压住
        mmi->ptMinTrackSize.x = g_colW + (g_histOpen ? g_rightW : 0) + chromeW;
        mmi->ptMinTrackSize.y = S(400);
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(w);
        int code = HIWORD(w);
        if (id == IDC_RECORD && (code == BN_CLICKED || code == 0)) {
            if (g_rec.Running()) StopRecording();
            else                 StartRecording();
            return 0;
        }
        if (id == IDC_FLOAT && (code == BN_CLICKED || code == 0)) {
            if (g_floatMode) ExitFloatMode(); else EnterFloatMode();
            return 0;
        }
        if (id == IDC_PICK && code == BN_CLICKED) {
            ShowWindow(h, SW_HIDE);
            Sleep(160);
            RECT r;
            bool ok = PickRegion(h, r);
            ShowWindow(h, SW_SHOW);
            SetForegroundWindow(h);
            if (ok) {
                wchar_t buf[64];
                swprintf(buf, 64, L"%d,%d,%d,%d", r.left, r.top,
                         r.right - r.left, r.bottom - r.top);
                g_cfg.Set("region", buf);
                g_cfg.Set("target", L"region");
                g_cfg.Save();
                RefreshTargetCombo();
            }
            return 0;
        }
        if (id == IDC_PICK_WIN && code == BN_CLICKED) {
            // 「录制单独程序」的入口。窗口列表就挂在目标下拉的末尾，
            // 所以这里不用另开一个对话框，刷新完直接把下拉拉下来就行。
            RefreshTargetCombo();
            SendMessageW(g_cmbTarget, CB_SHOWDROPDOWN, TRUE, 0);
            SetFocus(g_cmbTarget);
            return 0;
        }
        if (id == IDC_HIST_TOGGLE && code == BN_CLICKED) {
            ToggleHistory();
            return 0;
        }
        if (id == IDC_BROWSE && code == BN_CLICKED) {
            BROWSEINFOW bi = {};
            bi.hwndOwner = h;
            bi.lpszTitle = g_i18n.T("output_dir").c_str();
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
            if (pidl) {
                wchar_t path[MAX_PATH] = {};
                if (SHGetPathFromIDListW(pidl, path)) SetWindowTextW(g_edtDir, path);
                CoTaskMemFree(pidl);
            }
            return 0;
        }
        if (id == IDC_HOTKEY_SET && code == BN_CLICKED) {
            // 录制新组合前必须先把已注册的摘掉：否则按下旧组合会直接触发录制，
            // 录制窗口根本收不到那个按键。
            UnregisterHotKey(h, kHotkeyId);
            UINT mod = g_hotkeyMod, vk = g_hotkeyVk;
            if (PickHotkey(h, mod, vk)) {
                g_hotkeyMod = mod;
                g_hotkeyVk  = vk;
                g_cfg.Set("hotkey_mod", std::to_wstring(g_hotkeyMod));
                g_cfg.Set("hotkey_vk",  std::to_wstring(g_hotkeyVk));
                g_cfg.Save();
                if (!RegisterAppHotkey()) {
                    MessageBoxW(h, g_i18n.T("hotkey_fail").c_str(), L"LiteCap",
                                MB_ICONWARNING);
                }
                UpdateHotkeyText();
            } else {
                RegisterAppHotkey();   // 用户取消，把原来的装回去
            }
            return 0;
        }
        if (id == IDC_LANG && code == CBN_SELCHANGE) {
            int sel = (int)SendMessageW(g_cmbLang, CB_GETCURSEL, 0, 0);
            const auto& langs = g_i18n.Languages();
            if (sel >= 0 && sel < (int)langs.size()) {
                g_i18n.SetLang(langs[sel].code);
                g_cfg.Set("lang", U82W(langs[sel].code));
                g_cfg.Save();
                Retranslate();
                LayoutMain(true);
            }
            return 0;
        }
        if (id == IDC_QUALITY && code == CBN_SELCHANGE) {
            int q = (int)SendMessageW(g_cmbQuality, CB_GETCURSEL, 0, 0);
            const char* qk[] = { "high", "medium", "low" };
            g_cfg.Set("quality", U82W(qk[q < 0 ? 0 : q]));
            g_cfg.Save();
            return 0;
        }
        if (id == IDC_ENCODER && code == CBN_SELCHANGE) {
            // 编码器选择在选中时立即保存，不用等用户先录一段才生效。
            // 这样退出再打开仍是用户刚刚选的模式。
            int e = (int)SendMessageW(g_cmbEncoder, CB_GETCURSEL, 0, 0);
            if (e < 0 || e > 2) e = 0;
            const char* ek[] = { "auto", "hardware", "software" };
            const std::string mode = ek[e];
            g_cfg.Set("encoder", U82W(mode));
            g_cfg.Save();
            g_rec.SetEncoderMode(mode);
            return 0;
        }
        if ((id == IDC_AUDIO_SYSTEM || id == IDC_AUDIO_MIC) && code == BN_CLICKED) {
            const bool systemAudio = SendMessageW(g_chkSystemAudio, BM_GETCHECK, 0, 0) == BST_CHECKED;
            const bool microphoneAudio = SendMessageW(g_chkMicrophone, BM_GETCHECK, 0, 0) == BST_CHECKED;
            g_cfg.Set("audio_system", systemAudio ? L"1" : L"0");
            g_cfg.Set("audio_microphone", microphoneAudio ? L"1" : L"0");
            g_cfg.Save();
            return 0;
        }
        if (id == IDC_TARGET && code == CBN_SELCHANGE) {
            const int sel = (int)SendMessageW(g_cmbTarget, CB_GETCURSEL, 0, 0);
            const int fixed = TargetFixedCount();
            if (sel == 0) {
                g_cfg.Set("target", L"all");
            } else if (sel > 0 && sel < fixed) {
                if (sel <= (int)g_monitors.size()) {
                    wchar_t b[32]; swprintf(b, 32, L"mon:%d", sel);
                    g_cfg.Set("target", b);
                } else {
                    g_cfg.Set("target", L"region");
                }
            } else if (sel >= fixed && sel - fixed < (int)g_targetWins.size()) {
                const WinItem& w = g_targetWins[sel - fixed];
                g_cfg.Set("target", L"window");
                g_cfg.Set("window_title", w.title);
                g_cfg.Set("window_exe", w.exe);
            }
            g_cfg.Save();
            return 0;
        }
        if (id == IDC_FPS && code == CBN_SELCHANGE) {
            int sel = (int)SendMessageW(g_cmbFps, CB_GETCURSEL, 0, 0);
            if (sel < 0 || sel >= kFpsCount) sel = kFpsDefault;
            g_cfg.Set("fps", std::to_wstring(kFpsVals[sel]));
            g_cfg.Save();
            return 0;
        }
        break;
    }

    case WM_HOTKEY:
        if (w == (WPARAM)kHotkeyId) {
            if (g_rec.Running()) StopRecording();
            else                 StartRecording();
            return 0;
        }
        break;

    case WM_REC_TICK:
    case WM_REC_DONE:
    case WM_REC_FAIL:
        OnRecEvent(m, l);
        return 0;

    case WM_NOTIFY: {
        NMHDR* nh = (NMHDR*)l;
        if (nh->idFrom == IDC_LIST) {
            if (nh->code == NM_DBLCLK) {
                LPNMITEMACTIVATE nia = (LPNMITEMACTIVATE)l;
                int idx = nia->iItem;
                if (idx >= 0 && idx < (int)g_hist.Count()) {
                    const RecItem& it = g_hist.Items()[idx];
                    if (FileExists(it.path)) Player::Open(g_inst, it.path, &g_i18n);
                    else MessageBoxW(h, g_i18n.T("err_missing").c_str(), L"LiteCap", MB_ICONWARNING);
                }
                return 0;
            }
            if (nh->code == NM_RCLICK) {
                POINT pt; GetCursorPos(&pt);
                POINT cp = pt;
                ScreenToClient(g_list, &cp);
                LVHITTESTINFO ht = {};
                ht.pt = cp;
                int idx = (int)SendMessageW(g_list, LVM_HITTEST, 0, (LPARAM)&ht);
                if (idx >= 0) {
                    LVITEMW st = {};
                    st.mask = LVIF_STATE;
                    st.iItem = idx;
                    st.stateMask = LVIS_SELECTED;
                    st.state = LVIS_SELECTED;
                    SendMessageW(g_list, LVM_SETITEMSTATE, idx, (LPARAM)&st);
                    ShowListMenu(h, idx);
                }
                return 0;
            }
        }
        break;
    }

    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)w;
        SetBkMode(dc, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC dc = (HDC)w;
        SetBkMode(dc, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }

    case WM_CLOSE:
        if (g_rec.Running()) StopRecording();
        Player::Close();
        BarShow(false);
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        UnregisterHotKey(h, kHotkeyId);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------------- 入口 ----------------
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    g_inst = inst;

    // DPI 感知（尽量用 v2，失败则退回旧接口）
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32) {
        typedef BOOL(WINAPI* SetCtx)(HANDLE);
        auto setCtx = (SetCtx)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
        if (setCtx) setCtx((HANDLE)-4);   // PER_MONITOR_AWARE_V2
    }
    {
        HDC dc = GetDC(nullptr);
        g_dpi = (UINT)GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(nullptr, dc);
        if (g_dpi < 72 || g_dpi > 480) g_dpi = 96;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    std::wstring dir = ExeDir();
    g_cfg.Load(JoinPath(dir, L"settings.ini"));
    // 记录区展开状态要记住：上次收起的话，下次打开还是那条窄栏
    g_histOpen = g_cfg.GetInt("history_open", 1) != 0;
    // 先读 exe 内置语言包（单文件分发也能正常显示），再用 lang/*.ini 覆盖
    g_i18n.LoadBuiltin(inst);
    g_i18n.LoadDir(JoinPath(dir, L"lang"));
    g_i18n.SetLang(W2U8(g_cfg.Get("lang", L"zh-CN")));
    g_hist.Load(JoinPath(dir, L"history.tsv"));

    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfHeight = -MulDiv(lf.lfHeight < 0 ? -lf.lfHeight : 12, g_dpi, 96);
    g_font = CreateFontIndirectW(&lf);
    LOGFONTW lfb = lf;
    lfb.lfWeight = FW_SEMIBOLD;
    g_fontBold = CreateFontIndirectW(&lfb);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = MainProc;
    wc.hInstance = inst;
    wc.lpszClassName = kMainClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.hIcon   = LoadAppIcon(inst, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
    wc.hIconSm = LoadAppIcon(inst, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    RegisterClassExW(&wc);

    WNDCLASSEXW bc = { sizeof(bc) };
    bc.lpfnWndProc = BarProc;
    bc.hInstance = inst;
    bc.lpszClassName = kBarClass;
    bc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    bc.hbrBackground = nullptr;
    RegisterClassExW(&bc);

    WNDCLASSEXW sc = { sizeof(sc) };
    sc.lpfnWndProc = SelProc;
    sc.hInstance = inst;
    sc.lpszClassName = kSelClass;
    sc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    RegisterClassExW(&sc);

    WNDCLASSEXW hc = { sizeof(hc) };
    hc.lpfnWndProc = HotkeyProc;
    hc.hInstance = inst;
    hc.lpszClassName = kHotkeyClass;
    hc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    hc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    RegisterClassExW(&hc);

    // 两栏布局：左栏设置项固定宽度，右栏录制记录是展开时额外加出来的。
    // 先给个够大的临时尺寸，等控件建完、排版函数算出真正需要的尺寸，再一次性收到刚好。
    g_colW   = S(300);
    g_rightW = S(320);
    g_listH  = S(340);                // 记录列表的默认高度，之后跟着窗口走
    int ww = g_colW + g_rightW + S(20), wh = S(820);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    g_main = CreateWindowExW(0, kMainClass, L"LiteCap",
                             WS_OVERLAPPEDWINDOW,
                             (sw - ww) / 2, (sh - wh) / 2, ww, wh,
                             nullptr, nullptr, inst, nullptr);

    // ---- 控件 ----
    auto mk = [&](const wchar_t* cls, DWORD style, int id) {
        HWND c = CreateWindowExW(0, cls, L"", WS_CHILD | WS_VISIBLE | style,
                                 0, 0, 10, 10, g_main, (HMENU)(INT_PTR)id, inst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
        return c;
    };

    g_lblTarget  = mk(L"STATIC", SS_LEFT, 0);
    g_cmbTarget  = mk(L"COMBOBOX", CBS_DROPDOWNLIST | WS_VSCROLL, IDC_TARGET);
    g_btnPick    = mk(L"BUTTON", BS_PUSHBUTTON, IDC_PICK);
    g_btnPickWin = mk(L"BUTTON", BS_PUSHBUTTON, IDC_PICK_WIN);
    g_lblFps     = mk(L"STATIC", SS_LEFT, 0);
    g_cmbFps     = mk(L"COMBOBOX", CBS_DROPDOWNLIST | WS_VSCROLL, IDC_FPS);
    g_lblQuality = mk(L"STATIC", SS_LEFT, 0);
    g_cmbQuality = mk(L"COMBOBOX", CBS_DROPDOWNLIST | WS_VSCROLL, IDC_QUALITY);
    g_lblEncoder = mk(L"STATIC", SS_LEFT, 0);
    g_cmbEncoder = mk(L"COMBOBOX", CBS_DROPDOWNLIST | WS_VSCROLL, IDC_ENCODER);
    g_chkSystemAudio = mk(L"BUTTON", BS_AUTOCHECKBOX | BS_LEFT, IDC_AUDIO_SYSTEM);
    g_chkMicrophone = mk(L"BUTTON", BS_AUTOCHECKBOX | BS_LEFT, IDC_AUDIO_MIC);
    g_lblLang    = mk(L"STATIC", SS_LEFT, 0);
    g_cmbLang    = mk(L"COMBOBOX", CBS_DROPDOWNLIST | WS_VSCROLL, IDC_LANG);
    g_lblDir     = mk(L"STATIC", SS_LEFT, 0);
    g_edtDir     = mk(L"EDIT", ES_AUTOHSCROLL | WS_BORDER, IDC_DIR);
    g_btnBrowse  = mk(L"BUTTON", BS_PUSHBUTTON, IDC_BROWSE);
    g_lblHotkey  = mk(L"STATIC", SS_LEFT, 0);
    g_edtHotkey  = mk(L"EDIT", ES_AUTOHSCROLL | ES_READONLY | WS_BORDER,
                      IDC_HOTKEY_EDIT);
    g_btnHotkey  = mk(L"BUTTON", BS_PUSHBUTTON, IDC_HOTKEY_SET);
    g_btnRecord  = mk(L"BUTTON", BS_PUSHBUTTON, IDC_RECORD);
    g_btnFloat   = mk(L"BUTTON", BS_PUSHBUTTON, IDC_FLOAT);
    // 记录区标题做成扁平按钮：整行都能点，而且按下去有反馈，
    // 比 STATIC + SS_NOTIFY 只认文字那一小块强。
    g_lblHist    = mk(L"BUTTON", BS_LEFT | BS_FLAT, IDC_HIST_TOGGLE);
    g_lblHint    = mk(L"STATIC", SS_LEFT, 0);

    SendMessageW(g_btnRecord, WM_SETFONT, (WPARAM)g_fontBold, TRUE);
    SendMessageW(g_lblHist, WM_SETFONT, (WPARAM)g_fontBold, TRUE);

    g_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, 0, 10, 10, g_main, (HMENU)(INT_PTR)IDC_LIST, inst, nullptr);
    SendMessageW(g_list, WM_SETFONT, (WPARAM)g_font, TRUE);
    ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

    // FPS 下拉
    for (int i = 0; i < kFpsCount; ++i) {
        wchar_t b[32]; swprintf(b, 32, L"%d fps", kFpsVals[i]);
        SendMessageW(g_cmbFps, CB_ADDSTRING, 0, (LPARAM)b);
    }

    RefreshMonitors();
    Retranslate();

    // 从配置恢复
    {
        int fps = g_cfg.GetInt("fps", 30);
        int fi = kFpsDefault;
        for (int i = 0; i < kFpsCount; ++i) if (kFpsVals[i] == fps) fi = i;
        SendMessageW(g_cmbFps, CB_SETCURSEL, fi, 0);

        std::wstring q = g_cfg.Get("quality", L"high");
        int qi = q == L"low" ? 2 : q == L"medium" ? 1 : 0;
        SendMessageW(g_cmbQuality, CB_SETCURSEL, qi, 0);

        std::wstring e = g_cfg.Get("encoder", L"auto");
        int ei = e == L"hardware" ? 1 : e == L"software" ? 2 : 0;
        SendMessageW(g_cmbEncoder, CB_SETCURSEL, ei, 0);

        const bool systemAudio = g_cfg.GetInt("audio_system", 1) != 0;
        const bool microphoneAudio = g_cfg.GetInt("audio_microphone", 0) != 0;
        SendMessageW(g_chkSystemAudio, BM_SETCHECK,
                     systemAudio ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(g_chkMicrophone, BM_SETCHECK,
                     microphoneAudio ? BST_CHECKED : BST_UNCHECKED, 0);

        SetWindowTextW(g_edtDir, g_cfg.Get("output_dir", DefaultOutputDir()).c_str());

        g_hotkeyMod = (UINT)g_cfg.GetInt("hotkey_mod", MOD_CONTROL | MOD_ALT);
        g_hotkeyVk  = (UINT)g_cfg.GetInt("hotkey_vk", 'R');
    }

    RefreshHistoryList();

    // 宽度锁死（左栏 + 右栏），高度按内容算 —— 少一行留白都不留。
    {
        int chromeW = 0, chromeH = 0;
        GetChromeSize(chromeW, chromeH);
        const int need = LayoutMain(false);
        const int clientW = g_colW + (g_histOpen ? g_rightW : 0);
        RECT wr = {};
        GetWindowRect(g_main, &wr);
        SetWindowPos(g_main, nullptr, wr.left, wr.top,
                     clientW + chromeW, need + chromeH,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        LayoutMain(false);
    }

    ShowWindow(g_main, SW_SHOW);
    UpdateWindow(g_main);

    // 全局热键要等主窗口显示之后再注册，否则失败提示框会弹在窗口后面
    if (!RegisterAppHotkey() && g_hotkeyVk) {
        MessageBoxW(g_main, g_i18n.T("hotkey_fail").c_str(), L"LiteCap", MB_ICONWARNING);
    }
    UpdateHotkeyText();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (Player::IsOpen() && msg.hwnd == nullptr) { /* no-op */ }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_font) DeleteObject(g_font);
    if (g_fontBold) DeleteObject(g_fontBold);
    MFShutdown();
    CoUninitialize();
    return 0;
}
