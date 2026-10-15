"""回归：关闭快捷键录制窗口后，父窗口恢复输入并可正常退出。

问题根因：快捷键窗口的 DefWindowProc 收到 WM_CLOSE 后会销毁窗口，但模态循环
仍在等 g_hkDone；主窗口因而永远处于 EnableWindow(FALSE) 状态。
"""
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from e2e import u32, MAIN_CLASS, IDC_LIST

EXE = os.path.join(ROOT, "LiteCap.exe")
IDC_RECORD = 1008
IDC_AUDIO_SYSTEM = 1017
WM_COMMAND = 0x0111
WM_CLOSE = 0x0010
WM_KEYDOWN = 0x0100
WM_SYSKEYDOWN = 0x0104
VK_ESCAPE = 0x1B
VK_F4 = 0x73
BM_CLICK = 0x00F5
BM_GETCHECK = 0x00F0
BST_CHECKED = 1

failures = []

def check(name, ok, detail=""):
    print(("  [ok] " if ok else "  [FAIL] ") + name + (("  " + detail) if detail else ""))
    if not ok:
        failures.append(name)
    return ok

def wait_dialog(timeout=5):
    end = time.time() + timeout
    while time.time() < end:
        h = u32.FindWindowW("LiteCapHotkeyWnd", None)
        if h:
            return h
        time.sleep(0.05)
    return None

def wait_dialog_gone(timeout=5):
    end = time.time() + timeout
    while time.time() < end:
        if not u32.FindWindowW("LiteCapHotkeyWnd", None):
            return True
        time.sleep(0.05)
    return False

def wait_main(procid, timeout=20):
    end = time.time() + timeout
    pid = wt.DWORD()
    while time.time() < end:
        h = u32.FindWindowW(MAIN_CLASS, None)
        if h and u32.GetDlgItem(h, IDC_LIST):
            u32.GetWindowThreadProcessId(h, ctypes.byref(pid))
            if pid.value == procid:
                return h
        time.sleep(0.15)
    return None

def open_dialog(main):
    btn = u32.GetDlgItem(main, 1013)  # IDC_HOTKEY_SET
    u32.PostMessageW(main, WM_COMMAND, 1013, btn)
    return wait_dialog()

def parent_restored(main, dialog_closed):
    return (dialog_closed and u32.IsWindow(main) and u32.IsWindowEnabled(main))

def main():
    cfg = os.path.join(ROOT, "settings.ini")
    hist = os.path.join(ROOT, "history.tsv")
    saved = {p: open(p, "rb").read() if os.path.exists(p) else None for p in (cfg, hist)}
    app = None
    try:
        app = subprocess.Popen([EXE], cwd=ROOT)
        main_hwnd = wait_main(app.pid)
        if not main_hwnd:
            check("LiteCap 主窗口启动", False)
            return 1
        print("主窗口 hwnd = 0x%X" % main_hwnd)

        print("\n[1] 点标题栏 X 关闭快捷键录制窗口")
        dlg = open_dialog(main_hwnd)
        check("快捷键录制窗口出现", bool(dlg), "0x%X" % (dlg or 0))
        check("模态期间主窗口禁用", not bool(u32.IsWindowEnabled(main_hwnd)))
        if dlg:
            # 点 X 最终向窗口发送 WM_CLOSE。直接投递相同消息稳定复现系统默认处理。
            u32.PostMessageW(dlg, WM_CLOSE, 0, 0)
        closed = wait_dialog_gone()
        check("关闭后快捷键窗口消失", closed)
        check("父窗口恢复启用", parent_restored(main_hwnd, closed))

        print("\n[2] 验证父窗口控件还能响应点击")
        check_box = u32.GetDlgItem(main_hwnd, IDC_AUDIO_SYSTEM)
        before = u32.SendMessageW(check_box, BM_GETCHECK, 0, 0)
        u32.SendMessageW(check_box, BM_CLICK, 0, 0)
        time.sleep(0.15)
        after = u32.SendMessageW(check_box, BM_GETCHECK, 0, 0)
        check("主窗口复选框可以点击", before != after,
              "%s -> %s" % ("checked" if before == BST_CHECKED else "unchecked",
                             "checked" if after == BST_CHECKED else "unchecked"))
        u32.SendMessageW(check_box, BM_CLICK, 0, 0)  # 恢复原状态

        print("\n[3] Alt+F4 关闭快捷键录制窗")
        dlg = open_dialog(main_hwnd)
        check("第三次打开录制窗口", bool(dlg))
        if dlg:
            # bit 29 表示系统键上下文（Alt 按下），应走系统关闭而非录入快捷键。
            u32.PostMessageW(dlg, WM_SYSKEYDOWN, VK_F4, 1 << 29)
        closed = wait_dialog_gone()
        check("Alt+F4 后父窗口恢复启用", parent_restored(main_hwnd, closed))

        print("\n[4] Esc 取消也恢复父窗口")
        dlg = open_dialog(main_hwnd)
        check("第四次打开录制窗口", bool(dlg))
        if dlg:
            u32.PostMessageW(dlg, WM_KEYDOWN, VK_ESCAPE, 0)
        closed = wait_dialog_gone()
        check("Esc 后父窗口恢复启用", parent_restored(main_hwnd, closed))

        print("\n[5] 主窗口可以通过托盘退出命令正常结束进程")
        u32.PostMessageW(main_hwnd, WM_COMMAND, 4102, 0)  # IDM_TRAY_EXIT
        try:
            app.wait(timeout=6)
            exited = True
        except subprocess.TimeoutExpired:
            exited = False
        check("关闭主窗口后进程退出", exited, "returncode=%r" % app.poll())
    finally:
        if app and app.poll() is None:
            app.kill()
            try:
                app.wait(timeout=5)
            except Exception:
                pass
        for path, data in saved.items():
            if data is None:
                try:
                    os.remove(path)
                except OSError:
                    pass
            else:
                with open(path, "wb") as f:
                    f.write(data)

    print("\n" + "=" * 52)
    if failures:
        print("失败 %d 项: %s" % (len(failures), ", ".join(failures)))
        return 1
    print("全部通过")
    return 0

if __name__ == "__main__":
    sys.exit(main())
