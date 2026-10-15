"""Isolated app regression: missing recordings, Ctrl/Shift selection, tray lifecycle.

Usage: python history_tray_test.py [path/to/LiteCap.exe]
"""
import ctypes as c
import ctypes.wintypes as w
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import struct

from e2e import u32, force_foreground
from single_key_hotkey_test import window, wait_for
from hotkey_test import key

LVM_FIRST = 0x1000
WM_COMMAND, WM_CLOSE = 0x111, 0x10
TRAY_MESSAGE = 0x8000 + 100
IDM_TRAY_SHOW, IDM_TRAY_EXIT, IDM_CLOSE_TRAY, IDM_CLOSE_EXIT = 4101, 4102, 4103, 4104
failures = []


def check(name, condition):
    print(('PASS ' if condition else 'FAIL ') + name, flush=True)
    if not condition:
        failures.append(name)


def click_row(lst, row, modifier=0):
    # Read the item rectangle through remote memory (LVM pointers are not marshalled).
    k32 = c.windll.kernel32
    k32.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
    k32.OpenProcess.restype = w.HANDLE
    k32.VirtualAllocEx.argtypes = [w.HANDLE, c.c_void_p, c.c_size_t, w.DWORD, w.DWORD]
    k32.VirtualAllocEx.restype = c.c_void_p
    k32.WriteProcessMemory.argtypes = [w.HANDLE, c.c_void_p, c.c_void_p, c.c_size_t, c.c_void_p]
    k32.ReadProcessMemory.argtypes = [w.HANDLE, c.c_void_p, c.c_void_p, c.c_size_t, c.c_void_p]
    k32.VirtualFreeEx.argtypes = [w.HANDLE, c.c_void_p, c.c_size_t, w.DWORD]
    k32.CloseHandle.argtypes = [w.HANDLE]
    pid = w.DWORD()
    u32.GetWindowThreadProcessId(lst, c.byref(pid))
    process = k32.OpenProcess(0x38, False, pid.value)
    remote = k32.VirtualAllocEx(process, None, c.sizeof(w.RECT), 0x3000, 4)
    try:
        rect = w.RECT(0, 0, 0, 0)
        assert k32.WriteProcessMemory(process, remote, c.byref(rect), c.sizeof(rect), None)
        assert u32.SendMessageW(lst, LVM_FIRST + 14, row, remote)
        assert k32.ReadProcessMemory(process, remote, c.byref(rect), c.sizeof(rect), None)
        point = ((rect.top + rect.bottom) // 2 << 16) | (rect.left + 12)
        vk = 0x11 if modifier == 8 else 0x10 if modifier == 4 else None
        if vk:
            key(vk)
            time.sleep(0.05)
        u32.PostMessageW(lst, 0x201, 1 | modifier, point)
        u32.PostMessageW(lst, 0x202, modifier, point)
        time.sleep(0.15)
        if vk:
            key(vk, True)
            time.sleep(0.05)
    finally:
        k32.VirtualFreeEx(process, remote, 0, 0x8000)
        k32.CloseHandle(process)


def selected(lst):
    result = []
    row = -1
    while True:
        row = u32.SendMessageW(lst, LVM_FIRST + 12, row, 2)
        if row < 0:
            return result
        result.append(row)


def main():
    source = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1] / 'LiteCap.exe'
    app = None
    locked_file = None
    with tempfile.TemporaryDirectory(prefix='LiteCap-history-tray-') as directory:
        root = Path(directory)
        exe = root / 'LiteCap.exe'
        shutil.copy2(source, exe)
        config = root / 'settings.ini'
        config.write_text('hotkey_mod=0\nhotkey_vk=0\nhistory_open=1\naudio_system=0\naudio_microphone=0\n'
                          f'target=region\nregion=0,0,160,120\nfps=10\nencoder=software\noutput_dir={root}\n', encoding='utf-8')
        files = [root / f'recording-{i}.mp4' for i in range(6)]
        for path in files:
            path.write_bytes(b'test recording')
        missing = root / 'already-deleted.mp4'
        history = root / 'history.tsv'
        history.write_text(''.join(f'{i}\t1000\t14\t{path}\n' for i, path in enumerate([*files, missing])), encoding='utf-8')
        try:
            app = subprocess.Popen([str(exe)], cwd=root)
            hwnd = wait_for(lambda: window(app.pid, 'LiteCapMainWnd'), 'main missing', 15)
            lst = wait_for(lambda: u32.GetDlgItem(hwnd, 1010), 'list missing')
            force_foreground(hwnd)
            time.sleep(0.2)
            check('missing source removed on startup', u32.SendMessageW(lst, LVM_FIRST + 4, 0, 0) == 6)
            check('cleanup persisted to history.tsv', 'already-deleted.mp4' not in history.read_text(encoding='utf-8-sig'))
            click_row(lst, 0)
            click_row(lst, 2, 8)  # MK_CONTROL
            check('Ctrl click selects non-adjacent rows', selected(lst) == [0, 2])
            click_row(lst, 4, 4)  # MK_SHIFT
            check('Shift click selects a range', all(row in selected(lst) for row in (2, 3, 4)))
            click_row(lst, 2)
            files[0].unlink()
            try:
                wait_for(lambda: u32.SendMessageW(lst, LVM_FIRST + 4, 0, 0) == 5,
                         'runtime cleanup did not occur', 5)
                removed = True
            except AssertionError:
                removed = False
            check('external deletion removed while app stays open', removed)
            check('cleanup keeps surviving selected row', removed and selected(lst) == [1])

            click_row(lst, 1)
            click_row(lst, 3, 8)
            u32.SendMessageW(hwnd, WM_COMMAND, 4004, 0)
            check('batch remove updates selected records', u32.SendMessageW(lst, LVM_FIRST + 4, 0, 0) == 3)
            check('remove from list keeps source files', files[2].exists() and files[4].exists())
            k32 = c.windll.kernel32
            k32.SetFileAttributesW.argtypes = [w.LPCWSTR, w.DWORD]
            assert k32.SetFileAttributesW(str(files[1]), 1)  # Deliberately fail one deletion.
            k32.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, c.c_void_p, w.DWORD, w.DWORD, w.HANDLE]
            k32.CreateFileW.restype = w.HANDLE
            locked_file = k32.CreateFileW(str(files[1]), 0x80000000, 1, None, 3, 0x80, None)
            assert locked_file not in (None, c.c_void_p(-1).value), 'fixture lock failed'
            click_row(lst, 0)
            click_row(lst, 1, 8)
            check('batch delete has both intended rows selected', selected(lst) == [0, 1])
            u32.PostMessageW(hwnd, WM_COMMAND, 4005, 0)
            dialog = wait_for(lambda: window(app.pid, '#32770'), 'batch confirmation missing')
            wait_for(lambda: u32.GetDlgItem(dialog, 6) and u32.IsWindowVisible(dialog), 'confirmation not ready')
            u32.PostMessageW(dialog, WM_COMMAND, 6, 0)  # IDYES
            wait_for(lambda: not u32.IsWindow(dialog), 'confirmation did not close')
            error = wait_for(lambda: window(app.pid, '#32770'), 'delete failure notification missing')
            wait_for(lambda: u32.IsWindowVisible(error), 'failure notification not ready')
            u32.PostMessageW(error, WM_CLOSE, 0, 0)
            wait_for(lambda: not u32.IsWindow(error), 'failure notification did not close')
            check('batch delete removes successful files', not files[3].exists())
            check('failed deletion keeps file and record', files[1].exists() and
                  'recording-1.mp4' in history.read_text(encoding='utf-8-sig') and
                  u32.SendMessageW(lst, LVM_FIRST + 4, 0, 0) == 2)
            k32.SetFileAttributesW(str(files[1]), 0x80)
            k32.CloseHandle(locked_file)
            locked_file = None

            u32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
            time.sleep(0.3)
            alive = app.poll() is None
            check('default close keeps app alive in tray', alive and u32.IsWindow(hwnd) and not u32.IsWindowVisible(hwnd))
            if alive:
                u32.PostMessageW(hwnd, TRAY_MESSAGE, 0, 0x7B)  # Tray WM_CONTEXTMENU callback.
                popup = wait_for(lambda: window(app.pid, '#32768'), 'tray right-click menu missing')
                check('tray right click opens native menu', bool(popup))
                u32.PostMessageW(hwnd, 0x1F, 0, 0)  # WM_CANCELMODE
                wait_for(lambda: not u32.IsWindow(popup), 'tray menu did not dismiss')
                u32.SendMessageW(hwnd, WM_COMMAND, IDM_TRAY_SHOW, 0)
                check('tray open restores main window', bool(u32.IsWindowVisible(hwnd)))
                u32.SendMessageW(hwnd, WM_COMMAND, IDM_CLOSE_EXIT, 0)
                check('close action saved', 'close_action=exit' in config.read_text(encoding='utf-8-sig'))
                u32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
                app.wait(timeout=5)
                check('direct close preference exits', app.returncode == 0)
                app = subprocess.Popen([str(exe)], cwd=root)
                hwnd = wait_for(lambda: window(app.pid, 'LiteCapMainWnd'), 'restart missing', 15)
                u32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
                app.wait(timeout=5)
                check('direct close preference survives restart', app.returncode == 0)
                app = subprocess.Popen([str(exe)], cwd=root)
                hwnd = wait_for(lambda: window(app.pid, 'LiteCapMainWnd'), 'restart missing', 15)
                u32.SendMessageW(hwnd, WM_COMMAND, IDM_CLOSE_TRAY, 0)
                record = u32.GetDlgItem(hwnd, 1008)
                from e2e import control_text
                idle = control_text(record)
                u32.PostMessageW(hwnd, WM_COMMAND, 1008, record)
                wait_for(lambda: control_text(record) != idle, 'recording did not start')
                time.sleep(0.8)
                u32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
                time.sleep(0.2)
                check('tray close preference restored', app.poll() is None and not u32.IsWindowVisible(hwnd))
                check('recording continues after close to tray', control_text(record) != idle)
                time.sleep(0.4)
                u32.PostMessageW(hwnd, WM_COMMAND, IDM_TRAY_EXIT, 0)
                app.wait(timeout=5)
                check('tray Exit terminates process', app.returncode == 0)
                videos = list(root.glob('LiteCap_*.mp4'))
                assert len(videos) == 1, 'recording output missing'
                data = videos[0].read_bytes()
                atoms = []
                offset = 0
                while offset + 8 <= len(data):
                    size, kind = struct.unpack_from('>I4s', data, offset)
                    if size == 1:
                        size = struct.unpack_from('>Q', data, offset+8)[0]
                    if size == 0:
                        size = len(data)-offset
                    if size < 8 or offset+size > len(data):
                        break
                    atoms.append(kind)
                    offset += size
                check('tray Exit finalizes recording MP4', b'moov' in atoms and b'mdat' in atoms)
                check('tray Exit saves final recording to history', videos[0].name in history.read_text(encoding='utf-8-sig'))
        finally:
            key(0x11, True)
            key(0x10, True)
            if locked_file:
                c.windll.kernel32.CloseHandle(locked_file)
            if app and app.poll() is None:
                u32.PostMessageW(hwnd, WM_COMMAND, IDM_TRAY_EXIT, 0)
                try:
                    app.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    app.kill()
                    app.wait()
            for path in files:
                if path.exists():
                    c.windll.kernel32.SetFileAttributesW(str(path), 0x80)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
