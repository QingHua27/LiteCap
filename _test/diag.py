"""诊断：双击列表后，LiteCap 进程里到底出现了哪些窗口"""
import ctypes, ctypes.wintypes as wt, time, sys

u32 = ctypes.windll.user32
k32 = ctypes.windll.kernel32

u32.FindWindowW.argtypes = [wt.LPCWSTR, wt.LPCWSTR]; u32.FindWindowW.restype = wt.HWND
u32.GetDlgItem.argtypes = [wt.HWND, ctypes.c_int]; u32.GetDlgItem.restype = wt.HWND
u32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
u32.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u32.GetWindowTextW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u32.SendMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]; u32.SendMessageW.restype = ctypes.c_ssize_t
u32.ClientToScreen.argtypes = [wt.HWND, ctypes.POINTER(wt.POINT)]
u32.SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]
u32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u32.IsWindowVisible.argtypes = [wt.HWND]
u32.GetForegroundWindow.restype = wt.HWND
k32.GetCurrentThreadId.restype = wt.DWORD
u32.AttachThreadInput.argtypes = [wt.DWORD, wt.DWORD, wt.BOOL]
u32.SetForegroundWindow.argtypes = [wt.HWND]
u32.BringWindowToTop.argtypes = [wt.HWND]

IDC_LIST = 1010
EnumWindows = u32.EnumWindows
EnumWindows.argtypes = [ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM), wt.LPARAM]


def dump(pid, tag):
    print("--- %s (PID=%d) 的顶层窗口 ---" % (tag, pid))
    found = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(h, l):
        p = wt.DWORD()
        u32.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value != pid:
            return True
        cn = ctypes.create_unicode_buffer(256); u32.GetClassNameW(h, cn, 256)
        tt = ctypes.create_unicode_buffer(512); u32.GetWindowTextW(h, tt, 512)
        r = wt.RECT(); u32.GetWindowRect(h, ctypes.byref(r))
        vis = bool(u32.IsWindowVisible(h))
        found.append(cn.value)
        print("   vis=%-5s class=%-24s rect=(%d,%d,%d,%d) title=%r"
              % (vis, cn.value, r.left, r.top, r.right, r.bottom, tt.value))
        return True
    EnumWindows(cb, 0)
    if not found:
        print("   (无)")
    return found


def main():
    main_hwnd = u32.FindWindowW("LiteCapMainWnd", None)
    if not main_hwnd:
        print("找不到主窗口"); return 1
    pid = wt.DWORD(); u32.GetWindowThreadProcessId(main_hwnd, ctypes.byref(pid))
    pid = pid.value
    print("主窗口 hwnd=0x%X pid=%d" % (main_hwnd, pid))
    dump(pid, "双击前")

    # 提升前台
    fg = u32.GetForegroundWindow()
    fg_tid = u32.GetWindowThreadProcessId(fg, None) if fg else 0
    my_tid = k32.GetCurrentThreadId()
    u32.AttachThreadInput(my_tid, fg_tid, True)
    u32.ShowWindow(main_hwnd, 9)
    u32.BringWindowToTop(main_hwnd)
    u32.SetForegroundWindow(main_hwnd)
    u32.AttachThreadInput(my_tid, fg_tid, False)
    time.sleep(0.4)
    print("前台已是 LiteCap =", u32.GetForegroundWindow() == main_hwnd)

    # 列表项矩形
    lst = u32.GetDlgItem(main_hwnd, IDC_LIST)
    lr = wt.RECT(); u32.GetWindowRect(lst, ctypes.byref(lr))
    print("列表屏幕矩形 =", (lr.left, lr.top, lr.right, lr.bottom))

    pt = wt.POINT(lr.left + 120, lr.top + 32)
    u32.SetCursorPos(pt.x, pt.y)
    time.sleep(0.3)

    # 关键诊断：这个屏幕坐标下最顶层的窗口是谁？
    u32.WindowFromPoint.argtypes = [wt.POINT]
    u32.WindowFromPoint.restype = wt.HWND
    u32.RealChildWindowFromPoint.argtypes = [wt.HWND, wt.POINT]
    u32.RealChildWindowFromPoint.restype = wt.HWND
    cur = wt.POINT(); u32.GetCursorPos(ctypes.byref(cur))
    hit = u32.WindowFromPoint(cur)
    hcn = ctypes.create_unicode_buffer(256); u32.GetClassNameW(hit, hcn, 256)
    hitpid = wt.DWORD(); u32.GetWindowThreadProcessId(hit, ctypes.byref(hitpid))
    print("光标实际位置 =", (cur.x, cur.y))
    print("该点最顶层窗口 = 0x%X class=%s pid=%d" % (hit, hcn.value, hitpid.value))
    child = u32.RealChildWindowFromPoint(main_hwnd, wt.POINT(cur.x - lr.left, cur.y - lr.top))
    ccn = ctypes.create_unicode_buffer(256); u32.GetClassNameW(child, ccn, 256)
    print("主窗口下命中子控件 = 0x%X class=%s" % (child, ccn.value))

    # 改用 SendInput，并检查返回
    class MOUSEINPUT(ctypes.Structure):
        _fields_ = [("dx", wt.LONG), ("dy", wt.LONG), ("mouseData", wt.DWORD),
                    ("dwFlags", wt.DWORD), ("time", wt.DWORD), ("dwExtraInfo", ctypes.c_void_p)]

    class INPUT(ctypes.Structure):
        class _U(ctypes.Union):
            _fields_ = [("mi", MOUSEINPUT)]
        _anonymous_ = ("u",)
        _fields_ = [("type", wt.DWORD), ("u", _U)]

    def send(flag):
        inp = INPUT(type=0, mi=MOUSEINPUT(0, 0, 0, flag, 0, None))
        return u32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))

    u32.SendInput.argtypes = [wt.UINT, ctypes.c_void_p, ctypes.c_int]
    for i in range(2):
        r1 = send(0x0002)
        r2 = send(0x0004)
        time.sleep(0.05)
    print("SendInput down/up =", r1, r2)
    time.sleep(2.5)

    dump(pid, "双击后")
    return 0


sys.exit(main())
