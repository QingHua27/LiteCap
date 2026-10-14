"""验证单文件分发：把 exe 单独放进一个空目录（没有 lang/）跑起来，看界面文案。

内置语言包是以 RCDATA 嵌在 exe 里的，所以这种情况下界面仍然应该是中文，
而不是 target / pick_window / col_name 这种键名。
"""
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from e2e import find_window, capture, MAIN_CLASS   # noqa: E402

TMP = os.path.join(HERE, "standalone_dir")


def main():
    if os.path.isdir(TMP):
        shutil.rmtree(TMP)
    os.makedirs(TMP)
    dst = os.path.join(TMP, "LiteCap.exe")
    shutil.copy2(os.path.join(ROOT, "LiteCap.exe"), dst)

    print("空目录内容:", os.listdir(TMP))
    app = subprocess.Popen([dst], cwd=TMP)
    try:
        h = find_window(MAIN_CLASS, timeout=15)
        if not h:
            print("FAIL 主窗口没起来")
            return 1
        time.sleep(1.2)
        out = os.path.join(HERE, "standalone.png")
        print("OK 截图 =", capture(h, out))
        return 0
    finally:
        app.kill()
        time.sleep(0.4)


if __name__ == "__main__":
    sys.exit(main())
