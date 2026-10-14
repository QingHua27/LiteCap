"""把 e2e.py 的流程跑起来（它假定 LiteCap 已经在运行）。

顺手把 settings.ini 清掉，回到「整个桌面 + 30fps」的默认配置，
这样跑的是最普通的那条路，和之前版本的行为可以直接对照。
"""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import e2e   # noqa: E402

for f in ("settings.ini", "history.tsv"):
    p = os.path.join(ROOT, f)
    if os.path.exists(p):
        os.remove(p)

app = subprocess.Popen([os.path.join(ROOT, "LiteCap.exe")], cwd=ROOT)
rc = 1
try:
    time.sleep(3.0)
    rc = e2e.main()
finally:
    app.kill()
    time.sleep(0.5)

print("e2e 返回码 =", rc)
sys.exit(rc)
