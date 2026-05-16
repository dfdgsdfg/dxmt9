#!/usr/bin/env python3

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

from PIL import Image


REPO_ROOT = Path(__file__).resolve().parents[3]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from scripts.check import check_debug_result_schema as debug_result_schema  # noqa: E402


DEFAULT_WINDOW_TITLE = "dxmt9 WSI test"
ENV_KEYS = [
    "WINEDLLOVERRIDES",
    "DXMT_CAPTURE_FRAME",
    "DXMT_CAPTURE_FRAMES",
    "DXMT_CAPTURE_RANGE",
    "DXMT_EXPERIMENT_CAPTURE_DIR",
    "DXMT_EXPERIMENT_CAPTURE_PATH",
    "DXMT_LOG_LEVEL",
]
HWND_RE = re.compile(r"hwnd=(?P<hwnd>0x[0-9a-fA-F]+)")
FRAMES_RE = re.compile(r"OK: (?P<frames>\d+) frames presented without error")


def selected_environment(env: dict[str, str] | None = None) -> dict[str, str]:
    source = env or os.environ
    return {key: source[key] for key in ENV_KEYS if key in source}


def text_tail(value: str | bytes | None) -> str:
    if value is None:
        return ""
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace")[-8192:]
    return value[-8192:]


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def parse_wsi_stdout(stdout: str) -> dict[str, Any]:
    evidence: dict[str, Any] = {}
    hwnd_match = HWND_RE.search(stdout)
    if hwnd_match:
        evidence["hwnd"] = hwnd_match.group("hwnd")
    frames_match = FRAMES_RE.search(stdout)
    if frames_match:
        evidence["presented_frames"] = int(frames_match.group("frames"))
    elif "OK: reached frame" in stdout:
        reached = re.findall(r"OK: reached frame (\d+)", stdout)
        if reached:
            evidence["presented_frames"] = int(reached[-1])
    return evidence


def image_artifact(path: Path, *, root: Path, source: str) -> dict[str, Any]:
    with Image.open(path) as image:
        width, height = image.size
        pixel_format = image.mode
    return {
        "role": "capture",
        "path": path.relative_to(root).as_posix(),
        "format": path.suffix.lstrip(".") or "png",
        "source": source,
        "byte_size": path.stat().st_size,
        "hash": sha256_file(path),
        "width": width,
        "height": height,
        "pixel_format": pixel_format,
        "pitch": width * 4,
    }


def run_window_capture(args: argparse.Namespace, output_dir: Path) -> tuple[dict[str, Any] | None, str | None]:
    if args.capture_path is None and not args.capture_full_screen and not args.window_id:
        return None, None

    capture_path = args.capture_path
    if capture_path is None:
        capture_path = output_dir / "captures" / "wsi_present.png"
    capture_path = capture_path.expanduser()
    if not capture_path.is_absolute():
        capture_path = output_dir / capture_path
    capture_path.parent.mkdir(parents=True, exist_ok=True)

    if args.window_id:
        command = ["screencapture", "-x", "-l", str(args.window_id), str(capture_path)]
        source = "window_id"
    elif args.capture_full_screen:
        command = ["screencapture", "-x", str(capture_path)]
        source = "full_screen"
    else:
        return None, "capture requested without --window-id or --capture-full-screen"

    try:
        completed = subprocess.run(
            command,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=args.capture_timeout,
            check=False,
        )
    except subprocess.TimeoutExpired:
        return None, f"screencapture timed out after {args.capture_timeout}s"
    if completed.returncode != 0:
        detail = (completed.stderr or completed.stdout or "screencapture failed").strip()
        return None, detail
    if not capture_path.is_file():
        return None, "screencapture did not create an artifact"
    return image_artifact(capture_path, root=output_dir, source=source), None


def classify_failure(stdout: str, stderr: str, returncode: int | None) -> tuple[str, str, str]:
    combined = stdout + "\n" + stderr
    if returncode == 0:
        return "none", "unavailable", "runner does not acquire CAMetalLayer directly"
    if "CreateWindow failed" in combined:
        return "wsi-layer-acquisition", "acquisition_failure", "Win32 window creation failed"
    if "Direct3DCreate9 returned nullptr" in combined or "CreateDevice hr=" in combined:
        return "wsi-layer-acquisition", "acquisition_failure", "D3D9 device creation failed"
    if "Present hr=" in combined:
        return "wsi-visible-output", "acquisition_failure", "D3D9 Present failed"
    if returncode is None:
        return "wsi-visible-output", "unavailable", "runner timed out before WSI completion"
    return "wsi-visible-output", "unavailable", "runner exited before WSI completion"


def build_debug_result(
    *,
    command: list[str],
    environment: dict[str, str],
    window_title: str,
    failure_category: str,
    layer_acquisition: str,
    capture_source: str,
    reason_key: str,
    reason: str,
    artifacts: list[dict[str, Any]] | None = None,
    exit_code: int | None = None,
    hwnd: str | None = None,
    presented_frames: int | None = None,
    capture_error: str | None = None,
) -> dict[str, Any]:
    diagnostics_wsi = {
        "layer_acquisition": layer_acquisition,
        "window_title": window_title,
        "capture_source": capture_source,
        reason_key: reason,
    }
    if hwnd:
        diagnostics_wsi["hwnd"] = hwnd
    if capture_error:
        diagnostics_wsi["failure_reason"] = capture_error
    diagnostics = {
        "wsi": diagnostics_wsi,
        "wsi_runner": {
            "exit_code": exit_code,
            "window_title": window_title,
        },
    }
    if presented_frames is not None:
        diagnostics["wsi_runner"]["presented_frames"] = presented_frames
    if capture_source == "full_screen":
        diagnostics_wsi["visible_output_proves_layer"] = False

    return {
        "schema": debug_result_schema.SCHEMA,
        "module": "wsi-present",
        "boundary": "wsi-present",
        "command": command,
        "correlation": {
            "run_id": "wsi-present:non-catalogue",
        },
        "environment": environment,
        "artifacts": artifacts or [],
        "diagnostics": diagnostics,
        "failure_category": failure_category,
    }


def validate_or_raise(result: dict[str, Any]) -> None:
    errors = debug_result_schema.validate_result(result, require={"wsi"})
    if errors:
        raise ValueError("invalid WSI debug result: " + "; ".join(errors))


def emit_debug_result(result: dict[str, Any], output: Path | None) -> None:
    validate_or_raise(result)
    text = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if output is None:
        print(text, end="")
        return
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(text, encoding="utf-8")
    print(f"wrote {output}")


def scaffold_result(args: argparse.Namespace) -> int:
    result = build_debug_result(
        command=sys.argv,
        environment=selected_environment(),
        window_title=args.window_title,
        failure_category="wsi-layer-acquisition",
        layer_acquisition="unavailable",
        capture_source="unavailable",
        reason_key="unavailable_reason",
        reason="scaffold result only; WSI runtime lane not executed",
        exit_code=0,
    )
    emit_debug_result(result, args.output)
    return 0


def run(args: argparse.Namespace) -> int:
    env = os.environ.copy()
    if args.dll_overrides:
        env["WINEDLLOVERRIDES"] = args.dll_overrides

    command = [str(args.wine), str(args.exe)]
    artifacts: list[dict[str, Any]] = []
    output_dir = args.output.parent
    log_path = output_dir / "logs" / "wsi_present.log"
    capture_artifact: dict[str, Any] | None = None
    capture_error: str | None = None
    try:
        process = subprocess.Popen(
            command,
            cwd=args.exe.parent,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if args.capture_path is not None or args.capture_full_screen or args.window_id:
            time.sleep(max(0.0, args.capture_delay))
            capture_artifact, capture_error = run_window_capture(args, output_dir)
        stdout_value, stderr_value = process.communicate(timeout=args.timeout)
        stdout = stdout_value[-8192:]
        stderr = stderr_value[-8192:]
        exit_code: int | None = process.returncode
    except subprocess.TimeoutExpired as exc:
        process.kill()
        stdout_value, stderr_value = process.communicate()
        stdout = text_tail(exc.stdout)
        stderr = text_tail(exc.stderr)
        if not stdout and stdout_value:
            stdout = stdout_value[-8192:]
        if not stderr and stderr_value:
            stderr = stderr_value[-8192:]
        exit_code = None

    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_text(f"===== stdout =====\n{stdout}\n===== stderr =====\n{stderr}\n", encoding="utf-8")
    artifacts.append(
        {
            "role": "log",
            "path": log_path.relative_to(output_dir).as_posix(),
            "format": "text",
        }
    )
    if capture_artifact is not None:
        artifacts.append(capture_artifact)

    failure, layer, reason = classify_failure(stdout, stderr, exit_code)
    stdout_evidence = parse_wsi_stdout(stdout)
    layer_acquisition = args.layer_acquisition or layer
    capture_source = capture_artifact.get("source") if capture_artifact else args.capture_source
    if exit_code == 0 and capture_error:
        failure = "wsi-visible-output"
        reason = capture_error
    elif exit_code == 0 and capture_artifact is not None:
        failure = "none"
        reason = "live window capture artifact recorded"
    result = build_debug_result(
        command=command,
        environment=selected_environment(env),
        window_title=args.window_title,
        failure_category=failure,
        layer_acquisition=layer_acquisition,
        capture_source=capture_source,
        reason_key="unavailable_reason" if layer_acquisition == "unavailable" else "failure_reason",
        reason=reason,
        artifacts=artifacts,
        exit_code=exit_code,
        hwnd=args.hwnd or stdout_evidence.get("hwnd"),
        presented_frames=stdout_evidence.get("presented_frames"),
        capture_error=capture_error,
    )
    emit_debug_result(result, args.output)
    return 0 if exit_code == 0 else 1


def validate_result(path: Path) -> int:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except Exception as exc:
        print(f"invalid JSON: {exc}", file=sys.stderr)
        return 1
    errors = debug_result_schema.validate_result(data, require={"wsi"})
    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1
    print(f"WSI debug result ok: {path}")
    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="dxmt9 non-catalogue WSI present runner")
    subparsers = parser.add_subparsers(dest="command", required=True)

    scaffold = subparsers.add_parser("emit-scaffold-result")
    scaffold.add_argument("--output", type=Path)
    scaffold.add_argument("--window-title", default=DEFAULT_WINDOW_TITLE)

    run_parser = subparsers.add_parser("run")
    run_parser.add_argument("--wine", type=Path, required=True)
    run_parser.add_argument("--exe", type=Path, required=True)
    run_parser.add_argument("--output", type=Path, required=True)
    run_parser.add_argument("--window-title", default=DEFAULT_WINDOW_TITLE)
    run_parser.add_argument("--hwnd")
    run_parser.add_argument(
        "--layer-acquisition",
        choices=sorted(debug_result_schema.VALID_LAYER_ACQUISITION),
    )
    run_parser.add_argument("--dll-overrides")
    run_parser.add_argument("--timeout", type=int, default=15)
    run_parser.add_argument(
        "--capture-source",
        choices=sorted(debug_result_schema.VALID_CAPTURE_SOURCES),
        default="none",
    )
    run_parser.add_argument("--capture-path", type=Path)
    run_parser.add_argument("--window-id")
    run_parser.add_argument("--capture-full-screen", action="store_true")
    run_parser.add_argument("--capture-delay", type=float, default=1.0)
    run_parser.add_argument("--capture-timeout", type=int, default=5)

    validate = subparsers.add_parser("validate-result")
    validate.add_argument("path", type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.command == "emit-scaffold-result":
        return scaffold_result(args)
    if args.command == "run":
        args.wine = args.wine.expanduser().resolve()
        args.exe = args.exe.expanduser().resolve()
        return run(args)
    if args.command == "validate-result":
        return validate_result(args.path)
    raise AssertionError(args.command)


if __name__ == "__main__":
    raise SystemExit(main())
