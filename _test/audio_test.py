"""端到端验证 WASAPI 系统声音 / 麦克风开关。

用系统 WAV tone 触发一个已知的扬声器信号，录制后由独立 FFmpeg 解码 AAC，
检查 MP4 音轨存在且非静音；同时验证默认值与重启持久化。
"""
import ctypes
import ctypes.wintypes as wt
import math
import os
import struct
import subprocess
import sys
import time
import wave
import winsound

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from e2e import u32, find_window, MAIN_CLASS, IDC_LIST, IDC_RECORD

FFMPEG = r"D:\Program Files\EVCapture\ffmpeg.exe"
EXE = os.path.join(ROOT, "LiteCap.exe")
IDC_AUDIO_SYSTEM = 1017
IDC_AUDIO_MIC = 1018
BM_GETCHECK = 0x00F0
BM_SETCHECK = 0x00F1
BST_CHECKED = 1
BST_UNCHECKED = 0

failures = []

def check(name, ok, detail=""):
    print(("  [ok] " if ok else "  [FAIL] ") + name + (("  " + detail) if detail else ""))
    if not ok: failures.append(name)
    return ok

def kill(p):
    try:
        p.kill(); p.wait(timeout=5)
    except Exception:
        pass

def launch(procs):
    # 主窗口有销毁延迟；不要把上一轮仍在 teardown 的 hwnd 当成本轮实例。
    end_gone = time.time() + 10
    while time.time() < end_gone:
        old = u32.FindWindowW(MAIN_CLASS, None)
        if not old or not u32.GetDlgItem(old, IDC_LIST):
            break
        time.sleep(0.2)
    app = subprocess.Popen([EXE], cwd=ROOT)
    procs.append(app)
    end = time.time() + 20
    while time.time() < end:
        h = u32.FindWindowW(MAIN_CLASS, None)
        if h and u32.GetDlgItem(h, IDC_LIST) and u32.GetDlgItem(h, IDC_RECORD):
            time.sleep(1.0)
            return app, h
        time.sleep(0.25)
    kill(app)
    return app, None

def set_audio(h, system, mic):
    c1 = u32.GetDlgItem(h, IDC_AUDIO_SYSTEM)
    c2 = u32.GetDlgItem(h, IDC_AUDIO_MIC)
    u32.SendMessageW(c1, BM_SETCHECK, BST_CHECKED if system else BST_UNCHECKED, 0)
    u32.SendMessageW(c2, BM_SETCHECK, BST_CHECKED if mic else BST_UNCHECKED, 0)
    u32.PostMessageW(h, 0x0111, IDC_AUDIO_SYSTEM & 0xffff, c1)
    time.sleep(0.2)
    return c1, c2

def dialog_text():
    h = u32.FindWindowW("#32770", None)
    if not h: return ""
    u32.EnumChildWindows.argtypes = [wt.HWND, ctypes.c_void_p, wt.LPARAM]
    u32.EnumChildWindows.restype = wt.BOOL
    CB = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    out = []
    def cb(ch, lp):
        b = ctypes.create_unicode_buffer(512)
        u32.SendMessageW(ch, 0x000D, 512, ctypes.addressof(b))
        if b.value: out.append(b.value)
        return True
    u32.EnumChildWindows(h, CB(cb), 0)
    return " | ".join(out)

def check_state(c1, c2):
    return (u32.SendMessageW(c1, BM_GETCHECK, 0, 0) == BST_CHECKED,
            u32.SendMessageW(c2, BM_GETCHECK, 0, 0) == BST_CHECKED)

def make_tone(path, seconds=2.5, hz=880):
    rate = 48000
    with wave.open(path, "wb") as wf:
        wf.setnchannels(2); wf.setsampwidth(2); wf.setframerate(rate)
        frames = bytearray()
        for i in range(int(rate * seconds)):
            v = int(11000 * math.sin(2 * math.pi * hz * i / rate))
            frames += struct.pack("<hh", v, v)
        wf.writeframes(frames)

def ffmpeg_pcm(mp4):
    p = subprocess.run([FFMPEG, "-v", "error", "-i", mp4, "-map", "0:a:0",
                        "-f", "s16le", "-ac", "2", "-ar", "48000", "-"],
                       capture_output=True)
    return p.returncode, p.stdout, p.stderr

def record_case(procs, out_dir, system, mic, tone_path=None, expected_mic_unavailable=False):
    # preserve only the test output directory: test recorder is started with this path
    os.makedirs(out_dir, exist_ok=True)
    for f in os.listdir(out_dir):
        if f.lower().endswith(".mp4"):
            try: os.remove(os.path.join(out_dir, f))
            except OSError: pass
    with open(os.path.join(ROOT, "settings.ini"), "w", encoding="utf-8") as f:
        f.write("output_dir=%s\nfps=30\nquality=high\nlang=zh-CN\nhistory_open=1\nencoder=software\n" % out_dir)
        f.write("audio_system=%d\naudio_microphone=%d\n" % (system, mic))
    app, h = launch(procs)
    if not h:
        check("LiteCap 启动", False)
        return None
    c1, c2 = u32.GetDlgItem(h, IDC_AUDIO_SYSTEM), u32.GetDlgItem(h, IDC_AUDIO_MIC)
    check("音源开关恢复", check_state(c1, c2) == (bool(system), bool(mic)), str(check_state(c1, c2)))
    u32.PostMessageW(h, 0x0111, IDC_RECORD & 0xffff, u32.GetDlgItem(h, IDC_RECORD))
    time.sleep(1.0)
    startup_dialog = dialog_text()
    if startup_dialog:
        print("      启动错误提示 = %s" % startup_dialog)
        no_input_device = "microphone" in startup_dialog.lower() and "80070490" in startup_dialog.lower()
        if expected_mic_unavailable and no_input_device:
            print("  [skip] 本测试环境没有默认录音输入端点（0x80070490），无法实测麦克风采样")
        else:
            check("WASAPI 音源启动", False, startup_dialog)
        kill(app)
        return None
    if tone_path and system:
        winsound.PlaySound(tone_path, winsound.SND_FILENAME | winsound.SND_ASYNC)
    time.sleep(3.0)
    u32.PostMessageW(h, 0x0111, IDC_RECORD & 0xffff, u32.GetDlgItem(h, IDC_RECORD))
    time.sleep(3.0)
    files = [os.path.join(out_dir, f) for f in os.listdir(out_dir) if f.lower().endswith(".mp4")]
    if not files:
        print("      失败对话框 = %s" % dialog_text())
    check("录制完成并生成 MP4", len(files) == 1, str(files))
    kill(app)
    return files[0] if files else None

def main():
    # 保存并恢复项目运行时配置，测试不覆盖用户原有选择。
    cfg = os.path.join(ROOT, "settings.ini")
    hist = os.path.join(ROOT, "history.tsv")
    backup_cfg = open(cfg, "rb").read() if os.path.exists(cfg) else None
    backup_hist = open(hist, "rb").read() if os.path.exists(hist) else None
    procs = []
    out_dir = os.path.join(HERE, "out", "audio")
    tone_path = os.path.join(HERE, "out", "audio_tone.wav")
    try:
        os.makedirs(out_dir, exist_ok=True)
        make_tone(tone_path)

        # 配置中不写 audio_system / audio_microphone，观察程序默认。
        if os.path.exists(cfg): os.remove(cfg)
        app, h = launch(procs)
        if h:
            c1, c2 = u32.GetDlgItem(h, IDC_AUDIO_SYSTEM), u32.GetDlgItem(h, IDC_AUDIO_MIC)
            check("默认系统声音开启", u32.SendMessageW(c1, BM_GETCHECK, 0, 0) == BST_CHECKED)
            check("默认麦克风关闭", u32.SendMessageW(c2, BM_GETCHECK, 0, 0) == BST_UNCHECKED)
            set_audio(h, False, True)
            kill(app)
            time.sleep(0.8)
            app2, h2 = launch(procs)
            if h2:
                c1, c2 = u32.GetDlgItem(h2, IDC_AUDIO_SYSTEM), u32.GetDlgItem(h2, IDC_AUDIO_MIC)
                check("两路开关选择被记住", check_state(c1, c2) == (False, True), str(check_state(c1, c2)))
            kill(app2)
        else:
            check("主窗口启动", False)

        # 系统声音开启、麦克风关闭：播放已知 tone，独立解码确认 AAC 轨可听。
        mp4 = record_case(procs, out_dir, True, False, tone_path)
        if mp4:
            rc, pcm, err = ffmpeg_pcm(mp4)
            check("MP4 有可解码 AAC 音轨", rc == 0 and len(pcm) > 48000,
                  "bytes=%d %s" % (len(pcm), err.decode("utf-8", "replace")[-300:]))
            if pcm:
                samples = struct.unpack("<%dh" % (len(pcm)//2), pcm[:len(pcm)//2*2])
                peak = max(abs(v) for v in samples)
                rms = math.sqrt(sum(v*v for v in samples) / len(samples))
                check("系统音轨收到扬声器信号", peak > 500 and rms > 100,
                      "peak=%d rms=%.1f" % (peak, rms))

        # 麦克风开启时验证输入端点能初始化并写出 AAC 音轨。
        mic_mp4 = record_case(procs, out_dir, False, True, expected_mic_unavailable=True)
        if mic_mp4:
            rc, pcm, err = ffmpeg_pcm(mic_mp4)
            check("麦克风单独开启可生成 AAC 音轨", rc == 0 and len(pcm) > 48000,
                  "bytes=%d %s" % (len(pcm), err.decode("utf-8", "replace")[-300:]))

        # 两种音源同时开启，检查共享 AAC 音轨能持续写入且两个采集线程可并行。
        both_mp4 = record_case(procs, out_dir, True, True, tone_path)
        if both_mp4:
            rc, pcm, err = ffmpeg_pcm(both_mp4)
            check("系统声音 + 麦克风可同时录到同一 AAC 音轨",
                  rc == 0 and len(pcm) > 48000,
                  "bytes=%d %s" % (len(pcm), err.decode("utf-8", "replace")[-300:]))

        # 两项都关闭时必须仍能录视频，但 MP4 不应产生空音轨。
        silent_mp4 = record_case(procs, out_dir, False, False)
        if silent_mp4:
            rc, pcm, err = ffmpeg_pcm(silent_mp4)
            check("关闭两路音频后仍可录制纯视频", rc != 0 and not pcm,
                  "ffmpeg audio-map rc=%d" % rc)

    finally:
        for p in procs: kill(p)
        winsound.PlaySound(None, 0)
        if backup_cfg is None:
            try: os.remove(cfg)
            except OSError: pass
        else:
            with open(cfg, "wb") as f: f.write(backup_cfg)
        if backup_hist is None:
            try: os.remove(hist)
            except OSError: pass
        else:
            with open(hist, "wb") as f: f.write(backup_hist)

    print("\n" + "=" * 52)
    if failures:
        print("失败 %d 项: %s" % (len(failures), ", ".join(failures)))
        return 1
    print("全部通过")
    return 0

if __name__ == "__main__":
    sys.exit(main())
