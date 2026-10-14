"""生成 LiteCap 图标。

纯标准库实现（不依赖 Pillow）：先在高分辨率画布上做几何判定，
再降采样得到抗锯齿结果，最后打包成多尺寸 .ico。

设计：深色圆角方块 + 白色「屏幕」描边 + 中心红色录制点。
配色与程序内的悬浮窗（#1A1C20）和录制按钮（#D63B3B）保持一致。

用法：
    python _tools/make_icon.py            # 生成 res/app.ico
    python _tools/make_icon.py --preview  # 额外输出各尺寸预览图
"""
import argparse
import math
import os
import struct
import sys
import zlib

# ---------- 配色 ----------
BG_TOP    = (58, 65, 80)      # 圆角方块顶部
BG_BOTTOM = (30, 33, 39)      # 圆角方块底部
SCREEN    = (245, 247, 250)   # 屏幕描边
DOT       = (214, 59, 59)     # 录制点（与程序内按钮同色）

SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
SS = 4                        # 超采样倍数


def rounded_box_sdf(px, py, cx, cy, hw, hh, r):
    """圆角矩形有符号距离：负值在内部。"""
    qx = abs(px - cx) - (hw - r)
    qy = abs(py - cy) - (hh - r)
    return math.hypot(max(qx, 0.0), max(qy, 0.0)) + min(max(qx, qy), 0.0) - r


def render(size):
    """渲染单个尺寸，返回逐行 RGBA bytes。"""
    n = size * SS
    inv = 1.0 / n

    # 小尺寸要把描边和圆点都收一点，否则 16px 下红点会贴住白框、糊成一团。
    # 注意方向：是「更细更小」而不是「更粗更大」。
    if size <= 24:
        stroke_ratio, dot_ratio, inset_ratio = 0.080, 0.158, 0.150
    else:
        stroke_ratio, dot_ratio, inset_ratio = 0.070, 0.175, 0.180

    c = n * 0.5
    bg_r  = 0.225 * n                      # 背景圆角半径
    scr_h = n * (0.5 - inset_ratio)        # 屏幕半宽/半高
    scr_r = 0.115 * n                      # 屏幕圆角半径
    stroke = stroke_ratio * n
    dot_r = dot_ratio * n

    rows = []
    for y in range(size):
        row = bytearray()
        for x in range(size):
            acc = [0.0, 0.0, 0.0, 0.0]     # premultiplied RGBA 累加
            for sy in range(SS):
                for sx in range(SS):
                    px = (x * SS + sx + 0.5)
                    py = (y * SS + sy + 0.5)

                    # 1) 背景圆角方块
                    if rounded_box_sdf(px, py, c, c, c, c, bg_r) > 0:
                        continue

                    t = py * inv
                    r = BG_TOP[0] + (BG_BOTTOM[0] - BG_TOP[0]) * t
                    g = BG_TOP[1] + (BG_BOTTOM[1] - BG_TOP[1]) * t
                    b = BG_TOP[2] + (BG_BOTTOM[2] - BG_TOP[2]) * t

                    # 2) 屏幕描边（圆角矩形环）
                    d = rounded_box_sdf(px, py, c, c, scr_h, scr_h, scr_r)
                    if abs(d) <= stroke * 0.5:
                        r, g, b = SCREEN

                    # 3) 中心录制点
                    if math.hypot(px - c, py - c) <= dot_r:
                        r, g, b = DOT

                    acc[0] += r
                    acc[1] += g
                    acc[2] += b
                    acc[3] += 255.0

            k = SS * SS
            a = acc[3] / k
            if a <= 0:
                row += b"\x00\x00\x00\x00"
            else:
                # 颜色按覆盖到的样本数平均，避免边缘出现暗边
                cov = acc[3] / 255.0
                row += bytes((
                    int(round(acc[0] / cov)),
                    int(round(acc[1] / cov)),
                    int(round(acc[2] / cov)),
                    int(round(a)),
                ))
        rows.append(bytes(row))
    return rows


def png_bytes(w, h, rows):
    """把逐行 RGBA 打成 PNG。"""
    raw = bytearray()
    for row in rows:
        raw.append(0)          # filter: None
        raw += row

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
            + chunk(b"IEND", b""))


def build_ico(sizes):
    """打包成 ICO（PNG 内嵌，Windows Vista+ 支持）。"""
    images = []
    for s in sizes:
        print("  渲染 %dx%d ..." % (s, s), flush=True)
        images.append((s, png_bytes(s, s, render(s))))

    out = bytearray()
    out += struct.pack("<HHH", 0, 1, len(images))     # ICONDIR

    offset = 6 + 16 * len(images)
    for s, data in images:
        dim = 0 if s >= 256 else s
        out += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    for _, data in images:
        out += data
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preview", action="store_true", help="同时输出各尺寸预览图")
    ap.add_argument("-o", "--out", default=None, help="输出 ico 路径")
    args = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_ico = args.out or os.path.join(root, "res", "app.ico")
    os.makedirs(os.path.dirname(out_ico), exist_ok=True)

    print("生成图标：")
    ico = build_ico(SIZES)
    with open(out_ico, "wb") as f:
        f.write(ico)
    print("\n已写入 %s  (%.1f KB, %d 个尺寸)"
          % (out_ico, len(ico) / 1024.0, len(SIZES)))

    if args.preview:
        prev_dir = os.path.join(root, "_tools", "preview")
        os.makedirs(prev_dir, exist_ok=True)
        for s in SIZES:
            p = os.path.join(prev_dir, "icon_%d.png" % s)
            with open(p, "wb") as f:
                f.write(png_bytes(s, s, render(s)))
        print("预览图已输出到 %s" % prev_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
