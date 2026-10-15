"""Verify production MP4 orientation via independent FFmpeg decoding.

Usage: python encoder_orientation_test.py fixture.exe ffmpeg.exe output_directory
"""
from pathlib import Path
import subprocess
import sys

fixture, ffmpeg, output = sys.argv[1:4]
output = Path(output)
output.mkdir(parents=True, exist_ok=True)
for mode in ("auto", "software", "hardware"):
    (output / f"{mode}.mp4").unlink(missing_ok=True)
subprocess.run([fixture, str(output)], check=True)
expected = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0)]
failed = False
for mode in ("auto", "software", "hardware"):
    video = output / f"{mode}.mp4"
    if not video.exists() or not video.stat().st_size:
        if mode == "hardware":
            continue
        raise AssertionError(f"{mode} output missing")
    result = subprocess.run([ffmpeg, "-v", "error", "-i", str(video), "-frames:v", "1",
                             "-f", "rawvideo", "-pix_fmt", "rgb24", "-"], capture_output=True, check=True)
    assert len(result.stdout) == 160 * 120 * 3, "wrong decoded frame size"
    colors = []
    for x, y in ((40, 30), (120, 30), (40, 90), (120, 90)):
        offset = (y * 160 + x) * 3
        colors.append(tuple(result.stdout[offset:offset+3]))
    ok = all(max(abs(actual[i] - ref[i]) for i in range(3)) < 35
             for actual, ref in zip(colors, expected))
    print(f"{'PASS' if ok else 'FAIL'} {mode}: {colors}")
    failed |= not ok
sys.exit(1 if failed else 0)
