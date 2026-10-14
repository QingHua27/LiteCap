import ctypes, ctypes.wintypes as wt, time, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from e2e import capture_screen, find_window

u32 = ctypes.windll.user32
u32.GetDlgItem.argtypes = [wt.HWND, ctypes.c_int]; u32.GetDlgItem.restype = wt.HWND
u32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u32.SendMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]; u32.SendMessageW.restype = ctypes.c_ssize_t

h = find_window("LiteCapMainWnd")
if not h:
    print("FAIL"); sys.exit(1)
time.sleep(0.5)

for name, cid, out in [("帧率", 1003, "combo_fps.png"), ("画质", 1004, "combo_quality.png")]:
    c = u32.GetDlgItem(h, cid)
    r = wt.RECT(); u32.GetWindowRect(c, ctypes.byref(r))
    u32.SendMessageW(c, 0x014F, 1, 0)   # CB_SHOWDROPDOWN
    time.sleep(0.6)
    print(name, "截图:", capture_screen(os.path.join(os.path.dirname(os.path.abspath(__file__)), out),
          (r.left - 20, r.top - 20, r.right + 20, r.top + 220)))
    u32.SendMessageW(c, 0x014F, 0, 0)
    time.sleep(0.3)
