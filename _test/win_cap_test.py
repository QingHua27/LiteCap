"""窗口录制的端到端验证。

覆盖四件事：
  1. 界面宽度减半 + 录制记录可收起（收起后窗口真的变矮）
  2. 目标下拉里出现「窗口: xxx (exe)」条目
  3. 选中窗口后录出来的 MP4 分辨率是窗口尺寸，不是整屏
  4. 遮挡无关：录制过程中用另一个窗口完全盖住靶子，录到的仍然是靶子

第 4 条是这次改动的核心，也是唯一能证明 WGC 真的接对了的证据。
做法：绿窗口当靶子，红窗口盖上去；录完在软件内播放，取播放器中心区域的平均色。
绿色 = 抓的是靶子本身（WGC 生效）；红色 = 抓的是盖住它的窗口（退回了 GDI 行为）。
"""
import ctypes
import ctypes.wintypes as wt
import os
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from e2e import (u32, g32, capture, control_text, find_window, force_foreground,
                 click, double_click_first_row, MAIN_CLASS, IDC_RECORD, IDC_LIST,
                 BITMAPINFO, BITMAPINFOHEADER, DIB_RGB_COLORS, BI_RGB)

IDC_TARGET, IDC_PICK_WIN, IDC_HIST_TOGGLE = 1001, 1014, 1015
CB_GETCOUNT, CB_GETLBTEXT, CB_SETCURSEL = 0x0146, 0x0148, 0x014E
WM_COMMAND, CBN_SELCHANGE = 0x0111, 1
PW_RENDERFULLCONTENT = 2

k32 = ctypes.windll.kernel32
k32.OpenProcess.restype = wt.HANDLE
k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.VirtualAllocEx.restype = ctypes.c_void_p
k32.VirtualAllocEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_size_t, wt.DWORD, wt.DWORD]
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                  ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.VirtualFreeEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_size_t, wt.DWORD]
k32.CloseHandle.argtypes = [wt.HANDLE]
u32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]

EXE = os.path.join(ROOT, "LiteCap.exe")
PY = sys.executable

failures = []


def check(name, ok, detail=""):
    print(("  [ok] " if ok else "  [FAIL] ") + name + (("  " + detail) if detail else ""))
    if not ok:
        failures.append(name)
    return ok


def combo_items(hwnd):
    """读下拉的全部条目。

    CB_GETLBTEXT 看着像个"控件内部消息"，但它编号 0x0148 落在 WM_USER 以下，
    而 user32 对这条消息是**替调用方封送**的（和 WM_GETTEXT 同一个名单）。
    所以缓冲区必须放在本进程 —— 放到目标进程反而读回空串。
    （反例见 LVM_GETITEMRECT：那条不封送，必须用远程缓冲区。）
    """
    n = int(u32.SendMessageW(hwnd, CB_GETCOUNT, 0, 0))
    out = []
    for i in range(n):
        s = ctypes.create_unicode_buffer(1024)
        u32.SendMessageW(hwnd, CB_GETLBTEXT, i, ctypes.addressof(s))
        out.append(s.value)
    return out


def grab(hwnd):
    """抓窗口像素，返回 (w, h, buf)。"""
    r = wt.RECT()
    u32.GetWindowRect(hwnd, ctypes.byref(r))
    w, h = r.right - r.left, r.bottom - r.top
    hdc = u32.GetWindowDC(hwnd)
    mem = g32.CreateCompatibleDC(hdc)
    bmi = BITMAPINFO()
    bmi.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bmi.bmiHeader.biWidth = w
    bmi.bmiHeader.biHeight = -h
    bmi.bmiHeader.biPlanes = 1
    bmi.bmiHeader.biBitCount = 32
    bmi.bmiHeader.biCompression = BI_RGB
    bits = ctypes.c_void_p()
    bmp = g32.CreateDIBSection(hdc, ctypes.byref(bmi), DIB_RGB_COLORS,
                               ctypes.byref(bits), None, 0)
    old = g32.SelectObject(mem, bmp)
    u32.PrintWindow(hwnd, mem, PW_RENDERFULLCONTENT)
    buf = ctypes.string_at(bits, w * h * 4)
    g32.SelectObject(mem, old)
    g32.DeleteObject(bmp)
    g32.DeleteDC(mem)
    u32.ReleaseDC(hwnd, hdc)
    return w, h, buf


def mean_rgb(hwnd, inset=0.3):
    """窗口中心区域的平均 RGB。

    只取中心：播放器四周可能有黑边或控制条，边缘取样会把结果拉偏。
    """
    w, h, buf = grab(hwnd)
    x0, x1 = int(w * inset), int(w * (1 - inset))
    y0, y1 = int(h * inset), int(h * (1 - inset))
    sr = sg = sb = 0
    n = 0
    for y in range(y0, y1, 3):
        base = y * w * 4
        for x in range(x0, x1, 3):
            o = base + x * 4
            sb += buf[o]
            sg += buf[o + 1]
            sr += buf[o + 2]
            n += 1
    if not n:
        return (0, 0, 0)
    return (sr / n, sg / n, sb / n)


def mp4_size(path):
    """从 MP4 的 avc1 box 里直接抠出宽高，不用解码。

    布局：box size(4) 'avc1'(4) reserved(6)+data_ref_idx(2) pre_defined/reserved(16)
          width(2) height(2)
    """
    d = open(path, "rb").read()
    i = d.find(b"avc1")
    if i < 0:
        return None
    w, h = struct.unpack(">HH", d[i + 28:i + 32])
    return w, h


def newest_mp4(d):
    best, bt = None, 0
    for f in os.listdir(d):
        if f.lower().endswith(".mp4"):
            t = os.path.getmtime(os.path.join(d, f))
            if t > bt:
                best, bt = os.path.join(d, f), t
    return best


def main():
    for f in ("settings.ini", "history.tsv"):
        p = os.path.join(ROOT, f)
        if os.path.exists(p):
            os.remove(p)

    out_dir = os.path.join(ROOT, "_test", "out")
    os.makedirs(out_dir, exist_ok=True)
    for f in os.listdir(out_dir):
        if f.lower().endswith(".mp4"):
            os.remove(os.path.join(out_dir, f))

    procs = []
    try:
        # ---- 靶子：绿窗口在左上，红窗口稍后盖上去 ----
        green = subprocess.Popen([PY, os.path.join(HERE, "solid_win.py"),
                                  "LiteCapTestGreen", "0000FF00", "60", "60", "560", "420", "120"])
        procs.append(green)
        time.sleep(2.0)
        gh = u32.FindWindowW(None, "LiteCapTestGreen")
        print("绿窗口 hwnd = 0x%X" % (gh or 0))
        if not gh:
            print("FAIL: 靶子窗口没起来")
            return 1

        # 让 LiteCap 直接以「窗口录制」模式启动
        with open(os.path.join(ROOT, "settings.ini"), "w", encoding="utf-8") as f:
            f.write("target=window\n")
            f.write("window_title=LiteCapTestGreen\n")
            f.write("window_exe=%s\n" % os.path.basename(PY))
            f.write("output_dir=%s\n" % out_dir)
            f.write("fps=30\n")
            f.write("quality=high\n")
            f.write("lang=zh-CN\n")

        app = subprocess.Popen([EXE], cwd=ROOT)
        procs.append(app)
        main_hwnd = find_window(MAIN_CLASS)
        if not main_hwnd:
            print("FAIL: LiteCap 主窗口没起来")
            return 1
        print("LiteCap 主窗口 = 0x%X" % main_hwnd)
        time.sleep(1.0)

        # ---- 1. 两栏布局：记录列表在右栏，收起后窗口收窄回单栏 ----
        print("\n[1] 两栏布局与记录区收起")
        r = wt.RECT()
        u32.GetWindowRect(main_hwnd, ctypes.byref(r))
        w_exp, h_exp = r.right - r.left, r.bottom - r.top
        capture(main_hwnd, os.path.join(HERE, "ui_expanded.png"))

        lst = u32.GetDlgItem(main_hwnd, IDC_LIST)
        lr = wt.RECT()
        u32.GetWindowRect(lst, ctypes.byref(lr))
        cmb_r = wt.RECT()
        u32.GetWindowRect(u32.GetDlgItem(main_hwnd, IDC_TARGET), ctypes.byref(cmb_r))
        check("展开时列表在设置项的右侧", lr.left > cmb_r.right,
              "列表 x=%d，下拉右边界 x=%d" % (lr.left, cmb_r.right))
        check("展开时列表可见", bool(u32.IsWindowVisible(lst)))

        click(main_hwnd, IDC_HIST_TOGGLE)
        time.sleep(0.8)
        u32.GetWindowRect(main_hwnd, ctypes.byref(r))
        w_col, h_col = r.right - r.left, r.bottom - r.top
        capture(main_hwnd, os.path.join(HERE, "ui_collapsed.png"))
        check("收起后窗口收窄回单栏", w_col < 380 and w_exp - w_col > 200,
              "%d -> %d px（原来整窗 600）" % (w_exp, w_col))
        check("收起后列表被隐藏", not u32.IsWindowVisible(lst))
        check("收起后左栏设置项没被压扁", abs(h_col - h_exp) < 40,
              "高度 %d -> %d px" % (h_exp, h_col))

        click(main_hwnd, IDC_HIST_TOGGLE)
        time.sleep(0.8)
        u32.GetWindowRect(main_hwnd, ctypes.byref(r))
        check("再点一次能展开回来",
              abs((r.right - r.left) - w_exp) <= 2 and abs((r.bottom - r.top) - h_exp) <= 2,
              "%dx%d -> %dx%d" % (w_col, h_col, r.right - r.left, r.bottom - r.top))
        check("展开后列表回来了", bool(u32.IsWindowVisible(lst)))

        # ---- 2. 目标下拉里的窗口条目 ----
        print("\n[2] 目标下拉")
        cmb = u32.GetDlgItem(main_hwnd, IDC_TARGET)
        items = combo_items(cmb)
        for i, s in enumerate(items):
            print("      [%d] %s" % (i, s))
        hit = [i for i, s in enumerate(items) if "LiteCapTestGreen" in s]
        check("下拉里有靶子窗口条目", bool(hit),
              "共 %d 项" % len(items))

        # ---- 3. 遮挡下录窗口 ----
        print("\n[3] 遮挡状态下录制窗口")
        if hit:
            u32.SendMessageW(cmb, CB_SETCURSEL, hit[0], 0)
            u32.PostMessageW(main_hwnd, WM_COMMAND,
                             (IDC_TARGET & 0xFFFF) | (CBN_SELCHANGE << 16), cmb)
            time.sleep(0.5)

        # 红窗口盖满绿窗口（含标题栏），并提到最前
        red = subprocess.Popen([PY, os.path.join(HERE, "solid_win.py"),
                                "LiteCapTestRed", "000000FF", "40", "40", "620", "500", "120"])
        procs.append(red)
        time.sleep(1.5)
        rh = u32.FindWindowW(None, "LiteCapTestRed")
        print("红窗口 hwnd = 0x%X" % (rh or 0))
        check("红窗口已盖住绿窗口", bool(rh) and bool(gh))
        if rh:
            force_foreground(rh)
            time.sleep(0.6)

        click(main_hwnd, IDC_RECORD)
        time.sleep(2.0)
        # 窗口收窄之后标题栏会被截断，编码器提示改挂在录制按钮上。
        # BUTTON 的文字就是它的窗口标题，所以 GetWindowTextW 读得到（EDIT 不行）。
        btn = control_text(u32.GetDlgItem(main_hwnd, IDC_RECORD))
        print("      录制按钮文字 = %r" % btn)
        check("录制按钮上标出了编码器", ("GPU" in btn) or ("CPU" in btn), btn)
        capture(main_hwnd, os.path.join(HERE, "ui_recording.png"))
        time.sleep(2.0)
        click(main_hwnd, IDC_RECORD)
        time.sleep(3.5)

        mp4 = newest_mp4(out_dir)
        check("生成了录像文件", bool(mp4), mp4 or "")
        if mp4:
            size = mp4_size(mp4)
            scr_w = u32.GetSystemMetrics(0)
            scr_h = u32.GetSystemMetrics(1)
            print("      分辨率 = %s，屏幕 = %dx%d，文件 = %d 字节"
                  % (size, scr_w, scr_h, os.path.getsize(mp4)))
            check("录的是窗口尺寸而不是整屏",
                  bool(size) and size[0] < scr_w and size[1] < scr_h,
                  "%s vs %dx%d" % (size, scr_w, scr_h))

        # ---- 4. 播出来看颜色 ----
        print("\n[4] 在软件内播放，看画面颜色")
        if mp4:
            if not double_click_first_row(main_hwnd):
                check("打开播放器", False)
            else:
                # 播放器是另起进程 + 建 EVR 的，窗口出现没有确定时点。
                # 固定 sleep 3 秒太赌，改成轮询；早期版本就是栽在这儿，
                # 明明是好的却报"播放器窗口没出现"。
                pw = None
                t_end = time.time() + 15
                while time.time() < t_end and not pw:
                    pw = u32.FindWindowW("LiteCapPlayerWnd", None)
                    if not pw:
                        time.sleep(0.3)
                check("播放器窗口出现", bool(pw), "0x%X" % (pw or 0))
                if pw:
                    force_foreground(pw)
                    time.sleep(1.2)
                    rgb = mean_rgb(pw)
                    capture(pw, os.path.join(HERE, "ui_player.png"))
                    print("      播放器中心平均 RGB = (%.0f, %.0f, %.0f)" % rgb)
                    check("画面是被遮挡的绿窗口，不是盖住它的红窗口",
                          rgb[1] > rgb[0] + 40 and rgb[1] > rgb[2] + 40,
                          "绿=%.0f 红=%.0f 蓝=%.0f" % rgb)
                    u32.PostMessageW(pw, 0x0010, 0, 0)
                    time.sleep(0.8)

    finally:
        for p in procs:
            try:
                p.kill()
            except Exception:
                pass
        time.sleep(0.5)

    print("\n" + "=" * 56)
    if failures:
        print("失败 %d 项: %s" % (len(failures), ", ".join(failures)))
        return 1
    print("全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
