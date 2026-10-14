"""用 FFmpeg 独立解码，判定录像文件的画面朝向。

为什么不复用软件内的播放器：那也走 Media Foundation。如果 SinkWriter 和
MFPlay 各自都带一个方向上的偏差，用它们互相验证会刚好抵消，看不出问题。
FFmpeg 有自己独立的 H.264 解码器，是最干净的外部裁判。

靶子是无边框四色块窗口（左上红 / 右上绿 / 左下蓝 / 右下黄）。
两个录制路径各录一段，解一帧出来看四个象限的顺序。
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

from e2e import (u32, find_window, click, control_text, MAIN_CLASS, IDC_RECORD, IDC_LIST)

FFMPEG = r"D:\Program Files\EVCapture\ffmpeg.exe"
EXE = os.path.join(ROOT, "LiteCap.exe")
PY = sys.executable
TARGET_W, TARGET_H = 640, 480
SWP_NOSIZE, SWP_NOZORDER, SWP_NOACTIVATE = 0x0001, 0x0004, 0x0008

u32.SetWindowPos.argtypes = [wt.HWND, wt.HWND, ctypes.c_int, ctypes.c_int,
                             ctypes.c_int, ctypes.c_int, wt.UINT]

failures = []


def check(name, ok, detail=""):
    print(("  [ok] " if ok else "  [FAIL] ") + name + (("  " + detail) if detail else ""))
    if not ok:
        failures.append(name)
    return ok


def ffmpeg_frame(path, n, out_png=None):
    """解第 n 帧，返回 (w, h, rgb24 bytes)。"""
    filt = "select=eq(n\\,%d)" % n
    cmd = [FFMPEG, "-v", "error", "-i", path, "-vf", filt, "-vframes", "1",
           "-f", "rawvideo", "-pix_fmt", "rgb24", "-"]
    p = subprocess.run(cmd, capture_output=True)
    data = p.stdout
    if len(data) < TARGET_W * TARGET_H * 3:
        return None, None, None
    if out_png:
        subprocess.run([FFMPEG, "-y", "-v", "error", "-i", path, "-vf", filt,
                        "-vframes", "1", out_png], capture_output=True)
    return TARGET_W, TARGET_H, data[:TARGET_W * TARGET_H * 3]


def quads_rgb(w, h, rgb):
    out = []
    for i in range(4):
        cx = (w // 4) * (3 if (i & 1) else 1)
        cy = (h // 4) * (3 if (i >> 1) else 1)
        sr = sg = sb = n = 0
        for y in range(cy - h // 8, cy + h // 8, 3):
            base = y * w * 3
            for x in range(cx - w // 8, cx + w // 8, 3):
                o = base + x * 3
                sr += rgb[o]
                sg += rgb[o + 1]
                sb += rgb[o + 2]
                n += 1
        out.append((sr / n, sg / n, sb / n) if n else (0, 0, 0))
    return out


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


def kill(p):
    try:
        p.kill()
        p.wait(timeout=5)
    except Exception:
        pass


def newest_mp4(d):
    best, bt = None, 0
    for f in os.listdir(d):
        if f.lower().endswith(".mp4"):
            t = os.path.getmtime(os.path.join(d, f))
            if t > bt:
                best, bt = os.path.join(d, f), t
    return best


def wait_gone(prev, timeout=15.0):
    """等上一个实例彻底消失。

    主窗口从收到 WM_CLOSE 到真正 DestroyWindow 之间有一段空档，
    这段时间 FindWindow 仍会返回它的句柄，但 GetDlgItem 拿到的控件已经不可用了。
    上一次 B 段「没生成录像」就是踩了这个：拿旧句柄去点录制，等于什么都没点。
    """
    end = time.time() + timeout
    while time.time() < end:
        h = u32.FindWindowW(MAIN_CLASS, None)
        if not h or h != prev:
            return True
        time.sleep(0.2)
    return False


def launch(procs, prev=None):
    if not wait_gone(prev):
        print("      上一个实例没退干净")
    app = subprocess.Popen([EXE], cwd=ROOT)
    procs.append(app)
    end = time.time() + 20
    while time.time() < end:
        h = u32.FindWindowW(MAIN_CLASS, None)
        # 必须是一个「新的、活着的」窗口：句柄换了，且列表控件真的存在
        if h and h != prev and u32.GetDlgItem(h, IDC_LIST):
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


def run_case(tag, out_dir, cfg, procs, prev=None, seconds=3.0,
             title="LiteCapQuad", style="popup"):
    """录一段，返回 (四象限颜色, 本次用到的窗口句柄)。句柄要往上传，
    下一段靠它确认自己拿到的是新实例而不是上一个的残影。

    每段自己起一个靶子窗口：A 段测 GDI 区域，B/C 段测窗口捕获。
    样式预设见 solid_win.py 的 STYLES —— WGC 的 CreateForWindow 只认带
    WS_CAPTION 的窗口，所以 B 段必须用带标题栏的，C 段专门验无边框的回退。
    """
    write_cfg(out_dir, cfg)
    for f in os.listdir(out_dir):
        if f.lower().endswith(".mp4"):
            os.remove(os.path.join(out_dir, f))

    tgt = subprocess.Popen([PY, os.path.join(HERE, "solid_win.py"), title, "QUAD",
                            "0", "0", str(TARGET_W), str(TARGET_H), "300", style])
    procs.append(tgt)
    time.sleep(2.0)
    if not u32.FindWindowW(None, title):
        print("      靶子窗口 %r 没起来" % title)
        kill(tgt)
        return None, None

    app, h = launch(procs, prev)
    if not h:
        print("      主窗口没就绪")
        kill(app)
        kill(tgt)
        return None, None
    u32.SetWindowPos(h, None, 1200, 60, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)
    time.sleep(0.6)
    # 先把录制按钮的文字读下来：回退成区域模式时按钮会标出来
    btn = u32.GetDlgItem(h, IDC_RECORD)
    rec_before = control_text(btn) if btn else ""
    click(h, IDC_RECORD)
    time.sleep(seconds)
    rec_during = control_text(btn) if btn else ""
    click(h, IDC_RECORD)
    time.sleep(3.5)
    kill(app)
    kill(tgt)
    time.sleep(1.2)

    print("      录制按钮: %r -> %r" % (rec_before, rec_during))
    mp4 = newest_mp4(out_dir)
    if not mp4:
        print("      没生成录像")
        return None, h
    print("      录像 = %s (%d 字节)" % (mp4, os.path.getsize(mp4)))
    w, hh, rgb = ffmpeg_frame(mp4, 60, os.path.join(HERE, "ff_%s.png" % tag))
    if not rgb:
        print("      ffmpeg 解码失败")
        return None, h
    return quads_rgb(w, hh, rgb), h


def main():
    out_dir = os.path.join(HERE, "out")
    os.makedirs(out_dir, exist_ok=True)
    procs = []
    try:
        print("靶子真值（左上 右上 左下 右下）: 红 绿 蓝 黄")

        print("\n[A] GDI 抓屏幕区域（靶子置顶，避开桌面上的全屏程序）")
        qa, ha = run_case("A", out_dir, {"target": "region",
                                         "region": "0,0,%d,%d" % (TARGET_W, TARGET_H)},
                          procs, title="CapA", style="popup_top")
        print("      ffmpeg 解出的四象限: %s" % describe(qa))

        print("\n[B] WGC 抓窗口（带标题栏的靶子）")
        qb, hb = run_case("B", out_dir, {"target": "window",
                                        "window_title": "CapB",
                                        "window_exe": os.path.basename(PY)},
                         procs, title="CapB", style="overlap_top")
        print("      ffmpeg 解出的四象限: %s" % describe(qb))

        print("\n[C] WGC 抓无边框窗口 —— CreateForWindow 会失败，应回退区域模式")
        qc, _ = run_case("C", out_dir, {"target": "window",
                                        "window_title": "CapC",
                                        "window_exe": os.path.basename(PY)},
                         procs, prev=hb, title="CapC", style="popup_top")
        print("      ffmpeg 解出的四象限: %s" % describe(qc))

        print("\n[结论]")
        check("GDI 路径方向正确", describe(qa) == "红 绿 蓝 黄", describe(qa))
        check("WGC 路径方向正确", describe(qb) == "红 绿 蓝 黄", describe(qb))
        check("WGC 无边框回退后方向正确", describe(qc) == "红 绿 蓝 黄", describe(qc))
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
