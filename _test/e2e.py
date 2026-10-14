"""LiteCap C++ 版端到端测试：启动 -> 截图 -> 录制 -> 记录 -> 悬浮窗 -> 退出"""
import ctypes, ctypes.wintypes as wt, os, struct, sys, time, zlib

u32 = ctypes.windll.user32
g32 = ctypes.windll.gdi32

# 关键：HWND/HDC/HBITMAP 都是 64 位指针，必须显式声明返回类型，否则会被截断成 int32
u32.FindWindowW.argtypes = [wt.LPCWSTR, wt.LPCWSTR]
u32.FindWindowW.restype = wt.HWND
u32.GetDlgItem.argtypes = [wt.HWND, ctypes.c_int]
u32.GetDlgItem.restype = wt.HWND
u32.GetWindowDC.argtypes = [wt.HWND]
u32.GetWindowDC.restype = wt.HDC
u32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u32.ReleaseDC.argtypes = [wt.HWND, wt.HDC]
u32.PrintWindow.argtypes = [wt.HWND, wt.HDC, wt.UINT]
u32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
u32.SendMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
u32.SendMessageW.restype = ctypes.c_ssize_t
u32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
u32.ClientToScreen.argtypes = [wt.HWND, ctypes.POINTER(wt.POINT)]
u32.SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]
u32.SendInput.argtypes = [wt.UINT, ctypes.c_void_p, ctypes.c_int]
u32.SetProcessDpiAwarenessContext.argtypes = [ctypes.c_void_p]

g32.CreateCompatibleDC.argtypes = [wt.HDC]
g32.CreateCompatibleDC.restype = wt.HDC
g32.CreateDIBSection.argtypes = [wt.HDC, ctypes.c_void_p, wt.UINT,
                                 ctypes.POINTER(ctypes.c_void_p), wt.HANDLE, wt.DWORD]
g32.CreateDIBSection.restype = wt.HBITMAP
g32.SelectObject.argtypes = [wt.HDC, wt.HANDLE]
g32.SelectObject.restype = wt.HANDLE
g32.DeleteObject.argtypes = [wt.HANDLE]
g32.DeleteDC.argtypes = [wt.HDC]
# 不声明的话 HDC 会被当 int32 传，句柄值一大就 OverflowError
g32.BitBlt.argtypes = [wt.HDC, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                       wt.HDC, ctypes.c_int, ctypes.c_int, wt.DWORD]
g32.BitBlt.restype = wt.BOOL

try:
    u32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    pass

MAIN_CLASS = "LiteCapMainWnd"
IDC_RECORD = 1008
IDC_FLOAT = 1009
IDC_LIST = 1010

SRCCOPY = 0x00CC0020
DIB_RGB_COLORS = 0
BI_RGB = 0
PW_RENDERFULLCONTENT = 2

MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", wt.LONG), ("dy", wt.LONG), ("mouseData", wt.DWORD),
                ("dwFlags", wt.DWORD), ("time", wt.DWORD), ("dwExtraInfo", ctypes.c_void_p)]


class INPUT(ctypes.Structure):
    class _U(ctypes.Union):
        _fields_ = [("mi", MOUSEINPUT)]
    _anonymous_ = ("u",)
    _fields_ = [("type", wt.DWORD), ("u", _U)]


def send_mouse(flag):
    inp = INPUT(type=0, mi=MOUSEINPUT(0, 0, 0, flag, 0, None))
    return u32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG),
                ("biPlanes", wt.WORD), ("biBitCount", wt.WORD), ("biCompression", wt.DWORD),
                ("biSizeImage", wt.DWORD), ("biXPelsPerMeter", wt.LONG),
                ("biYPelsPerMeter", wt.LONG), ("biClrUsed", wt.DWORD), ("biClrImportant", wt.DWORD)]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", BITMAPINFOHEADER), ("bmiColors", wt.DWORD * 3)]


def find_window(cls, timeout=15):
    end = time.time() + timeout
    while time.time() < end:
        h = u32.FindWindowW(cls, None)
        if h:
            return h
        time.sleep(0.2)
    return None


def capture(hwnd, path):
    rect = wt.RECT()
    u32.GetWindowRect(hwnd, ctypes.byref(rect))
    w, h = rect.right - rect.left, rect.bottom - rect.top
    if w <= 0 or h <= 0:
        return None

    hdc = u32.GetWindowDC(hwnd)
    mem = g32.CreateCompatibleDC(hdc)
    bmi = BITMAPINFO()
    bmi.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bmi.bmiHeader.biWidth = w
    bmi.bmiHeader.biHeight = -h
    bmi.bmiHeader.biPlanes = 1
    bmi.bmiHeader.biBitCount = 32
    bmi.bmiHeader.biCompression = BI_RGB

    bits = ctypes.c_void_p()
    bmp = g32.CreateDIBSection(hdc, ctypes.byref(bmi), DIB_RGB_COLORS, ctypes.byref(bits), None, 0)
    old = g32.SelectObject(mem, bmp)
    u32.PrintWindow(hwnd, mem, PW_RENDERFULLCONTENT)

    buf = ctypes.string_at(bits, w * h * 4)
    g32.SelectObject(mem, old)
    g32.DeleteObject(bmp)
    g32.DeleteDC(mem)
    u32.ReleaseDC(hwnd, hdc)
    return _write_png(path, w, h, buf)


def capture_screen(path, rect=None):
    """抓真实屏幕（PrintWindow 抓不到弹出菜单这类独立窗口）。"""
    vx = u32.GetSystemMetrics(76)   # SM_XVIRTUALSCREEN
    vy = u32.GetSystemMetrics(77)
    vw = u32.GetSystemMetrics(78)
    vh = u32.GetSystemMetrics(79)
    if rect:
        vx, vy = rect[0], rect[1]
        vw, vh = rect[2] - rect[0], rect[3] - rect[1]
    w, h = vw, vh

    hdc = u32.GetDC(None)
    mem = g32.CreateCompatibleDC(hdc)
    bmi = BITMAPINFO()
    bmi.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bmi.bmiHeader.biWidth = w
    bmi.bmiHeader.biHeight = -h
    bmi.bmiHeader.biPlanes = 1
    bmi.bmiHeader.biBitCount = 32
    bmi.bmiHeader.biCompression = BI_RGB
    bits = ctypes.c_void_p()
    bmp = g32.CreateDIBSection(hdc, ctypes.byref(bmi), DIB_RGB_COLORS, ctypes.byref(bits), None, 0)
    old = g32.SelectObject(mem, bmp)
    g32.BitBlt(mem, 0, 0, w, h, hdc, vx, vy, SRCCOPY | 0x40000000)  # CAPTUREBLT
    buf = ctypes.string_at(bits, w * h * 4)
    g32.SelectObject(mem, old)
    g32.DeleteObject(bmp)
    g32.DeleteDC(mem)
    u32.ReleaseDC(None, hdc)
    return _write_png(path, w, h, buf)


def _write_png(path, w, h, buf):
    rows = bytearray()
    for y in range(h):
        row = buf[y * w * 4:(y + 1) * w * 4]
        rgb = bytearray()
        for x in range(w):
            rgb += bytes((row[x * 4 + 2], row[x * 4 + 1], row[x * 4]))
        rows.append(0)
        rows += rgb

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(bytes(rows), 6))
           + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)
    return w, h


def click(main, ctrl_id):
    btn = u32.GetDlgItem(main, ctrl_id)
    if not btn:
        return False
    # WM_COMMAND: LOWORD=id, HIWORD=0(BN_CLICKED 分支兼容 0)
    u32.PostMessageW(main, 0x0111, ctrl_id & 0xFFFF, btn)
    return True


def force_foreground(hwnd):
    """AttachThreadInput 绕过 Windows 前台锁定，让测试进程能把 LiteCap 提到最前。"""
    k = ctypes.windll.kernel32
    u32.GetForegroundWindow.restype = wt.HWND
    k.GetCurrentThreadId.restype = wt.DWORD
    u32.AttachThreadInput.argtypes = [wt.DWORD, wt.DWORD, wt.BOOL]
    u32.SetForegroundWindow.argtypes = [wt.HWND]
    u32.BringWindowToTop.argtypes = [wt.HWND]

    fg = u32.GetForegroundWindow()
    fg_tid = u32.GetWindowThreadProcessId(fg, None) if fg else 0
    my_tid = k.GetCurrentThreadId()
    u32.AttachThreadInput(my_tid, fg_tid, True)
    u32.ShowWindow(hwnd, 9)          # SW_RESTORE
    u32.BringWindowToTop(hwnd)
    u32.SetForegroundWindow(hwnd)
    u32.AttachThreadInput(my_tid, fg_tid, False)
    time.sleep(0.3)
    return u32.GetForegroundWindow() == hwnd


def double_click_first_row(main):
    """取列表第一项矩形 -> 真实鼠标双击。
    必须用真实输入：ListView 的双击判定读的是消息队列里的 GetMessagePos/GetMessageTime，
    用 SendMessage 投递的合成消息不会触发 NM_DBLCLK。"""
    kernel32 = ctypes.windll.kernel32
    lst = u32.GetDlgItem(main, IDC_LIST)
    if not lst:
        print("  列表控件不存在")
        return False

    # --- 远程内存取第一项矩形（LVM_GETITEMRECT 需要指针参数）---
    pid = wt.DWORD()
    u32.GetWindowThreadProcessId(lst, ctypes.byref(pid))
    PROCESS_VM = 0x0008 | 0x0010 | 0x0020
    kernel32.OpenProcess.restype = wt.HANDLE
    kernel32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
    kernel32.VirtualAllocEx.restype = ctypes.c_void_p
    kernel32.VirtualAllocEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_size_t, wt.DWORD, wt.DWORD]
    kernel32.WriteProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
    kernel32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
    kernel32.VirtualFreeEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_size_t, wt.DWORD]
    kernel32.CloseHandle.argtypes = [wt.HANDLE]

    hp = kernel32.OpenProcess(PROCESS_VM, False, pid.value)
    if not hp:
        print("  OpenProcess 失败")
        return False
    p = kernel32.VirtualAllocEx(hp, None, 4096, 0x1000 | 0x2000, 0x04)
    r = wt.RECT(0, 0, 0, 0)          # left=行号, top=LVIR_BOUNDS
    n = ctypes.c_size_t()
    kernel32.WriteProcessMemory(hp, p, ctypes.byref(r), ctypes.sizeof(r), ctypes.byref(n))
    ok = u32.SendMessageW(lst, 0x100E, 0, p)   # LVM_GETITEMRECT
    kernel32.ReadProcessMemory(hp, p, ctypes.byref(r), ctypes.sizeof(r), ctypes.byref(n))
    kernel32.VirtualFreeEx(hp, p, 0, 0x8000)
    kernel32.CloseHandle(hp)
    if not ok:
        print("  LVM_GETITEMRECT 失败（列表为空？）")
        return False

    # 列表客户区坐标 -> 屏幕坐标
    pt = wt.POINT((r.left + r.right) // 2, (r.top + r.bottom) // 2)
    u32.ClientToScreen(lst, ctypes.byref(pt))
    print("  第一项矩形 = (%d,%d,%d,%d) -> 屏幕点击 (%d,%d)" % (r.left, r.top, r.right, r.bottom, pt.x, pt.y))

    print("  提升前台窗口 =", force_foreground(main))
    u32.SetCursorPos(pt.x, pt.y)
    time.sleep(0.3)
    # 注意：必须用 SendInput。已废弃的 mouse_event 在当前系统上不产生双击，
    # 且 ListView 的双击判定读的是消息队列状态，SendMessage 合成的消息同样无效。
    for _ in range(2):
        send_mouse(MOUSEEVENTF_LEFTDOWN)
        send_mouse(MOUSEEVENTF_LEFTUP)
        time.sleep(0.05)
    return True


def main():
    out = os.path.dirname(os.path.abspath(__file__))
    main_hwnd = find_window(MAIN_CLASS)
    if not main_hwnd:
        print("FAIL: 未找到主窗口")
        return 1
    print("OK 找到主窗口 hwnd=0x%X" % main_hwnd)

    time.sleep(0.6)
    print("shot1(空闲) =", capture(main_hwnd, os.path.join(out, "shot1_idle.png")))

    # 开始录制
    print("click 开始录制 =", click(main_hwnd, IDC_RECORD))
    time.sleep(3.5)
    print("shot2(录制中) =", capture(main_hwnd, os.path.join(out, "shot2_recording.png")))

    # 悬浮窗模式
    print("click 悬浮窗 =", click(main_hwnd, IDC_FLOAT))
    time.sleep(1.2)
    bar = u32.FindWindowW("LiteCapBarWnd", None)
    print("悬浮窗 hwnd = 0x%X" % (bar or 0))
    if bar:
        print("shot3(悬浮窗) =", capture(bar, os.path.join(out, "shot3_bar.png")))
        u32.PostMessageW(main_hwnd, 0x0111, IDC_FLOAT & 0xFFFF, u32.GetDlgItem(main_hwnd, IDC_FLOAT))
        time.sleep(1.0)

    # 停止录制
    print("click 停止录制 =", click(main_hwnd, IDC_RECORD))
    time.sleep(3.5)
    print("shot4(完成后) =", capture(main_hwnd, os.path.join(out, "shot4_done.png")))

    # ---- 软件内播放：双击列表第一行 ----
    print("双击列表打开播放器 =", double_click_first_row(main_hwnd))
    time.sleep(2.5)
    pw = u32.FindWindowW("LiteCapPlayerWnd", None)
    print("播放器窗口 hwnd = 0x%X" % (pw or 0))
    if pw:
        print("shot5(软件内播放) =", capture(pw, os.path.join(out, "shot5_player.png")))
        # 空格键暂停再播放，验证播放控制
        u32.PostMessageW(pw, 0x0100, 0x20, 0)   # WM_KEYDOWN VK_SPACE
        time.sleep(1.2)
        print("shot6(暂停后) =", capture(pw, os.path.join(out, "shot6_paused.png")))
        u32.PostMessageW(pw, 0x0010, 0, 0)      # WM_CLOSE
        time.sleep(1.0)
        print("播放器已关闭 =", not bool(u32.FindWindowW("LiteCapPlayerWnd", None)))
    else:
        print("FAIL: 播放器窗口未出现")

    # ---- 右键菜单 ----
    print("提升前台窗口 =", force_foreground(main_hwnd))
    lst = u32.GetDlgItem(main_hwnd, IDC_LIST)
    lr = wt.RECT(); u32.GetWindowRect(lst, ctypes.byref(lr))
    rp = wt.POINT(lr.left + 120, lr.top + 32)
    u32.SetCursorPos(rp.x, rp.y)
    time.sleep(0.3)
    send_mouse(0x0008)   # MOUSEEVENTF_RIGHTDOWN
    send_mouse(0x0010)   # MOUSEEVENTF_RIGHTUP
    time.sleep(1.0)
    print("shot7(右键菜单) =", capture_screen(
        os.path.join(out, "shot7_menu.png"),
        (rp.x - 40, rp.y - 40, rp.x + 340, rp.y + 260)))
    u32.keybd_event(0x1B, 0, 0, 0)        # VK_ESCAPE
    u32.keybd_event(0x1B, 0, 2, 0)
    time.sleep(0.8)

    return 0


if __name__ == "__main__":
    sys.exit(main())
