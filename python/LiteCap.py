#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
LiteCap - 极简离线录屏工具 / Minimal offline screen recorder

设计约束（由用户需求确定）：
  1. 零网络：整个程序不导入任何网络模块，不含任何网络调用。
  2. 极简 UI：单窗口、一屏内完成所有操作，无标签页、无向导、无广告位。
  3. 完全离线：不检查更新、不联网激活、不发送任何遥测。
  4. 可自由本地化：所有界面文案来自 lang/*.json，新增语言只需新增一个 JSON 文件。

依赖：PySide6（UI）、mss（抓屏）、av/PyAV（H.264 编码，自带 FFmpeg）、numpy。
运行：LiteCap.bat  或  python LiteCap.py
"""

from __future__ import annotations

import json
import os
import sys
import time
from fractions import Fraction
from pathlib import Path

import av
import mss
import numpy as np
from PySide6.QtCore import Qt, QRect, QThread, QTimer, Signal
from PySide6.QtGui import QGuiApplication, QImage, QPainter, QColor, QPen, QFont, QIcon, QPixmap
from PySide6.QtWidgets import (
    QApplication, QWidget, QMainWindow, QVBoxLayout, QHBoxLayout, QGridLayout,
    QLabel, QPushButton, QComboBox, QLineEdit, QFileDialog, QMessageBox, QFrame,
    QSizePolicy,
)

APP_NAME = "LiteCap"
APP_VERSION = "1.0.0"


def _app_dir() -> Path:
    """程序所在目录（打包成 exe 后是 exe 自己的目录）。"""
    if getattr(sys, "frozen", False):
        return Path(sys.executable).resolve().parent
    return Path(__file__).resolve().parent


def _resource_dir() -> Path:
    """只读资源目录。onefile 打包时资源被解到临时目录。"""
    if getattr(sys, "frozen", False):
        return Path(getattr(sys, "_MEIPASS", _app_dir()))
    return _app_dir()


LANG_DIR = _resource_dir() / "lang"
SETTINGS_FILE = _app_dir() / "settings.json"

# --------------------------------------------------------------------------
# 本地化 / Localization
# --------------------------------------------------------------------------

BUILTIN_LANGS = {"en-US": "English", "zh-CN": "简体中文"}


class Loc:
    """从 lang/*.json 载入界面文案。缺失键回退到英文，再回退到键名本身。"""

    def __init__(self) -> None:
        self._tables: dict[str, dict] = {}
        self._fallback: dict = {}
        self.lang = "zh-CN"
        self.load_all()

    def load_all(self) -> None:
        if LANG_DIR.is_dir():
            for p in sorted(LANG_DIR.glob("*.json")):
                try:
                    self._tables[p.stem] = json.loads(p.read_text(encoding="utf-8"))
                except Exception:
                    pass
        self._fallback = self._tables.get("en-US", {})

    def available(self) -> list[str]:
        keys = set(BUILTIN_LANGS) | set(self._tables)
        return sorted(keys)

    def set_lang(self, lang: str) -> None:
        self.lang = lang if lang in self._tables else "en-US"

    def t(self, key: str, **kw) -> str:
        table = self._tables.get(self.lang, {})
        text = table.get(key) or self._fallback.get(key) or key
        return text.format(**kw) if kw else text

    def name_of(self, lang: str) -> str:
        return self._tables.get(lang, {}).get("_name", BUILTIN_LANGS.get(lang, lang))


# --------------------------------------------------------------------------
# 设置 / Settings
# --------------------------------------------------------------------------

def _default_output_dir() -> str:
    """默认输出目录：优先 D 盘，D 盘不存在时退回用户视频目录。"""
    if os.path.isdir("D:\\"):
        return "D:\\Recordings"
    return str(Path.home() / "Videos")


DEFAULTS = {
    "lang": "zh-CN",
    "fps": 30,
    "quality": "high",          # high | medium | low
    "output_dir": _default_output_dir(),
    "target": "all",            # all | mon:<idx> | region
    "region": None,             # [x, y, w, h] in physical pixels
    "monitor": 0,
}

QUALITY_PRESETS = {
    "high":   {"crf": 18, "preset": "veryfast"},
    "medium": {"crf": 23, "preset": "veryfast"},
    "low":    {"crf": 28, "preset": "ultrafast"},
}


def load_settings() -> dict:
    data = dict(DEFAULTS)
    if SETTINGS_FILE.is_file():
        try:
            data.update(json.loads(SETTINGS_FILE.read_text(encoding="utf-8")))
        except Exception:
            pass
    return data


def save_settings(data: dict) -> None:
    try:
        SETTINGS_FILE.write_text(
            json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
    except Exception:
        pass


# --------------------------------------------------------------------------
# 录制线程 / Recorder worker
# --------------------------------------------------------------------------

class RecorderWorker(QThread):
    """抓屏 + 编码。所有耗时操作都在本线程，UI 线程只收信号。"""

    tick = Signal(int, float)        # frames_written, elapsed_seconds
    done = Signal(str, int, float)   # output_path, frames, elapsed
    failed = Signal(str)

    def __init__(self, region: tuple[int, int, int, int], fps: int,
                 quality: str, out_path: str, parent=None) -> None:
        super().__init__(parent)
        self.region = region
        self.fps = max(1, min(120, int(fps)))
        self.quality = quality
        self.out_path = out_path
        self._stop = False

    def stop(self) -> None:
        self._stop = True

    def run(self) -> None:  # noqa: C901
        x, y, w, h = self.region
        # H.264 要求偶数宽高
        w -= w % 2
        h -= h % 2
        if w < 16 or h < 16:
            self.failed.emit("region too small")
            return

        q = QUALITY_PRESETS.get(self.quality, QUALITY_PRESETS["medium"])
        frames = 0
        t0 = time.monotonic()
        container = None
        try:
            container = av.open(self.out_path, mode="w")
            stream = container.add_stream("libx264", rate=self.fps)
            stream.width = w
            stream.height = h
            stream.pix_fmt = "yuv420p"
            stream.options = {"crf": str(q["crf"]), "preset": q["preset"],
                              "tune": "zerolatency"}

            # 抓屏句柄必须在本线程内创建
            with mss.MSS() as sct:
                frame_interval = 1.0 / self.fps
                next_due = t0
                last_emit = t0
                last_pts = -1
                while not self._stop:
                    now = time.monotonic()
                    if now < next_due:
                        time.sleep(min(frame_interval, next_due - now))
                        continue
                    next_due += frame_interval
                    if next_due < now - frame_interval:   # 落后太多则重新对齐
                        next_due = now + frame_interval

                    shot = sct.grab({"left": x, "top": y, "width": w, "height": h})
                    arr = np.frombuffer(shot.bgra, dtype=np.uint8).reshape(h, w, 4)
                    frame = av.VideoFrame.from_ndarray(arr, format="bgra")
                    # 时间戳必须严格递增：重复的 PTS 会让封装器直接报 EINVAL
                    pts = int(round((time.monotonic() - t0) * self.fps))
                    pts = max(pts, last_pts + 1)
                    last_pts = pts
                    frame.pts = pts
                    frame.time_base = Fraction(1, self.fps)
                    for packet in stream.encode(frame):
                        container.mux(packet)
                    frames += 1

                    if time.monotonic() - last_emit >= 0.25:
                        last_emit = time.monotonic()
                        self.tick.emit(frames, last_emit - t0)

                for packet in stream.encode(None):
                    container.mux(packet)
            container.close()
            container = None
            elapsed = time.monotonic() - t0
            self.done.emit(self.out_path, frames, elapsed)
        except Exception as exc:                              # noqa: BLE001
            if container is not None:
                try:
                    container.close()
                except Exception:
                    pass
            self.failed.emit(f"{type(exc).__name__}: {exc}")


# --------------------------------------------------------------------------
# 区域选择器 / Region selector
# --------------------------------------------------------------------------

class RegionSelector(QWidget):
    """全屏半透明遮罩，拖拽框选一块矩形区域。返回物理像素坐标。"""

    picked = Signal(object)   # tuple | None

    def __init__(self) -> None:
        super().__init__(None, Qt.WindowType.FramelessWindowHint |
                         Qt.WindowType.WindowStaysOnTopHint |
                         Qt.WindowType.Tool)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, False)
        self.setCursor(Qt.CursorShape.CrossCursor)
        self._origin = None
        self._current = None
        self._shot = None
        self._dpr = 1.0
        self._virtual_origin = (0, 0)
        self._capture()

    def _capture(self) -> None:
        """抓一张虚拟桌面全景作为背景，并计算物理↔逻辑坐标映射。"""
        with mss.MSS() as sct:
            mon = sct.monitors[0]
            raw = sct.grab(mon)
            self._virtual_origin = (mon["left"], mon["top"])
            self._phys_size = (raw.width, raw.height)
            img = QImage(bytes(raw.bgra), raw.width, raw.height,
                         raw.width * 4, QImage.Format.Format_ARGB32).copy()
            self._shot = QPixmap.fromImage(img)

        # 逻辑尺寸 = 所有屏幕的并集（Qt 逻辑坐标）
        rect = QRect()
        for scr in QGuiApplication.screens():
            rect = rect.united(scr.geometry())
        self._dpr = self._phys_size[0] / max(1, rect.width())
        self.setGeometry(rect)

    def paintEvent(self, _event) -> None:  # noqa: N802
        p = QPainter(self)
        p.drawPixmap(self.rect(), self._shot)
        p.fillRect(self.rect(), QColor(0, 0, 0, 110))
        sel = self._sel_rect()
        if sel is not None and sel.width() > 1 and sel.height() > 1:
            p.drawPixmap(sel, self._shot,
                         QRect(int(sel.x() * self._dpr), int(sel.y() * self._dpr),
                               int(sel.width() * self._dpr), int(sel.height() * self._dpr)))
            pen = QPen(QColor(60, 160, 255), 2)
            p.setPen(pen)
            p.drawRect(sel)
            p.setFont(QFont("Segoe UI", 11))
            p.setPen(QColor(255, 255, 255))
            p.drawText(sel.x() + 6, max(20, sel.y() - 8),
                       f"{int(sel.width()*self._dpr)} x {int(sel.height()*self._dpr)}")
        else:
            p.setFont(QFont("Segoe UI", 15))
            p.setPen(QColor(255, 255, 255))
            p.drawText(self.rect(), Qt.AlignmentFlag.AlignCenter, "Drag to select  ·  Esc to cancel")

    def _sel_rect(self) -> QRect | None:
        if self._origin is None or self._current is None:
            return None
        return QRect(self._origin, self._current).normalized()

    def mousePressEvent(self, e) -> None:  # noqa: N802
        self._origin = e.position().toPoint()
        self._current = self._origin
        self.update()

    def mouseMoveEvent(self, e) -> None:  # noqa: N802
        if self._origin is not None:
            self._current = e.position().toPoint()
            self.update()

    def mouseReleaseEvent(self, e) -> None:  # noqa: N802
        sel = self._sel_rect()
        self.close()
        if sel is None or sel.width() < 8 or sel.height() < 8:
            self.picked.emit(None)
            return
        ox, oy = self._virtual_origin
        phys = (int(round(sel.x() * self._dpr)) + ox,
                int(round(sel.y() * self._dpr)) + oy,
                int(round(sel.width() * self._dpr)),
                int(round(sel.height() * self._dpr)))
        self.picked.emit(phys)

    def keyPressEvent(self, e) -> None:  # noqa: N802
        if e.key() == Qt.Key.Key_Escape:
            self.close()
            self.picked.emit(None)


# --------------------------------------------------------------------------
# 悬浮窗 / Floating bar
# --------------------------------------------------------------------------

BAR_CARD = ("#card{background:rgba(26,28,32,238);border-radius:19px;"
            "border:1px solid rgba(255,255,255,42)}")
BAR_BTN_IDLE = ("QPushButton{background:#d63b3b;color:#fff;border:none;"
                "border-radius:14px;padding:0 12px;font:600 12px 'Segoe UI'}"
                "QPushButton:hover{background:#e04848}")
BAR_BTN_LIVE = ("QPushButton{background:#3a3f47;color:#fff;border:none;"
                "border-radius:14px;padding:0 12px;font:600 12px 'Segoe UI'}"
                "QPushButton:hover{background:#474d56}")
BAR_BTN_ICON = ("QPushButton{color:#c3c8cf;background:transparent;border:none;"
                "font-size:14px}"
                "QPushButton:hover{color:#fff;background:rgba(255,255,255,34);"
                "border-radius:13px}")

REC_BTN_IDLE = ("QPushButton{font-size:15px;font-weight:600;border-radius:6px;"
                "background:#d63b3b;color:#fff;border:none}"
                "QPushButton:hover{background:#c22f2f}"
                "QPushButton:disabled{background:#c9ccd1;color:#f2f3f5}")
REC_BTN_LIVE = ("QPushButton{font-size:15px;font-weight:600;border-radius:6px;"
                "background:#3b3f46;color:#fff;border:none}"
                "QPushButton:hover{background:#2f333a}"
                "QPushButton:disabled{background:#c9ccd1;color:#f2f3f5}")


class FloatingBar(QWidget):
    """置顶悬浮小条：不打开主窗口也能一键开始 / 停止录制。

    - 整条可拖动，位置记进 settings.json
    - 录制中按钮变灰显示“停止”，并实时显示已录时长
    - ▣ 回到完整窗口
    """

    def __init__(self, main: "MainWindow") -> None:
        super().__init__(None, Qt.WindowType.FramelessWindowHint |
                         Qt.WindowType.WindowStaysOnTopHint |
                         Qt.WindowType.Tool)
        self.main = main
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, True)
        self.setWindowTitle(f"{APP_NAME} bar")
        self._drag_offset = None
        self._build_ui()
        self._retranslate()
        self._place()

    def _build_ui(self) -> None:
        lay = QHBoxLayout(self)
        lay.setContentsMargins(0, 0, 0, 0)
        card = QFrame()
        card.setObjectName("card")
        card.setStyleSheet(BAR_CARD)
        inner = QHBoxLayout(card)
        inner.setContentsMargins(7, 5, 7, 5)
        inner.setSpacing(7)

        self.btn_rec = QPushButton()
        self.btn_rec.setFixedHeight(28)
        self.btn_rec.setMinimumWidth(96)
        self.btn_rec.setCursor(Qt.CursorShape.PointingHandCursor)
        self.btn_rec.clicked.connect(self.main._toggle_record)

        self.lbl_time = QLabel("00:00")
        self.lbl_time.setStyleSheet("color:#e8eaed;font:600 13px 'Segoe UI'")
        self.lbl_time.setMinimumWidth(46)
        self.lbl_time.setAlignment(Qt.AlignmentFlag.AlignCenter)

        self.btn_back = QPushButton("▣")
        self.btn_back.setFixedSize(26, 26)
        self.btn_back.setCursor(Qt.CursorShape.PointingHandCursor)
        self.btn_back.setStyleSheet(BAR_BTN_ICON)
        self.btn_back.clicked.connect(self.main._exit_float_mode)

        inner.addWidget(self.btn_rec)
        inner.addWidget(self.lbl_time)
        inner.addWidget(self.btn_back)
        lay.addWidget(card)

    def _retranslate(self) -> None:
        t = self.main.loc.t
        self.btn_back.setToolTip(t("bar_back"))
        self.set_recording(self.main.worker is not None)

    def set_recording(self, recording: bool) -> None:
        t = self.main.loc.t
        self.btn_rec.setText(("■ " + t("bar_stop")) if recording
                             else ("● " + t("bar_start")))
        self.btn_rec.setStyleSheet(BAR_BTN_LIVE if recording else BAR_BTN_IDLE)
        self.btn_rec.setEnabled(True)
        self.adjustSize()

    def set_time(self, text: str) -> None:
        self.lbl_time.setText(text)

    # ---- 拖动 ----

    def _place(self) -> None:
        pos = self.main.cfg.get("float_pos")
        self.adjustSize()
        if pos:
            self.move(int(pos[0]), int(pos[1]))
        else:
            scr = QGuiApplication.primaryScreen().availableGeometry()
            self.move(scr.right() - self.width() - 24,
                      scr.bottom() - self.height() - 24)

    def mousePressEvent(self, e) -> None:  # noqa: N802
        if e.button() == Qt.MouseButton.LeftButton:
            self._drag_offset = (e.globalPosition().toPoint()
                                 - self.frameGeometry().topLeft())
            e.accept()

    def mouseMoveEvent(self, e) -> None:  # noqa: N802
        if self._drag_offset is not None and (e.buttons() & Qt.MouseButton.LeftButton):
            self.move(e.globalPosition().toPoint() - self._drag_offset)
            e.accept()

    def mouseReleaseEvent(self, e) -> None:  # noqa: N802
        if self._drag_offset is not None:
            self._drag_offset = None
            self.main.cfg["float_pos"] = [self.x(), self.y()]
            self.main._persist()
        e.accept()


# --------------------------------------------------------------------------
# 主窗口 / Main window
# --------------------------------------------------------------------------

class MainWindow(QMainWindow):

    def __init__(self) -> None:
        super().__init__()
        self.loc = Loc()
        self.cfg = load_settings()
        self.loc.set_lang(self.cfg.get("lang", "zh-CN"))
        self.worker: RecorderWorker | None = None
        self.bar: FloatingBar | None = None
        self._started_at = 0.0

        self.setMinimumWidth(430)
        self.setWindowIcon(self._make_icon())
        self._build_ui()
        self._retranslate()
        self._refresh_targets()
        self._sync_from_cfg()
        if not SETTINGS_FILE.is_file():   # 首次运行落一份默认配置，方便用户手改
            save_settings(self.cfg)

    # ---------------- UI ----------------

    def _make_icon(self) -> QIcon:
        pm = QPixmap(32, 32)
        pm.fill(Qt.GlobalColor.transparent)
        p = QPainter(pm)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.setBrush(QColor(220, 60, 60))
        p.setPen(Qt.PenStyle.NoPen)
        p.drawEllipse(6, 6, 20, 20)
        p.end()
        return QIcon(pm)

    def _build_ui(self) -> None:
        root = QWidget()
        self.setCentralWidget(root)
        outer = QVBoxLayout(root)
        outer.setContentsMargins(16, 14, 16, 14)
        outer.setSpacing(10)

        grid = QGridLayout()
        grid.setHorizontalSpacing(10)
        grid.setVerticalSpacing(8)
        grid.setColumnStretch(1, 1)

        self.lbl_target = QLabel()
        self.cmb_target = QComboBox()
        self.btn_region = QPushButton()
        self.btn_region.clicked.connect(self._pick_region)
        row = QHBoxLayout()
        row.setSpacing(6)
        row.addWidget(self.cmb_target, 1)
        row.addWidget(self.btn_region)
        grid.addWidget(self.lbl_target, 0, 0)
        grid.addLayout(row, 0, 1)

        self.lbl_fps = QLabel()
        self.cmb_fps = QComboBox()
        for v in (10, 15, 20, 24, 30, 60):
            self.cmb_fps.addItem(f"{v} fps", v)
        grid.addWidget(self.lbl_fps, 1, 0)
        grid.addWidget(self.cmb_fps, 1, 1)

        self.lbl_quality = QLabel()
        self.cmb_quality = QComboBox()
        for key in ("high", "medium", "low"):
            self.cmb_quality.addItem(key, key)
        grid.addWidget(self.lbl_quality, 2, 0)
        grid.addWidget(self.cmb_quality, 2, 1)

        self.lbl_lang = QLabel()
        self.cmb_lang = QComboBox()
        for code in self.loc.available():
            self.cmb_lang.addItem(self.loc.name_of(code), code)
        self.cmb_lang.currentIndexChanged.connect(self._on_lang_changed)
        grid.addWidget(self.lbl_lang, 3, 0)
        grid.addWidget(self.cmb_lang, 3, 1)

        outer.addLayout(grid)
        for box in (self.cmb_fps, self.cmb_quality, self.cmb_lang):
            box.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)

        line = QFrame()
        line.setFrameShape(QFrame.Shape.HLine)
        line.setFrameShadow(QFrame.Shadow.Sunken)
        outer.addWidget(line)

        self.lbl_dir = QLabel()
        dir_row = QHBoxLayout()
        dir_row.setSpacing(6)
        self.edt_dir = QLineEdit()
        self.edt_dir.editingFinished.connect(self._persist)
        self.btn_browse = QPushButton()
        self.btn_browse.clicked.connect(self._browse)
        dir_row.addWidget(self.edt_dir, 1)
        dir_row.addWidget(self.btn_browse)
        outer.addWidget(self.lbl_dir)
        outer.addLayout(dir_row)

        self.btn_record = QPushButton()
        self.btn_record.setMinimumHeight(42)
        self.btn_record.setStyleSheet(REC_BTN_IDLE)
        self.btn_record.clicked.connect(self._toggle_record)

        self.btn_float = QPushButton()
        self.btn_float.setMinimumHeight(42)
        self.btn_float.setMinimumWidth(96)
        self.btn_float.setStyleSheet(
            "QPushButton{font-size:13px;border-radius:6px;background:#eceef1;"
            "color:#2b3138;border:1px solid #d5d9de}"
            "QPushButton:hover{background:#e2e5e9}"
            "QPushButton:disabled{color:#a7adb5}")
        self.btn_float.clicked.connect(self._enter_float_mode)

        rec_row = QHBoxLayout()
        rec_row.setSpacing(8)
        rec_row.addWidget(self.btn_record, 1)
        rec_row.addWidget(self.btn_float)
        outer.addLayout(rec_row)

        self.lbl_status = QLabel()
        self.lbl_status.setWordWrap(True)
        self.lbl_status.setStyleSheet("color:#5a6169;font-size:12px")
        outer.addWidget(self.lbl_status)

    def _retranslate(self) -> None:
        t = self.loc.t
        self.setWindowTitle(f"{APP_NAME} {APP_VERSION}  ·  {t('app_subtitle')}")
        self.lbl_target.setText(t("target"))
        self.lbl_fps.setText(t("fps"))
        self.lbl_quality.setText(t("quality"))
        self.lbl_lang.setText(t("language"))
        self.lbl_dir.setText(t("output_dir"))
        self.btn_region.setText(t("pick_region"))
        self.btn_browse.setText(t("browse"))
        self.btn_float.setText(t("float_mode"))
        self.btn_record.setText(t("start") if self.worker is None else t("stop"))
        for i, key in enumerate(("high", "medium", "low")):
            self.cmb_quality.setItemText(i, t(f"quality_{key}"))
        self._refresh_targets()
        if self.worker is None:
            self.lbl_status.setText(t("ready"))
        if self.bar is not None:
            self.bar._retranslate()

    # ---------------- state ----------------

    def _monitors(self):
        with mss.MSS() as sct:
            return list(sct.monitors)   # [0] = 所有屏幕并集

    def _refresh_targets(self) -> None:
        t = self.loc.t
        cur = self.cmb_target.currentData()
        self.cmb_target.blockSignals(True)
        self.cmb_target.clear()
        self.cmb_target.addItem(t("target_all"), "all")
        try:
            mons = self._monitors()
        except Exception:
            mons = [None]
        for i in range(1, len(mons)):
            m = mons[i]
            self.cmb_target.addItem(
                t("target_monitor", n=i, w=m["width"], h=m["height"]), f"mon:{i}")
        if self.cfg.get("region"):
            self.cmb_target.addItem(t("target_region_custom"), "region")
        idx = self.cmb_target.findData(cur) if cur else 0
        self.cmb_target.setCurrentIndex(max(0, idx))
        self.cmb_target.blockSignals(False)

    def _sync_from_cfg(self) -> None:
        # 同步期间屏蔽信号，否则语言下拉框会在输出目录填充前触发一次保存
        widgets = (self.cmb_fps, self.cmb_quality, self.cmb_lang,
                   self.cmb_target, self.edt_dir)
        for w in widgets:
            w.blockSignals(True)
        try:
            i = self.cmb_fps.findData(int(self.cfg.get("fps", 30)))
            self.cmb_fps.setCurrentIndex(i if i >= 0 else 4)
            i = self.cmb_quality.findData(self.cfg.get("quality", "high"))
            self.cmb_quality.setCurrentIndex(i if i >= 0 else 0)
            i = self.cmb_lang.findData(self.cfg.get("lang", "zh-CN"))
            self.cmb_lang.setCurrentIndex(i if i >= 0 else 0)
            self.edt_dir.setText(self.cfg.get("output_dir") or DEFAULTS["output_dir"])
            tgt = self.cfg.get("target", "all")
            i = self.cmb_target.findData(tgt)
            if i >= 0:
                self.cmb_target.setCurrentIndex(i)
        finally:
            for w in widgets:
                w.blockSignals(False)

    def _persist(self) -> None:
        self.cfg.update({
            "lang": self.cmb_lang.currentData(),
            "fps": self.cmb_fps.currentData(),
            "quality": self.cmb_quality.currentData(),
            "output_dir": self.edt_dir.text().strip() or DEFAULTS["output_dir"],
            "target": self.cmb_target.currentData(),
        })
        save_settings(self.cfg)

    def _on_lang_changed(self) -> None:
        self.loc.set_lang(self.cmb_lang.currentData())
        self._retranslate()
        self._persist()

    def _browse(self) -> None:
        d = QFileDialog.getExistingDirectory(self, self.loc.t("output_dir"),
                                             self.edt_dir.text() or str(Path.home()))
        if d:
            self.edt_dir.setText(d)
            self._persist()

    def _pick_region(self) -> None:
        self.hide()
        QTimer.singleShot(220, self._show_selector)

    def _show_selector(self) -> None:
        self.sel = RegionSelector()
        self.sel.picked.connect(self._on_region_picked)
        # 不用 showFullScreen：多显示器下它只会铺满一块屏，
        # 这里直接把窗口铺到所有屏幕的逻辑并集上。
        self.sel.show()
        self.sel.raise_()
        self.sel.activateWindow()

    def _on_region_picked(self, rect) -> None:
        self.show()
        self.raise_()
        if rect:
            self.cfg["region"] = list(rect)
            self._refresh_targets()
            i = self.cmb_target.findData("region")
            if i >= 0:
                self.cmb_target.setCurrentIndex(i)
            self._persist()
            self.lbl_status.setText(
                self.loc.t("region_set", w=rect[2], h=rect[3]))

    def _resolve_region(self):
        tgt = self.cmb_target.currentData()
        if tgt == "all":
            m = self._monitors()[0]
        elif isinstance(tgt, str) and tgt.startswith("mon:"):
            m = self._monitors()[int(tgt.split(":")[1])]
        elif tgt == "region" and self.cfg.get("region"):
            return tuple(self.cfg["region"])
        else:
            m = self._monitors()[0]
        return (m["left"], m["top"], m["width"], m["height"])

    # ---------------- record ----------------

    def _toggle_record(self) -> None:
        if self.worker is not None:
            self.worker.stop()
            self.btn_record.setEnabled(False)
            if self.bar is not None:
                self.bar.btn_rec.setEnabled(False)
            self.lbl_status.setText(self.loc.t("finalizing"))
            return
        self._persist()
        out_dir = Path(self.edt_dir.text().strip() or DEFAULTS["output_dir"])
        try:
            out_dir.mkdir(parents=True, exist_ok=True)
        except Exception as exc:                              # noqa: BLE001
            QMessageBox.warning(self, APP_NAME, f"{self.loc.t('err_dir')}\n{exc}")
            return
        out_path = str(out_dir / time.strftime("LiteCap_%Y%m%d_%H%M%S.mp4"))

        region = self._resolve_region()
        self.worker = RecorderWorker(region, self.cmb_fps.currentData(),
                                     self.cmb_quality.currentData(), out_path, self)
        self.worker.tick.connect(self._on_tick)
        self.worker.done.connect(self._on_done)
        self.worker.failed.connect(self._on_failed)
        self._started_at = time.monotonic()
        self.worker.start()
        self._apply_recording_state(True)

    def _apply_recording_state(self, recording: bool) -> None:
        """把录制状态同步到主窗口和悬浮窗两套界面。"""
        if recording:
            self.btn_record.setText(self.loc.t("stop"))
            self.btn_record.setStyleSheet(REC_BTN_LIVE)
        else:
            self.btn_record.setText(self.loc.t("start"))
            self.btn_record.setStyleSheet(REC_BTN_IDLE)
            self.btn_record.setEnabled(True)
        for w in (self.cmb_target, self.cmb_fps, self.cmb_quality,
                  self.cmb_lang, self.edt_dir, self.btn_browse, self.btn_region):
            w.setEnabled(not recording)
        if self.bar is not None:
            self.bar.set_recording(recording)
            if not recording:
                self.bar.set_time("00:00")

    # ---- 悬浮窗模式 ----

    def _enter_float_mode(self) -> None:
        if self.bar is None:
            self.bar = FloatingBar(self)
        self.bar.set_recording(self.worker is not None)
        self.bar.show()
        self.bar.raise_()
        self.hide()

    def _exit_float_mode(self) -> None:
        # 顺序很重要：先让主窗口出现，再藏悬浮窗，
        # 否则中间会有一瞬间“零可见窗口”，Qt 会直接退出进程。
        self.show()
        self.raise_()
        self.activateWindow()
        if self.bar is not None:
            self.bar.hide()

    def _on_tick(self, frames: int, elapsed: float) -> None:
        self.lbl_status.setText(
            self.loc.t("recording", time=_fmt_time(elapsed), frames=frames))
        if self.bar is not None:
            self.bar.set_time(_fmt_time(elapsed))

    def _on_done(self, path: str, frames: int, elapsed: float) -> None:
        self._cleanup_worker()
        size = os.path.getsize(path) if os.path.isfile(path) else 0
        self.lbl_status.setText(
            self.loc.t("saved", name=Path(path).name,
                       size=_fmt_size(size), time=_fmt_time(elapsed)))

    def _on_failed(self, msg: str) -> None:
        self._cleanup_worker()
        self.lbl_status.setText(self.loc.t("failed", msg=msg))

    def _cleanup_worker(self) -> None:
        if self.worker is not None:
            self.worker.wait(4000)
        self.worker = None
        self._apply_recording_state(False)

    def closeEvent(self, event) -> None:  # noqa: N802
        if self.worker is not None:
            self.worker.stop()
            self.worker.wait(5000)
        if self.bar is not None:
            self.bar.hide()
            self.bar.deleteLater()
            self.bar = None
        event.accept()


def _fmt_time(sec: float) -> str:
    sec = int(sec)
    return f"{sec // 60:02d}:{sec % 60:02d}"


def _fmt_size(n: int) -> str:
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return f"{n:.0f} {unit}" if unit == "B" else f"{n:.1f} {unit}"
        n /= 1024.0
    return f"{n:.1f} GB"


# --------------------------------------------------------------------------
# 自检：确认程序不含任何网络能力
# --------------------------------------------------------------------------

NETWORK_TOKENS = (
    "socket", "urllib", "http.client", "httplib", "requests", "ftplib",
    "smtplib", "telnetlib", "asyncio", "xmlrpc", "webbrowser",
    "QtNetwork", "QNetwork", "aiohttp", "websocket", "paramiko",
)


def self_check() -> int:
    src = Path(__file__).read_text(encoding="utf-8")
    bad = []
    for line in src.splitlines():
        s = line.strip()
        if s.startswith("#"):
            continue
        for tok in NETWORK_TOKENS:
            if f"import {tok}" in s or f"from {tok}" in s:
                bad.append(s)
    print(f"{APP_NAME} {APP_VERSION} — offline self-check")
    print(f"  source file : {Path(__file__).name}")
    print(f"  network imports found: {len(bad)}")
    for b in bad:
        print("    !", b)
    print("  verdict:", "OFFLINE-ONLY (no network imports)" if not bad else "REVIEW NEEDED")
    return 0 if not bad else 1


def main() -> int:
    if "--self-check" in sys.argv:
        return self_check()
    QApplication.setApplicationName(APP_NAME)
    app = QApplication(sys.argv)
    app.setStyle("Fusion")
    win = MainWindow()
    win.show()
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
