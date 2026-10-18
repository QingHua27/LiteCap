"""Run the native fallback fixture with isolated settings and recording output.

Usage: python _test/run_fullscreen_hotkey_test.py build/fullscreen_hotkey_test.exe
"""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    fixture = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="LiteCap-fullscreen-hotkey-") as directory:
        root = Path(directory)
        executable = root / "LiteCap-key-test.exe"
        shutil.copy2(fixture, executable)
        (root / "settings.ini").write_text(
            "hotkey_mod=0\nhotkey_vk=119\naudio_system=0\naudio_microphone=0\n"
            "fps=10\nencoder=software\ntarget=region\nregion=0,0,160,120\n"
            f"close_action=exit\noutput_dir={root}\n", encoding="utf-8"
        )
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
        subprocess.run([str(executable)], cwd=root, check=True, timeout=30,
                       startupinfo=startup)
        videos = list(root.glob("*.mp4"))
        assert len(videos) == 1 and videos[0].stat().st_size > 1024, "No finalized recording"
        assert b"moov" in videos[0].read_bytes(), "Recording was not finalized"
        print("PASS fallback recording produces a finalized MP4")


if __name__ == "__main__":
    main()
