"""Run the native clipboard/feedback regression in a disposable app directory."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix="LiteCap-clipboard-") as directory:
    root = Path(directory)
    exe = root / "LiteCap-clipboard-test.exe"
    shutil.copy2(Path(sys.argv[1]).resolve(), exe)
    (root / "settings.ini").write_text(
        "hotkey_vk=0\naudio_system=0\naudio_microphone=0\nfps=10\n"
        "encoder=software\ntarget=region\nregion=0,0,160,120\nhistory_open=1\n"
        f"close_action=exit\noutput_dir={root}\n", encoding="utf-8"
    )
    rows = []
    for i, name in enumerate(("视频 一.mp4", "视频 二.mp4")):
        file = root / name
        file.write_bytes(b"fixture")
        rows.append(f"{200-i}\t1000\t7\t{file}\n")
    (root / "history.tsv").write_text("".join(rows), encoding="utf-8")
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    subprocess.run([str(exe)], cwd=root, timeout=35, check=True, startupinfo=startup)
    videos = list(root.glob("LiteCap_*.mp4"))
    assert len(videos) == 2 and all(b"moov" in video.read_bytes() for video in videos)
    print("PASS copied recording has finalized MP4 metadata")
    from recording_clipboard_test_util import clipboard_files
    assert clipboard_files() == [str(max(videos, key=lambda video: video.stat().st_mtime))]
    print("PASS exit while recording leaves finished video on clipboard")
