"""跑一次录制并报告结果，用于对照不同编码器/帧率。

用法：enc_run.py <帧率下拉索引> <录制秒数>
调用前请先在 settings.ini 里写好 encoder=auto|software|hardware。
"""
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from e2e import find_window, u32              # noqa: E402
from hotkey_test import press_combo, btn_text  # noqa: E402

u32.GetDlgItem.argtypes = [wt.HWND, ctypes.c_int]
u32.GetDlgItem.restype = wt.HWND
u32.SendMessageW.argtypes = [wt.HWND, wt.UINT, ctypes.c_size_t, ctypes.c_void_p]
u32.SendMessageW.restype = ctypes.c_ssize_t

IDC_FPS = 1003
REC_DIR = "D:\\Recordings"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def newest(d):
    fs = [os.path.join(d, f) for f in os.listdir(d) if f.endswith(".mp4")]
    return max(fs, key=os.path.getmtime) if fs else None


def main():
    idx = int(sys.argv[1]) if len(sys.argv) > 1 else 6
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 6.0

    m = find_window("LiteCapMainWnd", timeout=15)
    if not m:
        print("FAIL 未找到主窗口")
        return 1

    cmb = u32.GetDlgItem(m, IDC_FPS)
    u32.SendMessageW(cmb, 0x014E, idx, None)              # CB_SETCURSEL
    u32.PostMessageW(m, 0x0111, (1 << 16) | IDC_FPS, cmb)  # CBN_SELCHANGE
    time.sleep(0.4)

    before = newest(REC_DIR)
    press_combo()
    time.sleep(secs)

    # 录制中顺便把标题读出来（能看出实际用了哪个编码器）
    b = ctypes.create_unicode_buffer(256)
    u32.GetWindowTextW(m, b, 256)
    title = b.value

    press_combo()
    time.sleep(4.0)

    after = newest(REC_DIR)
    if not after or after == before:
        print("FAIL 没有产生新录像")
        return 1

    print("TITLE %s" % title)
    out = subprocess.run([os.path.join(ROOT, "_probe", "fps_probe.exe"), after],
                         capture_output=True)
    print(out.stdout.decode("utf-8", "replace").strip())
    return 0


if __name__ == "__main__":
    sys.exit(main())
