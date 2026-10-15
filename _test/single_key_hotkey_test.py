"""Single-key capture, registration, persistence and recording in an isolated copy.

Usage: py -3.12 _test/single_key_hotkey_test.py [path/to/LiteCap.exe]
"""
import ctypes
import ctypes.wintypes as wt
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

from e2e import u32, control_text, force_foreground
from hotkey_test import key, btn_text

WM_COMMAND, WM_KEYDOWN, WM_CLOSE = 0x111, 0x100, 0x10
VK_ESCAPE, VK_BACK, VK_DELETE, VK_F8 = 0x1B, 0x08, 0x2E, 0x77
u32.RegisterHotKey.argtypes = [wt.HWND, ctypes.c_int, wt.UINT, wt.UINT]
u32.UnregisterHotKey.argtypes = [wt.HWND, ctypes.c_int]
u32.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
ENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
u32.EnumWindows.argtypes = [ENUMPROC, wt.LPARAM]


def wait_for(predicate, message, timeout=5):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        result = predicate()
        if result:
            return result
        time.sleep(0.05)
    raise AssertionError(message)


def window(pid, class_name):
    found = []

    @ENUMPROC
    def visit(hwnd, _):
        owner = wt.DWORD()
        u32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        name = ctypes.create_unicode_buffer(128)
        u32.GetClassNameW(hwnd, name, len(name))
        if owner.value == pid and name.value == class_name:
            found.append(hwnd)
        return True

    u32.EnumWindows(visit, 0)
    return found[0] if found else None


def open_dialog(main, pid):
    u32.PostMessageW(main, WM_COMMAND, 1013, u32.GetDlgItem(main, 1013))
    return wait_for(lambda: window(pid, "LiteCapHotkeyWnd"), "capture dialog missing")


def capture(main, pid, vk):
    dialog = open_dialog(main, pid)
    u32.PostMessageW(dialog, WM_KEYDOWN, vk, 0)
    wait_for(lambda: not window(pid, "LiteCapHotkeyWnd"),
             f"single key 0x{vk:X} was rejected")
    wait_for(lambda: u32.IsWindowEnabled(main), "parent stayed disabled")


def check_registered(vk, mod=0):
    # A second registration must fail while the app owns this global hotkey.
    registered = u32.RegisterHotKey(None, 0x6A01, mod, vk)
    if registered:
        u32.UnregisterHotKey(None, 0x6A01)
    assert not registered, f"key 0x{vk:X} is not registered"


def close(app, main):
    u32.PostMessageW(main, WM_CLOSE, 0, 0)
    app.wait(timeout=6)


def main():
    source = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1] / "LiteCap.exe"
    app = None
    with tempfile.TemporaryDirectory(prefix="LiteCap-hotkey-") as directory:
        root = Path(directory)
        exe = root / "LiteCap.exe"
        shutil.copy2(source, exe)
        config = root / "settings.ini"
        config.write_text(f"close_action=exit\nhotkey_mod=0\nhotkey_vk=0\naudio_system=0\naudio_microphone=0\n"
                          f"target=region\nregion=0,0,160,120\nfps=10\nencoder=software\n"
                          f"output_dir={root}\n", encoding="utf-8")
        try:
            app = subprocess.Popen([str(exe)], cwd=root)
            hwnd = wait_for(lambda: window(app.pid, "LiteCapMainWnd"), "main window missing", 15)
            for vk, label in [(VK_F8, "F8"), (ord("R"), "R"), (ord("8"), "8")]:
                capture(hwnd, app.pid, vk)
                text = control_text(u32.GetDlgItem(hwnd, 1012))
                assert text == label, (label, text)
                saved = dict(line.split("=", 1) for line in config.read_text(encoding="utf-8-sig").splitlines() if "=" in line)
                assert saved["hotkey_mod"] == "0" and int(saved["hotkey_vk"]) == vk, saved
                check_registered(vk)
                print(f"PASS capture/display/save/register: {label}")

            capture(hwnd, app.pid, VK_F8)
            close(app, hwnd)
            app = subprocess.Popen([str(exe)], cwd=root)
            hwnd = wait_for(lambda: window(app.pid, "LiteCapMainWnd"), "restart missing", 15)
            wait_for(lambda: control_text(u32.GetDlgItem(hwnd, 1012)) == "F8", "F8 not restored")
            check_registered(VK_F8)
            print("PASS restart restores single-key hotkey")

            force_foreground(hwnd)
            idle = btn_text(hwnd)
            key(VK_F8)
            key(VK_F8, True)
            wait_for(lambda: btn_text(hwnd) != idle, "F8 did not start recording")
            time.sleep(0.5)
            key(VK_F8)
            key(VK_F8, True)
            wait_for(lambda: btn_text(hwnd) == idle, "F8 did not stop recording", 10)
            assert any(p.stat().st_size > 0 for p in root.glob("*.mp4")), "no recorded MP4"
            print("PASS real F8 input starts/stops recording and produces MP4")

            capture(hwnd, app.pid, VK_ESCAPE)
            assert control_text(u32.GetDlgItem(hwnd, 1012)) == "F8", "Escape changed hotkey"
            for clear in (VK_BACK, VK_DELETE):
                capture(hwnd, app.pid, clear)
                saved = config.read_text(encoding="utf-8-sig")
                assert "hotkey_vk=0\n" in saved, saved
                capture(hwnd, app.pid, VK_F8)
            print("PASS Escape cancels; Backspace/Delete clear")

            dialog = open_dialog(hwnd, app.pid)
            force_foreground(dialog)
            key(0x11)  # Ctrl alone must keep waiting for a primary key.
            time.sleep(0.1)
            assert window(app.pid, "LiteCapHotkeyWnd"), "modifier alone was accepted"
            key(0x10)  # Shift
            key(0x78)  # F9
            key(0x78, True)
            key(0x10, True)
            key(0x11, True)
            wait_for(lambda: not window(app.pid, "LiteCapHotkeyWnd"), "combination rejected")
            wait_for(lambda: control_text(u32.GetDlgItem(hwnd, 1012)) == "Ctrl+Shift+F9",
                     "combination display changed")
            check_registered(0x78, 0x2 | 0x4)
            print("PASS modifier alone waits; Ctrl+Shift+F9 still registers")
            close(app, hwnd)
        finally:
            for modifier in (0x11, 0x10):
                key(modifier, True)
            if app and app.poll() is None:
                app.kill()
                app.wait(timeout=5)
    print("PASS all single-key hotkey checks")
    return 0


if __name__ == "__main__":
    sys.exit(main())
