"""验证全局快捷键能否开始/停止录制。

用 SendInput 模拟真实按键（RegisterHotKey 只认真实输入，
PostMessage 伪造的按键不会触发 WM_HOTKEY）。
"""
import ctypes
import ctypes.wintypes as wt
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from e2e import find_window, u32   # noqa: E402

u32.GetDlgItem.argtypes = [wt.HWND, ctypes.c_int]
u32.GetDlgItem.restype = wt.HWND
u32.GetWindowTextW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]

IDC_RECORD = 1008


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", wt.LONG), ("dy", wt.LONG), ("mouseData", wt.DWORD),
                ("dwFlags", wt.DWORD), ("time", wt.DWORD),
                ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong))]


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wt.WORD), ("wScan", wt.WORD), ("dwFlags", wt.DWORD),
                ("time", wt.DWORD), ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong))]


class HARDWAREINPUT(ctypes.Structure):
    _fields_ = [("uMsg", wt.DWORD), ("wParamL", wt.WORD), ("wParamH", wt.WORD)]


class _U(ctypes.Union):
    _fields_ = [("mi", MOUSEINPUT), ("ki", KEYBDINPUT), ("hi", HARDWAREINPUT)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wt.DWORD), ("u", _U)]


u32.SendInput.argtypes = [wt.UINT, ctypes.POINTER(INPUT), ctypes.c_int]

KEYEVENTF_KEYUP = 0x0002
VK_CONTROL, VK_MENU, VK_R = 0x11, 0x12, 0x52


def key(vk, up=False):
    inp = INPUT()
    inp.type = 1                       # INPUT_KEYBOARD
    inp.u.ki.wVk = vk
    inp.u.ki.dwFlags = KEYEVENTF_KEYUP if up else 0
    u32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))


def press_combo():
    key(VK_CONTROL)
    key(VK_MENU)
    time.sleep(0.05)
    key(VK_R)
    time.sleep(0.05)
    key(VK_R, True)
    key(VK_MENU, True)
    key(VK_CONTROL, True)


def btn_text(main):
    b = u32.GetDlgItem(main, IDC_RECORD)
    buf = ctypes.create_unicode_buffer(64)
    u32.GetWindowTextW(b, buf, 64)
    return buf.value


def newest_mp4(d):
    if not os.path.isdir(d):
        return None
    fs = [os.path.join(d, f) for f in os.listdir(d) if f.endswith(".mp4")]
    return max(fs, key=os.path.getmtime) if fs else None


def main():
    main_wnd = find_window("LiteCapMainWnd", timeout=15)
    if not main_wnd:
        print("FAIL 未找到主窗口")
        return 1

    rec_dir = "D:\\Recordings"
    before = newest_mp4(rec_dir)
    t0 = btn_text(main_wnd)
    print("[*] 初始按钮文字 = %r" % t0)

    print("[*] 发送 Ctrl+Alt+R ...")
    press_combo()
    time.sleep(2.5)
    t1 = btn_text(main_wnd)
    print("[*] 按下后按钮文字 = %r" % t1)

    started = (t1 != t0)
    print("    => 录制已开始" if started else "    => [x] 热键没有生效")

    print("[*] 再发一次 Ctrl+Alt+R（应当停止）...")
    press_combo()
    time.sleep(3.5)
    t2 = btn_text(main_wnd)
    print("[*] 再按后按钮文字 = %r" % t2)

    stopped = (t2 == t0)
    print("    => 录制已停止" if stopped else "    => [x] 没有停止")

    after = newest_mp4(rec_dir)
    newfile = after and after != before
    if newfile:
        print("[ok] 产生新录像 = %s (%.1f MB)"
              % (os.path.basename(after), os.path.getsize(after) / 1e6))
    else:
        print("[!] 没有产生新录像")

    ok = started and stopped and newfile
    print("\n%s" % ("PASS 全局快捷键可用" if ok else "FAIL 全局快捷键异常"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
