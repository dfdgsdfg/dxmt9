#!/usr/bin/env python3
"""Validate dxmt9 debug / WSI / dump / capture result JSON.

The checker is intentionally lightweight: it validates the machine-readable
contract used by harnesses, but does not produce artifacts or run Wine.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any


SCHEMA = "dxmt9.debug.result.v1"

VALID_FAILURE_CATEGORIES = {
    "none",
    "env-registry-drift",
    "metal-capture",
    "metal-gpu-fault",
    "wsi-layer-acquisition",
    "wsi-visible-output",
    "wine-macdrv-symbols",
    "wine-provider-locator",
    "wine-abi-handshake",
    "headless-unsupported",
    "boundary-dump",
    "render-frame-sequence",
    "render-video-segment",
}

VALID_CAPTURE_SOURCES = {
    "internal_dump",
    "window_id",
    "frontmost_window",
    "full_screen",
    "manual_observation",
    "none",
    "unavailable",
}

WINDOW_CAPTURE_SOURCES = {
    "window_id",
    "frontmost_window",
    "full_screen",
    "manual_observation",
}

VALID_LAYER_ACQUISITION = {
    "macdrv_functions",
    "legacy_macdrv_get_cocoa_view",
    "no_window_fallback",
    "acquisition_failure",
    "unavailable",
}

LAYER_SUCCESS = {
    "macdrv_functions",
    "legacy_macdrv_get_cocoa_view",
}

VALID_PHASES = {"before", "after", "derived"}
VALID_RENDER_MODES = {
    "single-frame",
    "frame-list",
    "interval-range",
    "event-window",
}

BINARY_ARTIFACT_ROLES = {
    "chunk-blob",
    "buffer",
    "texture-slice",
    "frame-image",
    "capture",
    "video-segment",
}

IMAGE_ARTIFACT_ROLES = {"texture-slice", "frame-image", "capture"}
JOIN_KEYS = {
    "frame_id",
    "present_id",
    "seq_id",
    "chunk_id",
    "record_index",
    "draw_index",
    "resource_handle",
    "shader_handle",
    "command_buffer_id",
}


def load_json(path: Path) -> Any:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def require_dict(value: Any, label: str, errors: list[str]) -> dict[str, Any] | None:
    if not isinstance(value, dict):
        errors.append(f"{label} must be an object")
        return None
    return value


def require_nonempty_string(table: dict[str, Any], key: str, label: str, errors: list[str]) -> str | None:
    value = table.get(key)
    if not isinstance(value, str) or not value:
        errors.append(f"{label}.{key} must be a non-empty string")
        return None
    return value


def optional_string(table: dict[str, Any], key: str) -> str | None:
    value = table.get(key)
    return value if isinstance(value, str) and value else None


def require_relative_path(table: dict[str, Any], key: str, label: str, errors: list[str]) -> str | None:
    value = require_nonempty_string(table, key, label, errors)
    if value is None:
        return None
    path = Path(value)
    if path.is_absolute() or ".." in path.parts or "\\" in value:
        errors.append(f"{label}.{key} must be a relative artifact path")
    return value


def require_int(table: dict[str, Any], key: str, label: str, errors: list[str]) -> int | None:
    value = table.get(key)
    if not isinstance(value, int) or isinstance(value, bool):
        errors.append(f"{label}.{key} must be an integer")
        return None
    return value


def require_number(table: dict[str, Any], key: str, label: str, errors: list[str]) -> float | None:
    value = table.get(key)
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        errors.append(f"{label}.{key} must be a number")
        return None
    return float(value)


def validate_capture_source(value: Any, label: str, errors: list[str]) -> str | None:
    if not isinstance(value, str) or not value:
        errors.append(f"{label} must be a non-empty string")
        return None
    if value not in VALID_CAPTURE_SOURCES:
        errors.append(
            f"{label} has invalid source {value!r}; expected one of "
            f"{', '.join(sorted(VALID_CAPTURE_SOURCES))}"
        )
    return value


def validate_correlation(
    correlation: Any,
    label: str,
    errors: list[str],
    *,
    require_join_key: bool,
) -> None:
    if correlation is None:
        errors.append(f"{label} is required")
        return
    table = require_dict(correlation, label, errors)
    if table is None:
        return
    require_nonempty_string(table, "run_id", label, errors)
    if require_join_key and not any(key in table for key in JOIN_KEYS):
        errors.append(
            f"{label} must include at least one join key: "
            f"{', '.join(sorted(JOIN_KEYS))}"
        )


def validate_artifacts(result: dict[str, Any], errors: list[str]) -> None:
    artifacts = result.get("artifacts")
    if not isinstance(artifacts, list):
        errors.append("artifacts must be a list")
        return

    for index, artifact in enumerate(artifacts):
        label = f"artifacts[{index}]"
        table = require_dict(artifact, label, errors)
        if table is None:
            continue
        role = require_nonempty_string(table, "role", label, errors)
        require_relative_path(table, "path", label, errors)
        require_nonempty_string(table, "format", label, errors)

        if "source" in table:
            validate_capture_source(table["source"], f"{label}.source", errors)

        if role in BINARY_ARTIFACT_ROLES:
            require_int(table, "byte_size", label, errors)
            require_nonempty_string(table, "hash", label, errors)

        if role in IMAGE_ARTIFACT_ROLES:
            require_int(table, "width", label, errors)
            require_int(table, "height", label, errors)
            require_nonempty_string(table, "pixel_format", label, errors)
            require_int(table, "pitch", label, errors)


def validate_limits(result: dict[str, Any], errors: list[str], *, require: set[str]) -> None:
    diagnostics = result.get("diagnostics")
    if not isinstance(diagnostics, dict):
        diagnostics = {}
    needs_limits = bool(
        "dumps" in require
        or "render-capture" in require
        or "video" in require
        or diagnostics.get("dumps")
        or diagnostics.get("render_capture")
        or diagnostics.get("video_segments")
    )
    if not needs_limits:
        return

    limits = result.get("limits")
    table = require_dict(limits, "limits", errors)
    if table is None:
        return
    require_int(table, "max_bytes", "limits", errors)
    if "render-capture" in require or diagnostics.get("render_capture"):
        require_int(table, "max_frames", "limits", errors)
    if "video" in require or diagnostics.get("video_segments"):
        require_number(table, "max_duration_sec", "limits", errors)


def validate_wsi(wsi: Any, errors: list[str], *, headless_active: bool) -> None:
    table = require_dict(wsi, "diagnostics.wsi", errors)
    if table is None:
        return

    source = None
    if "capture_source" in table:
        source = validate_capture_source(
            table["capture_source"],
            "diagnostics.wsi.capture_source",
            errors,
        )
        if (
            source == "full_screen"
            and table.get("visible_output_proves_layer") is not False
        ):
            errors.append(
                "diagnostics.wsi.visible_output_proves_layer must be false "
                "when capture_source is full_screen"
            )

    layer = require_nonempty_string(table, "layer_acquisition", "diagnostics.wsi", errors)
    if layer and layer not in VALID_LAYER_ACQUISITION:
        errors.append(f"diagnostics.wsi.layer_acquisition has invalid value {layer!r}")

    has_window_identity = bool(
        optional_string(table, "hwnd")
        or optional_string(table, "window_title")
        or optional_string(table, "unavailable_reason")
        or optional_string(table, "failure_reason")
    )
    if not has_window_identity:
        errors.append(
            "diagnostics.wsi must include hwnd, window_title, "
            "unavailable_reason, or failure_reason"
        )

    if headless_active:
        if layer in LAYER_SUCCESS:
            errors.append("headless diagnostics must not claim CAMetalLayer acquisition")
        if source in WINDOW_CAPTURE_SOURCES:
            errors.append("headless diagnostics must not claim window-capture evidence")


def validate_headless(diagnostics: dict[str, Any], result: dict[str, Any], errors: list[str]) -> bool:
    headless = diagnostics.get("headless")
    if headless is None:
        return False
    table = require_dict(headless, "diagnostics.headless", errors)
    if table is None:
        return False

    active = table.get("active")
    if not isinstance(active, bool):
        errors.append("diagnostics.headless.active must be boolean")
        return False
    if not active:
        return False

    if not optional_string(table, "reason"):
        errors.append("diagnostics.headless.reason is required when active is true")

    wsi = diagnostics.get("wsi")
    if isinstance(wsi, dict):
        layer = wsi.get("layer_acquisition")
        if layer in LAYER_SUCCESS:
            errors.append("headless result must not claim WSI layer acquisition")
        source = wsi.get("capture_source")
        if source in WINDOW_CAPTURE_SOURCES:
            errors.append("headless result must not claim window-capture evidence")

    for index, artifact in enumerate(result.get("artifacts") or []):
        if isinstance(artifact, dict) and artifact.get("source") in WINDOW_CAPTURE_SOURCES:
            errors.append(
                f"artifacts[{index}].source must not claim window capture "
                "when diagnostics.headless.active is true"
            )

    return True


def validate_dumps(dumps: Any, result: dict[str, Any], errors: list[str]) -> None:
    if not isinstance(dumps, list) or not dumps:
        errors.append("diagnostics.dumps must be a non-empty list")
        return

    validate_correlation(
        result.get("correlation"),
        "correlation",
        errors,
        require_join_key=True,
    )

    for index, dump in enumerate(dumps):
        label = f"diagnostics.dumps[{index}]"
        table = require_dict(dump, label, errors)
        if table is None:
            continue
        require_nonempty_string(table, "boundary", label, errors)
        phase = require_nonempty_string(table, "phase", label, errors)
        if phase and phase not in VALID_PHASES:
            errors.append(f"{label}.phase must be one of {sorted(VALID_PHASES)}")
        schema = require_nonempty_string(table, "schema", label, errors)
        if schema and not schema.startswith("dxmt9.boundary_dump."):
            errors.append(f"{label}.schema must start with 'dxmt9.boundary_dump.'")
        require_relative_path(table, "path", label, errors)

        sidecars = table.get("sidecars", [])
        if sidecars is None:
            sidecars = []
        if not isinstance(sidecars, list):
            errors.append(f"{label}.sidecars must be a list when present")
            continue
        for sidecar_index, sidecar in enumerate(sidecars):
            sidecar_label = f"{label}.sidecars[{sidecar_index}]"
            sidecar_table = require_dict(sidecar, sidecar_label, errors)
            if sidecar_table is None:
                continue
            require_relative_path(sidecar_table, "path", sidecar_label, errors)
            require_nonempty_string(sidecar_table, "semantic", sidecar_label, errors)
            require_nonempty_string(sidecar_table, "layout_version", sidecar_label, errors)
            require_nonempty_string(sidecar_table, "endianness", sidecar_label, errors)
            require_int(sidecar_table, "byte_size", sidecar_label, errors)
            require_nonempty_string(sidecar_table, "hash", sidecar_label, errors)
            if sidecar_table.get("kind") in {"texture", "frame"}:
                require_int(sidecar_table, "width", sidecar_label, errors)
                require_int(sidecar_table, "height", sidecar_label, errors)
                require_nonempty_string(sidecar_table, "format", sidecar_label, errors)
                require_int(sidecar_table, "pitch", sidecar_label, errors)


def validate_render_capture(render: Any, errors: list[str]) -> None:
    table = require_dict(render, "diagnostics.render_capture", errors)
    if table is None:
        return
    mode = require_nonempty_string(table, "mode", "diagnostics.render_capture", errors)
    if mode and mode not in VALID_RENDER_MODES:
        errors.append(f"diagnostics.render_capture.mode has invalid value {mode!r}")

    if "source" in table:
        validate_capture_source(
            table["source"],
            "diagnostics.render_capture.source",
            errors,
        )

    if mode == "interval-range":
        start = require_int(table, "start_frame", "diagnostics.render_capture", errors)
        end = require_int(table, "end_frame", "diagnostics.render_capture", errors)
        interval = require_int(table, "interval", "diagnostics.render_capture", errors)
        if start is not None and end is not None and end < start:
            errors.append("diagnostics.render_capture.end_frame must be >= start_frame")
        if interval is not None and interval <= 0:
            errors.append("diagnostics.render_capture.interval must be > 0")
    elif mode == "frame-list":
        frames = table.get("requested_frames")
        if not isinstance(frames, list) or not frames:
            errors.append("diagnostics.render_capture.requested_frames must be a non-empty list")
    elif mode == "event-window":
        require_int(table, "anchor_present_id", "diagnostics.render_capture", errors)
        require_int(table, "before_frames", "diagnostics.render_capture", errors)
        require_int(table, "after_frames", "diagnostics.render_capture", errors)
    elif mode == "single-frame":
        require_int(table, "frame_id", "diagnostics.render_capture", errors)

    frames = table.get("frames")
    if not isinstance(frames, list) or not frames:
        errors.append("diagnostics.render_capture.frames must list every captured frame")
    else:
        for index, frame in enumerate(frames):
            label = f"diagnostics.render_capture.frames[{index}]"
            frame_table = require_dict(frame, label, errors)
            if frame_table is None:
                continue
            if "frame_id" not in frame_table and "frame" not in frame_table:
                errors.append(f"{label} must include frame_id or frame")
            if "source" not in frame_table:
                errors.append(f"{label}.source is required")
            else:
                validate_capture_source(frame_table["source"], f"{label}.source", errors)
            require_relative_path(frame_table, "path", label, errors)
            require_int(frame_table, "byte_size", label, errors)
            require_nonempty_string(frame_table, "hash", label, errors)

    dropped = table.get("dropped_frames", [])
    if dropped is None:
        dropped = []
    if not isinstance(dropped, list):
        errors.append("diagnostics.render_capture.dropped_frames must be a list when present")
        return
    for index, frame in enumerate(dropped):
        label = f"diagnostics.render_capture.dropped_frames[{index}]"
        frame_table = require_dict(frame, label, errors)
        if frame_table is None:
            continue
        require_int(frame_table, "frame_id", label, errors)
        validate_capture_source(frame_table.get("source"), f"{label}.source", errors)
        require_nonempty_string(frame_table, "reason", label, errors)


def validate_video_segments(segments: Any, errors: list[str]) -> None:
    if not isinstance(segments, list) or not segments:
        errors.append("diagnostics.video_segments must be a non-empty list")
        return

    for index, segment in enumerate(segments):
        label = f"diagnostics.video_segments[{index}]"
        table = require_dict(segment, label, errors)
        if table is None:
            continue
        require_relative_path(table, "path", label, errors)
        validate_capture_source(table.get("source"), f"{label}.source", errors)
        require_nonempty_string(table, "container", label, errors)
        require_nonempty_string(table, "codec", label, errors)
        require_nonempty_string(table, "timebase", label, errors)
        require_number(table, "nominal_fps", label, errors)
        require_int(table, "width", label, errors)
        require_int(table, "height", label, errors)
        require_int(table, "byte_size", label, errors)
        require_nonempty_string(table, "hash", label, errors)
        has_frame_span = "start_frame" in table and "end_frame" in table
        has_time_span = "start_time" in table and "end_time" in table
        if not has_frame_span and not has_time_span:
            errors.append(f"{label} must include start/end frame or start/end time")
        if has_frame_span:
            start = require_int(table, "start_frame", label, errors)
            end = require_int(table, "end_frame", label, errors)
            if start is not None and end is not None and end < start:
                errors.append(f"{label}.end_frame must be >= start_frame")
        if has_time_span:
            start_time = require_number(table, "start_time", label, errors)
            end_time = require_number(table, "end_time", label, errors)
            if start_time is not None and end_time is not None and end_time < start_time:
                errors.append(f"{label}.end_time must be >= start_time")
        acceptance = require_nonempty_string(table, "acceptance", label, errors)
        if acceptance and acceptance not in {"triage", "extractable_frames", "human_review"}:
            errors.append(f"{label}.acceptance has invalid value {acceptance!r}")


def validate_result(data: Any, *, require: set[str] | None = None) -> list[str]:
    require = require or set()
    errors: list[str] = []
    result = require_dict(data, "result", errors)
    if result is None:
        return errors

    if result.get("schema") != SCHEMA:
        errors.append(f"schema must be {SCHEMA!r}")
    require_nonempty_string(result, "module", "result", errors)

    command = result.get("command")
    if (
        not isinstance(command, list)
        or not command
        or any(not isinstance(item, str) or not item for item in command)
    ):
        errors.append("command must be a non-empty string list")

    environment = result.get("environment")
    if not isinstance(environment, dict):
        errors.append("environment must be an object")
    elif any(not isinstance(key, str) or not key for key in environment):
        errors.append("environment keys must be non-empty strings")

    failure = result.get("failure_category")
    if not isinstance(failure, str) or failure not in VALID_FAILURE_CATEGORIES:
        errors.append(
            "failure_category must be one of "
            f"{', '.join(sorted(VALID_FAILURE_CATEGORIES))}"
        )

    diagnostics = result.get("diagnostics")
    diagnostics_table = require_dict(diagnostics, "diagnostics", errors)
    if diagnostics_table is None:
        diagnostics_table = {}

    validate_artifacts(result, errors)
    headless_active = validate_headless(diagnostics_table, result, errors)

    if "wsi" in require and "wsi" not in diagnostics_table:
        errors.append("diagnostics.wsi is required")
    if "wsi" in diagnostics_table:
        validate_wsi(diagnostics_table["wsi"], errors, headless_active=headless_active)

    if "headless" in require and "headless" not in diagnostics_table:
        errors.append("diagnostics.headless is required")
    if "dumps" in require and "dumps" not in diagnostics_table:
        errors.append("diagnostics.dumps is required")
    if "dumps" in diagnostics_table:
        validate_dumps(diagnostics_table["dumps"], result, errors)

    if "render-capture" in require and "render_capture" not in diagnostics_table:
        errors.append("diagnostics.render_capture is required")
    if "render_capture" in diagnostics_table:
        validate_render_capture(diagnostics_table["render_capture"], errors)

    if "video" in require and "video_segments" not in diagnostics_table:
        errors.append("diagnostics.video_segments is required")
    if "video_segments" in diagnostics_table:
        validate_video_segments(diagnostics_table["video_segments"], errors)

    validate_limits(result, errors, require=require)
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Validate dxmt9 debug result JSON schema."
    )
    parser.add_argument("result", nargs="+", type=Path)
    parser.add_argument(
        "--require",
        action="append",
        default=[],
        choices=["wsi", "headless", "dumps", "render-capture", "video"],
        help="Require a schema section in addition to validating present sections.",
    )
    args = parser.parse_args()

    had_errors = False
    for path in args.result:
        try:
            errors = validate_result(load_json(path), require=set(args.require))
        except Exception as exc:
            errors = [str(exc)]
        if errors:
            had_errors = True
            for error in errors:
                print(f"{path}: {error}", file=sys.stderr)
        else:
            print(f"debug result schema ok: {path}")

    return 1 if had_errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
