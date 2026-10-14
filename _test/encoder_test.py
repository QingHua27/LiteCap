"""验证「自动 / 硬件 / 软件」三个编码器选项。

不能只看下拉框选上了没有 —— 那只能证明界面通了。
真正的验收标准是**录出来的文件确实走了对应的编码器**：
  1. 选「硬件编码」：按钮上应显示「GPU 编码」
  2. 选「软件编码」：按钮上应显示「CPU 编码」
  3. 选「自动」： 优先硬件，正常应显示「GPU 编码」
下拉框序号必须和 recorder.h 的 EncMode 对齐（Auto=0 / Hardware=1 / Software=2），
对不上的话界面会"选的是硬件、跑的是软件"，而且看不出来。
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

from e2e import (u32, control_text, MAIN_CLASS, IDC_RECORD, IDC_LIST)

EXE = os.path.join(ROOT, "LiteCap.exe")
IDC_ENCODER = 1016

failures = []


def check(name, ok, detail=""):
    print(("  [ok] " if ok else "  [FAIL] ") + name + (("  " + detail) if detail else ""))
    if not ok:
        failures.append(name)
    return ok


def combo_state(h, cid):
    """读某个下拉框的条目数、当前选中项和选中项文字。"""
    c = u32.GetDlgItem(h, cid)
    if not c:
        return -1, -1, []
    n = u32.SendMessageW(c, 0x0146, 0, 0)                 # CB_GETCOUNT
    cur = u32.SendMessageW(c, 0x0147, 0, 0)               # CB_GETCURSEL
    items = []
    for i in range(n):
        buf = ctypes.create_unicode_buffer(256)
        u32.SendMessageW(c, 0x0148, i, ctypes.addressof(buf))   # CB_GETLBTEXT
        items.append(buf.value)
    return n, cur, items


def set_cfg(out_dir, enc):
    for p in (os.path.join(ROOT, "settings.ini"), os.path.join(ROOT, "history.tsv")):
        if os.path.exists(p):
            os.remove(p)
    with open(os.path.join(ROOT, "settings.ini"), "w", encoding="utf-8") as f:
        f.write("output_dir=%s\n" % out_dir)
        f.write("fps=30\nquality=high\nlang=zh-CN\nhistory_open=1\n")
        f.write("encoder=%s\n" % enc)


def run_case(tag, enc_value, expect_sel, expect_button, procs):
    print("\n[%s] 编码器 = %s" % (tag, enc_value))
    out_dir = os.path.join(HERE, "out")
    os.makedirs(out_dir, exist_ok=True)
    for f in os.listdir(out_dir):
        if f.lower().endswith(".mp4"):
            os.remove(os.path.join(out_dir, f))
    set_cfg(out_dir, enc_value)

    app = subprocess.Popen([EXE], cwd=ROOT)
    procs.append(app)
    h = None
    end = time.time() + 20
    while time.time() < end:
        h = u32.FindWindowW(MAIN_CLASS, None)
        if h and u32.GetDlgItem(h, IDC_LIST):
            break
        time.sleep(0.3)
    if not h:
        print("      主窗口没就绪")
        app.kill()
        return None
    time.sleep(1.0)

    n, cur, items = combo_state(h, IDC_ENCODER)
    print("      编码器下拉: %d 项, 选中 %d" % (n, cur))
    for i, s in enumerate(items):
        print("        [%d] %s%s" % (i, s, "  <==" if i == cur else ""))
    check("下拉里有三项", n == 3, "实际 %d" % n)
    check("选中项 == %d" % expect_sel, cur == expect_sel, "实际 %d" % cur)

    u32.SetWindowPos(h, None, 1200, 60, 0, 0, 1 | 4 | 8)
    time.sleep(0.5)
    btn = u32.GetDlgItem(h, IDC_RECORD)
    before = control_text(btn)
    u32.PostMessageW(h, 0x0111, IDC_RECORD & 0xFFFF, btn)
    time.sleep(3.0)
    during = control_text(btn)
    u32.PostMessageW(h, 0x0111, IDC_RECORD & 0xFFFF, btn)
    time.sleep(3.5)
    print("      录制按钮: %r -> %r" % (before, during))
    check("按钮标出了「%s」" % expect_button, expect_button in during, during)

    mp4 = [f for f in os.listdir(out_dir) if f.lower().endswith(".mp4")]
    check("生成了录像", len(mp4) == 1, str(mp4))
    app.kill()
    time.sleep(1.2)
    return during


def test_ui_persistence(procs):
    """从界面改选软件编码，检查立即写配置，重启后选项仍被恢复。"""
    print("\n[D] 验证界面选择会立即保存并在重启后恢复")
    out_dir = os.path.join(HERE, "out")
    set_cfg(out_dir, "auto")
    app = subprocess.Popen([EXE], cwd=ROOT)
    procs.append(app)
    end = time.time() + 20
    h = None
    while time.time() < end:
        h = u32.FindWindowW(MAIN_CLASS, None)
        if h and u32.GetDlgItem(h, IDC_LIST):
            break
        time.sleep(0.3)
    if not h:
        check("主窗口就绪", False)
        app.kill()
        return

    combo = u32.GetDlgItem(h, IDC_ENCODER)
    u32.SendMessageW(combo, 0x014E, 2, 0)            # CB_SETCURSEL 软件编码
    u32.PostMessageW(h, 0x0111, (1 << 16) | IDC_ENCODER, combo)  # CBN_SELCHANGE
    time.sleep(0.8)
    cfg_path = os.path.join(ROOT, "settings.ini")
    with open(cfg_path, encoding="utf-8") as f:
        saved = f.read()
    check("切换后立即保存 encoder=software", "encoder=software" in saved)

    old_hwnd = h
    app.kill()
    time.sleep(1.0)
    app2 = subprocess.Popen([EXE], cwd=ROOT)
    procs.append(app2)
    end = time.time() + 20
    h2 = None
    while time.time() < end:
        h2 = u32.FindWindowW(MAIN_CLASS, None)
        if h2 and h2 != old_hwnd and u32.GetDlgItem(h2, IDC_LIST):
            break
        time.sleep(0.3)
    if not h2 or h2 == old_hwnd:
        check("重启后的主窗口就绪", False)
        app2.kill()
        return
    _, cur, _ = combo_state(h2, IDC_ENCODER)
    check("重启后恢复软件编码选项", cur == 2, "实际选中项 %d" % cur)
    app2.kill()
    time.sleep(1.2)


def main():
    procs = []
    try:
        test_ui_persistence(procs)
        run_case("A", "auto",     0, "GPU 编码", procs)
        run_case("B", "hardware", 1, "GPU 编码", procs)
        run_case("C", "software", 2, "CPU 编码", procs)
    finally:
        for p in procs:
            try:
                p.kill()
            except Exception:
                pass

    print("\n" + "=" * 56)
    if failures:
        print("失败 %d 项: %s" % (len(failures), ", ".join(failures)))
        return 1
    print("全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
