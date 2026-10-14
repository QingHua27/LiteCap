"""验证 120fps：选中 120 档 -> 录制 -> 用 fps_probe 读真实帧率"""
import ctypes, ctypes.wintypes as wt, os, subprocess, sys, time

u32 = ctypes.windll.user32
u32.FindWindowW.argtypes = [wt.LPCWSTR, wt.LPCWSTR]; u32.FindWindowW.restype = wt.HWND
u32.GetDlgItem.argtypes = [wt.HWND, ctypes.c_int]; u32.GetDlgItem.restype = wt.HWND
u32.SendMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]; u32.SendMessageW.restype = ctypes.c_ssize_t
u32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REC_DIR = "D:\\Recordings"
IDC_FPS, IDC_RECORD = 1003, 1008
CBN_SELCHANGE = 1
WM_COMMAND = 0x0111


def find(timeout=15):
    end = time.time() + timeout
    while time.time() < end:
        h = u32.FindWindowW("LiteCapMainWnd", None)
        if h:
            return h
        time.sleep(0.2)
    return None


def set_fps(main, index):
    cmb = u32.GetDlgItem(main, IDC_FPS)
    n = u32.SendMessageW(cmb, 0x0146, 0, 0)      # CB_GETCOUNT
    u32.SendMessageW(cmb, 0x014E, index, 0)      # CB_SETCURSEL
    u32.PostMessageW(main, WM_COMMAND, (CBN_SELCHANGE << 16) | IDC_FPS, cmb)
    return n


def rec_click(main):
    u32.PostMessageW(main, WM_COMMAND, IDC_RECORD & 0xFFFF, u32.GetDlgItem(main, IDC_RECORD))


def newest_mp4():
    fs = [os.path.join(REC_DIR, f) for f in os.listdir(REC_DIR) if f.endswith(".mp4")]
    return max(fs, key=os.path.getmtime) if fs else None


def main():
    h = find()
    if not h:
        print("FAIL 未找到主窗口"); return 1

    before = newest_mp4()
    n = set_fps(h, 6)     # 索引 6 = 120 fps
    print("帧率下拉项数 =", n, "(应为 7)")
    time.sleep(0.5)

    print("开始录制…")
    rec_click(h)
    time.sleep(5.0)
    rec_click(h)
    time.sleep(4.0)

    after = newest_mp4()
    if not after or after == before:
        print("FAIL 没有产生新录像"); return 1

    print("新录像 =", after, "  %.1f MB" % (os.path.getsize(after) / 1e6))
    ini = os.path.join(ROOT, "settings.ini")
    txt = open(ini, "r", encoding="utf-8", errors="replace").read()
    print("settings.ini 中的 fps =", [l for l in txt.splitlines() if l.startswith("fps")])

    probe = os.path.join(ROOT, "_probe", "fps_probe.exe")
    print("--- fps_probe 输出 ---")
    out = subprocess.run([probe, after], capture_output=True)
    print(out.stdout.decode("utf-8", "replace"))
    if out.stderr:
        print(out.stderr.decode("utf-8", "replace"))
    return 0


sys.exit(main())
