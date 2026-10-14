"""验证方案 C 的核心价值：窗口被遮挡时，仍能录到窗口本身。

对比三方：
  1. 屏幕截图（现有 GDI BitBlt 实现）—— 录到的是挡住它的窗口
  2. WGC 窗口捕获（方案 C）        —— 录到窗口自身内容

做法：开两个记事本，A 设成有辨识度的内容，B 最大化盖住 A，
然后分别用三种方式抓 A。
"""
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from e2e import capture_screen, _write_png   # noqa: E402

u32 = ctypes.windll.user32
u32.FindWindowW.argtypes = [wt.LPCWSTR, wt.LPCWSTR]
u32.FindWindowW.restype = wt.HWND
u32.FindWindowExW.argtypes = [wt.HWND, wt.HWND, wt.LPCWSTR, wt.LPCWSTR]
u32.FindWindowExW.restype = wt.HWND
u32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u32.SetWindowPos.argtypes = [wt.HWND, wt.HWND, ctypes.c_int, ctypes.c_int,
                             ctypes.c_int, ctypes.c_int, wt.UINT]
u32.SendMessageW.argtypes = [wt.HWND, wt.UINT, ctypes.c_size_t, ctypes.c_void_p]
u32.SendMessageW.restype = ctypes.c_ssize_t
u32.WindowFromPoint.argtypes = [wt.POINT]
u32.WindowFromPoint.restype = wt.HWND
u32.GetAncestor.argtypes = [wt.HWND, wt.UINT]
u32.GetAncestor.restype = wt.HWND
u32.ShowWindow.argtypes = [wt.HWND, ctypes.c_int]

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROBE = os.path.join(ROOT, "_probe", "wgc_probe.exe")
OUT = os.path.join(ROOT, "_probe", "build")
MARK = "遮挡测试-WGC能抓到这行字"


def wait_window(cls, timeout=15):
    end = time.time() + timeout
    while time.time() < end:
        h = u32.FindWindowW(cls, None)
        if h:
            return h
        time.sleep(0.2)
    return None


def main():
    os.makedirs(OUT, exist_ok=True)

    print("[*] 启动记事本 A ...")
    a_proc = subprocess.Popen(["notepad.exe"])
    a = wait_window("Notepad")
    if not a:
        print("FAIL 找不到记事本 A")
        return 1

    # 固定 A 的位置和大小，方便后面复现
    u32.SetWindowPos(a, None, 80, 80, 80 + 720, 80 + 520, 0x0040)
    time.sleep(0.6)

    # 往 A 里塞一行有辨识度的字
    edit = u32.FindWindowExW(a, None, "Edit", None)
    if edit:
        buf = ctypes.create_unicode_buffer(MARK)
        u32.SendMessageW(edit, 0x000C, 0, ctypes.addressof(buf))   # WM_SETTEXT
        print("[ok] 已写入标记文本")
    else:
        print("[!] 没找到 Edit 子控件，仅靠标题栏区分")

    r = wt.RECT()
    u32.GetWindowRect(a, ctypes.byref(r))
    print("[ok] A 窗口矩形 = (%d,%d)-(%d,%d)  %dx%d"
          % (r.left, r.top, r.right, r.bottom, r.right - r.left, r.bottom - r.top))

    print("[*] 启动记事本 B 并最大化，盖住 A ...")
    b_proc = subprocess.Popen(["notepad.exe"])
    time.sleep(2.0)
    b = u32.FindWindowW("Notepad", None)
    while b == a:
        time.sleep(0.3)
        b = u32.FindWindowW("Notepad", None)
    u32.ShowWindow(b, 3)          # SW_MAXIMIZE
    time.sleep(1.5)

    # 确认 A 真的被挡住了
    cx, cy = (r.left + r.right) // 2, (r.top + r.bottom) // 2
    top = u32.WindowFromPoint(wt.POINT(cx, cy))
    top = u32.GetAncestor(top, 2) or top     # GA_ROOT
    print("[*] A 中心点最上层窗口 = %s   A = %s" % (hex(top or 0), hex(a)))
    if top == a:
        print("[!] 警告：A 没有被完全遮挡，这个测试就不算数了")

    print("\n[1/3] 屏幕截图（现有 GDI 实现会得到这个）...")
    rect = (r.left, r.top, r.right, r.bottom)
    capture_screen(os.path.join(OUT, "occ_screen.png"), rect)
    print("      -> occ_screen.png")

    print("[2/3] WGC 窗口捕获（方案 C）...")
    raw = os.path.join(OUT, "occ_wgc.raw")
    p = subprocess.run([PROBE, "%X" % a, "1.2", raw],
                       capture_output=True)
    print(p.stdout.decode("utf-8", "replace").strip())
    if os.path.exists(raw):
        # 探针输出的是 BGRA，尺寸从输出里解析
        w = h = 0
        for line in p.stdout.decode("utf-8", "replace").splitlines():
            if "CaptureItem 尺寸" in line:
                w, h = [int(x) for x in line.split("=")[1].replace("x", " ").split()]
        data = open(raw, "rb").read()
        if w and h and len(data) >= w * h * 4:
            _write_png(os.path.join(OUT, "occ_wgc.png"), w, h, data)
            print("      -> occ_wgc.png (%dx%d)" % (w, h))

    print("[3/3] 完成")
    a_proc.terminate()
    b_proc.terminate()
    time.sleep(0.5)
    subprocess.run(["taskkill", "/F", "/IM", "notepad.exe"],
                   capture_output=True)
    print("[ok] 已清理记事本")
    return 0


sys.exit(main())
