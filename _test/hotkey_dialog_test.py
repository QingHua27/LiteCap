"""验证「设置…」对话框能录制新的组合键，并真的生效。"""
import ctypes
import ctypes.wintypes as wt
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from e2e import find_window, force_foreground, control_text, u32   # noqa: E402
from hotkey_test import key, btn_text, newest_mp4                 # noqa: E402

u32.GetDlgItem.argtypes = [wt.HWND, ctypes.c_int]
u32.GetDlgItem.restype = wt.HWND
u32.GetWindowTextW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
u32.FindWindowW.argtypes = [wt.LPCWSTR, wt.LPCWSTR]
u32.FindWindowW.restype = wt.HWND

IDC_HOTKEY_EDIT = 1012
IDC_HOTKEY_SET = 1013
IDC_RECORD = 1008

VK_CONTROL, VK_SHIFT, VK_F9 = 0x11, 0x10, 0x78
WM_COMMAND = 0x0111

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def edit_text(main):
    b = u32.GetDlgItem(main, IDC_HOTKEY_EDIT)
    return control_text(b) or ""


def main():
    m = find_window("LiteCapMainWnd", timeout=15)
    if not m:
        print("FAIL 未找到主窗口")
        return 1
    force_foreground(m)
    time.sleep(0.5)

    print("[*] 当前快捷键 = %r" % edit_text(m))

    print("[*] 点「设置…」...")
    btn = u32.GetDlgItem(m, IDC_HOTKEY_SET)
    u32.PostMessageW(m, WM_COMMAND, IDC_HOTKEY_SET & 0xFFFF, btn)

    # 等录制窗口出现
    dlg = None
    for _ in range(40):
        time.sleep(0.1)
        dlg = u32.FindWindowW("LiteCapHotkeyWnd", None)
        if dlg:
            break
    if not dlg:
        print("FAIL 录制窗口没出现")
        return 1
    print("[ok] 录制窗口已出现 %s" % hex(dlg))

    time.sleep(0.6)
    print("[*] 按下 Ctrl+Shift+F9 ...")
    key(VK_CONTROL)
    key(VK_SHIFT)
    time.sleep(0.05)
    key(VK_F9)
    time.sleep(0.05)
    key(VK_F9, True)
    key(VK_SHIFT, True)
    key(VK_CONTROL, True)

    time.sleep(1.2)
    if u32.FindWindowW("LiteCapHotkeyWnd", None):
        print("[!] 录制窗口还开着")

    now = edit_text(m)
    print("[*] 新快捷键 = %r" % now)
    ok_set = ("Ctrl" in now and "Shift" in now and "F9" in now)

    # settings.ini 是否落盘
    ini = os.path.join(ROOT, "settings.ini")
    txt = open(ini, "r", encoding="utf-8", errors="replace").read()
    lines = [l for l in txt.splitlines() if l.startswith("hotkey")]
    print("[*] settings.ini: %s" % lines)
    ok_ini = any(l.startswith("hotkey_vk") and l.strip().endswith("120") for l in lines)
    # 120 = 0x78 = VK_F9

    # 新组合键是否真的生效
    before = newest_mp4("D:\\Recordings")
    t0 = btn_text(m)
    print("[*] 用新组合键试一次 ...")
    key(VK_CONTROL)
    key(VK_SHIFT)
    time.sleep(0.05)
    key(VK_F9)
    time.sleep(0.05)
    key(VK_F9, True)
    key(VK_SHIFT, True)
    key(VK_CONTROL, True)
    time.sleep(2.5)
    t1 = btn_text(m)
    ok_fire = (t1 != t0)
    print("    => %s" % ("新热键触发了录制" if ok_fire else "[x] 新热键无效"))

    if ok_fire:
        # 停掉并等文件写完
        key(VK_CONTROL)
        key(VK_SHIFT)
        time.sleep(0.05)
        key(VK_F9)
        time.sleep(0.05)
        key(VK_F9, True)
        key(VK_SHIFT, True)
        key(VK_CONTROL, True)
        time.sleep(3.5)

    print()
    print("设置生效 = %s | 落盘 = %s | 可触发 = %s" % (ok_set, ok_ini, ok_fire))
    ok = ok_set and ok_ini and ok_fire
    print("PASS 自定义快捷键可用" if ok else "FAIL 自定义快捷键异常")
    return 0 if ok else 1


sys.exit(main())
