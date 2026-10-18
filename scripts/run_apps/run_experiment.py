#!/usr/bin/env python3
from __future__ import annotations

import argparse
import atexit
import hashlib
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
import uuid
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
    validate_wsi_spawn,
)
from scripts.wine.bootstrap_prefix import (  # noqa: E402
    APPS_3RD_ROOT,
    PREFIXES_ROOT,
    BootstrapResult,
    bootstrap as bootstrap_prefix,
)
from scripts.run_apps.benchmark_result_artifacts import (  # noqa: E402
    BenchmarkResultCapturePlan,
    collect_benchmark_result_files,
    prepare_benchmark_result_capture,
)
from scripts.run_apps.tracing_harness import (  # noqa: E402
    TRACE_PROFILES,
    attempt_paths,
    atomic_write_json,
    binary_architecture,
    apply_trace_launch_configuration,
    host_identity,
    make_trace_request,
    parent_envelope,
    parent_result_binding,
    seal_trace_manifest,
    trace_launch_configuration,
    sha256_file,
    source_identity,
    utc_now,
    verify_engine_receipt,
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
CATALOGUE_SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".hlsl", ".fx"}

SSIM_THRESHOLD = 0.90
BLACK_LUMA_THRESHOLD = 8.0
BLACK_VARIANCE_THRESHOLD = 16.0
FRAME_PATTERNS = (
    re.compile(r"OK: .*finished at frame\s+(\d+)", re.IGNORECASE),
    re.compile(r"OK: rendered frame\s+(\d+)", re.IGNORECASE),
)
PERF_COUNTER_PATTERN = re.compile(r"^\[dxmt9-perf\]\s+(.*)$")
BRIDGE_COUNTER_PATTERN = re.compile(r"^\[dxmt9-bridge-perf\]\s+(.*)$")
PE_RECORDER_COUNTER_PATTERN = re.compile(
    r"^\[dxmt9-device\]\s+(?:[A-Za-z]+:\s+)?"
    r"pe_recorder_(?:stats|gap_call_stats|gap_tail_stats|"
    r"gap_body_stats|gap_between_call_stats|gap_transition_stats|"
    r"gap_name_transition_stats|gap_callsite_stats)\s+(.*)$"
)
PERF_PROBE_PATTERN = re.compile(r"^\[perf-probe\]\s+(.*)$")
PERF_COUNTER_VALUE_PATTERN = re.compile(r"([A-Za-z0-9_]+)=([^\s}]+)")
# R-WMB-6.2 / test_wild.rules.md: surface which WSI layer acquisition path the
# PE WSI control path selected after a successful unix adoption.
# The presenter emits one line per process via the dxmt9-wsi logger tag, e.g.:
#   [dxmt9-wsi] info: layer_acquisition=extescape-v1 hwnd=0x12345
WSI_LAYER_ACQUISITION_PATTERN = re.compile(
    r"^\[dxmt9-wsi\]\s+\w+:\s+layer_acquisition=([A-Za-z0-9_-]+)"
)
VALID_WSI_LAYER_ACQUISITION_PATHS = (
    "extescape-v1",
    "legacy-macdrv-symbols",
    "unavailable",
)
# The launcher records the experiment profile it resolved
# (`experiments/launchers/common.sh::exp_resolve_profile_defaults`) as one
# machine-readable line, e.g.:
#   [experiment] profile: name=perf source=default validate=0 log_level=warn ...
# so `result.json` can state the configuration the run actually used. The
# `debug` profile costs ~4x throughput; a measurement that does not name its
# profile has already been misread once as a renderer regression.
EXPERIMENT_PROFILE_PATTERN = re.compile(r"^\[experiment\]\s+profile:\s+(.*)$")
THREEDMARK_LANE_PATTERN = re.compile(r"^\[3dmark-lane\]\s+(.*)$")
_DRIVE_LETTER_RE = re.compile(r"^([A-Za-z]):[\\/](.*)$")


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
    require_positive_timeout: bool = False
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
    # Per-app renderer selection (R-BACK-39.5). Resolved ONCE at spawn into
    # DXMT9_RENDER_MODE in the launched wine process environment, where the
    # backend factory (src/dxmt9/render/backend_factory.cpp) reads it once.
    # Valid: "traditional" / "framegraph" (default). Absent => "framegraph".
    render_mode: str = "framegraph"
    # Catalogue-default launcher environment. Keys are exported into the
    # launcher process only when the caller's environment does not already
    # define them, so an explicit process value stays a diagnostic-run
    # override (same precedence rule as render_mode). Harness-owned
    # DXMT_EXPERIMENT_* / WSI-identity keys are applied after this and can
    # never be overridden from here. The effective values are recorded in
    # result.json:launcher_env.
    launcher_env: dict[str, str] = field(default_factory=dict)
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
        app = cls(
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
            require_positive_timeout=bool(data.get("require_positive_timeout", False)),
            skip_stage=bool(data.get("skip_stage", False)),
            wine_dll_overrides=data.get("wine_dll_overrides"),
            cx_bottle=data.get("cx_bottle"),
            build_script=data.get("build_script"),
            wine_id=data.get("wine_id"),
            wine_alternatives=list(data.get("wine_alternatives") or []),
            install_drive_letter=data.get("install_drive_letter", "d"),
            render_mode=str(data.get("render_mode", "framegraph")),
            launcher_env={
                str(key): str(value)
                for key, value in dict(data.get("launcher_env") or {}).items()
            },
        )
        for key in app.launcher_env:
            if not key or key != key.strip():
                raise ValueError(
                    f"{app.name}: launcher_env key {key!r} must be a non-empty "
                    "name without surrounding whitespace"
                )
            if key.startswith("DXMT_EXPERIMENT_") or key in (
                "DXMT9_WINE_METAL_SURFACE_PROTOCOL",
                "DXMT9_WINE_MANIFEST_ID",
            ):
                raise ValueError(
                    f"{app.name}: launcher_env may not set harness-owned key {key}"
                )
        if app.render_mode not in ("traditional", "framegraph"):
            raise ValueError(
                f"{app.name}: render_mode must be 'traditional' or 'framegraph', "
                f"got {app.render_mode!r}"
            )
        if app.require_positive_timeout and app.run_timeout_sec <= 0.0:
            raise ValueError(
                f"{app.name}: require_positive_timeout needs run_timeout_sec > 0"
            )
        return app

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


def is_git_ignored(path: Path) -> bool:
    try:
        repo_relative = path.relative_to(REPO_ROOT)
    except ValueError:
        return False
    try:
        result = subprocess.run(
            ["git", "check-ignore", "--quiet", str(repo_relative)],
            cwd=REPO_ROOT,
            check=False,
        )
    except (OSError, subprocess.SubprocessError):
        return False
    return result.returncode == 0


def has_catalogue_source_near_binary(binary_path: Path) -> bool:
    parent = binary_path.parent
    if not parent.is_dir():
        return False
    for candidate in parent.iterdir():
        if candidate == binary_path or not candidate.is_file():
            continue
        if candidate.suffix.lower() not in CATALOGUE_SOURCE_SUFFIXES:
            continue
        if not is_git_ignored(candidate):
            return True
    return False


def catalogue_blockers(app: ExperimentApp) -> list[str]:
    blockers: list[str] = []
    binary_path = app.binary_path
    try:
        binary_path.relative_to(REPO_ROOT / "experiments" / "apps")
    except ValueError:
        return blockers
    if (
        binary_path.name.lower().endswith(".exe")
        and is_git_ignored(binary_path)
        and not has_catalogue_source_near_binary(binary_path)
    ):
        blockers.append("ignored-binary-missing-source")
    return blockers


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
            return info
        except subprocess.CalledProcessError as exc:
            last_error = exc.stderr.strip() or exc.stdout.strip() or str(exc)
            time.sleep(0.2)
    raise RuntimeError(last_error or "unable to capture frontmost window")


def capture_full_screen(output_path: Path) -> dict[str, Any]:
    run_command(["screencapture", "-x", str(output_path)])
    return {"capture_mode": "full_screen"}


def image_luma_metrics(path: Path) -> dict[str, float]:
    image = Image.open(path).convert("RGB")
    arr = np.asarray(image, dtype=np.float32)
    luma = 0.2126 * arr[:, :, 0] + 0.7152 * arr[:, :, 1] + 0.0722 * arr[:, :, 2]
    return {
        "mean_luma": float(np.mean(luma)),
        "variance": float(np.var(luma)),
    }


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


# The five artifacts Wine actually loads, relative to a wine root. Recording
# their identity is not bookkeeping: on 2026-08-01 two separate incidents in one
# session hinged on not knowing which binary a run used. An A/B compared a
# release tree against a debugoptimized one and reported a phantom +29.7%; and
# the D3D9 conformance suite was found testing whichever tree last ran a wild
# experiment, because builtin-signed PE DLLs are resolved from the wine root no
# matter what path LoadLibrary is given -- so md5-checking the build output and
# the app-local copy both read as confirmation while the loaded file was a third
# one. A hash of what is actually staged, in the run's own output, makes both
# failures visible in the artifact instead of requiring someone to suspect them.
STAGED_ARTIFACTS = (
    "lib/wine/x86_64-windows/d3d9.dll",
    "lib/wine/x86_64-windows/winemetal_dxmt9.dll",
    "lib/wine/x86_64-unix/winemetal_dxmt9.so",
    "lib/wine/i386-windows/d3d9.dll",
    "lib/wine/i386-windows/winemetal_dxmt9.dll",
)


def describe_staged_build(wine_root: Path | None, build_dirs: dict[str, Path]) -> dict[str, Any]:
    """Identity of the binaries this run will actually load."""
    info: dict[str, Any] = {
        "build_dirs": {name: str(path) for name, path in build_dirs.items()},
        "artifacts": {},
    }
    if wine_root is None:
        return info
    for rel in STAGED_ARTIFACTS:
        path = wine_root / rel
        try:
            data = path.read_bytes()
        except OSError:
            info["artifacts"][rel] = None
            continue
        info["artifacts"][rel] = {
            "sha256": hashlib.sha256(data).hexdigest()[:16],
            "bytes": len(data),
        }
    return info


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


def extract_wsi_layer_acquisition(log_path: Path) -> str:
    """Return the WSI layer acquisition path the presenter selected.

    Parses the one-time `[dxmt9-wsi] info: layer_acquisition=<path> hwnd=...`
    line emitted by the PE WSI control path after unix adoption (R-WMB-15.2 /
    `agents/rules/test_wild.rules.md`). Returns one of
    `VALID_WSI_LAYER_ACQUISITION_PATHS`, or the verbatim token for a future
    path unknown to this parser. If the log is absent or never emitted (e.g.
    the process exited before adoption), returns ``"unavailable"``.
    """
    if not log_path.exists():
        return "unavailable"
    for line in log_path.read_text(errors="replace").splitlines():
        match = WSI_LAYER_ACQUISITION_PATTERN.match(line)
        if not match:
            continue
        path = match.group(1)
        if path in VALID_WSI_LAYER_ACQUISITION_PATHS:
            return path
        # Unknown path token — record verbatim so the diagnostic is visible
        # but flag it so reviewers know to update the valid set.
        return path
    return "unavailable"


def parse_experiment_profile_line(line: str) -> dict[str, str]:
    """Parse one `[experiment] profile: ...` record into its fields.

    Values stay strings: this is a record of a configuration, not a
    measurement, and `validate=0` must not be confused with a counter.
    """
    match = EXPERIMENT_PROFILE_PATTERN.match(line)
    if not match:
        return {}
    return dict(PERF_COUNTER_VALUE_PATTERN.findall(match.group(1)))


def extract_experiment_profile(log_path: Path) -> dict[str, str]:
    """Return the experiment profile the launcher resolved for this run.

    Parses the `[experiment] profile: name=<profile> source=<var|default> ...`
    line emitted by `experiments/launchers/common.sh::exp_resolve_profile_defaults`.
    The reported values are the effective ones, including any single-knob env
    override the caller applied on top of the profile.

    If the log is absent or the line was never emitted (e.g. a launcher that
    never reached `exp_resolve_profile_defaults`), returns the `"unavailable"`
    sentinel so a reader can distinguish "not recorded" from a recorded
    selection — the same convention as `extract_wsi_layer_acquisition`.
    """
    unavailable = {"name": "unavailable", "source": "unavailable"}
    if not log_path.exists():
        return unavailable
    for line in log_path.read_text(errors="replace").splitlines():
        parsed = parse_experiment_profile_line(line)
        if parsed:
            return parsed
    return unavailable


def extract_3dmark_lane(log_path: Path) -> dict[str, str] | None:
    """Return the canonical 3DMark lane resolved by the launcher.

    The launcher is authoritative because it has already applied raw-argument
    precedence and validated the product-specific preset table. Non-3DMark
    experiments return ``None`` and therefore do not gain a misleading lane.
    """
    if not log_path.exists():
        return None
    for line in log_path.read_text(errors="replace").splitlines():
        match = THREEDMARK_LANE_PATTERN.match(line)
        if not match:
            continue
        fields = dict(PERF_COUNTER_VALUE_PATTERN.findall(match.group(1)))
        if {"product", "lane", "source"}.issubset(fields):
            return {
                "product": f"3dmark{fields['product']}",
                "name": fields["lane"],
                "source": fields["source"],
            }
    return None


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


def resolve_trace_runtime(app: ExperimentApp, args: argparse.Namespace) -> dict[str, Any]:
    """Resolve the runtime/build inputs used by run without creating or staging."""
    entry: WineEntry | None = None
    source: str | None = None
    cli_wine_id = getattr(args, "wine_id", None)
    env_wine_id = os.environ.get("DXMT_EXPERIMENT_WINE_ID")
    if app.wine_id or cli_wine_id or env_wine_id:
        manifest_path = getattr(args, "wine_manifest", None) or DEFAULT_MANIFEST_PATH
        entries = load_manifest(manifest_path)
        entry, source = resolve_wine_id(
            entries=entries, cli_arg=cli_wine_id, env_var=env_wine_id,
            catalogue_value=app.wine_id, app_name=app.name,
        )
        validate_wsi_spawn(entry, allow_unsupported_negative_test=getattr(
            args, "allow_unsupported_wsi_negative_test", False
        ))
    wine_root = entry.path if entry is not None else (
        Path(args.wine_root).expanduser().resolve() if args.wine_root
        else detect_heroic_wine_root()
    )
    if entry is not None and not args.wine_bin:
        wine_bin = entry.wine_bin()
    elif args.wine_bin:
        wine_bin = Path(args.wine_bin).expanduser().resolve()
    elif wine_root is not None:
        wine_bin = resolve_wine_bin(wine_root)
    elif app.requires_wine:
        raise FileNotFoundError("no Wine root supplied and Heroic runtime auto-detect failed")
    else:
        wine_bin = None
    build_dirs = {
        "pe": Path(args.pe_build_dir).expanduser().resolve() if args.pe_build_dir else DEFAULT_PE_BUILD_DIR,
        "runtime_pe": Path(args.runtime_pe_build_dir).expanduser().resolve() if args.runtime_pe_build_dir else DEFAULT_RUNTIME_PE_BUILD_DIR,
        "unix": Path(args.unix_build_dir).expanduser().resolve() if args.unix_build_dir else DEFAULT_UNIX_BUILD_DIR,
        "wow64_pe": Path(args.wow64_pe_build_dir).expanduser().resolve() if args.wow64_pe_build_dir else DEFAULT_WOW64_PE_BUILD_DIR,
        "wow64_runtime_pe": Path(args.wow64_runtime_pe_build_dir).expanduser().resolve() if args.wow64_runtime_pe_build_dir else DEFAULT_WOW64_RUNTIME_PE_BUILD_DIR,
    }
    mingw_dirs = {
        "mingw": Path(args.mingw_bin_dir).expanduser().resolve() if args.mingw_bin_dir else DEFAULT_MINGW_BIN_DIR,
        "wow64_mingw": Path(args.wow64_mingw_bin_dir).expanduser().resolve() if args.wow64_mingw_bin_dir else DEFAULT_WOW64_MINGW_BIN_DIR,
    }
    if entry is not None:
        prefix = Path(args.prefix).expanduser().resolve() if args.prefix else PREFIXES_ROOT / app.name
        prefix_mode = "persistent"
    elif args.prefix:
        prefix = Path(args.prefix).expanduser().resolve()
        prefix_mode = "persistent"
    else:
        prefix = None
        prefix_mode = "temporary"
    binary_path = Path(args.binary).expanduser().resolve() if args.binary else app.binary_path
    return {
        "wine": ({"id": entry.id, "source": source, "version": entry.version,
                  "variant": entry.variant, "root": str(entry.path)} if entry else None),
        "wine_root": str(wine_root) if wine_root else None,
        "wine_bin": str(wine_bin) if wine_bin else None,
        "build_dirs": {name: str(path) for name, path in build_dirs.items()},
        "mingw_dirs": {name: str(path) for name, path in mingw_dirs.items()},
        "staged_build": describe_staged_build(wine_root, build_dirs),
        "skip_stage": bool(app.skip_stage or args.skip_stage),
        "stage_mingw_runtime": bool(getattr(args, "stage_mingw_runtime", False)),
        "prefix": str(prefix) if prefix else None,
        "prefix_mode": prefix_mode,
        "binary": str(binary_path),
    }


def run_experiment(app: ExperimentApp, args: argparse.Namespace) -> int:
    trace_profile_name = getattr(args, "trace_profile", None)
    trace_profile = TRACE_PROFILES.get(trace_profile_name) if trace_profile_name else None
    if trace_profile_name and trace_profile is None:
        raise ValueError(f"unknown trace profile {trace_profile_name!r}")
    if trace_profile is not None and trace_profile.name not in ("overview", "timeline", "gpu-detail") and (
        getattr(args, "trace_include_pe", False)
        or getattr(args, "trace_include_metal", False)
        or getattr(args, "trace_detail", False)
        or getattr(args, "trace_duration_ms", None) is not None
        or getattr(args, "trace_delay_ms", 0)
        or getattr(args, "trace_gpu_level", None) is not None
        or getattr(args, "trace_gpu_capacity", None) is not None
        or getattr(args, "trace_drain_mode", "deferred") != "deferred"
        or getattr(args, "trace_batch_events", 64) != 64
        or getattr(args, "trace_service_interval_ms", 10) != 10
        or getattr(args, "trace_transport_events", 16_384) != 16_384
    ):
        raise ValueError("trace window and source options require overview, timeline or gpu-detail")
    supplemental_bundles = []
    for spec in getattr(args, "trace_bundle", []):
        role, separator, path = spec.partition("=")
        if not separator or not role or not path or role == "engine":
            raise ValueError("--trace-bundle requires a non-engine ROLE=PATH")
        supplemental_bundles.append({"role": role, "path": str(Path(path).absolute())})
    supplemental_artifacts = []
    if getattr(args, "trace_artifacts", None):
        supplemental_artifacts = json.loads(args.trace_artifacts.read_text())
        if not isinstance(supplemental_artifacts, list) or any(
            not isinstance(row, dict) or not row.get("sha256")
            for row in supplemental_artifacts
        ):
            raise ValueError("--trace-artifacts requires a list of hashed artifact references")
    if trace_profile is None and (supplemental_bundles or supplemental_artifacts):
        raise ValueError("supplemental observations require --trace")
    trace_run_id = uuid.uuid4().hex if trace_profile is not None else None
    effective_trace_timeout = (
        getattr(args, "timeout", None)
        if getattr(args, "timeout", None) is not None else app.run_timeout_sec
    )
    trace_duration_ms = None
    trace_delay_ms = int(getattr(args, "trace_delay_ms", 0) or 0)
    trace_window_error: str | None = None
    if trace_profile is not None and trace_profile.window_mode == "duration":
        if effective_trace_timeout <= 0:
            trace_window_error = "timeline requires a positive runner timeout to reserve finalization time"
        else:
            timeout_ms = int(effective_trace_timeout * 1000)
            finalize_ms = trace_profile.finalize_timeout_ms
            available_ms = timeout_ms - finalize_ms - trace_delay_ms
            requested_ms = getattr(args, "trace_duration_ms", None)
            trace_duration_ms = min(5_000, available_ms) if requested_ms is None else int(requested_ms)
            if trace_delay_ms < 0 or trace_duration_ms <= 0:
                trace_window_error = "trace delay must be nonnegative and capture duration must be positive"
            elif trace_delay_ms + trace_duration_ms + finalize_ms > timeout_ms:
                trace_window_error = (
                    "runner timeout must cover trace delay, capture duration, and finalize deadline"
                )
            elif trace_duration_ms > 1_800_000:
                trace_window_error = "trace duration exceeds the frozen 30 minute profile bound"
    gpu_requested = bool(
        trace_profile is not None and trace_profile.name in ("overview", "gpu-detail")
    ) or bool(getattr(args, "trace_include_metal", False))
    gpu_level = getattr(args, "trace_gpu_level", None) or "command-buffer"
    requested_gpu_capacity = getattr(args, "trace_gpu_capacity", None)
    gpu_capacity = 256 if requested_gpu_capacity is None else requested_gpu_capacity
    if (trace_profile is not None and getattr(args, "trace_gpu_level", None)
            and not gpu_requested):
        trace_window_error = "--trace-gpu-level requires overview, gpu-detail or --trace-include-metal"
    if (trace_profile is not None and getattr(args, "trace_gpu_capacity", None) is not None
            and not gpu_requested):
        trace_window_error = "--trace-gpu-capacity requires overview, gpu-detail or --trace-include-metal"
    if gpu_requested and (gpu_capacity < 1 or gpu_capacity > 256):
        trace_window_error = "GPU timing capacity must be between 1 and 256"
    if getattr(args, "trace_dry_run", False):
        if trace_profile is None:
            raise ValueError("--trace-dry-run requires --trace-profile")
        runtime_error = None
        try:
            resolved_runtime = resolve_trace_runtime(app, args)
        except (ManifestError, OSError, ValueError) as exc:
            runtime_error = str(exc)
            resolved_runtime = None
        suffix = f"-{args.output_suffix}" if getattr(args, "output_suffix", None) else ""
        output_dir = DEFAULT_OUTPUT_ROOT / f"{app.name}{suffix}-trace-{trace_run_id}"
        paths = attempt_paths(output_dir, trace_run_id or "dry-run")
        request = (make_trace_request(trace_profile, run_id=trace_run_id or "dry-run",
                                      attempt_id=uuid.uuid4().hex, paths=paths,
                                      duration_ms=trace_duration_ms,
                                      delay_ms=trace_delay_ms,
                                      include_pe=getattr(args, "trace_include_pe", False),
                                      include_metal=getattr(args, "trace_include_metal", False),
                                      include_detail=getattr(args, "trace_detail", False),
                                      gpu_level=gpu_level, gpu_capacity=gpu_capacity,
                                      drain_mode=getattr(args, "trace_drain_mode", "deferred"),
                                      batch_events=getattr(args, "trace_batch_events", 64),
                                      service_interval_ms=getattr(args, "trace_service_interval_ms", 10),
                                      transport_events=getattr(args, "trace_transport_events", 16_384))
                   if trace_profile.name != "baseline" and not trace_window_error else None)
        source = source_identity(REPO_ROOT)
        provenance_error = None
        if not source.get("available") or not source.get("content_identity_complete"):
            provenance_error = "source provenance is unavailable or incomplete"
        preflight_error = trace_window_error or runtime_error or provenance_error
        launch_trace_config = trace_launch_configuration(
            trace_profile, request, request_path=paths["request"],
            receipt_path=paths["receipt"],
            requested_pe=getattr(args, "trace_include_pe", False),
        )
        print(json.dumps({
            "app": app.name, "profile": trace_profile.frozen_config(),
            "supported": trace_profile.supported and preflight_error is None,
            "unsupported_reason": trace_profile.unsupported_reason,
            "preflight_error": preflight_error,
            "run_id": trace_run_id, "output_dir": str(output_dir),
            "paths": {name: str(path) for name, path in paths.items()},
            "request": request, "runtime": resolved_runtime,
            "observer_config": launch_trace_config,
            "source": source, "will_launch": False,
            "supplemental_bundles": supplemental_bundles,
            "supplemental_artifacts": supplemental_artifacts,
        }, indent=2, sort_keys=True))
        return 0 if trace_profile.supported and preflight_error is None else 2
    blockers = catalogue_blockers(app)
    launch_preflight_error = None
    if not app.launcher_path.exists():
        launch_preflight_error = f"launcher not found: {app.launcher_path}"
    elif blockers and args.binary is None:
        launch_preflight_error = (
            f"{app.name}: source/build lane unavailable ({', '.join(blockers)}); "
            f"not running generated/ignored binary {app.binary_path}"
        )
    binary_path = Path(args.binary).expanduser().resolve() if args.binary else app.binary_path
    if not binary_path.exists() and launch_preflight_error is None:
        launch_preflight_error = f"binary not found: {binary_path}"
    if trace_profile is None and launch_preflight_error:
        raise FileNotFoundError(launch_preflight_error)

    output_name = app.name
    if getattr(args, "output_suffix", None):
        output_name = f"{app.name}-{args.output_suffix}"
    if trace_run_id:
        output_name = f"{output_name}-trace-{trace_run_id}"
    output_dir = DEFAULT_OUTPUT_ROOT / output_name
    output_dir.mkdir(parents=True, exist_ok=True)
    actual_path = output_dir / "actual.png"
    actual_dump_path = output_dir / "actual.bmp"
    reference_link_path = output_dir / "reference.png"
    diff_path = output_dir / "diff.png"
    ssim_path = output_dir / "ssim.txt"
    log_path = output_dir / "dxmt9.log"
    result_path = output_dir / "result.json"
    trace_paths = attempt_paths(output_dir, trace_run_id) if trace_run_id else None
    trace_request: dict[str, Any] | None = None
    trace_manifest: dict[str, Any] | None = None
    for path in (actual_path, actual_dump_path, diff_path, ssim_path, reference_link_path, log_path, result_path):
        if path.exists() or path.is_symlink():
            path.unlink()
    benchmark_result_dir = output_dir / "benchmark-results"
    if benchmark_result_dir.is_symlink() or benchmark_result_dir.is_file():
        benchmark_result_dir.unlink()
    elif benchmark_result_dir.is_dir():
        shutil.rmtree(benchmark_result_dir)

    trace_attempt_id: str | None = None
    trace_source = source_identity(REPO_ROOT) if trace_profile is not None else None
    if trace_source is not None and (
        not trace_source.get("available") or not trace_source.get("content_identity_complete")
    ):
        launch_preflight_error = "source provenance is unavailable or incomplete"
    if trace_profile is not None and trace_run_id is not None and trace_paths is not None:
        trace_paths["root"].mkdir(parents=True, exist_ok=False)
        trace_attempt_id = uuid.uuid4().hex
        trace_request = (
            make_trace_request(trace_profile, run_id=trace_run_id,
                               attempt_id=trace_attempt_id, paths=trace_paths,
                               duration_ms=trace_duration_ms,
                               delay_ms=trace_delay_ms,
                               include_pe=getattr(args, "trace_include_pe", False),
                               include_metal=getattr(args, "trace_include_metal", False),
                               include_detail=getattr(args, "trace_detail", False),
                               gpu_level=gpu_level, gpu_capacity=gpu_capacity,
                               drain_mode=getattr(args, "trace_drain_mode", "deferred"),
                               batch_events=getattr(args, "trace_batch_events", 64),
                               service_interval_ms=getattr(args, "trace_service_interval_ms", 10),
                               transport_events=getattr(args, "trace_transport_events", 16_384))
            if trace_profile.name != "baseline" and not trace_window_error else None
        )
        launch_trace_config = trace_launch_configuration(
            trace_profile, trace_request,
            request_path=trace_paths["request"], receipt_path=trace_paths["receipt"],
            requested_pe=getattr(args, "trace_include_pe", False),
        )
        profile_value = trace_profile.frozen_config()
        trace_manifest = {
            **parent_envelope(
                producer="scripts/run_apps/run_experiment.py", inputs=[],
                env_snapshot=launch_trace_config["environment"],
                validity={"status": "pending", "checks": []},
            ),
            "run_id": trace_run_id,
            "attempt_id": trace_attempt_id,
            "created_at": utc_now(),
            "question": trace_profile.question,
            "profile": profile_value,
            "profile_sha256": hashlib.sha256(
                json.dumps(profile_value, sort_keys=True, separators=(",", ":")).encode()
            ).hexdigest(),
            "request": trace_request,
            "observer_config": launch_trace_config,
            "host": host_identity(),
            "source": trace_source,
            "app": {"name": app.name, "binary": str(binary_path),
                    "binary_sha256": sha256_file(binary_path) if binary_path.exists() else None,
            },
            "collectors": [
                {"name": name, "required": True, "status": "pending"}
                for name in trace_profile.required_collectors
            ] + [
                {"name": name, "required": False, "status": "not_preflighted"}
                for name in trace_profile.optional_collectors
            ],
            "artifacts": [],
            "supplemental_bundles": supplemental_bundles,
            "supplemental_artifacts": supplemental_artifacts,
        }
        if trace_request is not None:
            atomic_write_json(trace_paths["request"], trace_request)
        atomic_write_json(trace_paths["state_manifest"], trace_manifest)
        if not trace_profile.supported or trace_window_error:
            reason = trace_window_error or trace_profile.unsupported_reason or "profile unsupported"
            trace_manifest["validity"] = {"status": "invalid", "checks": [reason]}
            trace_manifest["collectors"] = [
                {"name": collector["name"], "required": collector["required"],
                 "status": "unsupported", "reason": reason}
                for collector in trace_manifest["collectors"]
            ]
            sealed = seal_trace_manifest(trace_paths, trace_manifest)
            result_path.write_text(json.dumps({
                "name": app.name, "status": "fail", "returncode": None,
                "failures": [{"type": "trace_preflight", "message": reason}],
                "trace": {"run_id": trace_run_id, "attempt_id": trace_attempt_id,
                          "profile": trace_profile.name, "qualified": False,
                          **sealed, **parent_result_binding(
                              run_id=trace_run_id, attempt_id=trace_attempt_id,
                              workload_status="fail", trace_qualified=False, sealed=sealed)},
            }, indent=2, sort_keys=True) + "\n")
            print(f"trace profile unsupported: {reason}", file=sys.stderr)
            return 2
        if launch_preflight_error:
            reason = launch_preflight_error
            trace_manifest["validity"] = {"status": "invalid", "checks": [reason]}
            trace_manifest["qualification"] = {"status": "failed", "errors": [reason]}
            trace_manifest["completed_at"] = utc_now()
            sealed = seal_trace_manifest(trace_paths, trace_manifest)
            result_path.write_text(json.dumps({
                "name": app.name, "status": "fail", "returncode": None,
                "failures": [{"type": "trace_preflight", "message": reason}],
                "trace": {"run_id": trace_run_id, "attempt_id": trace_attempt_id,
                          "profile": trace_profile.name, "qualified": False,
                          **sealed, **parent_result_binding(
                              run_id=trace_run_id, attempt_id=trace_attempt_id,
                              workload_status="fail", trace_qualified=False, sealed=sealed)},
            }, indent=2, sort_keys=True) + "\n")
            print(f"trace preflight failed: {reason}", file=sys.stderr)
            return 2

    # Resolve a manifest entry first when the app or CLI requests one. This
    # implements R-RT-6.1 (CLI > env > CATALOGUE) and feeds both the prefix
    # selection (R-RT-4) and the wine_root/wine_bin resolution below.
    manifest_entry: WineEntry | None = None
    manifest_source: str | None = None
    cli_wine_id = getattr(args, "wine_id", None)
    env_wine_id = os.environ.get("DXMT_EXPERIMENT_WINE_ID")
    if app.wine_id or cli_wine_id or env_wine_id:
        manifest_path = getattr(args, "wine_manifest", None) or DEFAULT_MANIFEST_PATH
        try:
            entries = load_manifest(manifest_path)
            manifest_entry, manifest_source = resolve_wine_id(
                entries=entries,
                cli_arg=cli_wine_id,
                env_var=env_wine_id,
                catalogue_value=app.wine_id,
                app_name=app.name,
            )
        except ManifestError as exc:
            print(f"[runtime] manifest error: {exc}", file=sys.stderr)
            return 2
        print(
            f"[runtime] wine resolved via {manifest_source}: id={manifest_entry.id} "
            f"path={manifest_entry.path}",
            file=sys.stderr,
        )
        try:
            validate_wsi_spawn(
                manifest_entry,
                allow_unsupported_negative_test=getattr(
                    args, "allow_unsupported_wsi_negative_test", False
                ),
            )
        except ManifestError as exc:
            print(f"[runtime] WSI gate: {exc}", file=sys.stderr)
            return 2
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

    cleanup_enabled = temp_prefix and not args.keep_temp_prefix
    cleanup_done = False
    process: subprocess.Popen[str] | None = None
    capture_error: str | None = None
    window_info: dict[str, Any] | None = None
    timed_out = False
    process_started_at: float | None = None
    process_finished_at: float | None = None
    benchmark_result_plan: BenchmarkResultCapturePlan | None = None
    benchmark_result_payload: dict[str, Any] | None = None
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

        staged_build = describe_staged_build(
            wine_root,
            {
                "pe": pe_build_dir,
                "runtime_pe": runtime_pe_build_dir,
                "unix": unix_build_dir,
                "wow64_pe": wow64_pe_build_dir,
                "wow64_runtime_pe": wow64_runtime_pe_build_dir,
            },
        )
        staged_build["skipped"] = bool(skip_stage)

        env = os.environ.copy()
        # Catalogue-default launcher env: an explicit caller value wins, and
        # the harness-owned keys set below win over both (launcher_env cannot
        # name them — enforced at parse time).
        effective_launcher_env: dict[str, str] = {}
        for launcher_env_key, launcher_env_value in app.launcher_env.items():
            env.setdefault(launcher_env_key, launcher_env_value)
            effective_launcher_env[launcher_env_key] = env[launcher_env_key]
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
                "DXMT_EXPERIMENT_CAPTURE_PATH": str(actual_dump_path),
                "DXMT_CAPTURE_FRAME": str(app.capture_frame),
                "DXMT_EXPERIMENT_SKIP_STAGE": "1" if skip_stage else "",
                # R-BACK-39.5: resolve the per-app renderer ONCE here, at the
                # single point where the wine process is spawned, so it persists
                # for that process lifetime. An explicit process environment
                # value is a diagnostic-run override; otherwise the catalogue
                # remains authoritative.
                "DXMT9_RENDER_MODE": os.environ.get(
                    "DXMT9_RENDER_MODE", app.render_mode
                ),
            }
        )
        env.pop("DXMT9_WINE_METAL_SURFACE_PROTOCOL", None)
        env.pop("DXMT9_WINE_MANIFEST_ID", None)
        if trace_profile is not None:
            apply_trace_launch_configuration(env, launch_trace_config)
        if manifest_entry is not None:
            env["DXMT9_WINE_METAL_SURFACE_PROTOCOL"] = (
                manifest_entry.metal_surface_protocol
            )
            env["DXMT9_WINE_MANIFEST_ID"] = manifest_entry.id
        if trace_manifest is not None:
            staged_full_hashes: dict[str, Any] = {}
            if wine_root is not None:
                for relative in STAGED_ARTIFACTS:
                    staged_path = wine_root / relative
                    try:
                        staged_full_hashes[relative] = {
                            "sha256": sha256_file(staged_path),
                            "bytes": staged_path.stat().st_size,
                            "architecture": binary_architecture(staged_path),
                        }
                    except OSError:
                        staged_full_hashes[relative] = None
            trace_manifest["runtime"] = {
                "wine_id": manifest_entry.id if manifest_entry else None,
                "wine_version": manifest_entry.version if manifest_entry else None,
                "wine_root": str(wine_root) if wine_root else None,
                "wine_bin": str(wine_bin),
                "staged_build": staged_build,
                "staged_artifacts_full": staged_full_hashes,
                "bridge_abi_source_sha256": sha256_file(
                    REPO_ROOT / "src/winemetal/winemetal_thunks.hpp"
                ),
                "lane": app.name,
                "launcher_env": effective_launcher_env,
                "env_snapshot": {
                    key: value for key, value in env.items()
                    if key.startswith(("DXMT_", "DXMT9_", "DXMT_EXPERIMENT_"))
                    or key == "WINEDEBUG"
                },
            }
            trace_manifest["target"] = {
                "module": str(binary_path),
                "sha256": sha256_file(binary_path),
                "architecture": binary_architecture(binary_path),
            }
            trace_manifest["inputs"] = [
                {"path": str(binary_path), "sha256": sha256_file(binary_path)},
            ]
            atomic_write_json(trace_paths["state_manifest"], trace_manifest, replace_existing=True)  # type: ignore[index]
        if app.wine_dll_overrides:
            env["DXMT_EXPERIMENT_WINE_DLLOVERRIDES"] = app.wine_dll_overrides
        if app.cx_bottle:
            env["DXMT_EXPERIMENT_CX_BOTTLE"] = app.cx_bottle

        benchmark_result_plan = prepare_benchmark_result_capture(
            app_name=app.name,
            workdir=binary_path.parent,
            prefix=prefix,
            output_name=output_name,
            env=env,
        )

        with log_path.open("wb") as log_fp:
            process_started_at = time.monotonic()
            process = subprocess.Popen(
                ["bash", str(app.launcher_path)],
                cwd=REPO_ROOT,
                env=env,
                stdout=log_fp,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            )
            try:
                capture_delay_sec = (
                    float(args.capture_delay_sec)
                    if args.capture_delay_sec is not None
                    else app.capture_delay_sec
                )
                if capture_delay_sec < 0.0:
                    raise ValueError("--capture-delay-sec must be >= 0")
                deadline = time.monotonic() + capture_delay_sec
                while time.monotonic() < deadline and process.poll() is None:
                    time.sleep(0.1)
                if process.poll() is None and not actual_dump_path.exists():
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
                wait_timeout_sec = app.run_timeout_sec if args.timeout is None else args.timeout
                process.wait(timeout=None if wait_timeout_sec <= 0.0 else wait_timeout_sec)
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

        if benchmark_result_plan is not None:
            benchmark_result_payload = collect_benchmark_result_files(
                benchmark_result_plan,
                output_dir=output_dir,
            )

        trace_result: dict[str, Any] | None = None
        if trace_request is not None and trace_paths is not None:
            trace_result = verify_engine_receipt(
                trace_paths["receipt"], trace_request, trace_paths["pftrace"]
            )
            if trace_result["qualified"]:
                raw_path = Path(str(trace_paths["pftrace"]) + ".raw.jsonl")
                normalized_path = trace_paths["root"] / "normalized-bundle.json"
                consumer_path = trace_paths["root"] / "trace-processor-receipt.json"
                try:
                    from scripts.tools.tracing_normalize import adapt, validate_bundle
                    from scripts.tools.tracing_consumer import derive_expected, qualify_trace

                    receipt = trace_result["receipt"]
                    process_incarnation = str(
                        receipt.get("process", {}).get("incarnation", "unknown")
                    ) if isinstance(receipt.get("process"), dict) else "unknown"
                    bundle = adapt(raw_path, "auto", run_id=trace_run_id or "unknown",
                                   process_incarnation=process_incarnation)
                    normalization = validate_bundle(bundle, require_complete=True)
                    expected = derive_expected(bundle)
                    atomic_write_json(normalized_path, bundle)
                    processor_receipt = qualify_trace(
                        trace_paths["pftrace"], expected=expected, require_complete=True
                    )
                    atomic_write_json(consumer_path, processor_receipt)
                    trace_result["normalization"] = {
                        "qualified": True, "validity": normalization,
                        "path": str(normalized_path), "sha256": sha256_file(normalized_path),
                    }
                    trace_result["trace_processor"] = {
                        "qualified": processor_receipt.get("verdict") == "qualified",
                        "path": str(consumer_path), "sha256": sha256_file(consumer_path),
                        "consumer": processor_receipt.get("consumer"),
                        "queries": processor_receipt.get("queries"),
                    }
                    if processor_receipt.get("verdict") != "qualified":
                        raise ValueError("pinned Trace Processor returned an unqualified verdict")
                except Exception as exc:  # noqa: BLE001
                    trace_result["qualified"] = False
                    trace_result["status"] = "failed"
                    trace_result["errors"].append(f"normalization_or_consumer_failed:{exc}")
            trace_receipt_path = trace_paths["receipt"]
            trace_manifest["completed_at"] = utc_now()  # type: ignore[index]
            trace_manifest["process"] = {
                "returncode": process.returncode if process else None,
                "timed_out": timed_out,
            }
            trace_manifest["collectors"] = [
                {"name": "engine-perfetto", "required": True,
                 "status": "qualified" if trace_result["qualified"] else "failed",
                 "errors": trace_result["errors"]},
                *[{
                    "name": name,
                    "required": name in trace_profile.required_collectors,
                    "status": (
                        (trace_result.get("receipt", {}).get(
                            "collector_status" if name in trace_profile.required_collectors
                            else "optional_collectors", {}).get(name, {})
                         or {}).get("status", "outcome_missing")
                        if trace_request and name in (
                            trace_request.get("required_collectors", []) +
                            trace_request.get("optional_collectors", []))
                        else "not_requested"
                    ),
                    "outcome": (
                        trace_result.get("receipt", {}).get(
                            "collector_status" if name in trace_profile.required_collectors
                            else "optional_collectors", {}).get(name)
                        if trace_request and name in (
                            trace_request.get("required_collectors", []) +
                            trace_request.get("optional_collectors", []))
                        else None
                    ),
                } for name in dict.fromkeys(
                    [*trace_profile.required_collectors, *trace_profile.optional_collectors])
                  if name != "engine-perfetto"],
            ]
            trace_manifest["qualification"] = {
                "status": trace_result["status"],
                "required_trace_qualified": trace_result["qualified"],
                "errors": trace_result["errors"],
                "normalization": trace_result.get("normalization"),
                "trace_processor": trace_result.get("trace_processor"),
                "claims": {
                    "engine_timeline": {
                        "status": "qualified" if trace_result["qualified"] else "unqualified",
                        "sources": ["engine-perfetto"],
                    },
                    "pe_observation": {
                        "status": (
                            ((trace_result.get("receipt", {}).get("optional_collectors", {})
                              .get("pe-events", {}) or {}).get("status", "unknown"))
                            if trace_request and "pe-events" in trace_request["optional_collectors"]
                            else "not_requested"
                        ),
                        "reason": "optional PE source outcome is independent of engine timeline qualification",
                    },
                    "metal_timing": {
                        "status": (
                            ((trace_result.get("receipt", {}).get(
                                "collector_status" if "metal-timing" in
                                trace_profile.required_collectors else "optional_collectors", {})
                              .get("metal-timing", {}) or {}).get("status", "unknown"))
                            if trace_request and "metal-timing" in (
                                trace_request["required_collectors"] +
                                trace_request["optional_collectors"])
                            else "not_requested"
                        ),
                        "reason": (
                            "Metal timing is required by this profile"
                            if trace_profile and "metal-timing" in trace_profile.required_collectors
                            else "optional Metal source outcome is independent of engine timeline qualification"
                        ),
                    },
                },
            }
            trace_manifest["validity"] = {
                "status": "valid" if trace_result["qualified"] else "invalid",
                "checks": trace_result["errors"],
            }
            atomic_write_json(trace_paths["state_manifest"], trace_manifest, replace_existing=True)

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
            "name": app.name,
            "binary": str(binary_path),
            "launcher": str(app.launcher_path),
            "reference": str(app.reference_path),
            "prefix": str(prefix),
            "wine_root": str(wine_root) if wine_root else None,
            # Which binaries this run actually loaded -- see describe_staged_build.
            "staged_build": staged_build,
            "wine_bin": str(wine_bin),
            "returncode": process.returncode if process is not None else None,
            "capture_error": capture_error,
            "window_info": window_info,
            "failures": [],
            "timed_out": timed_out,
            "performance": performance,
            # State the configuration this measurement ran under. The `debug`
            # profile (Metal validation layer + debug logging) costs ~4x
            # throughput, so an fps or log-size number is only interpretable
            # alongside the profile that produced it.
            "profile": extract_experiment_profile(log_path),
        }
        benchmark_lane = extract_3dmark_lane(log_path)
        if benchmark_lane is not None:
            result["benchmark_lane"] = benchmark_lane
        if benchmark_result_payload is not None:
            result["benchmark_result_files"] = benchmark_result_payload
            if benchmark_result_payload["errors"]:
                result["failures"].append({
                    "type": "benchmark_result_capture",
                    "errors": benchmark_result_payload["errors"],
                })
        # R-RT-6.2: record the resolved manifest entry and the prefix
        # bootstrap result for diagnostic cross-reference.
        if manifest_entry is not None:
            result["wine"] = {
                "id": manifest_entry.id,
                "source": manifest_entry.source,
                "variant": manifest_entry.variant,
                "path": str(manifest_entry.path),
                "metal_surface_protocol": manifest_entry.metal_surface_protocol,
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
        # R-WMB-15.2: preserve the manifest declaration and the observed WSI
        # selection independently; an ExtEscape claim never bypasses probing.
        result["wsi"] = {
            "declared_protocol": (
                manifest_entry.metal_surface_protocol
                if manifest_entry is not None
                else "unknown"
            ),
            "layer_acquisition": extract_wsi_layer_acquisition(log_path),
        }
        if effective_launcher_env:
            result["launcher_env"] = effective_launcher_env

        if actual_dump_path.exists():
            Image.open(actual_dump_path).save(actual_path)

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
            if metrics["mean_luma"] <= BLACK_LUMA_THRESHOLD and metrics["variance"] <= BLACK_VARIANCE_THRESHOLD:
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

        if trace_profile is not None and trace_profile.name == "baseline":
            result["trace"] = {
                "run_id": trace_run_id, "attempt_id": trace_attempt_id,
                "profile": trace_profile.name, "qualified": True,
                "manifest": str(trace_paths["manifest"]) if trace_paths else None,
            }
            trace_manifest["completed_at"] = utc_now()  # type: ignore[index]
            trace_manifest["process"] = {
                "returncode": process.returncode if process else None,
                "timed_out": timed_out,
            }
            trace_manifest["validity"] = {"status": "valid", "checks": ["no tracing requested by baseline"]}
            atomic_write_json(trace_paths["state_manifest"], trace_manifest, replace_existing=True)  # type: ignore[index]
        elif trace_profile is not None and trace_request is not None and trace_paths is not None:
            result["trace"] = {
                "run_id": trace_run_id, "attempt_id": trace_attempt_id,
                "profile": trace_profile.name,
                "qualified": bool(trace_result and trace_result["qualified"]),
                "status": trace_result["status"] if trace_result else "failed",
                "manifest": str(trace_paths["manifest"]),
                "receipt": str(trace_paths["receipt"]),
                "pftrace": str(trace_paths["pftrace"]),
                "errors": trace_result["errors"] if trace_result else ["receipt_not_verified"],
            }
            if not trace_result or not trace_result["qualified"]:
                result["failures"].append({
                    "type": "trace_qualification",
                    "errors": trace_result["errors"] if trace_result else ["receipt_not_verified"],
                    "workload_status_independent": True,
                })

        result["status"] = "pass" if not result["failures"] else "fail"
        if trace_manifest is not None and trace_paths is not None:
            trace_manifest["workload_outcome"] = {
                "status": "fail" if any(f.get("type") != "trace_qualification"
                                         for f in result["failures"]) else "pass",
                "returncode": result.get("returncode"), "timed_out": timed_out,
            }
            trace_manifest["supplemental_artifacts"].append({
                "role": "workload-log", "path": str(log_path), "sha256": sha256_file(log_path),
                "format": "dxmt9-log", "collector": "workload-stdout-stderr",
                "reason": "acquisition transcript and failure breadcrumbs",
            })
            trace_manifest["test_provenance"] = {
                "producer": "run_experiment", "catalogue_sha256": sha256_file(CATALOGUE_PATH),
                "failures": result["failures"], "staged_build": result.get("staged_build"),
            }
            sealed = seal_trace_manifest(trace_paths, trace_manifest)
            result.setdefault("trace", {}).update(sealed)
            if sealed["observations"]["status"] == "partial":
                result["trace"]["qualified"] = False
                result["trace"]["status"] = "failed"
                result["failures"].append({"type": "observation_store", "errors":
                                           sealed["observations"]["normalization_errors"]})
                result["status"] = "fail"
            result["trace"].update(parent_result_binding(
                run_id=trace_run_id or "", attempt_id=trace_attempt_id or "",
                workload_status=trace_manifest["workload_outcome"]["status"],
                trace_qualified=bool(result.get("trace", {}).get("qualified")), sealed=sealed,
            ))
        result_path.write_text(json.dumps(result, indent=2, sort_keys=True))

        print(f"experiment: {app.name}")
        print(f"status: {result['status']}")
        print(f"output: {output_dir}")
        print(f"prefix: {prefix}")
        profile_record = result["profile"]
        print(
            f"profile: {profile_record.get('name', 'unavailable')} "
            f"(source: {profile_record.get('source', 'unavailable')})"
        )
        if "ssim" in result:
            print(f"ssim: {result['ssim']:.4f}")
        if result["failures"]:
            print("failures:")
            for failure in result["failures"]:
                print(f"  - {failure}")

        return 0 if result["status"] == "pass" else 1
    finally:
        if (trace_manifest is not None and trace_paths is not None
                and not trace_manifest.get("sealed")):
            validity = trace_manifest.get("validity", {})
            if validity.get("status") == "pending":
                trace_manifest["completed_at"] = utc_now()
                trace_manifest["validity"] = {
                    "status": "invalid",
                    "checks": ["runner_exited_before_trace_qualification"],
                }
                trace_manifest["qualification"] = {
                    "status": "failed",
                    "errors": ["runner_exited_before_trace_qualification"],
                }
                try:
                    sealed = seal_trace_manifest(trace_paths, trace_manifest)
                    if not result_path.exists():
                        binding = parent_result_binding(
                            run_id=trace_run_id or "", attempt_id=trace_attempt_id or "",
                            workload_status="fail",
                            trace_qualified=bool(trace_manifest.get("qualification", {}).get(
                                "required_trace_qualified", False)),
                            sealed=sealed,
                        )
                        result_path.write_text(json.dumps({
                            "name": app.name,
                            "binary": str(binary_path),
                            "launcher": str(app.launcher_path),
                            "reference": str(app.reference_path),
                            "returncode": process.returncode if process else None,
                            "failures": [{"type": "trace_runner_exit",
                                          "message": "runner exited before result publication"}],
                            "timed_out": timed_out,
                            "trace": {
                                "run_id": trace_run_id, "attempt_id": trace_attempt_id,
                                "profile": trace_profile.name if trace_profile else None,
                                "qualified": binding["parent_result_envelope"]["trace_qualified"],
                                **sealed, **binding,
                            },
                        }, indent=2, sort_keys=True) + "\n")
                except OSError:
                    print("[trace] failed to seal attempt failure metadata", file=sys.stderr)
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
        blockers = catalogue_blockers(app)
        blocker_text = f" blocker={','.join(blockers)}" if blockers else ""
        print(
            f"{app.name:28} status={app.status:8} "
            f"binary={'yes' if app.binary_path.exists() else 'no ':3} "
            f"reference={'yes' if app.reference_path.exists() else 'no ':3} "
            f"scope={app.license_scope:20} "
            f"features={','.join(app.features)}"
            f"{blocker_text}"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description="Run dxmt9 experiments")
    subparsers = parser.add_subparsers(dest="command", required=True)

    subparsers.add_parser("list", help="List catalogue entries")
    subparsers.add_parser("trace-profiles", help="List frozen tracing profiles and collector requirements")

    run_parser = subparsers.add_parser("run", help="Run one experiment")
    run_parser.add_argument("name", help="Catalogue entry name")
    run_parser.add_argument(
        "--trace-profile", "--trace", nargs="?", const="overview", choices=tuple(TRACE_PROFILES),
        help="Capture a bounded Perfetto overview (CPU and available GPU by default); baseline disables the integrated trace.",
    )
    run_parser.add_argument(
        "--trace-dry-run", action="store_true",
        help="Print the resolved tracing request and artifact paths without launching.",
    )
    run_parser.add_argument("--trace-bundle", action="append", default=[], metavar="ROLE=PATH",
                            help="Normalize supplemental CPU/wait/PE evidence into the attempt SQLite store.")
    run_parser.add_argument("--trace-artifacts", type=Path,
                            help="JSON list of supplemental artifact digests and explicit event/work links.")
    run_parser.add_argument("--trace-duration-ms", type=int,
                            help="Override the bounded trace window duration in milliseconds.")
    run_parser.add_argument("--trace-delay-ms", type=int, default=0,
                            help="Delay trace collection after engine preparation (milliseconds).")
    run_parser.add_argument("--trace-include-pe", action="store_true",
                            help="Request optional PE events for the timeline profile.")
    run_parser.add_argument("--trace-include-metal", action="store_true",
                            help="Request optional Metal timing for the timeline profile.")
    run_parser.add_argument("--trace-detail", action="store_true",
                            help="Request bounded detail events for the selected trace window.")
    run_parser.add_argument("--trace-gpu-level", choices=("command-buffer", "encoder-stage"),
                            default=None,
                            help="GPU timestamp scope for gpu-detail or --trace-include-metal.")
    run_parser.add_argument("--trace-gpu-capacity", type=int, default=None,
                            help="Bounded GPU timing sample capacity (maximum 256).")
    run_parser.add_argument("--trace-drain-mode", choices=("deferred", "async"), default="deferred",
                            help="Trace output transport; deferred is the default, async uses the bounded worker.")
    run_parser.add_argument("--trace-batch-events", type=int, default=64,
                            help="Maximum events per asynchronous transport batch.")
    run_parser.add_argument("--trace-service-interval-ms", type=int, default=10,
                            help="Asynchronous worker partial-batch service interval.")
    run_parser.add_argument("--trace-transport-events", type=int, default=16_384,
                            help="Bounded asynchronous transport event capacity.")
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
    run_parser.add_argument(
        "--allow-unsupported-wsi-negative-test",
        action="store_true",
        help="Allow an unsupported/unknown manifest entry only for an "
             "intentional negative compatibility run.",
    )
    run_parser.add_argument("--prefix", help="Wine prefix path")
    run_parser.add_argument("--binary", help="Override the binary path for this run")
    run_parser.add_argument(
        "--timeout",
        type=float,
        help="Override timeout seconds; <= 0 disables unless the catalogue entry requires a positive timeout",
    )
    run_parser.add_argument("--pe-build-dir", help="PE build dir containing d3d9.dll")
    run_parser.add_argument("--runtime-pe-build-dir", help="builtin PE build dir containing runtime winemetal_dxmt9.dll")
    run_parser.add_argument("--wow64-pe-build-dir", help="32-bit PE build dir containing d3d9.dll")
    run_parser.add_argument("--wow64-runtime-pe-build-dir", help="builtin 32-bit PE build dir containing runtime winemetal_dxmt9.dll")
    run_parser.add_argument("--unix-build-dir", help="Unix build dir containing winemetal_dxmt9.so")
    run_parser.add_argument("--skip-stage", action="store_true", help="Do not stage dxmt9 into the Wine runtime/prefix")
    run_parser.add_argument(
        "--stage-mingw-runtime",
        action="store_true",
        help="With --skip-stage, only copy libc++/libunwind into the temp prefix",
    )
    run_parser.add_argument("--mingw-bin-dir", help="x86_64 llvm-mingw bin dir for runtime DLL staging")
    run_parser.add_argument("--wow64-mingw-bin-dir", help="i686 llvm-mingw bin dir for runtime DLL staging")
    run_parser.add_argument("--accept-reference", action="store_true", help="Create the reference image if it does not exist")
    run_parser.add_argument("--keep-temp-prefix", action="store_true", help="Keep the auto-created temp prefix after the run (default: delete it)")
    run_parser.add_argument("--output-suffix", help="Append a suffix to the output directory name")
    run_parser.add_argument("--capture-delay-sec", type=float, default=None, help="Override CATALOGUE capture_delay_sec for this run")
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
    if args.command == "trace-profiles":
        print(json.dumps({name: profile.frozen_config()
                          for name, profile in TRACE_PROFILES.items()}, indent=2, sort_keys=True))
        return 0

    app = next((item for item in apps if item.name == args.name), None)
    if app is None:
        print(f"unknown experiment: {args.name}", file=sys.stderr)
        return 2
    if app.require_positive_timeout and args.timeout is not None and args.timeout <= 0.0:
        print(
            f"{app.name}: --timeout must be positive; this app can hang during shutdown. "
            f"Omit --timeout to use the catalogue default ({app.run_timeout_sec:g}s) "
            "or pass a positive value.",
            file=sys.stderr,
        )
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
    try:
        return run_experiment(app, args)
    except FileNotFoundError as exc:
        print(str(exc), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
