"""诊断「WGC 抓窗口」这一段为什么录不出文件。

不猜，直接把三件事读出来：
  1. 下拉框里到底有没有靶子窗口那一条（CB_GETLBTEXT 必须用本进程缓冲区）
  2. 当前选中项是第几条
  3. 点下录制之后有没有弹出模态 MessageBox（有的话主窗口会卡住，文件自然写不出来）
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

from e2e import u32, MAIN_CLASS, IDC_RECORD, IDC_LIST, click, capture_screen

EXE = os.path.join(ROOT, "LiteCap.exe")
PY = sys.executable
CB_GETLBTEXT, CB_GETCOUNT, CB_GETCURSEL = 0x0148, 0x0146, 0x0147


def combo_items(h):
    """读下拉框全部条目。

    CB_GETLBTEXT 和 WM_GETTEXT 是一家人：系统替调用方封送，缓冲区必须落在
    **本进程**。用 VirtualAllocEx 开到目标进程去会读回空串 —— 这个坑我踩过两次了。
    """
    n = u32.SendMessageW(h, CB_GETCOUNT, 0, 0)
    out = []
    for i in range(n):
        buf = ctypes.create_unicode_buffer(512)
        u32.SendMessageW(h, CB_GETLBTEXT, i, ctypes.addressof(buf))
        out.append(buf.value)
    return out, u32.SendMessageW(h, CB_GETCURSEL, 0, 0)


def find_dialog():
    """找 LiteCap 弹出的模态对话框（类名 #32770），并把里面所有静态文本读出来。

    注意 WM_GETTEXT 发给对话框本身拿到的是**标题**（"LiteCap"），
    正文在子控件里，得逐个枚举子窗口再取文本。
    """
    h = u32.FindWindowW("#32770", None)
    if not h:
        return None, ""

    u32.EnumChildWindows.argtypes = [wt.HWND, ctypes.c_void_p, wt.LPARAM]
    u32.EnumChildWindows.restype = wt.BOOL
    u32.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
    u32.GetClassNameW.restype = ctypes.c_int
    CB = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    parts = []

    def cb(child, lp):
        buf = ctypes.create_unicode_buffer(512)
        u32.SendMessageW(child, 0x000D, 512, ctypes.addressof(buf))   # WM_GETTEXT
        if buf.value.strip():
            parts.append(buf.value)
        return True

    u32.EnumChildWindows(h, CB(cb), 0)
    return h, " | ".join(parts)


def main():
    # 靶子窗口样式：从命令行传，默认无边框 popup。
    # 用来二分 WGC 的 CreateForWindow 到底接受哪种窗口。
    style = "popup"
    for a in sys.argv[1:]:
        if not a.startswith("--"):
            style = a
    popup = (style == "popup")
    print("靶子样式 = %s" % style)
    trace = os.path.join(HERE, "wgc_trace.txt")
    if os.path.exists(trace):
        os.remove(trace)
    out_dir = os.path.join(HERE, "out")
    os.makedirs(out_dir, exist_ok=True)
    procs = []
    try:
        args = [PY, os.path.join(HERE, "solid_win.py"),
                "LiteCapQuad", "QUAD", "0", "0", "640", "480", "300", style]
        tgt = subprocess.Popen(args)
        procs.append(tgt)
        time.sleep(2.0)
        th = u32.FindWindowW(None, "LiteCapQuad")
        print("靶子 hwnd = 0x%X" % (th or 0))

        # 靶子进程名，用来核对 combo 里的 (exe) 后缀
        pid = wt.DWORD()
        u32.GetWindowThreadProcessId(th, ctypes.byref(pid))
        k32 = ctypes.windll.kernel32
        hh = k32.OpenProcess(0x1000, False, pid.value)
        pbuf = ctypes.create_unicode_buffer(1024)
        sz = wt.DWORD(1024)
        k32.QueryFullProcessImageNameW(hh, 0, pbuf, ctypes.byref(sz))
        k32.CloseHandle(hh)
        print("靶子进程 = %s" % pbuf.value)

        for p in (os.path.join(ROOT, "settings.ini"), os.path.join(ROOT, "history.tsv")):
            if os.path.exists(p):
                os.remove(p)
        with open(os.path.join(ROOT, "settings.ini"), "w", encoding="utf-8") as f:
            f.write("target=window\n")
            f.write("window_title=LiteCapQuad\n")
            f.write("window_exe=%s\n" % os.path.basename(PY))
            f.write("output_dir=%s\n" % out_dir)
            f.write("fps=30\nquality=high\nlang=zh-CN\nhistory_open=1\n")

        app = subprocess.Popen([EXE], cwd=ROOT)
        procs.append(app)
        end = time.time() + 20
        h = None
        while time.time() < end:
            h = u32.FindWindowW(MAIN_CLASS, None)
            if h and u32.GetDlgItem(h, IDC_LIST):
                break
            time.sleep(0.3)
        time.sleep(1.0)
        print("主窗口 hwnd = 0x%X" % (h or 0))

        print("\n--- 下拉框 ---")
        cmb = None
        # 直接枚举子控件
        u32.EnumChildWindows.argtypes = [wt.HWND, ctypes.c_void_p, wt.LPARAM]
        u32.EnumChildWindows.restype = wt.BOOL
        u32.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
        u32.GetClassNameW.restype = ctypes.c_int
        CB = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
        found = []

        def cb(child, lp):
            cls = ctypes.create_unicode_buffer(128)
            u32.GetClassNameW(child, cls, 128)
            if cls.value == "ComboBox":
                found.append(child)
            return True

        u32.EnumChildWindows(h, CB(cb), 0)
        for c in found:
            items, cur = combo_items(c)
            if any("LiteCapQuad" in s for s in items) or len(items) > 3:
                cmb = c
                print("ComboBox hwnd=0x%X  条目数=%d  当前选中=%d" % (c, len(items), cur))
                for i, s in enumerate(items):
                    mark = " <== 选中" if i == cur else ""
                    print("    [%d] %s%s" % (i, s, mark))
                break

        if cmb is None:
            print("没找到目标下拉框")

        print("\n--- 点录制 ---")
        click(h, IDC_RECORD)
        time.sleep(2.0)
        d, dtext = find_dialog()
        print("模态对话框 = 0x%X  文本 = %r" % (d or 0, dtext))
        capture_screen(os.path.join(HERE, "diag_wgc.png"))
        time.sleep(2.0)
        mp4 = [f for f in os.listdir(out_dir) if f.lower().endswith(".mp4")]
        print("out 目录里的 mp4 =", mp4)
        if os.path.exists(trace):
            print("--- wgc_trace.txt ---")
            with open(trace, encoding="utf-8", errors="replace") as f:
                print(f.read().rstrip())
    finally:
        for p in procs:
            try:
                p.kill()
            except Exception:
                pass
        time.sleep(0.5)
    return 0


if __name__ == "__main__":
    sys.exit(main())
