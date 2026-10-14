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

// 主窗口控件
static HWND g_lblTarget, g_cmbTarget, g_btnPick, g_lblFps, g_cmbFps;
static HWND g_lblQuality, g_cmbQuality, g_lblLang, g_cmbLang;
static HWND g_lblDir, g_edtDir, g_btnBrowse, g_btnRecord, g_btnFloat;
static HWND g_lblHist, g_lblHint;

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

void RefreshTargetCombo() {
    SendMessageW(g_cmbTarget, CB_RESETCONTENT, 0, 0);
    SendMessageW(g_cmbTarget, CB_ADDSTRING, 0, (LPARAM)g_i18n.T("target_all").c_str());
    for (size_t i = 0; i < g_monitors.size(); ++i) {
        wchar_t buf[128];
        const RECT& r = g_monitors[i];
        swprintf(buf, 128, L"%s %d  (%d\u00D7%d)",
                 g_i18n.T("target_monitor").c_str(), (int)i + 1,
                 r.right - r.left, r.bottom - r.top);
        SendMessageW(g_cmbTarget, CB_ADDSTRING, 0, (LPARAM)buf);
    }
    if (!g_cfg.Get("region").empty())
        SendMessageW(g_cmbTarget, CB_ADDSTRING, 0, (LPARAM)g_i18n.T("target_region").c_str());

    std::wstring t = g_cfg.Get("target", L"all");
    int sel = 0;
    if (t == L"region" && !g_cfg.Get("region").empty()) sel = (int)g_monitors.size() + 1;
    else if (t.rfind(L"mon:", 0) == 0) {
        int idx = _wtoi(t.c_str() + 4);
        if (idx >= 1 && idx <= (int)g_monitors.size()) sel = idx;
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
        std::wstring when = FormatDateTime(it.epoch);
        SendMessageW(g_list, LVM_SETITEMTEXTW, row, (LPARAM)&(LVITEMW{ LVIF_TEXT, row, 1, 0, 0, (LPWSTR)dur.c_str() }));
        SendMessageW(g_list, LVM_SETITEMTEXTW, row, (LPARAM)&(LVITEMW{ LVIF_TEXT, row, 2, 0, 0, (LPWSTR)sz.c_str() }));
        SendMessageW(g_list, LVM_SETITEMTEXTW, row, (LPARAM)&(LVITEMW{ LVIF_TEXT, row, 3, 0, 0, (LPWSTR)when.c_str() }));
    }
}

void ApplyFont(HWND c) { SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE); }

void Retranslate() {
    std::wstring title = std::wstring(L"LiteCap 1.0  \u00B7  ") + g_i18n.T("app_subtitle");
    SetWindowTextW(g_main, title.c_str());
    SetWindowTextW(g_lblTarget, g_i18n.T("target").c_str());
    SetWindowTextW(g_lblFps, g_i18n.T("fps").c_str());
    SetWindowTextW(g_lblQuality, g_i18n.T("quality").c_str());
    SetWindowTextW(g_lblLang, g_i18n.T("language").c_str());
    SetWindowTextW(g_lblDir, g_i18n.T("output_dir").c_str());
    SetWindowTextW(g_lblHist, g_i18n.T("history").c_str());
    SetWindowTextW(g_lblHint, g_i18n.T("history_hint").c_str());
    SetWindowTextW(g_btnPick, g_i18n.T("pick_region").c_str());
    SetWindowTextW(g_btnBrowse, g_i18n.T("browse").c_str());
    SetWindowTextW(g_btnFloat, g_i18n.T("float_mode").c_str());
    SetWindowTextW(g_btnRecord, g_rec.Running() ? g_i18n.T("stop").c_str()
                                                : g_i18n.T("start").c_str());

    // 画质下拉
    int qs = (int)SendMessageW(g_cmbQuality, CB_GETCURSEL, 0, 0);
    SendMessageW(g_cmbQuality, CB_RESETCONTENT, 0, 0);
    SendMessageW(g_cmbQuality, CB_ADDSTRING, 0, (LPARAM)g_i18n.T("quality_high").c_str());
    SendMessageW(g_cmbQuality, CB_ADDSTRING, 0, (LPARAM)g_i18n.T("quality_medium").c_str());
    SendMessageW(g_cmbQuality, CB_ADDSTRING, 0, (LPARAM)g_i18n.T("quality_low").c_str());
    SendMessageW(g_cmbQuality, CB_SETCURSEL, qs < 0 ? 0 : qs, 0);

    // 列：LVM_SETCOLUMNW 只能改已有列，创建列必须用 LVM_INSERTCOLUMNW。
    // 切换语言时要把旧列删掉重建，否则表头文字不会更新。
    while (SendMessageW(g_list, LVM_DELETECOLUMN, 0, 0)) {}
    LVCOLUMNW col = {};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    const char* keys[4] = { "col_name", "col_duration", "col_size", "col_time" };
    int widths[4] = { 220, 72, 78, 130 };
    for (int i = 0; i < 4; ++i) {
        std::wstring txt = g_i18n.T(keys[i]);
        col.iSubItem = i;
        col.pszText = (LPWSTR)txt.c_str();
        col.cx = S(widths[i]);
        SendMessageW(g_list, LVM_INSERTCOLUMNW, i, (LPARAM)&col);
    }

    RefreshTargetCombo();
    RefreshLangCombo();
    if (g_bar) InvalidateRect(g_bar, nullptr, TRUE);
}

void LayoutMain() {
    RECT rc;
    GetClientRect(g_main, &rc);
    int W = rc.right;
    int pad = S(14), y = S(12), rowH = S(26), gap = S(8);
    int lw = S(74);                                  // 标签列宽
    int fx = pad + lw;
    int fw = W - fx - pad;
    int btnW = S(78);

    auto place = [&](HWND c, int x, int yy, int w, int h) {
        MoveWindow(c, x, yy, w, h, TRUE);
    };

    place(g_lblTarget, pad, y + S(3), lw, rowH);
    // ComboBox 的 height 参数指的是「下拉列表 + 文本区」的总高度，
    // 只给 rowH 会让列表部分≈0，展开后一项都看不到。这里留足列表空间。
    place(g_cmbTarget, fx, y, fw - btnW - gap, rowH + S(180));
    place(g_btnPick,   fx + fw - btnW, y, btnW, rowH);
    y += rowH + gap;

    place(g_lblFps, pad, y + S(3), lw, rowH);
    place(g_cmbFps, fx, y, fw, rowH + S(180));
    y += rowH + gap;

    place(g_lblQuality, pad, y + S(3), lw, rowH);
    place(g_cmbQuality, fx, y, fw, rowH + S(180));
    y += rowH + gap;

    place(g_lblLang, pad, y + S(3), lw, rowH);
    place(g_cmbLang, fx, y, fw, rowH + S(180));
    y += rowH + gap + S(4);

    place(g_lblDir, pad, y, lw, S(20));
    y += S(22);
    place(g_edtDir, pad, y, fw - btnW - gap, rowH);
    place(g_btnBrowse, pad + fw - btnW, y, btnW, rowH);
    y += rowH + gap + S(4);

    int recW = fw - S(96) - gap;
    place(g_btnRecord, pad, y, recW, S(38));
    place(g_btnFloat, pad + fw - S(96), y, S(96), S(38));
    y += S(38) + S(14);

    place(g_lblHist, pad, y, W - pad * 2, S(20));
    y += S(22);

    int listH = rc.bottom - y - S(30) - pad;
    if (listH < S(80)) listH = S(80);
    place(g_list, pad, y, W - pad * 2, listH);
    y += listH + S(4);
    place(g_lblHint, pad, y, W - pad * 2, S(20));

    // 双保险：v6 下让下拉列表至少显示 12 项（v5 会失败，靠上面的高度兜底）
    HWND combos[] = { g_cmbTarget, g_cmbFps, g_cmbQuality, g_cmbLang };
    for (HWND c : combos) SendMessageW(c, CB_SETMINVISIBLE, 12, 0);
}

void ApplyRecordingUI(bool recording) {
    SetWindowTextW(g_btnRecord, recording ? g_i18n.T("stop").c_str()
                                          : g_i18n.T("start").c_str());
    EnableWindow(g_btnRecord, TRUE);
    EnableWindow(g_cmbTarget, !recording);
    EnableWindow(g_cmbFps, !recording);
    EnableWindow(g_cmbQuality, !recording);
    EnableWindow(g_cmbLang, !recording);
    EnableWindow(g_edtDir, !recording);
    EnableWindow(g_btnBrowse, !recording);
    EnableWindow(g_btnPick, !recording);
    if (g_bar) InvalidateRect(g_bar, nullptr, TRUE);
}

void StartRecording() {
    // 保存当前设置
    int sel = (int)SendMessageW(g_cmbTarget, CB_GETCURSEL, 0, 0);
    std::wstring target = L"all";
    if (sel > 0 && sel <= (int)g_monitors.size()) {
        wchar_t buf[32]; swprintf(buf, 32, L"mon:%d", sel); target = buf;
    } else if (sel == (int)g_monitors.size() + 1) {
        target = L"region";
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
    g_cfg.Save();

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

    // 区域
    RECT region = {};
    if (target == L"all") {
        region.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
        region.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
        region.right = region.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        region.bottom = region.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    } else if (target == L"region") {
        std::wstring rs = g_cfg.Get("region");
        int a = 0, b = 0, c = 0, d = 0;
        if (swscanf_s(rs.c_str(), L"%d,%d,%d,%d", &a, &b, &c, &d) != 4) {
            MessageBoxW(g_main, g_i18n.T("err_no_region").c_str(), L"LiteCap", MB_ICONWARNING);
            return;
        }
        region = { a, b, a + c, b + d };
    } else {
        int idx = _wtoi(target.c_str() + 4);
        if (idx < 1 || idx >(int)g_monitors.size()) idx = 1;
        region = g_monitors[idx - 1];
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[128];
    swprintf(name, 128, L"LiteCap_%04d%02d%02d_%02d%02d%02d.mp4",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::wstring outPath = JoinPath(outDir, name);

    if (!g_rec.Start(g_main, region, fps, quality, outPath)) {
        MessageBoxW(g_main, g_i18n.T("err_start").c_str(), L"LiteCap", MB_ICONERROR);
        return;
    }
    g_barTime = L"00:00";
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
        ApplyRecordingUI(false);
        break;
    }
    case WM_REC_FAIL:
        MessageBoxW(g_main, (g_i18n.T("failed") + L"\n" + ev->error).c_str(),
                    L"LiteCap", MB_ICONERROR);
        g_barTime = L"00:00";
        ApplyRecordingUI(false);
        break;
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
        LayoutMain();
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)l;
        mmi->ptMinTrackSize.x = S(560);
        mmi->ptMinTrackSize.y = S(560);
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
        if (id == IDC_LANG && code == CBN_SELCHANGE) {
            int sel = (int)SendMessageW(g_cmbLang, CB_GETCURSEL, 0, 0);
            const auto& langs = g_i18n.Languages();
            if (sel >= 0 && sel < (int)langs.size()) {
                g_i18n.SetLang(langs[sel].code);
                g_cfg.Set("lang", U82W(langs[sel].code));
                g_cfg.Save();
                Retranslate();
                LayoutMain();
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
        if (id == IDC_TARGET && code == CBN_SELCHANGE) {
            int sel = (int)SendMessageW(g_cmbTarget, CB_GETCURSEL, 0, 0);
            if (sel == 0) g_cfg.Set("target", L"all");
            else if (sel == (int)g_monitors.size() + 1) g_cfg.Set("target", L"region");
            else { wchar_t b[32]; swprintf(b, 32, L"mon:%d", sel); g_cfg.Set("target", b); }
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
                    SendMessageW(g_list, LVM_SETITEMSTATE, idx,
                                 (LPARAM)&(LVITEMW{ LVIF_STATE, idx, 0, LVIS_SELECTED, LVIS_SELECTED, nullptr }));
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

    int ww = S(600), wh = S(620);
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
    g_lblFps     = mk(L"STATIC", SS_LEFT, 0);
    g_cmbFps     = mk(L"COMBOBOX", CBS_DROPDOWNLIST | WS_VSCROLL, IDC_FPS);
    g_lblQuality = mk(L"STATIC", SS_LEFT, 0);
    g_cmbQuality = mk(L"COMBOBOX", CBS_DROPDOWNLIST | WS_VSCROLL, IDC_QUALITY);
    g_lblLang    = mk(L"STATIC", SS_LEFT, 0);
    g_cmbLang    = mk(L"COMBOBOX", CBS_DROPDOWNLIST | WS_VSCROLL, IDC_LANG);
    g_lblDir     = mk(L"STATIC", SS_LEFT, 0);
    g_edtDir     = mk(L"EDIT", ES_AUTOHSCROLL | WS_BORDER, IDC_DIR);
    g_btnBrowse  = mk(L"BUTTON", BS_PUSHBUTTON, IDC_BROWSE);
    g_btnRecord  = mk(L"BUTTON", BS_PUSHBUTTON, IDC_RECORD);
    g_btnFloat   = mk(L"BUTTON", BS_PUSHBUTTON, IDC_FLOAT);
    g_lblHist    = mk(L"STATIC", SS_LEFT, 0);
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

        SetWindowTextW(g_edtDir, g_cfg.Get("output_dir", DefaultOutputDir()).c_str());
    }

    RefreshHistoryList();

    LayoutMain();
    ShowWindow(g_main, SW_SHOW);
    UpdateWindow(g_main);

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
