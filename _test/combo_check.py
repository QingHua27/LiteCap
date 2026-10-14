"""复现：展开各下拉框，量它们的下拉列表窗口高度"""
import ctypes, ctypes.wintypes as wt, time, sys

u32 = ctypes.windll.user32
u32.FindWindowW.argtypes = [wt.LPCWSTR, wt.LPCWSTR]; u32.FindWindowW.restype = wt.HWND
u32.GetDlgItem.argtypes = [wt.HWND, ctypes.c_int]; u32.GetDlgItem.restype = wt.HWND
u32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u32.SendMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]; u32.SendMessageW.restype = ctypes.c_ssize_t
u32.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
u32.IsWindowVisible.argtypes = [wt.HWND]

CB_SHOWDROPDOWN = 0x014F
CB_GETCOUNT = 0x0146
CB_GETDROPPEDSTATE = 0x0157
CB_SETMINVISIBLE = 0x1701
EnumWindows = u32.EnumWindows
EnumWindows.argtypes = [ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM), wt.LPARAM]

COMBOS = [("录制目标", 1001), ("帧率", 1003), ("画质", 1004), ("语言", 1005)]


def main():
    h = u32.FindWindowW("LiteCapMainWnd", None)
    if not h:
        print("FAIL 未找到主窗口"); return 1
    pid = wt.DWORD(); u32.GetWindowThreadProcessId(h, ctypes.byref(pid))

    for name, cid in COMBOS:
        c = u32.GetDlgItem(h, cid)
        if not c:
            print("%s: 控件不存在" % name); continue
        r = wt.RECT(); u32.GetWindowRect(c, ctypes.byref(r))
        n = u32.SendMessageW(c, CB_GETCOUNT, 0, 0)
        mv = u32.SendMessageW(c, CB_SETMINVISIBLE, 10, 0)
        print("%-6s 控件矩形高=%d  项数=%d  CB_SETMINVISIBLE 返回=%d"
              % (name, r.bottom - r.top, n, mv))

        # 展开下拉
        u32.SendMessageW(c, CB_SHOWDROPDOWN, 1, 0)
        time.sleep(0.4)

        # 找可见的 ComboLBox
        found = []
        @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
        def cb(win, l):
            p = wt.DWORD(); u32.GetWindowThreadProcessId(win, ctypes.byref(p))
            if p.value != pid.value or not u32.IsWindowVisible(win):
                return True
            cn = ctypes.create_unicode_buffer(64); u32.GetClassNameW(win, cn, 64)
            if cn.value == "ComboLBox":
                rr = wt.RECT(); u32.GetWindowRect(win, ctypes.byref(rr))
                found.append(rr.bottom - rr.top)
            return True
        EnumWindows(cb, 0)
        print("        展开后下拉列表高度 =", found if found else "(没有可见的 ComboLBox)")
        u32.SendMessageW(c, CB_SHOWDROPDOWN, 0, 0)
        time.sleep(0.2)
    return 0


sys.exit(main())
