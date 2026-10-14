"""验证录出来的画面朝向 —— 平均色是抓不到这个 bug 的（旋转不变）。

思路：拿一个四色块窗口当靶子（左上红 / 右上绿 / 左下蓝 / 右下黄），
  A. 用 GDI 抓屏幕区域录一遍  —— 这条路早就验证过是对的，当基准
  B. 用 WGC 抓窗口录一遍
两条路都把视频在软件内播出来，从**屏幕**上抓播放器的视频区域取四象限。

为什么从屏幕抓而不是 PrintWindow 抓播放器子窗口：那里是 EVR 的 D3D 交换链，
PrintWindow 未必给得出真实画面。屏幕 BitBlt 拿到的就是用户眼睛看到的东西，
而且它和录制端 GDI 路径用的是同一个原语。

靶子用无边框窗口：窗口尺寸 == 客户区尺寸，屏幕区域录和窗口录取景框一致，
两个结果可以直接逐象限对照。
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

from e2e import (u32, g32, find_window, force_foreground, click,
                 double_click_first_row, MAIN_CLASS, IDC_RECORD, IDC_LIST,
                 BITMAPINFO, BITMAPINFOHEADER, DIB_RGB_COLORS, BI_RGB)

SRCCOPY, CAPTUREBLT = 0x00CC0020, 0x40000000
SWP_NOSIZE, SWP_NOZORDER, SWP_NOACTIVATE = 0x0001, 0x0004, 0x0008
IDC_HIST_TOGGLE = 1015

u32.FindWindowExW.restype = wt.HWND
u32.FindWindowExW.argtypes = [wt.HWND, wt.HWND, wt.LPCWSTR, wt.LPCWSTR]
u32.SetWindowPos.argtypes = [wt.HWND, wt.HWND, ctypes.c_int, ctypes.c_int,
                             ctypes.c_int, ctypes.c_int, wt.UINT]

EXE = os.path.join(ROOT, "LiteCap.exe")
PY = sys.executable

TARGET_W, TARGET_H = 640, 480
failures = []


def check(name, ok, detail=""):
    print(("  [ok] " if ok else "  [FAIL] ") + name + (("  " + detail) if detail else ""))
    if not ok:
        failures.append(name)
    return ok


def grab_rect(x, y, w, h):
    """从屏幕抠一块，返回 (w, h, BGRA 自上而下)。"""
    if w <= 0 or h <= 0:
        return 0, 0, b""
    hdc = u32.GetDC(None)
    mem = g32.CreateCompatibleDC(hdc)
    bmi = BITMAPINFO()
    bmi.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bmi.bmiHeader.biWidth = w
    bmi.bmiHeader.biHeight = -h          # 负值 = 自上而下
    bmi.bmiHeader.biPlanes = 1
    bmi.bmiHeader.biBitCount = 32
    bmi.bmiHeader.biCompression = BI_RGB
    bits = ctypes.c_void_p()
    bmp = g32.CreateDIBSection(hdc, ctypes.byref(bmi), DIB_RGB_COLORS,
                               ctypes.byref(bits), None, 0)
    old = g32.SelectObject(mem, bmp)
    g32.BitBlt(mem, 0, 0, w, h, hdc, x, y, SRCCOPY | CAPTUREBLT)
    buf = ctypes.string_at(bits, w * h * 4)
    g32.SelectObject(mem, old)
    g32.DeleteObject(bmp)
    g32.DeleteDC(mem)
    u32.ReleaseDC(None, hdc)
    return w, h, buf


def save_png(buf, w, h, path):
    import struct
    import zlib
    rows = bytearray()
    for y in range(h):
        row = buf[y * w * 4:(y + 1) * w * 4]
        rgb = bytearray()
        for x in range(w):
            rgb += bytes((row[x * 4 + 2], row[x * 4 + 1], row[x * 4]))
        rows.append(0)
        rows += rgb

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n"
                + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(bytes(rows), 6))
                + chunk(b"IEND", b""))


def _avg(buf, w, cx, cy, hw, hh):
    sr = sg = sb = n = 0
    for y in range(cy - hh, cy + hh, 3):
        base = y * w * 4
        for x in range(cx - hw, cx + hw, 3):
            o = base + x * 4
            sb += buf[o]
            sg += buf[o + 1]
            sr += buf[o + 2]
            n += 1
    return (sr / n, sg / n, sb / n) if n else (0, 0, 0)


def quads_window(hwnd):
    """窗口本身（无边框，客户区 == 窗口）的四象限。"""
    r = wt.RECT()
    u32.GetWindowRect(hwnd, ctypes.byref(r))
    w, h, buf = grab_rect(r.left, r.top, r.right - r.left, r.bottom - r.top)
    if not buf:
        return None
    out = []
    for i in range(4):
        cx = (w // 4) * (3 if (i & 1) else 1)
        cy = (h // 4) * (3 if (i >> 1) else 1)
        out.append(_avg(buf, w, cx, cy, w // 8, h // 8))
    return out


def quads_video(hwnd, srcW, srcH):
    """播放器里视频区的四象限。

    视频是等比缩放 + 居中留黑边的，直接按窗口四等分会把黑边算进去，
    所以先按源分辨率算出视频实际显示的那块矩形。
    """
    r = wt.RECT()
    u32.GetWindowRect(hwnd, ctypes.byref(r))
    w, h = r.right - r.left, r.bottom - r.top
    w, h, buf = grab_rect(r.left, r.top, w, h)
    if not buf:
        return None, None
    scale = min(w / srcW, h / srcH)
    vw, vh = int(srcW * scale), int(srcH * scale)
    ox, oy = (w - vw) // 2, (h - vh) // 2
    out = []
    for i in range(4):
        cx = ox + vw * (3 if (i & 1) else 1) // 4
        cy = oy + vh * (3 if (i >> 1) else 1) // 4
        out.append(_avg(buf, w, cx, cy, vw // 8, vh // 8))
    return out, (w, h, buf)


def describe(q):
    if not q:
        return "无"
    names = ["红", "绿", "蓝", "黄", "黑", "白"]
    refs = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0), (0, 0, 0), (255, 255, 255)]
    out = []
    for c in q:
        best, bd = 0, 1e18
        for i, r in enumerate(refs):
            d = sum((c[k] - r[k]) ** 2 for k in range(3))
            if d < bd:
                best, bd = i, d
        out.append(names[best])
    return " ".join(out)


def newest_mp4(d):
    best, bt = None, 0
    for f in os.listdir(d):
        if f.lower().endswith(".mp4"):
            t = os.path.getmtime(os.path.join(d, f))
            if t > bt:
                best, bt = os.path.join(d, f), t
    return best


def kill(p):
    try:
        p.kill()
        p.wait(timeout=5)
    except Exception:
        pass


def launch_ready(procs, timeout=20):
    """起一个 LiteCap 并等到它的控件都建好。

    不能只等主窗口出现：上一个实例可能还没退干净，FindWindow 会拿到那个
    正在销毁的句柄，GetDlgItem 就会返回 0。
    """
    app = subprocess.Popen([EXE], cwd=ROOT)
    procs.append(app)
    end = time.time() + timeout
    while time.time() < end:
        h = u32.FindWindowW(MAIN_CLASS, None)
        if h and u32.GetDlgItem(h, IDC_LIST):
            time.sleep(1.0)
            return app, h
        time.sleep(0.3)
    return app, None


def write_cfg(out_dir, extra):
    for p in (os.path.join(ROOT, "settings.ini"), os.path.join(ROOT, "history.tsv")):
        if os.path.exists(p):
            os.remove(p)
    with open(os.path.join(ROOT, "settings.ini"), "w", encoding="utf-8") as f:
        for k, v in extra.items():
            f.write("%s=%s\n" % (k, v))
        f.write("output_dir=%s\n" % out_dir)
        f.write("fps=30\nquality=high\nlang=zh-CN\nhistory_open=1\n")


def run_case(tag, out_dir, cfg, procs, seconds=3.0):
    write_cfg(out_dir, cfg)
    for f in os.listdir(out_dir):
        if f.lower().endswith(".mp4"):
            os.remove(os.path.join(out_dir, f))

    app, h = launch_ready(procs)
    if not h:
        print("      主窗口没就绪")
        kill(app)
        return None
    # 挪到右边，别挡住靶子
    u32.SetWindowPos(h, None, 1200, 60, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)
    time.sleep(0.4)

    click(h, IDC_RECORD)
    time.sleep(seconds)
    click(h, IDC_RECORD)
    time.sleep(3.5)
    print("      录像 = %s" % newest_mp4(out_dir))

    if not double_click_first_row(h):
        print("      双击列表失败")
        kill(app)
        return None
    time.sleep(2.5)
    pw = u32.FindWindowW("LiteCapPlayerWnd", None)
    if not pw:
        print("      播放器没起来")
        kill(app)
        return None
    force_foreground(pw)
    time.sleep(1.5)
    video = u32.FindWindowExW(pw, None, "LiteCapVideoWnd", None)
    if not video:
        print("      找不到视频子窗口")
        u32.PostMessageW(pw, 0x0010, 0, 0)
        kill(app)
        return None

    q, raw = quads_video(video, TARGET_W, TARGET_H)
    if raw:
        save_png(raw[2], raw[0], raw[1], os.path.join(HERE, "orient_%s_player.png" % tag))
    u32.PostMessageW(pw, 0x0010, 0, 0)
    time.sleep(0.8)
    kill(app)
    time.sleep(0.8)
    return q


def main():
    out_dir = os.path.join(HERE, "out")
    os.makedirs(out_dir, exist_ok=True)

    procs = []
    try:
        tgt = subprocess.Popen([PY, os.path.join(HERE, "solid_win.py"),
                                "LiteCapQuad", "QUAD", "0", "0",
                                str(TARGET_W), str(TARGET_H), "300", "popup"])
        procs.append(tgt)
        time.sleep(2.0)
        th = u32.FindWindowW(None, "LiteCapQuad")
        print("靶子窗口 hwnd = 0x%X" % th or 0)
        if not th:
            print("FAIL 靶子窗口没起来")
            return 1
        src = quads_window(th)
        print("靶子原始画面四象限: %s" % describe(src))
        r = wt.RECT()
        u32.GetWindowRect(th, ctypes.byref(r))
        _, _, b = grab_rect(r.left, r.top, r.right - r.left, r.bottom - r.top)
        save_png(b, r.right - r.left, r.bottom - r.top, os.path.join(HERE, "orient_src.png"))

        print("\n[A] GDI 抓屏幕区域（基准）")
        qa = run_case("A", out_dir, {"target": "region",
                                     "region": "0,0,%d,%d" % (TARGET_W, TARGET_H)}, procs)
        print("      播放画面四象限  : %s" % describe(qa))

        print("\n[B] WGC 抓窗口")
        qb = run_case("B", out_dir, {"target": "window",
                                     "window_title": "LiteCapQuad",
                                     "window_exe": os.path.basename(PY)}, procs)
        print("      播放画面四象限  : %s" % describe(qb))

        print("\n[结论]")
        if not qa or not qb:
            check("两次录制都拿到了画面", False, "A=%s B=%s" % (bool(qa), bool(qb)))
        else:
            check("靶子本身朝向正常", describe(src) == "红 绿 蓝 黄", describe(src))
            check("GDI 路径朝向正确（基准）", describe(qa) == "红 绿 蓝 黄", describe(qa))
            check("WGC 路径朝向与 GDI 一致", describe(qb) == describe(qa),
                  "GDI=[%s]  WGC=[%s]" % (describe(qa), describe(qb)))
            d = describe(qb)
            if d == "蓝 黄 红 绿":
                print("      => WGC 画面上下颠倒")
            elif d == "黄 蓝 绿 红":
                print("      => WGC 画面左右镜像")
            elif d == "绿 红 黄 蓝":
                print("      => WGC 画面旋转 180 度")
    finally:
        for p in procs:
            kill(p)
        time.sleep(0.5)

    print("\n" + "=" * 56)
    if failures:
        print("失败 %d 项: %s" % (len(failures), ", ".join(failures)))
        return 1
    print("全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
