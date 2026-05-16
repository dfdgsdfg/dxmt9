#!/usr/bin/env python3
"""Helpers for dxmt9 debug artifact bundles.

The helpers are intentionally file-system only: they do not run Wine, capture a
window, or inspect renderer internals. Harnesses use them after collecting
values or frames so the resulting artifacts share paths, hashes, limits, and
debug-result JSON shape.
"""

from __future__ import annotations

import hashlib
import json
import shutil
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from PIL import Image


BOUNDARY_DUMP_MANIFEST_SCHEMA = "dxmt9.boundary_dump.manifest.v1"
FRAME_SEQUENCE_MANIFEST_SCHEMA = "dxmt9.render_capture.frames.v1"


class ArtifactBundleError(ValueError):
    """Raised when a debug artifact would violate the bundle contract."""


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return f"sha256:{digest.hexdigest()}"


def _json_bytes(payload: Any) -> bytes:
    return json.dumps(payload, indent=2, sort_keys=True).encode("utf-8") + b"\n"


def _check_relative(path: str | Path, label: str) -> Path:
    value = Path(path)
    if value.is_absolute() or ".." in value.parts or "\\" in value.as_posix():
        raise ArtifactBundleError(f"{label} must stay under the result directory: {path}")
    return value


def _safe_component(value: str, label: str) -> str:
    if not value:
        raise ArtifactBundleError(f"{label} must be non-empty")
    cleaned = "".join(ch if ch.isalnum() or ch in {"-", "_", "."} else "_" for ch in value)
    if cleaned in {"", ".", ".."}:
        raise ArtifactBundleError(f"{label} produced an unsafe path component")
    return cleaned


def _relative_to(path: Path, root: Path) -> str:
    try:
        return path.relative_to(root).as_posix()
    except ValueError as exc:
        raise ArtifactBundleError(f"artifact escaped result directory: {path}") from exc


@dataclass
class ArtifactBudget:
    max_bytes: int | None = None
    used_bytes: int = 0

    def add(self, byte_count: int, label: str) -> None:
        if byte_count < 0:
            raise ArtifactBundleError(f"{label} byte count must be non-negative")
        next_count = self.used_bytes + byte_count
        if self.max_bytes is not None and next_count > self.max_bytes:
            raise ArtifactBundleError(
                f"{label} would exceed debug artifact byte budget "
                f"({next_count} > {self.max_bytes})"
            )
        self.used_bytes = next_count


@dataclass
class BoundaryDumpBundle:
    result_dir: Path
    run_id: str
    max_bytes: int | None = None
    dumps: list[dict[str, Any]] = field(default_factory=list)
    _budget: ArtifactBudget = field(init=False)

    def __post_init__(self) -> None:
        self.result_dir = self.result_dir.resolve()
        self._budget = ArtifactBudget(self.max_bytes)
        (self.result_dir / "boundary_dumps").mkdir(parents=True, exist_ok=True)

    def write_sidecar(
        self,
        relative_path: str | Path,
        data: bytes,
        *,
        semantic: str,
        layout_version: str,
        endianness: str = "little",
        kind: str | None = None,
        width: int | None = None,
        height: int | None = None,
        format: str | None = None,
        pitch: int | None = None,
    ) -> dict[str, Any]:
        rel = _check_relative(relative_path, "sidecar path")
        if not rel.parts or rel.parts[0] != "boundary_dumps":
            rel = Path("boundary_dumps") / rel
        path = self.result_dir / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        self._budget.add(len(data), rel.as_posix())
        path.write_bytes(data)
        record: dict[str, Any] = {
            "path": rel.as_posix(),
            "semantic": semantic,
            "layout_version": layout_version,
            "endianness": endianness,
            "byte_size": len(data),
            "hash": sha256_file(path),
        }
        if kind is not None:
            record["kind"] = kind
        if kind in {"texture", "frame"}:
            if width is None or height is None or format is None or pitch is None:
                raise ArtifactBundleError("texture/frame sidecars require width, height, format, and pitch")
            record.update({"width": width, "height": height, "format": format, "pitch": pitch})
        return record

    def add_dump(
        self,
        *,
        boundary: str,
        phase: str,
        schema: str,
        payload: dict[str, Any],
        correlation: dict[str, Any] | None = None,
        sidecars: list[dict[str, Any]] | None = None,
        name: str | None = None,
    ) -> dict[str, Any]:
        if phase not in {"before", "after", "derived"}:
            raise ArtifactBundleError(f"invalid boundary dump phase: {phase}")
        if not schema.startswith("dxmt9.boundary_dump."):
            raise ArtifactBundleError("boundary dump schema must start with dxmt9.boundary_dump.")

        boundary_component = _safe_component(boundary, "boundary")
        name_component = _safe_component(name or f"{boundary}_{phase}_{len(self.dumps):04d}", "dump name")
        rel = Path("boundary_dumps") / boundary_component / f"{name_component}.json"
        path = self.result_dir / rel
        path.parent.mkdir(parents=True, exist_ok=True)

        joined_correlation = {"run_id": self.run_id}
        if correlation:
            joined_correlation.update(correlation)

        document = {
            "schema": schema,
            "boundary": boundary,
            "phase": phase,
            "correlation": joined_correlation,
            "payload": payload,
            "sidecars": sidecars or [],
        }
        encoded = _json_bytes(document)
        self._budget.add(len(encoded), rel.as_posix())
        path.write_bytes(encoded)

        record = {
            "boundary": boundary,
            "phase": phase,
            "schema": schema,
            "path": rel.as_posix(),
            "sidecars": sidecars or [],
        }
        self.dumps.append(record)
        return record

    def write_manifest(self) -> dict[str, Any]:
        rel = Path("boundary_dumps") / "manifest.json"
        path = self.result_dir / rel
        document = {
            "schema": BOUNDARY_DUMP_MANIFEST_SCHEMA,
            "run_id": self.run_id,
            "total_byte_size": self._budget.used_bytes,
            "dumps": self.dumps,
        }
        encoded = _json_bytes(document)
        self._budget.add(len(encoded), rel.as_posix())
        path.write_bytes(encoded)
        return {
            "role": "boundary-dump-manifest",
            "path": rel.as_posix(),
            "format": "json",
        }

    def debug_diagnostics(self) -> dict[str, Any]:
        return {"dumps": list(self.dumps)}


@dataclass
class RenderCaptureBundle:
    result_dir: Path
    run_id: str
    mode: str
    source: str
    max_bytes: int | None = None
    start_frame: int | None = None
    end_frame: int | None = None
    interval: int | None = None
    frame_id: int | None = None
    requested_frames: list[int] | None = None
    frames: list[dict[str, Any]] = field(default_factory=list)
    dropped_frames: list[dict[str, Any]] = field(default_factory=list)
    videos: list[dict[str, Any]] = field(default_factory=list)
    _budget: ArtifactBudget = field(init=False)

    def __post_init__(self) -> None:
        self.result_dir = self.result_dir.resolve()
        self._budget = ArtifactBudget(self.max_bytes)
        (self.result_dir / "frames").mkdir(parents=True, exist_ok=True)
        (self.result_dir / "video").mkdir(parents=True, exist_ok=True)

    def copy_frame(
        self,
        source_path: Path,
        *,
        frame_id: int,
        capture_source: str | None = None,
        name: str | None = None,
    ) -> dict[str, Any]:
        if not source_path.is_file():
            raise ArtifactBundleError(f"frame source missing: {source_path}")
        name_component = _safe_component(name or f"frame{frame_id:06d}.png", "frame name")
        rel = Path("frames") / name_component
        if rel.suffix.lower() != ".png":
            rel = rel.with_suffix(".png")
        dest = self.result_dir / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        with Image.open(source_path) as source_image:
            source_image.save(dest)
        byte_size = dest.stat().st_size
        self._budget.add(byte_size, rel.as_posix())
        with Image.open(dest) as image:
            width, height = image.size
            pixel_format = image.mode
        record = {
            "frame_id": frame_id,
            "source": capture_source or self.source,
            "path": rel.as_posix(),
            "byte_size": byte_size,
            "hash": sha256_file(dest),
            "width": width,
            "height": height,
            "pixel_format": pixel_format,
        }
        self.frames.append(record)
        return record

    def add_dropped_frame(
        self,
        *,
        frame_id: int,
        source: str,
        reason: str,
        sidecar_path: str | None = None,
        counters: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        record = {
            "frame_id": frame_id,
            "source": source,
            "reason": reason,
        }
        if sidecar_path:
            record["sidecar_path"] = sidecar_path
        if counters:
            record["counters"] = counters
        self.dropped_frames.append(record)
        return record

    def add_video_segment(
        self,
        source_path: Path,
        *,
        source: str,
        container: str,
        codec: str,
        timebase: str,
        nominal_fps: float,
        width: int,
        height: int,
        start_frame: int | None = None,
        end_frame: int | None = None,
        start_time: float | None = None,
        end_time: float | None = None,
        acceptance: str = "triage",
        name: str | None = None,
    ) -> dict[str, Any]:
        if not source_path.is_file():
            raise ArtifactBundleError(f"video source missing: {source_path}")
        if (start_frame is None or end_frame is None) and (start_time is None or end_time is None):
            raise ArtifactBundleError("video segment requires frame span or time span")
        name_component = _safe_component(name or source_path.name, "video name")
        rel = Path("video") / name_component
        dest = self.result_dir / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source_path, dest)
        byte_size = dest.stat().st_size
        self._budget.add(byte_size, rel.as_posix())
        record: dict[str, Any] = {
            "path": rel.as_posix(),
            "source": source,
            "container": container,
            "codec": codec,
            "timebase": timebase,
            "nominal_fps": nominal_fps,
            "width": width,
            "height": height,
            "byte_size": byte_size,
            "hash": sha256_file(dest),
            "acceptance": acceptance,
        }
        if start_frame is not None and end_frame is not None:
            record.update({"start_frame": start_frame, "end_frame": end_frame})
        if start_time is not None and end_time is not None:
            record.update({"start_time": start_time, "end_time": end_time})
        self.videos.append(record)
        return record

    def render_capture_diagnostics(self) -> dict[str, Any]:
        capture: dict[str, Any] = {
            "mode": self.mode,
            "source": self.source,
            "frames": list(self.frames),
        }
        if self.dropped_frames:
            capture["dropped_frames"] = list(self.dropped_frames)
        if self.mode == "interval-range":
            capture.update({
                "start_frame": self.start_frame,
                "end_frame": self.end_frame,
                "interval": self.interval,
            })
        elif self.mode == "single-frame":
            capture["frame_id"] = self.frame_id
        elif self.mode == "frame-list":
            capture["requested_frames"] = self.requested_frames or [frame["frame_id"] for frame in self.frames]
        return {"render_capture": capture}

    def video_diagnostics(self) -> dict[str, Any]:
        if not self.videos:
            return {}
        return {"video_segments": list(self.videos)}

    def write_frame_manifest(self) -> dict[str, Any]:
        rel = Path("frames") / "manifest.json"
        path = self.result_dir / rel
        document = {
            "schema": FRAME_SEQUENCE_MANIFEST_SCHEMA,
            "run_id": self.run_id,
            "mode": self.mode,
            "source": self.source,
            "frames": self.frames,
            "dropped_frames": self.dropped_frames,
            "videos": self.videos,
        }
        encoded = _json_bytes(document)
        self._budget.add(len(encoded), rel.as_posix())
        path.write_bytes(encoded)
        return {
            "role": "frame-sequence-manifest",
            "path": rel.as_posix(),
            "format": "json",
        }

    def video_artifacts(self) -> list[dict[str, Any]]:
        return [
            {
                "role": "video-segment",
                "path": video["path"],
                "format": video["container"],
                "source": video["source"],
                "byte_size": video["byte_size"],
                "hash": video["hash"],
            }
            for video in self.videos
        ]


def merge_debug_sections(*sections: dict[str, Any]) -> dict[str, Any]:
    merged: dict[str, Any] = {}
    for section in sections:
        for key, value in section.items():
            if isinstance(value, list):
                merged.setdefault(key, []).extend(value)
            elif isinstance(value, dict) and isinstance(merged.get(key), dict):
                merged[key].update(value)
            else:
                merged[key] = value
    return merged
