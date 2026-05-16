#!/usr/bin/env python3
from __future__ import annotations

import argparse
import atexit
import json
import math
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import tomllib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import numpy as np
from PIL import Image

# Make `scripts.wine.*` importable whether the harness is invoked as a script
# or as a module. Mirrors the sys.path bootstrap pattern used in
# scripts/wine/bootstrap_prefix.py so this file can be run directly.
_REPO_ROOT_GUESS = Path(__file__).resolve().parents[2]
if str(_REPO_ROOT_GUESS) not in sys.path:
    sys.path.insert(0, str(_REPO_ROOT_GUESS))

from scripts.wine.resolve import (  # noqa: E402
    ManifestError,
    WineEntry,
    load_manifest,
    resolve_wine_id,
)
from scripts.wine.bootstrap_prefix import (  # noqa: E402
    APPS_3RD_ROOT,
    PREFIXES_ROOT,
    BootstrapResult,
    bootstrap as bootstrap_prefix,
)
from scripts.tools.debug_artifact_bundle import (  # noqa: E402
    ArtifactBundleError,
    RenderCaptureBundle,
    merge_debug_sections,
)


REPO_ROOT = Path(__file__).resolve().parents[2]
CATALOGUE_PATH = REPO_ROOT / "experiments" / "CATALOGUE.toml"
DEFAULT_PE_BUILD_DIR = REPO_ROOT / "build-win32-x64-builtin" / "src" / "win32"
DEFAULT_RUNTIME_PE_BUILD_DIR = REPO_ROOT / "build-win32-x64-builtin" / "src" / "winemetal"
DEFAULT_WOW64_PE_BUILD_DIR = REPO_ROOT / "build-win32-x86-builtin" / "src" / "win32"
DEFAULT_WOW64_RUNTIME_PE_BUILD_DIR = REPO_ROOT / "build-win32-x86-builtin" / "src" / "winemetal"
DEFAULT_UNIX_BUILD_DIR = REPO_ROOT / "build-x86_64-builtin" / "src"
DEFAULT_OUTPUT_ROOT = REPO_ROOT / "experiments" / "output"
DEFAULT_TEMP_PREFIX_ROOT = REPO_ROOT / "tmp" / "prefixes"
DEFAULT_MINGW_BIN_DIR = Path.home() / "llvm-mingw" / "x86_64-w64-mingw32" / "bin"
DEFAULT_WOW64_MINGW_BIN_DIR = Path.home() / "llvm-mingw" / "i686-w64-mingw32" / "bin"
DEFAULT_MANIFEST_PATH = REPO_ROOT / "experiments" / "wine" / "manifest.toml"

SSIM_THRESHOLD = 0.90
BLACK_LUMA_THRESHOLD = 8.0
BLACK_VARIANCE_THRESHOLD = 16.0
FRAME_PATTERNS = (
    re.compile(r"OK: .*finished at frame\s+(\d+)", re.IGNORECASE),
    re.compile(r"OK: rendered frame\s+(\d+)", re.IGNORECASE),
)
PERF_COUNTER_PATTERN = re.compile(r"^\[dxmt9-perf\]\s+(.*)$")
BRIDGE_COUNTER_PATTERN = re.compile(r"^\[dxmt9-bridge-perf\]\s+(.*)$")
PE_RECORDER_COUNTER_PATTERN = re.compile(r"^\[dxmt9-device\]\s+(?:[A-Za-z]+:\s+)?pe_recorder_stats\s+(.*)$")
PERF_PROBE_PATTERN = re.compile(r"^\[perf-probe\]\s+(.*)$")
PERF_COUNTER_VALUE_PATTERN = re.compile(r"([A-Za-z0-9_]+)=([^\s}]+)")
_DRIVE_LETTER_RE = re.compile(r"^([A-Za-z]):[\\/](.*)$")
HARNESS_ENV_PREFIXES = ("DXMT", "DXMT9", "WINE")
HARNESS_ENV_KEYS = {"DYLD_LIBRARY_PATH"}


@dataclass
class ExperimentApp:
    name: str
    source: str
    license: str
    source_kind: str
    license_scope: str
    binary: str
    launcher: str
    reference: str
    features: list[str]
    status: str
    requires_wine: bool = True
    window_title: str | None = None
    capture_delay_sec: float = 3.0
    run_timeout_sec: float = 45.0
    capture_frame: int = 0
    reference_optional: bool = False
    allow_timeout: bool = False
    skip_stage: bool = False
    wine_dll_overrides: str | None = None
    cx_bottle: str | None = None
    build_script: str | None = None
    # Wine manifest integration (R-RT-6). When wine_id is set, the harness
    # resolves the wine root via experiments/wine/manifest.toml and bootstraps
    # experiments/prefixs/<name>/ instead of the legacy temp-prefix path.
    wine_id: str | None = None
    wine_alternatives: list[str] = field(default_factory=list)
    install_drive_letter: str = "d"
    # Optional probe-level expected-range gate. Each entry maps a counter
    # key (matching kCounterTable in src/dxmt9/dxmt9_perf_counters.cpp) to
    # an inclusive {min, max} range. Either bound may be omitted. Absent
    # counters are treated as 0 so min-bound checks still fire on a
    # regression to zero.
    expected_counters: dict[str, dict[str, float]] | None = None

    @classmethod
    def from_toml(cls, data: dict[str, Any]) -> "ExperimentApp":
        valid_source_kind = {
            "project-authored",
            "third-party-fixture",
            "structure-reference",
            "external-application",
        }
        valid_license_scope = {"project-mit", "third-party-fixture", "external-not-vendored"}
        source_kind = data["source_kind"]
        license_scope = data["license_scope"]
        if source_kind not in valid_source_kind:
            raise ValueError(f"{data.get('name', '<unknown>')}: invalid source_kind {source_kind!r}")
        if license_scope not in valid_license_scope:
            raise ValueError(f"{data.get('name', '<unknown>')}: invalid license_scope {license_scope!r}")
        if license_scope == "project-mit" and data["license"] != "mit":
            raise ValueError(f"{data.get('name', '<unknown>')}: project-mit entries must use license = 'mit'")
        return cls(
            name=data["name"],
            source=data["source"],
            license=data["license"],
            source_kind=source_kind,
            license_scope=license_scope,
            binary=data["binary"],
            launcher=data["launcher"],
            reference=data["reference"],
            features=list(data.get("features", [])),
            status=data.get("status", "untested"),
            requires_wine=bool(data.get("requires_wine", True)),
            window_title=data.get("window_title"),
            capture_delay_sec=float(data.get("capture_delay_sec", 3.0)),
            run_timeout_sec=float(data.get("run_timeout_sec", 45.0)),
            capture_frame=int(data.get("capture_frame", 0)),
            reference_optional=bool(data.get("reference_optional", False)),
            allow_timeout=bool(data.get("allow_timeout", False)),
            skip_stage=bool(data.get("skip_stage", False)),
            wine_dll_overrides=data.get("wine_dll_overrides"),
            cx_bottle=data.get("cx_bottle"),
            build_script=data.get("build_script"),
            wine_id=data.get("wine_id"),
            wine_alternatives=list(data.get("wine_alternatives") or []),
            install_drive_letter=data.get("install_drive_letter", "d"),
        )

    def attach_expected_counters(self, raw: dict[str, Any] | None) -> None:
        if not raw:
            self.expected_counters = None
            return
        cleaned: dict[str, dict[str, float]] = {}
        for key, bounds in raw.items():
            if not isinstance(bounds, dict):
                raise ValueError(
                    f"{self.name}: expected_counters[{key!r}] must be a table with min/max"
                )
            extracted: dict[str, float] = {}
            if "min" in bounds:
                extracted["min"] = float(bounds["min"])
            if "max" in bounds:
                extracted["max"] = float(bounds["max"])
            unknown = set(bounds.keys()) - {"min", "max"}
            if unknown:
                raise ValueError(
                    f"{self.name}: expected_counters[{key!r}] has unknown keys {sorted(unknown)!r}"
                )
            if not extracted:
                raise ValueError(
                    f"{self.name}: expected_counters[{key!r}] must specify at least one of min/max"
                )
            cleaned[key] = extracted
        self.expected_counters = cleaned or None

    @property
    def binary_is_windows_path(self) -> bool:
        """True when binary is a drive-letter path (e.g. 'D:/CAPCOM/...').

        Such paths only make sense for wild experiments where the apps_3rd
        install is junctioned into the prefix via dosdevices/<letter>:.
        """
        return bool(_DRIVE_LETTER_RE.match(self.binary))

    @property
    def binary_path(self) -> Path:
        """Local POSIX path to the binary on disk (for existence checks).

        For wild experiments using a drive-letter binary, this translates
        '<L>:/rest' to 'experiments/apps_3rd/<name>/rest', mirroring the
        dosdevices junction set up by scripts/wine/bootstrap_prefix.py.
        """
        m = _DRIVE_LETTER_RE.match(self.binary)
        if m:
            letter, rest = m.group(1), m.group(2)
            if letter.lower() != self.install_drive_letter.lower():
                raise ValueError(
                    f"{self.name}: binary drive letter {letter!r} != "
                    f"install_drive_letter {self.install_drive_letter!r}"
                )
            return REPO_ROOT / "experiments" / "apps_3rd" / self.name / rest.replace("\\", "/")
        return REPO_ROOT / self.binary

    @property
    def binary_for_wine(self) -> str:
        """Path string to hand to Wine.

        Windows-style for wild drive-letter installs (Wine resolves via
        dosdevices); the local POSIX path otherwise.
        """
        return self.binary if self.binary_is_windows_path else str(self.binary_path)

    @property
    def launcher_path(self) -> Path:
        return REPO_ROOT / self.launcher

    @property
    def reference_path(self) -> Path:
        return REPO_ROOT / self.reference

    @property
    def build_script_path(self) -> Path | None:
        if self.build_script is None:
            return None
        return REPO_ROOT / self.build_script


@dataclass(frozen=True)
class CaptureRequest:
    mode: str
    requested_frames: list[int]
    max_frames: int
    max_duration_sec: float
    max_bytes: int
    start_frame: int | None = None
    end_frame: int | None = None
    interval: int | None = None
    video_start_frame: int | None = None
    video_end_frame: int | None = None
    video_duration_sec: float | None = None
    video_acceptance: str = "triage"

    @property
    def primary_frame(self) -> int:
        return self.requested_frames[0] if self.requested_frames else 0

    @property
    def wants_multiple_frames(self) -> bool:
        return self.mode in {"frame-list", "interval-range"}

    @property
    def wants_video(self) -> bool:
        return self.video_duration_sec is not None or self.video_start_frame is not None


def load_catalogue(path: Path) -> list[ExperimentApp]:
    data = tomllib.loads(path.read_text())
    apps = [ExperimentApp.from_toml(item) for item in data.get("app", [])]
    # Optional sidecar table: [apps.<name>.expected_counters]. Matches by
    # `name`. Apps without a sidecar entry keep expected_counters=None and
    # bypass the gate entirely.
    sidecar = data.get("apps", {}) or {}
    if not isinstance(sidecar, dict):
        raise ValueError("[apps] must be a table keyed by app name")
    by_name = {app.name: app for app in apps}
    for app_name, app_cfg in sidecar.items():
        if app_name not in by_name:
            raise ValueError(f"[apps.{app_name}]: no matching [[app]] entry with name={app_name!r}")
        if not isinstance(app_cfg, dict):
            raise ValueError(f"[apps.{app_name}] must be a table")
        by_name[app_name].attach_expected_counters(app_cfg.get("expected_counters"))
    return apps


def detect_heroic_wine_root() -> Path | None:
    heroic_tools_root = Path.home() / "Library/Application Support/heroic/tools/wine"
    if not heroic_tools_root.is_dir():
        return None
    candidates = sorted(heroic_tools_root.glob("Wine-*"))
    plain = [candidate for candidate in candidates if "DXMT" not in candidate.name]
    preferred = plain if plain else candidates
    if not preferred:
        return None
    return preferred[-1] / "Contents/Resources/wine"


def resolve_wine_bin(wine_root: Path) -> Path:
    for relative in ("bin/wine", "bin/wine64"):
        candidate = wine_root / relative
        if candidate.exists():
            return candidate
    raise FileNotFoundError(f"wine executable not found under {wine_root}")


def run_command(cmd: list[str], **kwargs: Any) -> subprocess.CompletedProcess[str]:
    return subprocess.run(cmd, check=True, text=True, **kwargs)


def snapshot_harness_environment(env: dict[str, str]) -> dict[str, str]:
    return {
        key: env[key]
        for key in sorted(env)
        if env[key]
        and (
            key in HARNESS_ENV_KEYS
            or any(key.startswith(prefix) for prefix in HARNESS_ENV_PREFIXES)
        )
    }


def find_window_by_title(expected_title: str) -> dict[str, Any] | None:
    script = r"""
import Foundation
import CoreGraphics

let expected = CommandLine.arguments[1].lowercased()
let list = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID) as? [[String: Any]] ?? []
var matches: [[String: Any]] = []
for w in list {
    let owner = (w[kCGWindowOwnerName as String] as? String) ?? ""
    let name = (w[kCGWindowName as String] as? String) ?? ""
    let haystack = (owner + " " + name).lowercased()
    if !haystack.contains(expected) {
        continue
    }
    let bounds = (w[kCGWindowBounds as String] as? [String: Any]) ?? [:]
    let width = (bounds["Width"] as? Double) ?? 0
    let height = (bounds["Height"] as? Double) ?? 0
    if width < 64 || height < 64 {
        continue
    }
    let area = width * height
    matches.append([
        "window_id": w[kCGWindowNumber as String] ?? 0,
        "process_name": owner,
        "window_title": name,
        "x": Int((bounds["X"] as? Double) ?? 0),
        "y": Int((bounds["Y"] as? Double) ?? 0),
        "width": Int(width),
        "height": Int(height),
        "layer": w[kCGWindowLayer as String] ?? 0,
        "area": area,
    ])
}
if matches.isEmpty {
    print("null")
} else {
    let sorted = matches.sorted { lhs, rhs in
        let lArea = (lhs["area"] as? Double) ?? 0
        let rArea = (rhs["area"] as? Double) ?? 0
        if lArea != rArea { return lArea > rArea }
        let lLayer = (lhs["layer"] as? Int) ?? 0
        let rLayer = (rhs["layer"] as? Int) ?? 0
        return lLayer > rLayer
    }
    let best = sorted[0]
    let data = try! JSONSerialization.data(withJSONObject: best, options: [])
    print(String(data: data, encoding: .utf8)!)
}
"""
    result = run_command(["swift", "-e", script, expected_title], capture_output=True)
    payload = result.stdout.strip()
    if not payload or payload == "null":
        return None
    info = json.loads(payload)
    info.pop("area", None)
    return info


def capture_window_by_title(output_path: Path, expected_title: str, timeout_sec: float) -> dict[str, Any]:
    deadline = time.monotonic() + timeout_sec
    while time.monotonic() < deadline:
        info = find_window_by_title(expected_title)
        if info is not None:
            run_command(["screencapture", "-x", "-l", str(info["window_id"]), str(output_path)])
            info["capture_mode"] = "window_id"
            return info
        time.sleep(0.2)
    raise RuntimeError(f"unable to find onscreen window matching title: {expected_title}")


def capture_frontmost_window(output_path: Path, expected_title: str | None, timeout_sec: float) -> dict[str, Any]:
    script = """
        tell application "System Events"
            set procRef to first process whose frontmost is true
            set winRef to front window of procRef
            set posRef to position of winRef
            set sizeRef to size of winRef
            return (name of procRef as text) & linefeed & (name of winRef as text) & linefeed & (item 1 of posRef as text) & linefeed & (item 2 of posRef as text) & linefeed & (item 1 of sizeRef as text) & linefeed & (item 2 of sizeRef as text)
        end tell
    """
    deadline = time.monotonic() + timeout_sec
    last_error: str | None = None
    while time.monotonic() < deadline:
        try:
            result = run_command(["osascript", "-e", script], capture_output=True)
            lines = [line.strip() for line in result.stdout.splitlines() if line.strip()]
            if len(lines) != 6:
                last_error = f"unexpected osascript output: {lines!r}"
                time.sleep(0.2)
                continue
            info = {
                "process_name": lines[0],
                "window_title": lines[1],
                "x": max(0, int(float(lines[2]))),
                "y": max(0, int(float(lines[3]))),
                "width": max(1, int(float(lines[4]))),
                "height": max(1, int(float(lines[5]))),
            }
            if expected_title and expected_title.lower() not in info["window_title"].lower():
                time.sleep(0.2)
                continue
            rect = f"{info['x']},{info['y']},{info['width']},{info['height']}"
            run_command(["screencapture", "-x", "-R", rect, str(output_path)])
            info["capture_mode"] = "frontmost_window"
            return info
        except subprocess.CalledProcessError as exc:
            last_error = exc.stderr.strip() or exc.stdout.strip() or str(exc)
            time.sleep(0.2)
    raise RuntimeError(last_error or "unable to capture frontmost window")


def capture_full_screen(output_path: Path) -> dict[str, Any]:
    run_command(["screencapture", "-x", str(output_path)])
    return {"capture_mode": "full_screen"}


def capture_window_or_fallback(
    output_path: Path,
    *,
    expected_title: str | None,
    timeout_sec: float,
) -> tuple[dict[str, Any] | None, str | None]:
    try:
        if expected_title:
            return capture_window_by_title(output_path, expected_title, timeout_sec=timeout_sec), None
        return capture_frontmost_window(output_path, expected_title, timeout_sec=timeout_sec), None
    except Exception as exc:  # noqa: BLE001
        try:
            return capture_full_screen(output_path), str(exc)
        except Exception as fallback_exc:  # noqa: BLE001
            return None, f"{exc}; fullscreen fallback failed: {fallback_exc}"


def record_bounded_video(
    output_path: Path,
    *,
    duration_sec: float,
) -> tuple[Path | None, str | None]:
    try:
        run_command([
            "screencapture",
            "-x",
            "-v",
            "-V",
            str(max(1, math.ceil(duration_sec))),
            str(output_path),
        ])
        return output_path if output_path.exists() else None, None
    except Exception as exc:  # noqa: BLE001
        return None, str(exc)


def image_luma_metrics(path: Path) -> dict[str, float]:
    image = Image.open(path).convert("RGB")
    arr = np.asarray(image, dtype=np.float32)
    luma = 0.2126 * arr[:, :, 0] + 0.7152 * arr[:, :, 1] + 0.0722 * arr[:, :, 2]
    return {
        "mean_luma": float(np.mean(luma)),
        "variance": float(np.var(luma)),
    }


def is_black_screen(metrics: dict[str, float]) -> bool:
    return (
        metrics["mean_luma"] <= BLACK_LUMA_THRESHOLD
        and metrics["variance"] <= BLACK_VARIANCE_THRESHOLD
    )


def effective_capture_frame(app: ExperimentApp, args: argparse.Namespace) -> int:
    override = getattr(args, "capture_frame", None)
    if override is not None:
        return int(override)
    return app.capture_frame


def _parse_positive_int(value: str, label: str) -> int:
    try:
        parsed = int(value)
    except ValueError as exc:
        raise ValueError(f"{label} must be an integer: {value!r}") from exc
    if parsed < 0:
        raise ValueError(f"{label} must be >= 0: {value!r}")
    return parsed


def parse_capture_frames(value: str) -> list[int]:
    frames: list[int] = []
    for item in value.split(","):
        text = item.strip()
        if not text:
            continue
        frames.append(_parse_positive_int(text, "capture frame"))
    if not frames:
        raise ValueError("--capture-frames must include at least one frame")
    return frames


def parse_capture_range(value: str) -> tuple[int, int, int, list[int]]:
    parts = [part.strip() for part in value.split(":")]
    if len(parts) not in {2, 3}:
        raise ValueError("--capture-range must use START:END[:INTERVAL]")
    start = _parse_positive_int(parts[0], "capture range start")
    end = _parse_positive_int(parts[1], "capture range end")
    interval = _parse_positive_int(parts[2], "capture range interval") if len(parts) == 3 else 1
    if end < start:
        raise ValueError("--capture-range END must be >= START")
    if interval <= 0:
        raise ValueError("--capture-range INTERVAL must be > 0")
    return start, end, interval, list(range(start, end + 1, interval))


def parse_capture_video(value: str) -> tuple[int | None, int | None, float]:
    text = value.strip().lower()
    if not text:
        raise ValueError("--capture-video must not be empty")
    if text.endswith("s"):
        duration = float(text[:-1])
        if duration <= 0.0:
            raise ValueError("--capture-video duration must be > 0")
        return None, None, duration
    if ":" in text:
        start_text, end_text = text.split(":", 1)
        start = _parse_positive_int(start_text, "capture video start")
        end = _parse_positive_int(end_text, "capture video end")
        if end < start:
            raise ValueError("--capture-video END must be >= START")
        # Frame spans are still bounded by max_duration_sec at recording time.
        return start, end, 0.0
    duration = float(text)
    if duration <= 0.0:
        raise ValueError("--capture-video duration must be > 0")
    return None, None, duration


def build_capture_request(app: ExperimentApp, args: argparse.Namespace) -> CaptureRequest:
    max_frames = int(getattr(args, "capture_max_frames", 64) or 64)
    max_duration_sec = float(getattr(args, "capture_max_seconds", 5.0) or 5.0)
    max_bytes = int(getattr(args, "capture_max_bytes", 64 * 1024 * 1024) or 64 * 1024 * 1024)
    if max_frames <= 0:
        raise ValueError("--capture-max-frames must be > 0")
    if max_duration_sec <= 0.0:
        raise ValueError("--capture-max-seconds must be > 0")
    if max_bytes <= 0:
        raise ValueError("--capture-max-bytes must be > 0")

    capture_frames = getattr(args, "capture_frames", None)
    capture_range = getattr(args, "capture_range", None)
    if capture_frames and capture_range:
        raise ValueError("--capture-frames and --capture-range are mutually exclusive")

    start_frame = None
    end_frame = None
    interval = None
    if capture_range:
        start_frame, end_frame, interval, requested = parse_capture_range(capture_range)
        mode = "interval-range"
    elif capture_frames:
        requested = parse_capture_frames(capture_frames)
        mode = "frame-list"
    else:
        requested = [effective_capture_frame(app, args)]
        mode = "single-frame"

    if len(requested) > max_frames:
        requested = requested[:max_frames]

    video_start_frame = None
    video_end_frame = None
    video_duration_sec = None
    capture_video = getattr(args, "capture_video", None)
    if capture_video:
        video_start_frame, video_end_frame, parsed_duration = parse_capture_video(capture_video)
        video_duration_sec = parsed_duration if parsed_duration > 0.0 else max_duration_sec
        video_duration_sec = min(video_duration_sec, max_duration_sec)

    return CaptureRequest(
        mode=mode,
        requested_frames=requested,
        max_frames=max_frames,
        max_duration_sec=max_duration_sec,
        max_bytes=max_bytes,
        start_frame=start_frame,
        end_frame=end_frame,
        interval=interval,
        video_start_frame=video_start_frame,
        video_end_frame=video_end_frame,
        video_duration_sec=video_duration_sec,
        video_acceptance=getattr(args, "capture_video_acceptance", "triage"),
    )


def classify_capture_source(
    actual_dump_path: Path,
    actual_path: Path,
    window_info: dict[str, Any] | None,
) -> str | None:
    if actual_dump_path.exists():
        return "internal_dump"
    if window_info:
        mode = window_info.get("capture_mode")
        if mode == "window_id":
            return "window_capture"
        if mode == "frontmost_window":
            return "frontmost_window"
        if mode == "full_screen":
            return "full_screen"
    if actual_path.exists():
        return "external_capture"
    return None


def debug_capture_source(capture_source: str | None) -> str:
    if capture_source in {"internal_dump", "window_id", "frontmost_window", "full_screen", "manual_observation", "none", "unavailable"}:
        return capture_source
    if capture_source == "window_capture":
        return "window_id"
    if capture_source == "external_capture":
        return "manual_observation"
    if capture_source in {"internal_dump", "full_screen"}:
        return capture_source
    return "none"


def relative_artifact(path: Path, root: Path) -> str:
    try:
        return path.relative_to(root).as_posix()
    except ValueError:
        return path.as_posix()


def capture_request_environment(request: CaptureRequest, output_dir: Path, actual_dump_path: Path) -> dict[str, str]:
    values = {
        "DXMT_CAPTURE_FRAME": str(request.primary_frame),
        "DXMT_EXPERIMENT_CAPTURE_PATH": str(actual_dump_path),
    }
    if request.wants_multiple_frames:
        values["DXMT_CAPTURE_FRAMES"] = ",".join(str(frame) for frame in request.requested_frames)
        values["DXMT_EXPERIMENT_CAPTURE_DIR"] = str(output_dir / "internal_frames")
    if request.mode == "interval-range":
        values["DXMT_CAPTURE_RANGE"] = f"{request.start_frame}:{request.end_frame}:{request.interval}"
    return values


def internal_frame_candidates(output_dir: Path, frame_id: int) -> list[Path]:
    return [
        output_dir / "internal_frames" / f"frame{frame_id:06d}.png",
        output_dir / "internal_frames" / f"frame{frame_id:06d}.bmp",
        output_dir / "internal_frames" / f"frame_{frame_id:06d}.png",
        output_dir / "internal_frames" / f"frame_{frame_id:06d}.bmp",
        output_dir / f"actual_frame{frame_id:06d}.png",
        output_dir / f"actual_frame{frame_id:06d}.bmp",
    ]


def internal_frame_skip_candidates(output_dir: Path, frame_id: int) -> list[Path]:
    return [
        path.with_suffix(path.suffix + ".skipped.json")
        for path in internal_frame_candidates(output_dir, frame_id)
    ]


def first_existing_path(paths: list[Path]) -> Path | None:
    return next((path for path in paths if path.exists()), None)


def load_capture_skip_sidecar(path: Path) -> dict[str, Any] | None:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return None
    return data if isinstance(data, dict) else None


def collect_capture_frame_records(
    *,
    capture_request: CaptureRequest,
    output_dir: Path,
    actual_dump_path: Path,
    actual_path: Path,
    scheduled_window_frames: dict[int, dict[str, Any]],
    capture_source: str | None,
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    captured_frames: list[dict[str, Any]] = []
    dropped_frames: list[dict[str, Any]] = []
    for frame_id in capture_request.requested_frames:
        internal_frame = first_existing_path(internal_frame_candidates(output_dir, frame_id))
        if internal_frame is not None:
            captured_frames.append({
                "frame_id": frame_id,
                "path": internal_frame,
                "source": "internal_dump",
            })
            continue
        if frame_id == capture_request.primary_frame and actual_dump_path.exists():
            captured_frames.append({
                "frame_id": frame_id,
                "path": actual_path,
                "source": "internal_dump",
            })
            continue
        scheduled = scheduled_window_frames.get(frame_id)
        if scheduled is not None and Path(scheduled["path"]).exists():
            captured_frames.append({
                "frame_id": frame_id,
                "path": scheduled["path"],
                "source": scheduled.get("source", "window_capture"),
            })
            continue
        if frame_id == capture_request.primary_frame and actual_path.exists() and capture_source:
            captured_frames.append({
                "frame_id": frame_id,
                "path": actual_path,
                "source": capture_source,
            })
            continue
        sidecar = first_existing_path(internal_frame_skip_candidates(output_dir, frame_id))
        sidecar_payload = load_capture_skip_sidecar(sidecar) if sidecar is not None else None
        if sidecar is not None and sidecar_payload is not None:
            dropped_frames.append({
                "frame_id": frame_id,
                "source": sidecar_payload.get("source", "internal_dump"),
                "reason": sidecar_payload.get("reason", "runtime skipped internal capture"),
                "sidecar_path": str(sidecar),
                "counters": sidecar_payload.get("counters"),
            })
            continue
        dropped_frames.append({
            "frame_id": frame_id,
            "source": "internal_dump" if capture_request.wants_multiple_frames else "none",
            "reason": "requested frame was not emitted by internal dump or window capture",
        })
    return captured_frames, dropped_frames


def write_experiment_debug_result(
    *,
    app: ExperimentApp,
    output_dir: Path,
    debug_result_path: Path,
    log_path: Path,
    actual_path: Path,
    command: list[str],
    environment: dict[str, str],
    capture_frame: int,
    capture_source: str | None,
    capture_error: str | None,
    window_info: dict[str, Any] | None,
    capture_request: CaptureRequest | None = None,
    captured_frames: list[dict[str, Any]] | None = None,
    dropped_frames: list[dict[str, Any]] | None = None,
    video_path: Path | None = None,
    video_error: str | None = None,
) -> Path | None:
    captured_frames = list(captured_frames or [])
    dropped_frames = list(dropped_frames or [])
    has_video = video_path is not None and video_path.exists()
    if not actual_path.exists() and not captured_frames and not has_video:
        return None

    request = capture_request or CaptureRequest(
        mode="single-frame",
        requested_frames=[capture_frame],
        max_frames=1,
        max_duration_sec=5.0,
        max_bytes=64 * 1024 * 1024,
    )
    run_id = f"{app.name}-{request.mode}-{request.primary_frame}"
    source = debug_capture_source(capture_source)
    bundle = RenderCaptureBundle(
        output_dir,
        run_id=run_id,
        mode=request.mode,
        source=source,
        frame_id=request.primary_frame if request.mode == "single-frame" else None,
        start_frame=request.start_frame,
        end_frame=request.end_frame,
        interval=request.interval,
        requested_frames=request.requested_frames if request.mode == "frame-list" else None,
        max_bytes=request.max_bytes,
    )
    if actual_path.exists() and not captured_frames:
        captured_frames.append({
            "frame_id": request.primary_frame,
            "path": actual_path,
            "source": source,
        })
    captured_frame_ids = {int(frame["frame_id"]) for frame in captured_frames}
    dropped_frames = [
        frame for frame in dropped_frames
        if int(frame["frame_id"]) not in captured_frame_ids
    ]
    seen_frames: set[int] = set()
    for frame in captured_frames:
        frame_id = int(frame["frame_id"])
        if frame_id in seen_frames:
            continue
        seen_frames.add(frame_id)
        bundle.copy_frame(
            Path(frame["path"]),
            frame_id=frame_id,
            capture_source=debug_capture_source(frame.get("source")) if frame.get("source") else source,
            name=f"{debug_capture_source(frame.get('source')) or source}_frame{frame_id:06d}.png",
        )
    for frame in dropped_frames:
        sidecar_path = frame.get("sidecar_path")
        sidecar_rel = relative_artifact(Path(sidecar_path), output_dir) if sidecar_path else None
        bundle.add_dropped_frame(
            frame_id=int(frame["frame_id"]),
            source=debug_capture_source(frame.get("source")) if frame.get("source") else source,
            reason=str(frame["reason"]),
            sidecar_path=sidecar_rel,
            counters=frame.get("counters") if isinstance(frame.get("counters"), dict) else None,
        )
    if has_video:
        bundle.add_video_segment(
            video_path,  # type: ignore[arg-type]
            source="full_screen",
            container=video_path.suffix.lstrip(".") or "mov",  # type: ignore[union-attr]
            codec="system",
            timebase="frame" if request.video_start_frame is not None else "time",
            nominal_fps=60.0,
            width=1,
            height=1,
            start_frame=request.video_start_frame,
            end_frame=request.video_end_frame,
            start_time=0.0 if request.video_start_frame is None else None,
            end_time=request.video_duration_sec if request.video_start_frame is None else None,
            acceptance=request.video_acceptance,
        )
    frame_manifest = bundle.write_frame_manifest()

    artifacts: list[dict[str, Any]] = []
    if log_path.exists():
        artifacts.append({
            "role": "log",
            "path": relative_artifact(log_path, output_dir),
            "format": "text",
        })
    artifacts.append(frame_manifest)
    artifacts.extend(bundle.video_artifacts())
    for frame in dropped_frames:
        sidecar_path = frame.get("sidecar_path")
        if not sidecar_path:
            continue
        path = Path(sidecar_path)
        if path.exists():
            artifacts.append({
                "role": "log",
                "path": relative_artifact(path, output_dir),
                "format": "json",
            })

    diagnostics = merge_debug_sections(bundle.render_capture_diagnostics(), bundle.video_diagnostics())
    if window_info or capture_error:
        capture_mode = window_info.get("capture_mode") if window_info else None
        diagnostics["wsi"] = {
            "layer_acquisition": "unavailable",
            "capture_source": source,
            "visible_output_proves_layer": False if source == "full_screen" else None,
        }
        if window_info:
            if "window_id" in window_info:
                diagnostics["wsi"]["hwnd"] = str(window_info["window_id"])
            if "window_title" in window_info:
                diagnostics["wsi"]["window_title"] = str(window_info["window_title"])
            elif app.window_title:
                diagnostics["wsi"]["window_title"] = app.window_title
            else:
                diagnostics["wsi"]["failure_reason"] = "window identity unavailable"
        else:
            diagnostics["wsi"]["failure_reason"] = capture_error or "capture unavailable"
        diagnostics["wsi"] = {key: value for key, value in diagnostics["wsi"].items() if value is not None}

    result = {
        "schema": "dxmt9.debug.result.v1",
        "module": "experiment",
        "boundary": "render-capture",
        "command": command,
        "correlation": {
            "run_id": run_id,
            "frame_id": capture_frame,
        },
        "environment": environment,
        "limits": {
            "max_frames": 1,
            "max_duration_sec": request.max_duration_sec,
            "max_bytes": request.max_bytes,
        },
        "artifacts": artifacts,
        "diagnostics": diagnostics,
        "failure_category": (
            "render-video-segment"
            if video_error
            else "render-frame-sequence"
            if dropped_frames
            else "none"
        ),
    }
    debug_result_path.write_text(json.dumps(result, indent=2, sort_keys=True))
    return debug_result_path


def compute_ssim(actual_path: Path, reference_path: Path) -> float:
    actual = Image.open(actual_path).convert("L")
    reference = Image.open(reference_path).convert("L")
    if actual.size != reference.size:
        raise ValueError(f"image size mismatch: {actual.size} vs {reference.size}")
    x = np.asarray(actual, dtype=np.float64)
    y = np.asarray(reference, dtype=np.float64)
    mu_x = float(np.mean(x))
    mu_y = float(np.mean(y))
    sigma_x = float(np.var(x))
    sigma_y = float(np.var(y))
    sigma_xy = float(np.mean((x - mu_x) * (y - mu_y)))
    c1 = (0.01 * 255.0) ** 2
    c2 = (0.03 * 255.0) ** 2
    numerator = (2.0 * mu_x * mu_y + c1) * (2.0 * sigma_xy + c2)
    denominator = (mu_x * mu_x + mu_y * mu_y + c1) * (sigma_x + sigma_y + c2)
    if denominator == 0.0:
        return 1.0
    return float(numerator / denominator)


def write_diff_image(actual_path: Path, reference_path: Path, output_path: Path) -> None:
    actual = Image.open(actual_path).convert("RGB")
    reference = Image.open(reference_path).convert("RGB")
    if actual.size != reference.size:
        canvas = Image.new("RGB", (actual.width + reference.width, max(actual.height, reference.height)))
        canvas.paste(reference, (0, 0))
        canvas.paste(actual, (reference.width, 0))
        canvas.save(output_path)
        return
    actual_arr = np.asarray(actual, dtype=np.int16)
    reference_arr = np.asarray(reference, dtype=np.int16)
    delta = np.abs(actual_arr - reference_arr).astype(np.uint8)
    heat = np.zeros_like(delta)
    heat[:, :, 0] = np.max(delta, axis=2)
    heat[:, :, 1] = np.max(delta, axis=2) // 4
    heat[:, :, 2] = np.max(delta, axis=2) // 4
    blended = np.clip(0.6 * actual_arr + 0.8 * heat, 0, 255).astype(np.uint8)
    Image.fromarray(blended, mode="RGB").save(output_path)


def scan_log_for_failures(log_path: Path) -> list[str]:
    markers = [
        "DXMT_ASSERT",
        "Assertion failed",
        "Metal API Validation",
        "validation error",
    ]
    if not log_path.exists():
        return []
    content = log_path.read_text(errors="ignore")
    return [marker for marker in markers if marker in content]


def stage_dxmt9(
    prefix: Path,
    wine_root: Path | None,
    pe_build_dir: Path,
    runtime_pe_build_dir: Path,
    unix_build_dir: Path,
    wow64_pe_build_dir: Path | None = None,
    wow64_runtime_pe_build_dir: Path | None = None,
) -> None:
    cmd = [
        "bash",
        str(REPO_ROOT / "scripts" / "install" / "install_heroic_wine.sh"),
        "--prefix",
        str(prefix),
        "--pe-build-dir",
        str(pe_build_dir),
        "--runtime-pe-build-dir",
        str(runtime_pe_build_dir),
        "--unix-build-dir",
        str(unix_build_dir),
    ]
    if wow64_pe_build_dir is not None:
        cmd.extend(["--wow64-pe-build-dir", str(wow64_pe_build_dir)])
    if wow64_runtime_pe_build_dir is not None:
        cmd.extend(["--wow64-runtime-pe-build-dir", str(wow64_runtime_pe_build_dir)])
    if wine_root is not None:
        cmd.extend(["--wine-root", str(wine_root)])
    run_command(cmd, cwd=REPO_ROOT)


def stage_mingw_runtime(prefix: Path, mingw_bin_dir: Path, wow64_mingw_bin_dir: Path) -> None:
    targets = (
        (mingw_bin_dir / "libc++.dll", prefix / "drive_c/windows/system32/libc++.dll"),
        (mingw_bin_dir / "libunwind.dll", prefix / "drive_c/windows/system32/libunwind.dll"),
        (wow64_mingw_bin_dir / "libc++.dll", prefix / "drive_c/windows/syswow64/libc++.dll"),
        (wow64_mingw_bin_dir / "libunwind.dll", prefix / "drive_c/windows/syswow64/libunwind.dll"),
    )
    for source, target in targets:
        if not source.exists():
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        print(f"installed {source} -> {target}")


def extract_frame_count(log_path: Path) -> int | None:
    if not log_path.exists():
        return None
    max_frame = 0
    for line in log_path.read_text(errors="replace").splitlines():
        for pattern in FRAME_PATTERNS:
            match = pattern.search(line)
            if match:
                max_frame = max(max_frame, int(match.group(1)))
    return max_frame or None


def parse_perf_counter_value(raw: str) -> int | float | str:
    try:
        if "." in raw or "e" in raw.lower():
            return float(raw)
        return int(raw, 10)
    except ValueError:
        return raw


def extract_dxmt9_perf_counters(log_path: Path) -> dict[str, int | float | str]:
    if not log_path.exists():
        return {}
    counters: dict[str, int | float | str] = {}
    for line in log_path.read_text(errors="replace").splitlines():
        match = PERF_COUNTER_PATTERN.match(line)
        if not match:
            continue
        for key, value in PERF_COUNTER_VALUE_PATTERN.findall(match.group(1)):
            counters[key] = parse_perf_counter_value(value)
    return counters


def extract_dxmt9_bridge_counters(log_path: Path) -> dict[str, int | float | str]:
    if not log_path.exists():
        return {}
    counters: dict[str, int | float | str] = {}
    for line in log_path.read_text(errors="replace").splitlines():
        match = BRIDGE_COUNTER_PATTERN.match(line)
        if not match:
            continue
        for key, value in PERF_COUNTER_VALUE_PATTERN.findall(match.group(1)):
            counters[key] = parse_perf_counter_value(value)
    return counters


def extract_dxmt9_pe_recorder_counters(log_path: Path) -> dict[str, int | float | str]:
    if not log_path.exists():
        return {}
    counters: dict[str, int | float | str] = {}
    for line in log_path.read_text(errors="replace").splitlines():
        match = PE_RECORDER_COUNTER_PATTERN.match(line)
        if not match:
            continue
        for key, value in PERF_COUNTER_VALUE_PATTERN.findall(match.group(1)):
            counters[key] = parse_perf_counter_value(value)
    return counters


def extract_perf_probe_timings(log_path: Path) -> dict[str, int | float | str]:
    if not log_path.exists():
        return {}
    timings: dict[str, int | float | str] = {}
    for line in log_path.read_text(errors="replace").splitlines():
        match = PERF_PROBE_PATTERN.match(line)
        if not match:
            continue
        for key, value in PERF_COUNTER_VALUE_PATTERN.findall(match.group(1)):
            timings[key] = parse_perf_counter_value(value)
    return timings


def evaluate_counter_ranges(
    expected: dict[str, dict[str, float]] | None,
    counters: dict[str, int | float | str],
) -> list[dict[str, Any]]:
    """Compare counters against per-app expected ranges.

    Returns a list of violation records. Counters absent from `counters`
    are treated as 0; this lets a `min` bound flag a regression to zero
    even when the counter was never emitted (e.g. a probe that stopped
    issuing draws). Non-numeric counter values are skipped so that string
    values cannot crash the gate.
    """
    if not expected:
        return []
    violations: list[dict[str, Any]] = []
    for key, bounds in expected.items():
        raw = counters.get(key, 0)
        if isinstance(raw, bool) or not isinstance(raw, (int, float)):
            # Skip non-numeric counters; record as a violation so the gap
            # is visible rather than silently passing.
            violations.append({
                "key": key,
                "expected": dict(bounds),
                "actual": raw,
                "reason": "non_numeric",
            })
            continue
        actual = float(raw)
        out_of_range = False
        if "min" in bounds and actual < bounds["min"]:
            out_of_range = True
        if "max" in bounds and actual > bounds["max"]:
            out_of_range = True
        if out_of_range:
            violations.append({
                "key": key,
                "expected": dict(bounds),
                "actual": raw,
            })
    return violations


def terminate_process_group(process: subprocess.Popen[str] | None, sig: signal.Signals) -> None:
    if process is None or process.poll() is not None:
        return
    try:
        os.killpg(process.pid, sig)
    except ProcessLookupError:
        pass


def create_temp_prefix(app_name: str) -> Path:
    prefix_name = f"dxmt9-exp-{app_name}-"
    try:
        DEFAULT_TEMP_PREFIX_ROOT.mkdir(parents=True, exist_ok=True)
        return Path(tempfile.mkdtemp(prefix=prefix_name, dir=DEFAULT_TEMP_PREFIX_ROOT))
    except OSError:
        return Path(tempfile.mkdtemp(prefix=prefix_name))


def run_experiment(app: ExperimentApp, args: argparse.Namespace) -> int:
    if not app.launcher_path.exists():
        raise FileNotFoundError(f"launcher not found: {app.launcher_path}")
    binary_path = Path(args.binary).expanduser().resolve() if args.binary else app.binary_path
    if not binary_path.exists():
        raise FileNotFoundError(f"binary not found: {binary_path}")

    output_name = app.name
    if getattr(args, "output_suffix", None):
        output_name = f"{app.name}-{args.output_suffix}"
    output_dir = DEFAULT_OUTPUT_ROOT / output_name
    output_dir.mkdir(parents=True, exist_ok=True)
    actual_path = output_dir / "actual.png"
    actual_dump_path = output_dir / "actual.bmp"
    reference_link_path = output_dir / "reference.png"
    diff_path = output_dir / "diff.png"
    ssim_path = output_dir / "ssim.txt"
    log_path = output_dir / "dxmt9.log"
    result_path = output_dir / "result.json"
    debug_result_path = output_dir / "debug_result.json"
    for path in (
        actual_path,
        actual_dump_path,
        diff_path,
        ssim_path,
        reference_link_path,
        log_path,
        result_path,
        debug_result_path,
    ):
        if path.exists() or path.is_symlink():
            path.unlink()
    for directory in (output_dir / "frames", output_dir / "video", output_dir / "boundary_dumps"):
        if directory.is_dir():
            shutil.rmtree(directory)

    # Resolve a manifest entry first when the app or CLI requests one. This
    # implements R-RT-6.1 (CLI > env > CATALOGUE) and feeds both the prefix
    # selection (R-RT-4) and the wine_root/wine_bin resolution below.
    manifest_entry: WineEntry | None = None
    manifest_source: str | None = None
    cli_wine_id = getattr(args, "wine_id", None)
    if app.wine_id or cli_wine_id:
        manifest_path = getattr(args, "wine_manifest", None) or DEFAULT_MANIFEST_PATH
        try:
            entries = load_manifest(manifest_path)
            manifest_entry, manifest_source = resolve_wine_id(
                entries=entries,
                cli_arg=cli_wine_id,
                env_var=os.environ.get("DXMT_EXPERIMENT_WINE_ID"),
                catalogue_value=app.wine_id,
                app_name=app.name,
            )
        except ManifestError as exc:
            print(f"[runtime] manifest error: {exc}", file=sys.stderr)
            sys.exit(2)
        print(
            f"[runtime] wine resolved via {manifest_source}: id={manifest_entry.id} "
            f"path={manifest_entry.path}",
            file=sys.stderr,
        )
        # R-RT-6.3: warn when a non-vanilla variant is selected without an
        # explicit alternatives entry or --allow-non-vanilla override.
        if manifest_entry.variant != "vanilla":
            allow = getattr(args, "allow_non_vanilla", False)
            if manifest_entry.id not in app.wine_alternatives and not allow:
                print(
                    f"[runtime] WARNING: wine variant={manifest_entry.variant!r} "
                    f"is not vanilla and id {manifest_entry.id!r} is not in "
                    f"[[{app.name}]].wine_alternatives. "
                    f"See agents/rules/test_wild.rules.md.",
                    file=sys.stderr,
                )

    # Prefix selection. When a manifest entry was resolved, we use the
    # spec-defined experiments/prefixs/<name>/ path and bootstrap on demand
    # (R-RT-4). Otherwise the legacy temp-prefix logic is preserved exactly.
    prefix_bootstrap_payload: dict[str, Any] = {
        "ran": False,
        "mmap_errors": 0,
        "degraded": False,
    }
    if manifest_entry is not None:
        if args.prefix:
            prefix = Path(args.prefix).expanduser().resolve()
        else:
            prefix = PREFIXES_ROOT / app.name
        temp_prefix = False
        rebuild_prefix = getattr(args, "rebuild_prefix", False)
        needs_bootstrap = rebuild_prefix or not (prefix / "system.reg").exists()
        if needs_bootstrap and not args.prefix:
            boot: BootstrapResult = bootstrap_prefix(
                name=app.name,
                wine=manifest_entry,
                drive_letter=app.install_drive_letter,
                rebuild=rebuild_prefix,
            )
            prefix_bootstrap_payload = {
                "ran": True,
                "mmap_errors": boot.mmap_errors,
                "degraded": boot.degraded,
            }
            print(
                f"[runtime] prefix bootstrap: mmap_errors={boot.mmap_errors}"
                f"{' (DEGRADED)' if boot.degraded else ''}",
                file=sys.stderr,
            )
    elif args.prefix:
        prefix = Path(args.prefix).expanduser().resolve()
        temp_prefix = False
    else:
        prefix = create_temp_prefix(app.name)
        temp_prefix = True

    cleanup_enabled = temp_prefix and args.cleanup_temp_prefix
    cleanup_done = False
    process: subprocess.Popen[str] | None = None
    capture_error: str | None = None
    window_info: dict[str, Any] | None = None
    scheduled_window_frames: dict[int, dict[str, Any]] = {}
    video_path: Path | None = None
    video_error: str | None = None
    timed_out = False
    process_started_at: float | None = None
    process_finished_at: float | None = None
    previous_signal_handlers: dict[signal.Signals, Any] = {}

    def cleanup_temp_prefix() -> None:
        nonlocal cleanup_done
        if cleanup_done or not cleanup_enabled:
            return
        shutil.rmtree(prefix, ignore_errors=True)
        cleanup_done = True

    def handle_termination(signum: int, _frame: Any) -> None:
        terminate_process_group(process, signal.SIGTERM)
        cleanup_temp_prefix()
        raise SystemExit(128 + signum)

    if cleanup_enabled:
        atexit.register(cleanup_temp_prefix)
        for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            previous_signal_handlers[sig] = signal.getsignal(sig)
            signal.signal(sig, handle_termination)

    # Wine root / bin resolution. Manifest entries take precedence; legacy
    # apps fall back to the existing detect_heroic_wine_root() / resolve_wine_bin()
    # path so no fixture-side change is required.
    if manifest_entry is not None:
        wine_root = manifest_entry.path
    else:
        wine_root = (
            Path(args.wine_root).expanduser().resolve()
            if args.wine_root
            else detect_heroic_wine_root()
        )
    try:
        if app.requires_wine and wine_root is None and not args.wine_bin:
            raise FileNotFoundError("no Wine root supplied and Heroic runtime auto-detect failed")
        if manifest_entry is not None and not args.wine_bin:
            wine_bin = manifest_entry.wine_bin()
        else:
            wine_bin = (
                Path(args.wine_bin).expanduser().resolve()
                if args.wine_bin
                else resolve_wine_bin(wine_root)  # type: ignore[arg-type]
            )
        pe_build_dir = Path(args.pe_build_dir).expanduser().resolve() if args.pe_build_dir else DEFAULT_PE_BUILD_DIR
        runtime_pe_build_dir = (
            Path(args.runtime_pe_build_dir).expanduser().resolve()
            if args.runtime_pe_build_dir
            else DEFAULT_RUNTIME_PE_BUILD_DIR
        )
        unix_build_dir = Path(args.unix_build_dir).expanduser().resolve() if args.unix_build_dir else DEFAULT_UNIX_BUILD_DIR
        wow64_pe_build_dir = (
            Path(args.wow64_pe_build_dir).expanduser().resolve()
            if args.wow64_pe_build_dir
            else DEFAULT_WOW64_PE_BUILD_DIR
        )
        wow64_runtime_pe_build_dir = (
            Path(args.wow64_runtime_pe_build_dir).expanduser().resolve()
            if args.wow64_runtime_pe_build_dir
            else DEFAULT_WOW64_RUNTIME_PE_BUILD_DIR
        )
        mingw_bin_dir = Path(args.mingw_bin_dir).expanduser().resolve() if args.mingw_bin_dir else DEFAULT_MINGW_BIN_DIR
        wow64_mingw_bin_dir = (
            Path(args.wow64_mingw_bin_dir).expanduser().resolve()
            if args.wow64_mingw_bin_dir
            else DEFAULT_WOW64_MINGW_BIN_DIR
        )

        skip_stage = app.skip_stage or args.skip_stage
        if not skip_stage:
            stage_dxmt9(
                prefix,
                wine_root,
                pe_build_dir,
                runtime_pe_build_dir,
                unix_build_dir,
                wow64_pe_build_dir,
                wow64_runtime_pe_build_dir,
            )
        elif args.stage_mingw_runtime:
            stage_mingw_runtime(prefix, mingw_bin_dir, wow64_mingw_bin_dir)

        capture_request = build_capture_request(app, args)
        capture_frame = capture_request.primary_frame
        internal_capture_dir = output_dir / "internal_frames"
        if capture_request.wants_multiple_frames:
            internal_capture_dir.mkdir(parents=True, exist_ok=True)
        env = os.environ.copy()
        env.update(
            {
                "DXMT_EXPERIMENT_NAME": app.name,
                "DXMT_EXPERIMENT_BINARY": (
                    args.binary if args.binary else app.binary_for_wine
                ),
                "DXMT_EXPERIMENT_PREFIX": str(prefix),
                "DXMT_EXPERIMENT_WINE_ROOT": str(wine_root) if wine_root else "",
                "DXMT_EXPERIMENT_WINE_BIN": str(wine_bin),
                "DXMT_EXPERIMENT_PE_BUILD_DIR": str(pe_build_dir),
                "DXMT_EXPERIMENT_RUNTIME_PE_BUILD_DIR": str(runtime_pe_build_dir),
                "DXMT_EXPERIMENT_WOW64_PE_BUILD_DIR": str(wow64_pe_build_dir),
                "DXMT_EXPERIMENT_WOW64_RUNTIME_PE_BUILD_DIR": str(wow64_runtime_pe_build_dir),
                "DXMT_EXPERIMENT_UNIX_BUILD_DIR": str(unix_build_dir),
                "DXMT_EXPERIMENT_OUTPUT_DIR": str(output_dir),
                "DXMT_EXPERIMENT_LOG": str(log_path),
                "DXMT_EXPERIMENT_SKIP_STAGE": "1" if skip_stage else "",
            }
        )
        env.update(capture_request_environment(capture_request, output_dir, actual_dump_path))
        if app.wine_dll_overrides:
            env["DXMT_EXPERIMENT_WINE_DLLOVERRIDES"] = app.wine_dll_overrides
        if app.cx_bottle:
            env["DXMT_EXPERIMENT_CX_BOTTLE"] = app.cx_bottle

        command = ["bash", str(app.launcher_path)]
        environment_snapshot = snapshot_harness_environment(env)

        with log_path.open("wb") as log_fp:
            process_started_at = time.monotonic()
            process = subprocess.Popen(
                command,
                cwd=REPO_ROOT,
                env=env,
                stdout=log_fp,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            )
            try:
                deadline = time.monotonic() + app.capture_delay_sec
                while time.monotonic() < deadline and process.poll() is None:
                    time.sleep(0.1)
                if capture_request.wants_video and process.poll() is None:
                    raw_video_path = output_dir / "capture_work" / "segment.mov"
                    raw_video_path.parent.mkdir(parents=True, exist_ok=True)
                    video_path, video_error = record_bounded_video(
                        raw_video_path,
                        duration_sec=capture_request.video_duration_sec or capture_request.max_duration_sec,
                    )
                if process.poll() is None and capture_request.wants_multiple_frames:
                    for frame_id in capture_request.requested_frames:
                        if process.poll() is not None:
                            break
                        frame_path = output_dir / "capture_work" / f"window_frame{frame_id:06d}.png"
                        frame_path.parent.mkdir(parents=True, exist_ok=True)
                        info, error = capture_window_or_fallback(
                            frame_path,
                            expected_title=app.window_title,
                            timeout_sec=float(getattr(args, "capture_window_timeout", 2.0) or 2.0),
                        )
                        if info is not None and frame_path.exists():
                            scheduled_window_frames[frame_id] = {
                                "path": frame_path,
                                "source": "window_capture" if info.get("capture_mode") == "window_id" else info.get("capture_mode", "external_capture"),
                            }
                            if window_info is None:
                                window_info = info
                        if error and capture_error is None:
                            capture_error = error
                        time.sleep(float(getattr(args, "capture_interval_sec", 0.25) or 0.25))
                if (
                    process.poll() is None
                    and not capture_request.wants_multiple_frames
                    and not actual_dump_path.exists()
                ):
                    try:
                        if app.window_title:
                            window_info = capture_window_by_title(actual_path, app.window_title, timeout_sec=10.0)
                        else:
                            window_info = capture_frontmost_window(actual_path, app.window_title, timeout_sec=10.0)
                    except Exception as exc:  # noqa: BLE001
                        capture_error = str(exc)
                        try:
                            window_info = capture_full_screen(actual_path)
                        except Exception:  # noqa: BLE001
                            pass
                process.wait(timeout=args.timeout or app.run_timeout_sec)
            except subprocess.TimeoutExpired:
                timed_out = True
                terminate_process_group(process, signal.SIGTERM)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    terminate_process_group(process, signal.SIGKILL)
                    process.wait(timeout=5)
            finally:
                process_finished_at = time.monotonic()
                log_fp.flush()

        performance: dict[str, Any] = {}
        if process_started_at is not None and process_finished_at is not None:
            elapsed = max(0.0, process_finished_at - process_started_at)
            performance["process_elapsed_sec"] = elapsed
            frame_count = extract_frame_count(log_path)
            if frame_count is not None:
                performance["frames"] = frame_count
                if elapsed > 0.0:
                    performance["fps"] = frame_count / elapsed

        result: dict[str, Any] = {
            "schema": "dxmt9.experiment.result.v1",
            "name": app.name,
            "binary": str(binary_path),
            "launcher": str(app.launcher_path),
            "command": command,
            "environment": environment_snapshot,
            "reference": str(app.reference_path),
            "prefix": str(prefix),
            "wine_root": str(wine_root) if wine_root else None,
            "wine_bin": str(wine_bin),
            "exit_code": process.returncode if process is not None else None,
            "returncode": process.returncode if process is not None else None,
            "capture_error": capture_error,
            "window_info": window_info,
            "capture_frame": capture_frame,
            "failures": [],
            "timed_out": timed_out,
            "performance": performance,
        }
        # R-RT-6.2: record the resolved manifest entry and the prefix
        # bootstrap result for diagnostic cross-reference.
        if manifest_entry is not None:
            result["wine"] = {
                "id": manifest_entry.id,
                "source": manifest_entry.source,
                "variant": manifest_entry.variant,
                "path": str(manifest_entry.path),
            }
            result["prefix_bootstrap"] = prefix_bootstrap_payload
        dxmt9_perf_counters = extract_dxmt9_perf_counters(log_path)
        if dxmt9_perf_counters:
            result["dxmt9_perf_counters"] = dxmt9_perf_counters
        counter_violations = evaluate_counter_ranges(app.expected_counters, dxmt9_perf_counters)
        if counter_violations:
            result["counter_violations"] = counter_violations
            result["failures"].append({
                "type": "counter_out_of_range",
                "violations": counter_violations,
            })
            print(
                f"experiment {app.name} failed: {len(counter_violations)} counter range violations",
                file=sys.stderr,
            )
        dxmt9_bridge_counters = extract_dxmt9_bridge_counters(log_path)
        if dxmt9_bridge_counters:
            result["dxmt9_bridge_counters"] = dxmt9_bridge_counters
        dxmt9_pe_recorder_counters = extract_dxmt9_pe_recorder_counters(log_path)
        if dxmt9_pe_recorder_counters:
            result["dxmt9_pe_recorder_counters"] = dxmt9_pe_recorder_counters
        perf_probe_timings = extract_perf_probe_timings(log_path)
        if perf_probe_timings:
            result["perf_probe_timings"] = perf_probe_timings

        if actual_dump_path.exists():
            Image.open(actual_dump_path).save(actual_path)

        capture_source = classify_capture_source(actual_dump_path, actual_path, window_info)
        captured_frames, dropped_frames = collect_capture_frame_records(
            capture_request=capture_request,
            output_dir=output_dir,
            actual_dump_path=actual_dump_path,
            actual_path=actual_path,
            scheduled_window_frames=scheduled_window_frames,
            capture_source=capture_source,
        )
        if not actual_path.exists() and captured_frames:
            first_frame_path = Path(captured_frames[0]["path"])
            try:
                Image.open(first_frame_path).save(actual_path)
            except Exception:  # noqa: BLE001
                shutil.copy2(first_frame_path, actual_path)

        capture_source = classify_capture_source(actual_dump_path, actual_path, window_info)
        result["capture_source"] = capture_source
        result["capture_paths"] = {
            "actual": str(actual_path) if actual_path.exists() else None,
            "internal_dump": str(actual_dump_path) if actual_dump_path.exists() else None,
            "internal_frame_dir": str(internal_capture_dir) if internal_capture_dir.exists() else None,
        }
        debug_result = write_experiment_debug_result(
            app=app,
            output_dir=output_dir,
            debug_result_path=debug_result_path,
            log_path=log_path,
            actual_path=actual_path,
            command=command,
            environment=environment_snapshot,
            capture_frame=capture_frame,
            capture_source=capture_source,
            capture_error=capture_error,
            window_info=window_info,
            capture_request=capture_request,
            captured_frames=captured_frames,
            dropped_frames=dropped_frames,
            video_path=video_path,
            video_error=video_error,
        )
        if debug_result is not None:
            result["debug_result"] = str(debug_result)
        if dropped_frames:
            result["failures"].append({
                "type": "render-frame-sequence",
                "dropped_frames": dropped_frames,
            })
        if video_error:
            result["failures"].append({
                "type": "render-video-segment",
                "message": video_error,
            })

        if app.reference_path.exists():
            if reference_link_path.exists() or reference_link_path.is_symlink():
                reference_link_path.unlink()
            reference_link_path.symlink_to(app.reference_path.resolve())
        elif args.accept_reference and actual_path.exists():
            app.reference_path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(actual_path, app.reference_path)
            if reference_link_path.exists() or reference_link_path.is_symlink():
                reference_link_path.unlink()
            reference_link_path.symlink_to(app.reference_path.resolve())
            result["reference_created"] = True

        log_markers = scan_log_for_failures(log_path)
        if log_markers:
            result["failures"].append({"type": "log_markers", "markers": log_markers})

        if process is not None and process.returncode != 0 and not (timed_out and app.allow_timeout):
            result["failures"].append({"type": "process_exit", "returncode": process.returncode})

        if capture_error and not actual_dump_path.exists() and not actual_path.exists():
            result["failures"].append({"type": "capture", "message": capture_error})

        if actual_path.exists():
            metrics = image_luma_metrics(actual_path)
            result["image_metrics"] = metrics
            if is_black_screen(metrics):
                result["failures"].append({"type": "black_screen", **metrics})
        else:
            result["failures"].append({"type": "missing_capture"})

        if app.reference_path.exists() and actual_path.exists():
            write_diff_image(actual_path, app.reference_path, diff_path)
            try:
                ssim = compute_ssim(actual_path, app.reference_path)
                result["ssim"] = ssim
                ssim_path.write_text(f"{ssim:.6f}\n")
                if ssim < SSIM_THRESHOLD:
                    result["failures"].append({"type": "ssim", "value": ssim, "threshold": SSIM_THRESHOLD})
            except ValueError as exc:
                result["failures"].append({"type": "image_size_mismatch", "message": str(exc)})
        elif not app.reference_path.exists() and not app.reference_optional:
            result["failures"].append({"type": "missing_reference"})

        result["status"] = "pass" if not result["failures"] else "fail"
        result_path.write_text(json.dumps(result, indent=2, sort_keys=True))

        print(f"experiment: {app.name}")
        print(f"status: {result['status']}")
        print(f"output: {output_dir}")
        print(f"prefix: {prefix}")
        if "ssim" in result:
            print(f"ssim: {result['ssim']:.4f}")
        if result["failures"]:
            print("failures:")
            for failure in result["failures"]:
                print(f"  - {failure}")

        return 0 if result["status"] == "pass" else 1
    finally:
        if process is not None and process.poll() is None:
            terminate_process_group(process, signal.SIGTERM)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                terminate_process_group(process, signal.SIGKILL)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    pass
        for sig, previous in previous_signal_handlers.items():
            signal.signal(sig, previous)
        if cleanup_enabled:
            atexit.unregister(cleanup_temp_prefix)
            cleanup_temp_prefix()


def print_catalogue(apps: list[ExperimentApp]) -> None:
    for app in apps:
        print(
            f"{app.name:28} status={app.status:8} "
            f"binary={'yes' if app.binary_path.exists() else 'no ':3} "
            f"reference={'yes' if app.reference_path.exists() else 'no ':3} "
            f"scope={app.license_scope:20} "
            f"features={','.join(app.features)}"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description="Run dxmt9 experiments")
    subparsers = parser.add_subparsers(dest="command", required=True)

    subparsers.add_parser("list", help="List catalogue entries")

    run_parser = subparsers.add_parser("run", help="Run one experiment")
    run_parser.add_argument("name", help="Catalogue entry name")
    run_parser.add_argument("--wine-root", help="Wine runtime root")
    run_parser.add_argument("--wine-bin", help="Wine executable path")
    run_parser.add_argument(
        "--wine-id",
        help="Wine manifest id from experiments/wine/manifest.toml. "
             "Overrides CATALOGUE [[app]].wine_id and DXMT_EXPERIMENT_WINE_ID.",
    )
    run_parser.add_argument(
        "--wine-manifest",
        type=Path,
        default=None,
        help="Override default manifest path (experiments/wine/manifest.toml).",
    )
    run_parser.add_argument(
        "--rebuild-prefix",
        action="store_true",
        help="Delete experiments/prefixs/<name>/ and re-bootstrap.",
    )
    run_parser.add_argument(
        "--allow-non-vanilla",
        action="store_true",
        help="Suppress the warning when wine variant != vanilla (R-RT-6.3).",
    )
    run_parser.add_argument("--prefix", help="Wine prefix path")
    run_parser.add_argument("--binary", help="Override the binary path for this run")
    run_parser.add_argument("--timeout", type=float, help="Override timeout seconds")
    run_parser.add_argument("--pe-build-dir", help="PE build dir containing d3d9.dll")
    run_parser.add_argument("--runtime-pe-build-dir", help="builtin PE build dir containing runtime winemetal.dll")
    run_parser.add_argument("--wow64-pe-build-dir", help="32-bit PE build dir containing d3d9.dll")
    run_parser.add_argument("--wow64-runtime-pe-build-dir", help="builtin 32-bit PE build dir containing runtime winemetal.dll")
    run_parser.add_argument("--unix-build-dir", help="Unix build dir containing winemetal.so")
    run_parser.add_argument("--skip-stage", action="store_true", help="Do not stage dxmt9 into the Wine runtime/prefix")
    run_parser.add_argument(
        "--stage-mingw-runtime",
        action="store_true",
        help="With --skip-stage, only copy libc++/libunwind into the temp prefix",
    )
    run_parser.add_argument("--mingw-bin-dir", help="x86_64 llvm-mingw bin dir for runtime DLL staging")
    run_parser.add_argument("--wow64-mingw-bin-dir", help="i686 llvm-mingw bin dir for runtime DLL staging")
    run_parser.add_argument("--accept-reference", action="store_true", help="Create the reference image if it does not exist")
    run_parser.add_argument("--cleanup-temp-prefix", action="store_true", help="Delete the auto-created temp prefix after the run")
    run_parser.add_argument("--output-suffix", help="Append a suffix to the output directory name")
    run_parser.add_argument(
        "--capture-frame",
        type=int,
        default=None,
        help="Override CATALOGUE capture_frame / DXMT_CAPTURE_FRAME for this run",
    )
    run_parser.add_argument(
        "--capture-frames",
        help="Capture a comma-separated frame list, e.g. 120,125,130. Sets DXMT_CAPTURE_FRAMES.",
    )
    run_parser.add_argument(
        "--capture-range",
        help="Capture an inclusive frame interval START:END[:INTERVAL], e.g. 120:180:5.",
    )
    run_parser.add_argument(
        "--capture-video",
        help="Capture a bounded video segment as seconds (10s) or frame span START:END.",
    )
    run_parser.add_argument(
        "--capture-video-acceptance",
        choices=["triage", "extractable_frames", "human_review"],
        default="triage",
        help="Classify video evidence strength in dxmt9.debug.result.v1.",
    )
    run_parser.add_argument(
        "--capture-max-frames",
        type=int,
        default=64,
        help="Maximum frames retained for frame-list or interval capture.",
    )
    run_parser.add_argument(
        "--capture-max-seconds",
        type=float,
        default=5.0,
        help="Maximum seconds for video capture and runtime capture scheduling.",
    )
    run_parser.add_argument(
        "--capture-max-bytes",
        type=int,
        default=64 * 1024 * 1024,
        help="Maximum debug artifact bytes for frame/video manifests.",
    )
    run_parser.add_argument(
        "--capture-interval-sec",
        type=float,
        default=0.25,
        help="Window-capture scheduler delay between requested frames.",
    )
    run_parser.add_argument(
        "--capture-window-timeout",
        type=float,
        default=2.0,
        help="Per-frame timeout for locating the target window during scheduled captures.",
    )
    run_parser.add_argument(
        "--build",
        action="store_true",
        help="Run the app's build_script (from CATALOGUE.toml) before launching",
    )

    args = parser.parse_args()
    apps = load_catalogue(CATALOGUE_PATH)
    if args.command == "list":
        print_catalogue(apps)
        return 0

    app = next((item for item in apps if item.name == args.name), None)
    if app is None:
        print(f"unknown experiment: {args.name}", file=sys.stderr)
        return 2
    if getattr(args, "build", False):
        build_script_path = app.build_script_path
        if build_script_path is None:
            print(
                f"app {app.name} has no build_script in CATALOGUE; "
                f"add a build_script field or omit --build",
                file=sys.stderr,
            )
            return 2
        run_command(["bash", str(build_script_path)], cwd=REPO_ROOT)
    return run_experiment(app, args)


if __name__ == "__main__":
    raise SystemExit(main())
