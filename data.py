# -*- coding: utf-8 -*-
from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from datetime import datetime, timedelta
from pathlib import Path
from typing import Any, Iterable

import matplotlib as mpl
import matplotlib.dates as mdates
import numpy as np
import pandas as pd
from git import InvalidGitRepositoryError, NoSuchPathError, Repo


_TIME_FORMAT = "%Y-%m-%d %H:%M:%S"


# 控制面板/对比同步的字段顺序（尽量贴近日志 Settings 的常见顺序）
_CONTROL_FIELDS: list[tuple[str, str]] = [
    ("settingonline", "联网设置"),
    ("powerSavingMode", "节电模式"),
    ("settings_rev", "settings_rev"),
    ("waterTimeSeconds", "waterTimeSeconds"),
    ("sleepHours", "sleepHours"),
    ("sleepMinutes", "sleepMinutes"),
]


def _normalize_control_value(value: Any) -> Any:
    if isinstance(value, str):
        v = value.strip().lower()
        if v in {"true", "false"}:
            return v == "true"
        parsed_int = _parse_int(value)
        return parsed_int if parsed_int is not None else value.strip()
    return value


def _control_snapshot_from_json(
    version_data: dict[str, Any], settings_data: dict[str, Any]
) -> dict[str, Any]:
    out: dict[str, Any] = {}
    out["settingonline"] = _normalize_control_value(version_data.get("settingonline"))
    out["settings_rev"] = _normalize_control_value(version_data.get("settings_rev"))
    out["powerSavingMode"] = _normalize_control_value(
        settings_data.get("powerSavingMode")
    )

    gs = settings_data.get("globalSettings")
    if not isinstance(gs, dict):
        gs = {}
    out["waterTimeSeconds"] = _normalize_control_value(gs.get("waterTimeSeconds"))
    out["sleepHours"] = _normalize_control_value(gs.get("sleepHours"))
    out["sleepMinutes"] = _normalize_control_value(gs.get("sleepMinutes"))
    return out


def _control_snapshot_from_latest(latest_row: pd.Series) -> dict[str, Any]:
    settings = latest_row.get("settings")
    if not isinstance(settings, dict):
        settings = {}
    # 设备上报一般是扁平 key；这里用同样的 key 名做对比
    out: dict[str, Any] = {}
    for key, _label in _CONTROL_FIELDS:
        if key in settings:
            out[key] = _normalize_control_value(settings.get(key))
    return out


def _control_sync_status(
    latest_row: pd.Series,
    version_data: dict[str, Any] | None,
    settings_data: dict[str, Any] | None,
) -> str:
    if version_data is None or settings_data is None:
        return "未知（JSON 读取失败）"

    in_use = _control_snapshot_from_latest(latest_row)
    expected = _control_snapshot_from_json(version_data, settings_data)

    if not in_use:
        return "未知（日志 settings 无相关字段）"

    diffs: list[str] = []
    unknown: list[str] = []
    for key, label in _CONTROL_FIELDS:
        if key not in in_use:
            continue
        if key not in expected:
            unknown.append(label)
            continue
        if _normalize_control_value(in_use.get(key)) != _normalize_control_value(
            expected.get(key)
        ):
            diffs.append(label)

    if unknown:
        return f"未知（缺少: {', '.join(unknown)}）"
    if diffs:
        return f"有更新尚未同步（{', '.join(diffs)}）"
    return "已同步"


def _configure_matplotlib_fonts() -> None:
    """尽量保证中文不会显示成方框（Windows 常见问题）。"""
    mpl.rcParams["axes.unicode_minus"] = False
    # 依次尝试常见中文字体；如果不存在会自动回退到默认字体。
    mpl.rcParams["font.sans-serif"] = [
        "Microsoft YaHei",
        "SimHei",
        "Noto Sans CJK SC",
        "Arial Unicode MS",
        "DejaVu Sans",
    ]


def _read_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def _write_json(path: Path, data: dict[str, Any]) -> None:
    path.write_text(
        json.dumps(data, ensure_ascii=False, indent=4) + "\n", encoding="utf-8"
    )


@dataclass(frozen=True)
class DeviceReport:
    time: datetime
    wakeup_count: int | None
    moisture_percent: float | None
    current_version: str | None
    ota_checked: str | None
    status: str | None
    settings: dict[str, Any]
    source_file: str
    original_file: str | None
    log_kind: str  # "cur" or "supp"


def _safe_repo_pull(repo_path: Path, remote_name: str = "origin") -> str:
    try:
        repo = Repo(str(repo_path))
    except (InvalidGitRepositoryError, NoSuchPathError):
        return f"跳过 git pull：{repo_path} 不是有效 git 仓库"

    try:
        remote = repo.remote(remote_name)
    except Exception as exc:  # noqa: BLE001
        return f"跳过 git pull：未找到 remote '{remote_name}'（{exc}）"

    try:
        results = remote.pull()
        summary = ", ".join(r.summary for r in results if getattr(r, "summary", None))
        return f"git pull 完成：{summary or 'OK'}"
    except Exception as exc:  # noqa: BLE001
        return f"git pull 失败（继续处理日志）：{exc}"


def _safe_repo_commit_push(
    repo_path: Path,
    remote_name: str,
    *,
    message: str,
    file_paths: list[Path],
) -> str:
    try:
        repo = Repo(str(repo_path))
    except (InvalidGitRepositoryError, NoSuchPathError):
        return f"提交失败：{repo_path} 不是有效 git 仓库"

    try:
        rels: list[str] = []
        for p in file_paths:
            try:
                rels.append(str(p.resolve().relative_to(repo.working_tree_dir)))
            except Exception:  # noqa: BLE001
                rels.append(str(p))

        repo.index.add(rels)
        if not repo.is_dirty(untracked_files=True):
            return "没有检测到文件变更，无需提交"

        repo.index.commit(message)

        remote = repo.remote(remote_name)
        remote.push()
        return f"已提交并推送：{message}"
    except Exception as exc:  # noqa: BLE001
        return f"提交/推送失败：{exc}"


def _parse_kv_line(line: str) -> tuple[str, str] | None:
    if ":" not in line:
        return None
    key, value = line.split(":", 1)
    key = key.strip()
    value = value.strip()
    if not key:
        return None
    return key, value


def _parse_time(value: str) -> datetime | None:
    try:
        return datetime.strptime(value.strip(), _TIME_FORMAT)
    except ValueError:
        return None


def _parse_moisture(value: str) -> float | None:
    value = value.strip()
    match = re.match(r"^(-?\d+(?:\.\d+)?)%$", value)
    if not match:
        return None
    try:
        return float(match.group(1))
    except ValueError:
        return None


def _parse_int(value: str) -> int | None:
    try:
        return int(value.strip())
    except ValueError:
        return None


def _parse_settings(lines: list[str], start_index: int) -> tuple[dict[str, Any], int]:
    settings: dict[str, Any] = {}
    i = start_index
    while i < len(lines):
        raw = lines[i]
        if not raw.strip():
            i += 1
            continue
        if raw.startswith("--------------------------------") or raw.strip().startswith(
            "["
        ):
            break

        if not raw.startswith("  "):
            break

        kv = _parse_kv_line(raw)
        if kv:
            key, value = kv
            if value.lower() in {"true", "false"}:
                settings[key] = value.lower() == "true"
            else:
                parsed_int = _parse_int(value)
                settings[key] = parsed_int if parsed_int is not None else value
        i += 1

    return settings, i


def _parse_section(
    section_text: str,
    *,
    source_file: str,
    log_kind: str,
) -> list[DeviceReport]:
    lines = [ln.rstrip("\r\n") for ln in section_text.splitlines()]
    original_file: str | None = None
    for ln in lines:
        if ln.startswith("OriginalFile:"):
            original_file = ln.split(":", 1)[1].strip() or None
            break

    try:
        report_start = next(
            i for i, ln in enumerate(lines) if ln.strip() == "Device Report:"
        )
    except StopIteration:
        return []

    time: datetime | None = None
    wakeup_count: int | None = None
    moisture_percent: float | None = None
    current_version: str | None = None
    ota_checked: str | None = None
    status: str | None = None
    settings: dict[str, Any] = {}

    i = report_start + 1
    while i < len(lines):
        ln = lines[i].strip()
        if not ln:
            i += 1
            continue
        if ln.startswith("--------------------------------") or ln.startswith("["):
            break

        if ln == "Settings:":
            settings, i = _parse_settings(lines, i + 1)
            continue

        kv = _parse_kv_line(ln)
        if kv:
            key, value = kv
            if key == "Time":
                time = _parse_time(value)
            elif key == "Wakeup Count":
                wakeup_count = _parse_int(value)
            elif key == "Moisture":
                moisture_percent = _parse_moisture(value)
            elif key == "Current Version":
                current_version = value
            elif key == "OTA Checked":
                ota_checked = value
            elif key == "Status":
                status = value

        i += 1

    if time is None:
        return []

    return [
        DeviceReport(
            time=time,
            wakeup_count=wakeup_count,
            moisture_percent=moisture_percent,
            current_version=current_version,
            ota_checked=ota_checked,
            status=status,
            settings=settings,
            source_file=source_file,
            original_file=original_file,
            log_kind=log_kind,
        )
    ]


def parse_log_file(path: Path) -> list[DeviceReport]:
    name = path.name.lower()
    if name.endswith("_cur.txt"):
        log_kind = "cur"
    elif name.endswith("_supp.txt"):
        log_kind = "supp"
    else:
        log_kind = "unknown"

    text = path.read_text(encoding="utf-8", errors="replace")

    # _supp 通常用分隔线拼多个段；_cur 通常只有一个段。
    sections = text.split("--------------------------------")
    reports: list[DeviceReport] = []
    for section in sections:
        reports.extend(
            _parse_section(
                section,
                source_file=str(path),
                log_kind=log_kind,
            )
        )
    return reports


def collect_reports(logs_dir: Path) -> list[DeviceReport]:
    reports: list[DeviceReport] = []
    for path in sorted(logs_dir.glob("*.txt")):
        reports.extend(parse_log_file(path))
    return reports


def reports_to_dataframe(reports: Iterable[DeviceReport]) -> pd.DataFrame:
    rows: list[dict[str, Any]] = []
    for r in reports:
        rows.append(
            {
                "time": r.time,
                "wakeup_count": r.wakeup_count,
                "moisture_percent": r.moisture_percent,
                "current_version": r.current_version,
                "ota_checked": r.ota_checked,
                "status": r.status,
                "settings": r.settings,
                "source_file": r.source_file,
                "original_file": r.original_file,
                "log_kind": r.log_kind,
            }
        )

    df = pd.DataFrame(rows)
    if df.empty:
        return df
    df = df.sort_values("time").reset_index(drop=True)
    return df


def plot_metrics(df: pd.DataFrame, output_path: Path, title: str | None = None) -> None:
    fig = build_figure(df, title=title)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output_path, dpi=160)


def _run_qt_ui(*, logs_dir: Path, title: str, repo_path: Path, remote_name: str) -> int:
    """使用 PySide6 构建本地应用风格界面（更符合现代审美）。"""

    try:
        import os
        import PySide6

        # 兼容部分 Conda/环境变量场景：Qt 找不到 "windows" 平台插件。
        # 通过 PySide6 安装路径显式指向 plugins/platforms。
        pyside_dir = Path(PySide6.__file__).resolve().parent
        plugins_dir = pyside_dir / "plugins"
        platforms_dir = plugins_dir / "platforms"

        if platforms_dir.exists() and not os.environ.get("QT_QPA_PLATFORM_PLUGIN_PATH"):
            os.environ["QT_QPA_PLATFORM_PLUGIN_PATH"] = str(platforms_dir)
        if plugins_dir.exists() and not os.environ.get("QT_PLUGIN_PATH"):
            os.environ["QT_PLUGIN_PATH"] = str(plugins_dir)

        from PySide6.QtCore import QObject, QSize, Qt, QThread, Signal
        from PySide6.QtGui import QColor, QPainter, QPalette
        from PySide6.QtWidgets import (
            QApplication,
            QCheckBox,
            QComboBox,
            QFormLayout,
            QGroupBox,
            QHBoxLayout,
            QLabel,
            QMainWindow,
            QPushButton,
            QSpinBox,
            QTabWidget,
            QTableWidget,
            QTableWidgetItem,
            QVBoxLayout,
            QWidget,
        )

        from matplotlib.backends.backend_qtagg import (
            FigureCanvasQTAgg as FigureCanvas,
            NavigationToolbar2QT as NavigationToolbar,
        )
    except Exception as exc:  # noqa: BLE001
        print(
            "无法启动 Qt UI（缺少 PySide6）。\n"
            "请先安装：pip install PySide6==6.6.3.1\n"
            f"详细错误：{exc}"
        )
        return 4

    def apply_dark_palette(app: QApplication) -> None:
        app.setStyle("Fusion")
        palette = QPalette()
        palette.setColor(QPalette.Window, QColor(15, 23, 42))
        palette.setColor(QPalette.WindowText, QColor(226, 232, 240))
        palette.setColor(QPalette.Base, QColor(2, 6, 23))
        palette.setColor(QPalette.AlternateBase, QColor(15, 23, 42))
        palette.setColor(QPalette.ToolTipBase, QColor(226, 232, 240))
        palette.setColor(QPalette.ToolTipText, QColor(226, 232, 240))
        palette.setColor(QPalette.Text, QColor(226, 232, 240))
        palette.setColor(QPalette.Button, QColor(31, 41, 55))
        palette.setColor(QPalette.ButtonText, QColor(226, 232, 240))
        palette.setColor(QPalette.Highlight, QColor(59, 130, 246))
        palette.setColor(QPalette.HighlightedText, QColor(15, 23, 42))
        app.setPalette(palette)

    class ToggleSwitch(QCheckBox):
        """自绘开关，避免仅靠 QSS 时在不同平台出现“只有圆点/不显示”的问题。"""

        def __init__(self, checked: bool = False, *, enabled: bool = True):
            super().__init__("")
            self.setChecked(bool(checked))
            self.setEnabled(bool(enabled))
            self.setCursor(Qt.PointingHandCursor)
            self.setContentsMargins(0, 0, 0, 0)
            # 让点击命中更直观：点击整个控件区域即可切换
            self.setFocusPolicy(Qt.StrongFocus)

        def sizeHint(self) -> QSize:  # noqa: N802
            return QSize(52, 28)

        def paintEvent(self, event) -> None:  # noqa: N802
            # 关闭父类的 indicator 绘制，完全自绘
            painter = QPainter(self)
            painter.setRenderHint(QPainter.Antialiasing)

            w = self.width()
            h = self.height()
            track_h = min(24, h - 4)
            track_w = min(46, w - 4)
            x0 = (w - track_w) // 2
            y0 = (h - track_h) // 2
            radius = track_h // 2

            on = self.isChecked()
            enabled = self.isEnabled()

            if on:
                track = QColor("#22c55e")
            else:
                track = QColor("#334155")
            if not enabled:
                track.setAlpha(140)

            knob = QColor("#0f172a") if on else QColor("#e2e8f0")
            if not enabled:
                knob.setAlpha(170)

            painter.setPen(Qt.NoPen)
            painter.setBrush(track)
            painter.drawRoundedRect(x0, y0, track_w, track_h, radius, radius)

            knob_d = track_h - 4
            knob_y = y0 + 2
            knob_x = (x0 + track_w - knob_d - 2) if on else (x0 + 2)
            painter.setBrush(knob)
            painter.drawEllipse(knob_x, knob_y, knob_d, knob_d)

            painter.end()

        def mouseReleaseEvent(self, event) -> None:  # noqa: N802
            # QCheckBox 默认只在“indicator 区域”命中时才切换。
            # 我们自绘后 indicator 的几何与视觉不一致，因此改为整块可点。
            if event.button() == Qt.LeftButton and self.isEnabled():
                self.setChecked(not self.isChecked())
                event.accept()
                return
            return super().mouseReleaseEvent(event)

        def keyPressEvent(self, event) -> None:  # noqa: N802
            # 保持键盘可用性：空格切换
            if event.key() == Qt.Key_Space and self.isEnabled():
                self.setChecked(not self.isChecked())
                event.accept()
                return
            return super().keyPressEvent(event)

    def make_toggle(checked: bool = False, *, enabled: bool = True) -> ToggleSwitch:
        return ToggleSwitch(checked=checked, enabled=enabled)

    def _set_result_color(label: QLabel, text: str) -> None:
        # 简单按关键字上色
        if "已提交并推送" in text or "推送" in text and "失败" not in text:
            label.setStyleSheet("color: #22c55e; font-size: 12px;")
        elif "失败" in text or "错误" in text:
            label.setStyleSheet("color: #fb7185; font-size: 12px;")
        else:
            label.setStyleSheet("color: #93c5fd; font-size: 12px;")

    def table_from_pairs(
        pairs: list[tuple[str, str]],
        *,
        toggle_keys: set[str] | None = None,
    ) -> QTableWidget:
        toggle_keys = toggle_keys or set()
        table = QTableWidget()
        table.setColumnCount(2)
        table.setHorizontalHeaderLabels(["字段", "值"])
        table.setRowCount(len(pairs))
        table.verticalHeader().setVisible(False)
        table.setAlternatingRowColors(True)
        table.setEditTriggers(QTableWidget.NoEditTriggers)
        table.setSelectionBehavior(QTableWidget.SelectRows)
        table.setSelectionMode(QTableWidget.SingleSelection)
        table.setShowGrid(False)
        for r, (k, v) in enumerate(pairs):
            table.setItem(r, 0, QTableWidgetItem(k))
            if k in toggle_keys:
                widget = make_toggle(
                    checked=str(v).strip().lower() in {"1", "true", "yes", "on"},
                    enabled=False,
                )
                table.setCellWidget(r, 1, widget)
            else:
                table.setItem(r, 1, QTableWidgetItem(v))
        table.resizeColumnsToContents()
        table.horizontalHeader().setStretchLastSection(True)
        return table

    def _settings_pairs_ordered(latest: pd.Series) -> list[tuple[str, str]]:
        settings = latest.get("settings")
        if not isinstance(settings, dict) or not settings:
            return []

        key_name_map = {
            "powerSavingMode": "节电模式",
            "settingonline": "联网设置",
        }

        # 先按控制字段顺序输出（如果在日志 settings 中存在）
        pairs: list[tuple[str, str]] = []
        used: set[str] = set()
        for key, label in _CONTROL_FIELDS:
            if key in settings:
                pairs.append((label, str(settings.get(key))))
                used.add(key)

        # 再按日志原始顺序补齐其他字段（dict 保留插入顺序）
        for k, v in settings.items():
            if str(k) in used:
                continue
            pairs.append((key_name_map.get(str(k), str(k)), str(v)))

        return pairs

    class _PushWorker(QObject):
        finished = Signal(str)

        def __init__(
            self,
            *,
            repo_path: Path,
            remote_name: str,
            version_path: Path,
            settings_path: Path,
            ver: dict[str, Any],
            st: dict[str, Any],
        ):
            super().__init__()
            self.repo_path = repo_path
            self.remote_name = remote_name
            self.version_path = version_path
            self.settings_path = settings_path
            self.ver = ver
            self.st = st

        def run(self) -> None:
            try:
                _write_json(self.version_path, self.ver)
                _write_json(self.settings_path, self.st)
                msg = f"control change rev ='{self.ver.get('settings_rev')}'"
                result = _safe_repo_commit_push(
                    self.repo_path,
                    self.remote_name,
                    message=msg,
                    file_paths=[self.version_path, self.settings_path],
                )
                self.finished.emit(result)
            except Exception as exc:  # noqa: BLE001
                self.finished.emit(f"提交/推送失败：{exc}")

    class MainWindow(QMainWindow):
        def __init__(self):
            super().__init__()
            self.setWindowTitle("ESP32 WATER Logs Viewer")
            self.resize(1400, 820)

            self._config_dir = Path(__file__).resolve().parent / "releases"
            self._repo_path = repo_path
            self._remote_name = remote_name
            self._settings_path = self._config_dir / "settings.json"
            self._version_path = self._config_dir / "version.json"

            central = QWidget()
            self.setCentralWidget(central)
            root = QVBoxLayout(central)
            root.setContentsMargins(16, 16, 16, 16)
            root.setSpacing(12)

            header = QLabel("ESP32 WATER")
            header.setStyleSheet(
                "font-size: 22px; font-weight: 800; letter-spacing: 0.2px;"
            )
            subtitle = QLabel("Moisture & Wakeup • Logs Dashboard")
            subtitle.setStyleSheet("color: #94a3b8; font-size: 12px;")
            root.addWidget(header)
            root.addWidget(subtitle)

            bar = QHBoxLayout()
            bar.setSpacing(10)
            range_label = QLabel("图表范围")
            range_label.setStyleSheet("color: #94a3b8; font-size: 12px;")
            self.range_combo = QComboBox()
            self.range_combo.addItem("按天", "day")
            self.range_combo.addItem("按周", "week")
            self.range_combo.addItem("按月", "month")
            self.range_combo.addItem("全部", "all")
            self.range_combo.setCurrentIndex(3)
            self.range_combo.setFixedWidth(120)
            self.range_combo.currentIndexChanged.connect(self._on_time_range_changed)
            self.status = QLabel("")
            self.status.setStyleSheet("color: #93c5fd; font-size: 13px;")
            btn_quit = QPushButton("退出")
            btn_quit.setFixedSize(120, 34)
            btn_quit.clicked.connect(self.close)
            bar.addWidget(range_label)
            bar.addWidget(self.range_combo)
            bar.addWidget(self.status, 1)
            bar.addWidget(btn_quit)
            root.addLayout(bar)

            main = QHBoxLayout()
            main.setSpacing(12)
            root.addLayout(main, 1)

            # 左侧：图表
            left = QWidget()
            left_layout = QVBoxLayout(left)
            left_layout.setContentsMargins(0, 0, 0, 0)
            left_layout.setSpacing(6)
            main.addWidget(left, 3)
            self.plot_container = left_layout

            # 右侧：数据概览 / Settings
            right = QWidget()
            right_layout = QVBoxLayout(right)
            right_layout.setContentsMargins(0, 0, 0, 0)
            right_layout.setSpacing(8)
            # 右侧宽度减半：把 stretch 从 2 降到 1
            main.addWidget(right, 1)

            self.tabs = QTabWidget()
            right_layout.addWidget(self.tabs, 1)

            self.tab_overview = QWidget()
            self.tab_settings = QWidget()
            self.tabs.addTab(self.tab_overview, "概览")
            self.tabs.addTab(self.tab_settings, "Settings in use")

            # 右下：控制窗口
            self.ctrl_group = QGroupBox("控制")
            right_layout.addWidget(self.ctrl_group, 0)
            self._build_controls()

            self._plot_canvas: FigureCanvas | None = None
            self._plot_toolbar: NavigationToolbar | None = None

            self._latest_row: pd.Series | None = None
            self._plot_df: pd.DataFrame | None = None
            self._pushing = False
            self._push_thread: QThread | None = None

            # 延迟到窗口首次显示时再加载，避免阻塞窗口创建；同时避免重复渲染导致布局被覆盖。
            self._loaded = False

        def _build_controls(self) -> None:
            layout = QVBoxLayout(self.ctrl_group)
            layout.setContentsMargins(12, 10, 12, 12)
            layout.setSpacing(8)

            form = QFormLayout()
            form.setLabelAlignment(Qt.AlignLeft)
            form.setFormAlignment(Qt.AlignTop)
            form.setHorizontalSpacing(10)
            form.setVerticalSpacing(8)

            # 用开关样式替代小勾选框，提升可用性
            self.ctrl_settingonline = make_toggle(False, enabled=True)
            self.ctrl_powerSavingMode = make_toggle(False, enabled=True)

            self.ctrl_settings_rev = QSpinBox()
            self.ctrl_settings_rev.setRange(0, 10_000_000)

            self.ctrl_waterTimeSeconds = QSpinBox()
            self.ctrl_waterTimeSeconds.setRange(0, 3600)

            self.ctrl_sleepHours = QSpinBox()
            self.ctrl_sleepHours.setRange(0, 48)

            self.ctrl_sleepMinutes = QSpinBox()
            self.ctrl_sleepMinutes.setRange(0, 59)

            # 顺序与 Settings in use 保持一致
            form.addRow("联网设置", self.ctrl_settingonline)
            form.addRow("节电模式", self.ctrl_powerSavingMode)
            form.addRow("settings_rev", self.ctrl_settings_rev)
            form.addRow("waterTimeSeconds", self.ctrl_waterTimeSeconds)
            form.addRow("sleepHours", self.ctrl_sleepHours)
            form.addRow("sleepMinutes", self.ctrl_sleepMinutes)
            layout.addLayout(form)

            self.ctrl_hint = QLabel(
                "修改后点击确认，将写入 settings.json / version.json 并推送远程。"
            )
            self.ctrl_hint.setStyleSheet("color: #94a3b8; font-size: 12px;")
            layout.addWidget(self.ctrl_hint)

            btn_row = QHBoxLayout()
            self.ctrl_push_status = QLabel("")
            self.ctrl_push_status.setMinimumWidth(180)
            self.ctrl_push_status.setAlignment(Qt.AlignRight | Qt.AlignVCenter)
            _set_result_color(self.ctrl_push_status, "")
            btn_row.addWidget(self.ctrl_push_status, 1)
            self.btn_apply = QPushButton("确认并推送")
            self.btn_apply.setFixedHeight(34)
            self.btn_apply.clicked.connect(self._apply_control_changes)
            btn_row.addWidget(self.btn_apply)
            layout.addLayout(btn_row)

            self._load_control_values()

        def _load_control_values(self) -> None:
            try:
                ver = _read_json(self._version_path)
                st = _read_json(self._settings_path)

                self.ctrl_settingonline.setChecked(
                    bool(ver.get("settingonline", False))
                )
                self.ctrl_settings_rev.setValue(int(ver.get("settings_rev", 0) or 0))

                self.ctrl_powerSavingMode.setChecked(
                    bool(st.get("powerSavingMode", False))
                )
                gs = (
                    st.get("globalSettings")
                    if isinstance(st.get("globalSettings"), dict)
                    else {}
                )
                self.ctrl_waterTimeSeconds.setValue(
                    int(gs.get("waterTimeSeconds", 0) or 0)
                )
                self.ctrl_sleepHours.setValue(int(gs.get("sleepHours", 0) or 0))
                self.ctrl_sleepMinutes.setValue(int(gs.get("sleepMinutes", 0) or 0))
            except Exception as exc:  # noqa: BLE001
                self.status.setText(f"控制参数加载失败：{exc}")

        def _apply_control_changes(self) -> None:
            if self._pushing:
                return

            try:
                ver = _read_json(self._version_path)
                st = _read_json(self._settings_path)

                ver["settingonline"] = bool(self.ctrl_settingonline.isChecked())
                ver["settings_rev"] = int(self.ctrl_settings_rev.value())

                st["powerSavingMode"] = bool(self.ctrl_powerSavingMode.isChecked())
                gs = st.get("globalSettings")
                if not isinstance(gs, dict):
                    gs = {}
                    st["globalSettings"] = gs
                gs["waterTimeSeconds"] = int(self.ctrl_waterTimeSeconds.value())
                gs["sleepHours"] = int(self.ctrl_sleepHours.value())
                gs["sleepMinutes"] = int(self.ctrl_sleepMinutes.value())

                # UI 反馈 + 防连点
                self._pushing = True
                self.btn_apply.setEnabled(False)
                self.btn_apply.setText("推送中…")
                self.ctrl_push_status.setText("推送中…")
                _set_result_color(self.ctrl_push_status, "推送中")

                worker = _PushWorker(
                    repo_path=self._repo_path,
                    remote_name=self._remote_name,
                    version_path=self._version_path,
                    settings_path=self._settings_path,
                    ver=ver,
                    st=st,
                )
                thread = QThread(self)
                worker.moveToThread(thread)
                thread.started.connect(worker.run)

                def on_finished(result: str) -> None:
                    self._pushing = False
                    self.btn_apply.setEnabled(True)
                    self.btn_apply.setText("确认并推送")
                    self.ctrl_push_status.setText(result)
                    _set_result_color(self.ctrl_push_status, result)
                    self.status.setText(result)

                    # 刷新概览的“控制状态”显示
                    if self._latest_row is not None:
                        self._set_tables(self._latest_row)

                    worker.deleteLater()
                    thread.quit()

                worker.finished.connect(on_finished)
                thread.finished.connect(thread.deleteLater)
                thread.start()
                self._push_thread = thread
            except Exception as exc:  # noqa: BLE001
                self._pushing = False
                self.btn_apply.setEnabled(True)
                self.btn_apply.setText("确认并推送")
                self.ctrl_push_status.setText(f"保存失败：{exc}")
                _set_result_color(self.ctrl_push_status, f"保存失败：{exc}")
                self.status.setText(f"保存失败：{exc}")

        def _load_and_render(self) -> None:
            reports = collect_reports(logs_dir)
            df = reports_to_dataframe(reports)
            if df.empty:
                raise ValueError("未解析到任何 Device Report")
            self._plot_df = df
            latest = df.iloc[-1]
            self._latest_row = latest

            fig = build_figure(
                df,
                title=title,
                time_range=self._selected_time_range(),
            )
            self._set_plot(fig)
            self._set_tables(latest)
            self.status.setText(f"最后更新：{latest.get('time')}")

        def _selected_time_range(self) -> str:
            data = self.range_combo.currentData()
            return str(data) if data else "all"

        def _on_time_range_changed(self, _index: int) -> None:
            if self._plot_df is None or self._plot_df.empty:
                return
            fig = build_figure(
                self._plot_df,
                title=title,
                time_range=self._selected_time_range(),
            )
            self._set_plot(fig)

        def showEvent(self, event) -> None:  # noqa: N802
            # 首次显示时加载一次，避免窗口创建阶段阻塞 UI。
            try:
                if not self._loaded:
                    self._loaded = True
                    self._load_and_render()
            except Exception as exc:  # noqa: BLE001
                self.status.setText(f"加载失败：{exc}")
            return super().showEvent(event)

        def _set_plot(self, fig) -> None:
            if self._plot_toolbar is not None:
                self._plot_toolbar.setParent(None)
                self._plot_toolbar.deleteLater()
                self._plot_toolbar = None
            if self._plot_canvas is not None:
                self._plot_canvas.setParent(None)
                self._plot_canvas.deleteLater()
                self._plot_canvas = None

            canvas = FigureCanvas(fig)
            toolbar = NavigationToolbar(canvas, self)
            self.plot_container.addWidget(toolbar)
            self.plot_container.addWidget(canvas, 1)
            self._plot_canvas = canvas
            self._plot_toolbar = toolbar

        def _set_tables(self, latest: pd.Series) -> None:
            overview_pairs = _kv_pairs_from_latest(latest)

            ver: dict[str, Any] | None
            st: dict[str, Any] | None
            try:
                ver = _read_json(self._version_path)
                st = _read_json(self._settings_path)
            except Exception:  # noqa: BLE001
                ver = None
                st = None
            overview_pairs.append(("控制状态", _control_sync_status(latest, ver, st)))

            settings_pairs = _settings_pairs_ordered(latest)

            def clear_layout(layout: QVBoxLayout) -> None:
                while layout.count():
                    item = layout.takeAt(0)
                    w = item.widget()
                    if w is not None:
                        w.setParent(None)
                        w.deleteLater()

            ov_layout = self.tab_overview.layout()
            if ov_layout is None:
                ov_layout = QVBoxLayout(self.tab_overview)
                ov_layout.setContentsMargins(0, 0, 0, 0)
            clear_layout(ov_layout)
            ov_layout.addWidget(table_from_pairs(overview_pairs))

            st_layout = self.tab_settings.layout()
            if st_layout is None:
                st_layout = QVBoxLayout(self.tab_settings)
                st_layout.setContentsMargins(0, 0, 0, 0)
            clear_layout(st_layout)
            if settings_pairs:
                st_layout.addWidget(
                    table_from_pairs(
                        settings_pairs,
                        toggle_keys={"联网设置", "节电模式"},
                    )
                )
            else:
                empty = QLabel("(无 settings 数据)")
                empty.setStyleSheet("color: #94a3b8;")
                st_layout.addWidget(empty)

    app = QApplication.instance() or QApplication([sys.argv[0]])
    apply_dark_palette(app)
    win = MainWindow()
    win.show()
    return app.exec()


def _pchip_slopes(x: np.ndarray, y: np.ndarray) -> np.ndarray:
    """PCHIP(Fritsch-Carlson) 节点斜率：在多数场景下避免超调、形状保持更稳健。"""
    n = int(x.shape[0])
    if n < 2:
        raise ValueError("插值至少需要 2 个点")
    if np.any(np.diff(x) <= 0):
        raise ValueError("x 必须严格递增（时间戳需去重并排序）")

    h = np.diff(x)
    delta = np.diff(y) / h
    d = np.zeros(n, dtype=float)

    if n == 2:
        d[0] = delta[0]
        d[1] = delta[0]
        return d

    # 内点：加权调和平均；遇到变号则置 0
    for k in range(1, n - 1):
        if delta[k - 1] == 0.0 or delta[k] == 0.0 or (delta[k - 1] * delta[k] < 0.0):
            d[k] = 0.0
        else:
            w1 = 2.0 * h[k] + h[k - 1]
            w2 = h[k] + 2.0 * h[k - 1]
            d[k] = (w1 + w2) / (w1 / delta[k - 1] + w2 / delta[k])

    # 端点：形状保持的单边估计
    d0 = ((2.0 * h[0] + h[1]) * delta[0] - h[0] * delta[1]) / (h[0] + h[1])
    if np.sign(d0) != np.sign(delta[0]):
        d0 = 0.0
    elif (np.sign(delta[0]) != np.sign(delta[1])) and (abs(d0) > abs(3.0 * delta[0])):
        d0 = 3.0 * delta[0]
    d[0] = d0

    dn = ((2.0 * h[-1] + h[-2]) * delta[-1] - h[-1] * delta[-2]) / (h[-1] + h[-2])
    if np.sign(dn) != np.sign(delta[-1]):
        dn = 0.0
    elif (np.sign(delta[-1]) != np.sign(delta[-2])) and (
        abs(dn) > abs(3.0 * delta[-1])
    ):
        dn = 3.0 * delta[-1]
    d[-1] = dn

    return d


def _pchip_eval(
    x: np.ndarray, y: np.ndarray, d: np.ndarray, x_new: np.ndarray
) -> np.ndarray:
    """分段三次 Hermite 评估（PCHIP）。"""
    idx = np.searchsorted(x, x_new, side="right") - 1
    idx = np.clip(idx, 0, len(x) - 2)
    x_i = x[idx]
    x_ip1 = x[idx + 1]
    h = x_ip1 - x_i
    t = (x_new - x_i) / h

    y_i = y[idx]
    y_ip1 = y[idx + 1]
    d_i = d[idx]
    d_ip1 = d[idx + 1]

    h00 = (2.0 * t**3) - (3.0 * t**2) + 1.0
    h10 = (t**3) - (2.0 * t**2) + t
    h01 = (-2.0 * t**3) + (3.0 * t**2)
    h11 = (t**3) - (t**2)
    return h00 * y_i + h10 * h * d_i + h01 * y_ip1 + h11 * h * d_ip1


def _series_for_plot(df: pd.DataFrame, col: str) -> tuple[np.ndarray, np.ndarray]:
    series_df = df[["time", col]].dropna(subset=["time", col]).sort_values("time")
    # 去重：同一时刻多条时取最后一条
    series_df = series_df.drop_duplicates(subset=["time"], keep="last")
    if series_df.empty:
        return np.array([]), np.array([])
    x = mdates.date2num(series_df["time"].dt.to_pydatetime())
    y = series_df[col].astype(float).to_numpy()
    return x, y


def _filter_df_by_time_range(df: pd.DataFrame, range_key: str) -> pd.DataFrame:
    if df.empty or "time" not in df.columns:
        return df

    if range_key == "all":
        return df

    latest_time = df["time"].max()
    if pd.isna(latest_time):
        return df

    if range_key == "day":
        delta = timedelta(days=1)
    elif range_key == "week":
        delta = timedelta(weeks=1)
    elif range_key == "month":
        delta = timedelta(days=30)
    else:
        return df

    cutoff = latest_time - delta
    filtered = df[df["time"] >= cutoff].copy()
    return filtered if not filtered.empty else df


def _smooth_curve(
    x: np.ndarray, y: np.ndarray, points: int = 400
) -> tuple[np.ndarray, np.ndarray] | None:
    if x.size < 2:
        return None
    try:
        d = _pchip_slopes(x, y)
    except ValueError:
        return None
    x_new = np.linspace(x[0], x[-1], max(points, x.size * 10))
    y_new = _pchip_eval(x, y, d, x_new)
    return x_new, y_new


def build_figure(
    df: pd.DataFrame,
    title: str | None = None,
    *,
    time_range: str = "all",
):
    if df.empty:
        raise ValueError("没有可绘制的数据（未解析到任何 Device Report）")

    df_plot = _filter_df_by_time_range(df, time_range)

    _configure_matplotlib_fonts()

    from matplotlib.figure import Figure

    fig = Figure(figsize=(12, 7))
    ax0 = fig.add_subplot(2, 1, 1)
    ax1 = fig.add_subplot(2, 1, 2, sharex=ax0)

    # Moisture
    x_m, y_m = _series_for_plot(df_plot, "moisture_percent")
    if x_m.size:
        ax0.plot(x_m, y_m, linestyle="None", marker="o", markersize=4, label="raw")
        smooth = _smooth_curve(x_m, y_m)
        if smooth is not None:
            ax0.plot(smooth[0], smooth[1], linestyle="-", linewidth=1.5, label="pchip")
    ax0.set_ylabel("Moisture (%)")
    ax0.set_title("湿度变化")
    ax0.grid(True, alpha=0.3)
    ax0.legend(loc="best")

    # Wakeup Count
    x_w, y_w = _series_for_plot(df_plot, "wakeup_count")
    if x_w.size:
        ax1.plot(
            x_w,
            y_w,
            linestyle="None",
            marker="o",
            markersize=4,
            color="#d97706",
            label="raw",
        )
        smooth = _smooth_curve(x_w, y_w)
        if smooth is not None:
            ax1.plot(
                smooth[0],
                smooth[1],
                linestyle="-",
                linewidth=1.5,
                color="#b45309",
                label="pchip",
            )
    ax1.set_ylabel("Wakeup Count")
    ax1.set_title("唤醒计数")
    ax1.grid(True, alpha=0.3)
    ax1.legend(loc="best")

    ax1.xaxis.set_major_formatter(mdates.DateFormatter("%m-%d %H:%M"))
    fig.autofmt_xdate(rotation=30, ha="right")
    if title:
        fig.suptitle(title)
    # 预留更多边距，避免横坐标刻度在 Qt/高 DPI 场景下被截断
    fig.tight_layout(rect=(0, 0, 1, 0.95) if title else None, pad=1.6)
    fig.subplots_adjust(bottom=0.18, right=0.98)
    return fig


def _kv_pairs_from_latest(latest_row: pd.Series) -> list[tuple[str, str]]:
    time_val = latest_row.get("time")
    time_str = str(time_val) if time_val is not None else "-"
    moisture = latest_row.get("moisture_percent")
    moisture_str = f"{moisture}%" if moisture is not None else "-"

    pairs: list[tuple[str, str]] = [
        ("时间", time_str),
        (
            "唤醒次数",
            str(
                latest_row.get("wakeup_count")
                if latest_row.get("wakeup_count") is not None
                else "-"
            ),
        ),
        ("湿度", moisture_str),
        ("固件版本", str(latest_row.get("current_version") or "-")),
        ("OTA Checked", str(latest_row.get("ota_checked") or "-")),
        ("状态", str(latest_row.get("status") or "-")),
        ("日志类型", str(latest_row.get("log_kind") or "-")),
    ]
    if latest_row.get("original_file"):
        pairs.append(("OriginalFile", str(latest_row.get("original_file"))))
    return pairs


def _settings_items_from_latest(latest_row: pd.Series) -> list[tuple[str, str]]:
    settings = latest_row.get("settings")
    if not isinstance(settings, dict) or not settings:
        return []

    key_name_map = {
        "powerSavingMode": "节电模式",
        "settingonline": "联网设置",
    }
    items = [(key_name_map.get(str(k), str(k)), str(v)) for k, v in settings.items()]
    items.sort(key=lambda kv: kv[0])
    return items


def _format_latest_summary(latest_row: pd.Series) -> str:
    settings = latest_row.get("settings")
    if isinstance(settings, dict):
        settings_str = json.dumps(settings, ensure_ascii=False, sort_keys=True)
    else:
        settings_str = str(settings)

    parts = [
        f"Time: {latest_row.get('time')}",
        f"Wakeup Count: {latest_row.get('wakeup_count')}",
        f"Moisture: {latest_row.get('moisture_percent')}%",
        f"Current Version: {latest_row.get('current_version')}",
        f"OTA Checked: {latest_row.get('ota_checked')}",
        f"Status: {latest_row.get('status')}",
        f"Settings: {settings_str}",
        f"Source File: {latest_row.get('source_file')}",
    ]
    if latest_row.get("original_file"):
        parts.append(f"OriginalFile: {latest_row.get('original_file')}")
    return "\n".join(parts)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=(
            "1) 先对当前仓库执行 git pull；\n"
            "2) 解析 logs 下 *_cur.txt（单条）和 *_supp.txt（多条补充）日志；\n"
            "3) 绘制 Moisture/Wakeup Count 随时间变化折线图；\n"
            "4) 其他字段取时间最新的一条日志输出。"
        )
    )
    parser.add_argument(
        "--repo",
        type=Path,
        default=Path(__file__).resolve().parent / "releases",
        help="git 仓库路径（默认：项目根目录下的 releases/）",
    )
    parser.add_argument(
        "--remote",
        default="origin",
        help="git remote 名称（默认：origin）",
    )
    parser.add_argument(
        "--logs",
        type=Path,
        default=Path(__file__).resolve().parent / "releases" / "logs",
        help="日志目录（默认：releases/logs/）",
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=Path(__file__).resolve().parent / "releases" / "logs" / "metrics_over_time.png",
        help="折线图输出路径（默认：releases/logs/metrics_over_time.png）",
    )
    parser.add_argument(
        "--export",
        action="store_true",
        help="同时导出图片到 --out（默认不导出，只显示 UI）",
    )
    parser.add_argument(
        "--no-ui",
        action="store_true",
        help="不启动 UI（用于无界面环境）；可配合 --export 导出图片",
    )
    parser.add_argument(
        "--csv",
        type=Path,
        default=Path(__file__).resolve().parent / "releases" / "logs" / "parsed_reports.csv",
        help="解析结果 CSV 输出路径（默认：releases/logs/parsed_reports.csv）",
    )
    args = parser.parse_args(argv)

    print(_safe_repo_pull(args.repo, remote_name=args.remote))

    if not args.logs.exists():
        print(f"日志目录不存在：{args.logs}")
        return 2

    reports = collect_reports(args.logs)
    df = reports_to_dataframe(reports)
    if df.empty:
        print("未解析到任何 Device Report")
        return 3

    # 保存 CSV，便于后续进一步分析
    args.csv.parent.mkdir(parents=True, exist_ok=True)
    df.to_csv(args.csv, index=False, encoding="utf-8-sig")

    latest = df.iloc[-1]
    print("\n最新日志（按 Time 排序）：")
    print(_format_latest_summary(latest))

    title = "ESP32 WATER - Moisture & Wakeup Count Over Time"

    if args.export:
        fig = build_figure(df, title=title)
        args.out.parent.mkdir(parents=True, exist_ok=True)
        fig.savefig(args.out, dpi=160)
        print(f"\n已导出折线图：{args.out}")

    if args.no_ui:
        print(f"解析数据已导出：{args.csv}")
        return 0

    return _run_qt_ui(
        logs_dir=args.logs,
        title=title,
        repo_path=args.repo,
        remote_name=args.remote,
    )


if __name__ == "__main__":
    raise SystemExit(main())
