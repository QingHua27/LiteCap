"""验证 exe 在缺少 lang/ 目录时界面文案是否仍可读

把 exe 单独放到一个空目录里运行，截主窗口的图。
如果界面显示的是 target / fps / quality 这类键名，说明单文件分发不可用。
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from e2e import find_window, capture   # noqa: E402


def main():
    h = find_window("LiteCapMainWnd", timeout=15)
    if not h:
        print("FAIL 未找到主窗口")
        return 1
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "standalone.png")
    w, hh = capture(h, out)
    print("OK 已截图 %dx%d -> %s" % (w, hh, out))
    return 0


sys.exit(main())
