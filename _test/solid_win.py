"""给捕获测试开一个纯色 / 四色块靶子窗口。

用法: solid_win.py <窗口标题> <颜色|QUAD> <x> <y> <w> <h> [存活秒数] [popup]

为什么不用 tkinter：托管版 Python 不一定带它。这里只需要一个「颜色确定、
标题确定」的顶层窗口 —— 用来判断抓到的到底是哪一个窗口，颜色是最直接的证据。
COLORREF 是 0x00BBGGRR，纯绿 = 0000FF00，纯红 = 000000FF。

QUAD 会画四个色块（左上红、右上绿、左下蓝、右下黄）。
纯色只能验证"抓对了窗口"，验证不了朝向 —— 旋转 180 度之后纯色看起来一模一样。
四个象限各不相同，才能一眼看出画面是不是被翻转了。

popup 参数让窗口不带标题栏和边框，这样「窗口尺寸」和「客户区尺寸」完全一致，
拿屏幕区域录和拿窗口录才有一模一样的取景框，可以直接逐像素对比。
"""
import ctypes
import ctypes.wintypes as wt
import sys
import time

u32 = ctypes.windll.user32
g32 = ctypes.windll.gdi32
k32 = ctypes.windll.kernel32

LRESULT = ctypes.c_ssize_t
WNDPROC = ctypes.WINFUNCTYPE(LRESULT, wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)

u32.CreateWindowExW.restype = wt.HWND
u32.CreateWindowExW.argtypes = [wt.DWORD, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD,
                                ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                wt.HWND, wt.HMENU, ctypes.c_void_p, ctypes.c_void_p]
u32.DefWindowProcW.restype = LRESULT
u32.DefWindowProcW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
u32.BeginPaint.restype = wt.HDC
u32.BeginPaint.argtypes = [wt.HWND, ctypes.c_void_p]
u32.EndPaint.argtypes = [wt.HWND, ctypes.c_void_p]
u32.GetClientRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u32.PeekMessageW.argtypes = [ctypes.c_void_p, wt.HWND, wt.UINT, wt.UINT, wt.UINT]
u32.LoadCursorW.restype = ctypes.c_void_p
u32.LoadCursorW.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
g32.CreateSolidBrush.restype = ctypes.c_void_p
g32.CreateSolidBrush.argtypes = [wt.DWORD]
g32.DeleteObject.argtypes = [ctypes.c_void_p]
u32.FillRect.argtypes = [wt.HDC, ctypes.POINTER(wt.RECT), ctypes.c_void_p]


class WNDCLASSEXW(ctypes.Structure):
    _fields_ = [
        ("cbSize", wt.UINT), ("style", wt.UINT), ("lpfnWndProc", WNDPROC),
        ("cbClsExtra", ctypes.c_int), ("cbWndExtra", ctypes.c_int),
        ("hInstance", ctypes.c_void_p), ("hIcon", ctypes.c_void_p),
        ("hCursor", ctypes.c_void_p), ("hbrBackground", ctypes.c_void_p),
        ("lpszMenuName", wt.LPCWSTR), ("lpszClassName", wt.LPCWSTR),
        ("hIconSm", ctypes.c_void_p),
    ]


WM_PAINT, WM_ERASEBKGND, WM_CLOSE, WM_QUIT = 0x000F, 0x0014, 0x0010, 0x0012
WS_OVERLAPPEDWINDOW = 0x00CF0000
WS_POPUP = 0x80000000
WS_CAPTION = 0x00C00000
WS_SYSMENU = 0x00080000
WS_THICKFRAME = 0x00040000
WS_EX_TOOLWINDOW = 0x00000080
WS_EX_APPWINDOW = 0x00040000
WS_EX_TOPMOST = 0x00000008
SW_SHOW = 5
PM_REMOVE = 1

# 窗口样式预设。第 8 个参数选一个，用来二分 WGC 的 CreateForWindow 到底挑什么。
# WS_POPUP 那条会失败（E_INVALIDARG），所以要找出是哪个样式位让它成立。
STYLES = {
    "popup":     (WS_POPUP, 0),                                        # 无边框（默认，已知失败）
    "popup_cap": (WS_POPUP | WS_CAPTION | WS_SYSMENU, 0),              # 无边框但有标题栏
    "popup_thk": (WS_POPUP | WS_THICKFRAME, 0),                        # 只有可调边框
    "popup_tool": (WS_POPUP, WS_EX_TOOLWINDOW),                        # 无边框 + 工具窗口
    "overlap":   (WS_OVERLAPPEDWINDOW, 0),                             # 普通窗口（已知可用）
    # ---- 下面这些带 _top，加 WS_EX_TOPMOST ----
    # 测试机上可能有全屏程序（Godot 游戏、播放器）盖住左上角，GDI 那种按屏幕
    # 抓屏的用例就会录到别人家窗口。置顶把靶子抬到最上层，让 GDI 用例也能测。
    # 置顶只影响 z-order，不影响 CreateForWindow 的样式判定，WGC 用例照样有效。
    "popup_top":   (WS_POPUP, WS_EX_TOPMOST),
    "overlap_top": (WS_OVERLAPPEDWINDOW, WS_EX_TOPMOST),
}

# COLORREF = 0x00BBGGRR
RED, GREEN, BLUE, YELLOW = 0x000000FF, 0x0000FF00, 0x00FF0000, 0x0000FFFF
QUAD = (RED, GREEN, BLUE, YELLOW)          # 左上 右上 左下 右下


def main():
    title = sys.argv[1]
    spec = sys.argv[2]
    x, y, w, h = (int(v) for v in sys.argv[3:7])
    life = float(sys.argv[7]) if len(sys.argv) > 7 else 120.0
    style_name = sys.argv[8] if len(sys.argv) > 8 else "overlap"
    if style_name not in STYLES:
        print("未知样式 %r，可选: %s" % (style_name, ", ".join(STYLES)), flush=True)
        return 1
    style, exstyle = STYLES[style_name]

    quad = spec.upper() == "QUAD"
    colorref = 0 if quad else int(spec, 16)
    hinst = k32.GetModuleHandleW(None)

    def proc(hwnd, msg, wp, lp):
        if msg == WM_PAINT:
            ps = ctypes.create_string_buffer(80)
            hdc = u32.BeginPaint(hwnd, ps)
            r = wt.RECT()
            u32.GetClientRect(hwnd, ctypes.byref(r))
            if quad:
                cw, ch = r.right // 2, r.bottom // 2
                for i, cr in enumerate(QUAD):
                    q = wt.RECT((i & 1) * cw, (i >> 1) * ch,
                                (i & 1) * cw + cw, (i >> 1) * ch + ch)
                    br = g32.CreateSolidBrush(cr)
                    u32.FillRect(hdc, ctypes.byref(q), br)
                    g32.DeleteObject(br)
            else:
                br = g32.CreateSolidBrush(colorref)
                u32.FillRect(hdc, ctypes.byref(r), br)
                g32.DeleteObject(br)
            u32.EndPaint(hwnd, ps)
            return 0
        if msg == WM_ERASEBKGND:
            return 1
        if msg == WM_CLOSE:
            u32.PostQuitMessage(0)
            return 0
        return u32.DefWindowProcW(hwnd, msg, wp, lp)

    wndproc = WNDPROC(proc)          # 必须留住引用，否则回调会被 GC 掉
    cls = "LiteCapSolidTestWnd"

    wc = WNDCLASSEXW()
    wc.cbSize = ctypes.sizeof(WNDCLASSEXW)
    wc.lpfnWndProc = wndproc
    wc.hInstance = hinst
    wc.hCursor = u32.LoadCursorW(None, 32512)     # IDC_ARROW
    wc.lpszClassName = cls
    if not u32.RegisterClassExW(ctypes.byref(wc)) and k32.GetLastError() != 1410:
        print("RegisterClassExW 失败", k32.GetLastError(), flush=True)
        return 1

    hwnd = u32.CreateWindowExW(exstyle, cls, title, style,
                               x, y, w, h, None, None, hinst, None)
    if not hwnd:
        print("CreateWindowExW 失败", k32.GetLastError(), flush=True)
        return 1
    u32.ShowWindow(hwnd, SW_SHOW)
    u32.UpdateWindow(hwnd)
    print("READY 0x%X" % hwnd, flush=True)

    msg = wt.MSG()
    t0 = time.time()
    while time.time() - t0 < life:
        while u32.PeekMessageW(ctypes.byref(msg), None, 0, 0, PM_REMOVE):
            if msg.message == WM_QUIT:
                return 0
            u32.TranslateMessage(ctypes.byref(msg))
            u32.DispatchMessageW(ctypes.byref(msg))
        time.sleep(0.02)
    return 0


if __name__ == "__main__":
    sys.exit(main())
