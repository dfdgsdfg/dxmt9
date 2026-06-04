#!/usr/bin/env python3
"""Join mini-replay semantic bisection results with draw payload features.

The tool consumes a 3DMark05 mini-replay manifest plus the per-draw image
comparison CSV from a single-draw bisection. It reports whether semantic
failures can be isolated by broad state, shader, geometry, and payload hashes
before considering a runtime optimization predicate.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import itertools
import json
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Iterable

import numpy as np
from PIL import Image


REPO_ROOT = Path(__file__).resolve().parents[2]


BROAD_STATE_FIELDS = (
    "primitive_type",
    "index_count",
    "primitive_count",
    "index_type",
    "base_vertex",
    "stream0_offset",
    "stream0_stride",
    "texture_mask",
    "color_write",
    "alpha_blend",
    "src_blend",
    "dst_blend",
    "blend_op",
    "separate_alpha",
    "src_blend_alpha",
    "dst_blend_alpha",
    "blend_op_alpha",
    "alpha_test",
    "depth_enabled",
    "depth_write",
    "depth_func",
    "scissor",
    "scissor_l",
    "scissor_t",
    "scissor_r",
    "scissor_b",
    "cull",
    "fill",
)


STATE_SELECTOR_FIELDS = tuple(dict.fromkeys(BROAD_STATE_FIELDS + ("start_index",)))
GEOMETRY_SELECTOR_FIELDS = (
    "unique_indices",
    "cache_miss_64",
    "index_bytes",
    "stream0_bytes",
    "min_index",
    "max_index",
)
SHADER_SELECTOR_FIELDS = (
    "vs_hash",
    "ps_hash",
    "vsout",
    "ps_vsout_read_fields",
    "row_vs_hash",
    "row_ps_hash",
)

STATE_CSV_FIELDS = tuple(f"state_{field}" for field in BROAD_STATE_FIELDS + ("start_index",))
GEOMETRY_CSV_FIELDS = tuple(f"geometry_{field}" for field in GEOMETRY_SELECTOR_FIELDS)
SHADER_CSV_FIELDS = tuple(f"shader_{field}" for field in SHADER_SELECTOR_FIELDS)
RUNTIME_PROBE_HASH_FIELDS = (
    "vs_constants_hash",
    "ps_constants_hash",
    "uniform_payload_hash",
)
RUNTIME_PROBE_CSV_FIELDS = tuple(f"runtime_{field}" for field in RUNTIME_PROBE_HASH_FIELDS)


CSV_FIELDS = (
    "draw_index",
    "encoder_draw_index",
    "draw_ordinal",
    "semantic_status",
    "compare_returncode",
    "changed_pixels",
    "changed_pct",
    "max_delta",
    "ssim",
    "active_pixels_before",
    "active_pixels_after",
    "active_pct_before",
    "active_pct_after",
    "max_active_pixels",
    "visibility_class",
    "active_bbox_before",
    "active_bbox_after",
    "changed_bbox",
    "original_lru32",
    "cacheopt_lru32",
    "lru32_delta",
    "lru32_delta_pct",
    "broad_group_id",
    "broad_group_status",
    "broad_group_pass_draws",
    "broad_group_fail_draws",
    "index_hash",
    "stream0_hash",
    "extra_stream_hashes",
    "vsconsts_hash",
    "psconsts_hash",
    "ffpvs_hash",
    "ffpps_hash",
    "primitive_id_available",
    "primitive_identity_changed_pixels",
    "primitive_identity_changed_bbox",
    "color_changed_pixels",
    "color_and_primitive_changed_pixels",
    "color_change_primitive_overlap_pct",
    "primitive_owner_risk",
    "final_writer_oracle_status",
    "final_writer_oracle_action",
    *RUNTIME_PROBE_CSV_FIELDS,
    *STATE_CSV_FIELDS,
    *GEOMETRY_CSV_FIELDS,
    *SHADER_CSV_FIELDS,
    "shader_key",
    "state_key",
)


PRIMITIVE_MD_LABELS = {
    "primitive_identity_changed_pixels": "Primitive identity changed pixels",
    "primitive_identity_changed_bbox": "Primitive identity changed bbox",
    "color_changed_pixels": "Color changed pixels",
    "color_and_primitive_changed_pixels": "Color + primitive changed pixels",
}


def as_int(value: Any, default: int = 0) -> int:
    text = str(value or "").strip()
    if not text:
        return default
    try:
        return int(text, 0)
    except ValueError:
        try:
            return int(float(text))
        except ValueError:
            return default


def as_float(value: Any, default: float = 0.0) -> float:
    text = str(value or "").strip()
    if not text:
        return default
    try:
        return float(text)
    except ValueError:
        return default


def fmt_int(value: int) -> str:
    return f"{value:,}"


def fmt_pct(value: float | None) -> str:
    if value is None:
        return "n/a"
    return f"{value:+.4f}%"


def pct_delta(after: int, before: int) -> float | None:
    if before == 0:
        return None
    return (after - before) / before * 100.0


def fmt_maybe_pct(value: float | None) -> str:
    if value is None:
        return "n/a"
    return f"{value:.2f}%"


def resolve_path(path: str | Path) -> Path:
    value = Path(path)
    return value if value.is_absolute() else REPO_ROOT / value


def load_manifest(path: Path) -> dict[str, Any]:
    with path.open(encoding="utf-8") as handle:
        data = json.load(handle)
    if data.get("schema") != "dxmt9.3dmark05.mini_replay_manifest.v1":
        raise SystemExit(f"unsupported manifest schema: {data.get('schema')}")
    if not isinstance(data.get("draws"), list) or not data["draws"]:
        raise SystemExit("manifest has no draws")
    return data


def load_summary(path: Path) -> dict[int, dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))
    out: dict[int, dict[str, str]] = {}
    for row in rows:
        out[as_int(row.get("draw_index"))] = row
    return out


def clean_markdown_value(value: str) -> str:
    text = value.strip()
    if text.startswith("`") and text.endswith("`"):
        text = text[1:-1]
    return text.replace(",", "").strip()


def load_primitive_summary(single_draw_dir: Path, draw_index: int) -> dict[str, str]:
    prefix = single_draw_dir / f"draw{draw_index:03d}-primitive-id"
    csv_path = single_draw_dir / f"draw{draw_index:03d}-primitive-id-summary.csv"
    if csv_path.exists():
        with csv_path.open(newline="", encoding="utf-8") as handle:
            rows = list(csv.DictReader(handle))
        return rows[0] if rows else {}

    md_path = single_draw_dir / f"draw{draw_index:03d}-primitive-id-analysis.md"
    if not md_path.exists():
        return {}
    text = md_path.read_text(encoding="utf-8")
    out: dict[str, str] = {}
    for key, label in PRIMITIVE_MD_LABELS.items():
        match = re.search(rf"^\|\s*{re.escape(label)}\s*\|\s*(.*?)\s*\|$", text, re.MULTILINE)
        if match:
            out[key] = clean_markdown_value(match.group(1))
    if out:
        out.setdefault("primitive_id_report", str(prefix) + "-analysis.md")
    return out


def short_hash(path_text: str | None) -> str:
    if not path_text:
        return ""
    path = resolve_path(path_text)
    if not path.exists() or not path.is_file():
        return ""
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    return digest[:16]


def key_digest(parts: Iterable[tuple[str, str]]) -> str:
    digest = hashlib.sha256()
    for key, value in parts:
        digest.update(key.encode("utf-8"))
        digest.update(b"=")
        digest.update(value.encode("utf-8"))
        digest.update(b"\0")
    return digest.hexdigest()[:12]


def load_rgb(path: Path) -> np.ndarray | None:
    if not path.exists():
        return None
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.uint8)


def bbox_text(mask: np.ndarray) -> str:
    ys, xs = np.nonzero(mask)
    if len(xs) == 0:
        return ""
    return f"{int(xs.min())},{int(ys.min())}-{int(xs.max())},{int(ys.max())}"


@dataclass
class ImageStats:
    active_pixels: int = 0
    total_pixels: int = 0
    active_bbox: str = ""
    changed_bbox: str = ""

    @property
    def active_pct(self) -> float:
        if self.total_pixels == 0:
            return 0.0
        return self.active_pixels / self.total_pixels * 100.0


def image_stats(before_path: Path, after_path: Path, active_threshold: int) -> tuple[ImageStats, ImageStats]:
    before = load_rgb(before_path)
    after = load_rgb(after_path)
    if before is None:
        return ImageStats(), ImageStats()
    before_active = np.max(before, axis=2) > active_threshold
    before_stats = ImageStats(
        active_pixels=int(np.count_nonzero(before_active)),
        total_pixels=int(before_active.size),
        active_bbox=bbox_text(before_active),
    )
    if after is None:
        return before_stats, ImageStats()
    if before.shape != after.shape:
        return before_stats, ImageStats()
    after_active = np.max(after, axis=2) > active_threshold
    delta = np.max(np.abs(after.astype(np.int16) - before.astype(np.int16)), axis=2) > 0
    after_stats = ImageStats(
        active_pixels=int(np.count_nonzero(after_active)),
        total_pixels=int(after_active.size),
        active_bbox=bbox_text(after_active),
        changed_bbox=bbox_text(delta),
    )
    return before_stats, after_stats


def shader_key(draw: dict[str, Any]) -> str:
    shaders = draw.get("shaders", {})
    return "|".join([
        str(shaders.get("vs_hash", "")),
        str(shaders.get("ps_hash", "")),
        str(shaders.get("vsout", "")),
        str(shaders.get("ps_vsout_read_fields", "")),
    ])


def state_key(draw: dict[str, Any]) -> str:
    state = draw.get("state", {})
    return "|".join(f"{field}={state.get(field, '')}" for field in BROAD_STATE_FIELDS)


def extra_stream_hashes(draw: dict[str, Any]) -> str:
    hashes: list[str] = []
    for stream in draw.get("geometry", {}).get("streams", []):
        if as_int(stream.get("stream")) <= 0:
            continue
        hashes.append(f"s{as_int(stream.get('stream'))}:{short_hash(stream.get('file'))}")
    return ";".join(hashes)


@dataclass
class DrawAnalysis:
    draw_index: int
    manifest_draw: dict[str, Any]
    summary: dict[str, str]
    before_stats: ImageStats
    after_stats: ImageStats
    index_hash: str
    stream0_hash: str
    extra_stream_hashes: str
    vsconsts_hash: str
    psconsts_hash: str
    ffpvs_hash: str
    ffpps_hash: str
    runtime_probe: dict[str, str]
    primitive_summary: dict[str, str]
    broad_key: tuple[tuple[str, str], ...]
    sparse_active_pixels: int = 32
    broad_group_id: str = ""
    broad_group_status: str = ""
    broad_group_pass_draws: int = 0
    broad_group_fail_draws: int = 0

    @property
    def encoder_draw_index(self) -> int:
        return as_int(self.manifest_draw.get("encoder_draw_index"))

    @property
    def draw_ordinal(self) -> int:
        return as_int(self.manifest_draw.get("draw_ordinal"))

    @property
    def changed_pixels(self) -> int:
        return as_int(self.summary.get("changed_pixels"))

    @property
    def compare_returncode(self) -> int:
        return as_int(self.summary.get("compare_returncode"))

    @property
    def semantic_status(self) -> str:
        if self.compare_returncode == 0 and self.changed_pixels == 0:
            return "pass"
        return "fail"

    @property
    def lru32_delta(self) -> int:
        return as_int(self.summary.get("lru32_delta"))

    @property
    def max_active_pixels(self) -> int:
        return max(self.before_stats.active_pixels, self.after_stats.active_pixels)

    @property
    def visibility_class(self) -> str:
        if self.semantic_status != "pass":
            if self.max_active_pixels <= self.sparse_active_pixels:
                return "sparse-fail"
            return "visible-fail"
        if self.before_stats.active_pixels == 0 and self.after_stats.active_pixels == 0:
            return "no-final-color-exact-pass"
        if self.max_active_pixels <= self.sparse_active_pixels:
            return "sparse-exact-pass"
        return "visible-exact-pass"

    @property
    def primitive_id_available(self) -> bool:
        return bool(self.primitive_summary)

    @property
    def primitive_identity_changed_pixels(self) -> int:
        return as_int(self.primitive_summary.get("primitive_identity_changed_pixels"))

    @property
    def color_changed_pixels(self) -> int:
        return as_int(self.primitive_summary.get("color_changed_pixels"))

    @property
    def color_and_primitive_changed_pixels(self) -> int:
        return as_int(self.primitive_summary.get("color_and_primitive_changed_pixels"))

    @property
    def color_change_primitive_overlap_pct(self) -> float | None:
        if not self.color_changed_pixels:
            return None
        return self.color_and_primitive_changed_pixels / self.color_changed_pixels * 100.0

    @property
    def primitive_owner_risk(self) -> str:
        if not self.primitive_id_available:
            return "unknown"
        if self.color_changed_pixels and self.color_and_primitive_changed_pixels == self.color_changed_pixels:
            return "color-change-follows-primitive-owner"
        if self.color_changed_pixels:
            return "color-change-not-fully-owner-explained"
        if self.primitive_identity_changed_pixels:
            return "primitive-owner-changed-color-stable"
        return "primitive-owner-stable"

    @property
    def final_writer_oracle_status(self) -> str:
        if self.semantic_status != "pass":
            if self.primitive_owner_risk == "color-change-follows-primitive-owner":
                return "final-writer-color-hazard"
            if self.color_changed_pixels:
                return "final-color-hazard"
            return "semantic-fail-no-final-writer-proof"
        if self.primitive_owner_risk == "primitive-owner-changed-color-stable":
            return "owner-change-color-stable"
        if self.visibility_class == "no-final-color-exact-pass":
            return "no-final-color-positive-control"
        if self.visibility_class == "sparse-exact-pass":
            return "sparse-positive-control"
        if self.visibility_class == "visible-exact-pass":
            return "visible-final-color-stable"
        return "exact-pass"

    @property
    def final_writer_oracle_action(self) -> str:
        status = self.final_writer_oracle_status
        if status == "final-writer-color-hazard":
            return "reject reorder unless a final-writer selector excludes this draw"
        if status == "final-color-hazard":
            return "reject reorder unless a final-color selector excludes this draw"
        if status == "semantic-fail-no-final-writer-proof":
            return "requires stronger primitive-id/final-color analysis before promotion"
        if status == "owner-change-color-stable":
            return "owner-change reject would overreject; needs final-color oracle"
        if status == "no-final-color-positive-control":
            return "safe replay evidence only; needs runtime occlusion/no-final-color predicate"
        if status == "sparse-positive-control":
            return "safe replay evidence only; too little final-color coverage alone"
        if status == "visible-final-color-stable":
            return "candidate useful movement only if runtime selector excludes hazards"
        return "exact-pass evidence"

    def csv_row(self) -> dict[str, str]:
        state = self.manifest_draw.get("state", {})
        geometry = self.manifest_draw.get("geometry", {})
        shaders = self.manifest_draw.get("shaders", {})
        row = {
            "draw_index": str(self.draw_index),
            "encoder_draw_index": str(self.encoder_draw_index),
            "draw_ordinal": str(self.draw_ordinal),
            "semantic_status": self.semantic_status,
            "compare_returncode": str(self.compare_returncode),
            "changed_pixels": str(self.changed_pixels),
            "changed_pct": f"{as_float(self.summary.get('changed_pct')):.6f}",
            "max_delta": str(as_int(self.summary.get("max_delta"))),
            "ssim": f"{as_float(self.summary.get('ssim'), 1.0):.6f}",
            "active_pixels_before": str(self.before_stats.active_pixels),
            "active_pixels_after": str(self.after_stats.active_pixels),
            "active_pct_before": f"{self.before_stats.active_pct:.6f}",
            "active_pct_after": f"{self.after_stats.active_pct:.6f}",
            "max_active_pixels": str(self.max_active_pixels),
            "visibility_class": self.visibility_class,
            "active_bbox_before": self.before_stats.active_bbox,
            "active_bbox_after": self.after_stats.active_bbox,
            "changed_bbox": self.after_stats.changed_bbox,
            "original_lru32": str(as_int(self.summary.get("original_lru32"))),
            "cacheopt_lru32": str(as_int(self.summary.get("cacheopt_lru32"))),
            "lru32_delta": str(self.lru32_delta),
            "lru32_delta_pct": f"{as_float(self.summary.get('lru32_delta_pct')):.6f}",
            "broad_group_id": self.broad_group_id,
            "broad_group_status": self.broad_group_status,
            "broad_group_pass_draws": str(self.broad_group_pass_draws),
            "broad_group_fail_draws": str(self.broad_group_fail_draws),
            "index_hash": self.index_hash,
            "stream0_hash": self.stream0_hash,
            "extra_stream_hashes": self.extra_stream_hashes,
            "vsconsts_hash": self.vsconsts_hash,
            "psconsts_hash": self.psconsts_hash,
            "ffpvs_hash": self.ffpvs_hash,
            "ffpps_hash": self.ffpps_hash,
            "primitive_id_available": "1" if self.primitive_id_available else "0",
            "primitive_identity_changed_pixels": str(self.primitive_identity_changed_pixels),
            "primitive_identity_changed_bbox": self.primitive_summary.get("primitive_identity_changed_bbox", ""),
            "color_changed_pixels": str(self.color_changed_pixels),
            "color_and_primitive_changed_pixels": str(self.color_and_primitive_changed_pixels),
            "color_change_primitive_overlap_pct": (
                f"{self.color_change_primitive_overlap_pct:.2f}"
                if self.color_change_primitive_overlap_pct is not None else ""
            ),
            "primitive_owner_risk": self.primitive_owner_risk,
            "final_writer_oracle_status": self.final_writer_oracle_status,
            "final_writer_oracle_action": self.final_writer_oracle_action,
            "shader_key": shader_key(self.manifest_draw),
            "state_key": state_key(self.manifest_draw),
        }
        for field in RUNTIME_PROBE_HASH_FIELDS:
            row[f"runtime_{field}"] = self.runtime_probe.get(field, "")
        for field in BROAD_STATE_FIELDS + ("start_index",):
            row[f"state_{field}"] = str(state.get(field, ""))
        for field in GEOMETRY_SELECTOR_FIELDS:
            row[f"geometry_{field}"] = str(geometry.get(field, ""))
        for field in SHADER_SELECTOR_FIELDS:
            row[f"shader_{field}"] = str(shaders.get(field, ""))
        return row


def broad_key_for(draw: dict[str, Any],
                  index_hash: str,
                  stream0_hash: str,
                  extra_hashes: str) -> tuple[tuple[str, str], ...]:
    state = draw.get("state", {})
    geometry = draw.get("geometry", {})
    parts: list[tuple[str, str]] = [
        ("row", str(draw.get("row", ""))),
        ("shader", shader_key(draw)),
        ("index_hash", index_hash),
        ("stream0_hash", stream0_hash),
        ("extra_stream_hashes", extra_hashes),
        ("index_buffer", str(geometry.get("index_buffer", ""))),
        ("stream0_handle", str(geometry.get("stream0_handle", ""))),
    ]
    parts.extend((field, str(state.get(field, ""))) for field in BROAD_STATE_FIELDS)
    return tuple(parts)


def runtime_probe_key_for_draw(draw: dict[str, Any]) -> tuple[str, str, str]:
    return (
        str(draw.get("seq", "")),
        str(draw.get("encoder", "")),
        str(draw.get("encoder_draw_index", "")),
    )


def runtime_probe_key_for_row(row: dict[str, str]) -> tuple[str, str, str]:
    return (
        str(row.get("seq", "")),
        str(row.get("encoder", "")),
        str(row.get("encoder_draw_index", "")),
    )


def load_runtime_probe_csv(path: Path | None) -> dict[tuple[str, str, str], dict[str, str]]:
    if path is None:
        return {}
    with path.open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))
    out: dict[tuple[str, str, str], dict[str, str]] = {}
    for row in rows:
        key = runtime_probe_key_for_row(row)
        if not all(key):
            continue
        out[key] = {
            field: row.get(field, "")
            for field in RUNTIME_PROBE_HASH_FIELDS
            if row.get(field, "")
        }
    return out


def analyze(manifest_path: Path,
            summary_path: Path,
            single_draw_dir: Path,
            runtime_probe_csv: Path | None = None,
            active_threshold: int = 0,
            sparse_active_pixels: int = 32) -> list[DrawAnalysis]:
    manifest = load_manifest(manifest_path)
    summary = load_summary(summary_path)
    runtime_probe_rows = load_runtime_probe_csv(runtime_probe_csv)
    analyses: list[DrawAnalysis] = []
    for draw_index, draw in enumerate(manifest["draws"]):
        if draw_index not in summary:
            raise SystemExit(f"missing summary row for draw_index={draw_index}")
        geometry = draw.get("geometry", {})
        uniforms = draw.get("uniforms", {})
        before_path = single_draw_dir / f"draw{draw_index:03d}-original" / "original.ppm"
        after_path = single_draw_dir / f"draw{draw_index:03d}-cacheopt" / "cacheopt.ppm"
        before_stats, after_stats = image_stats(before_path, after_path, active_threshold)
        index_hash = short_hash(geometry.get("index_file"))
        stream0_hash = short_hash(geometry.get("stream0_file"))
        extra_hashes = extra_stream_hashes(draw)
        item = DrawAnalysis(
            draw_index=draw_index,
            manifest_draw=draw,
            summary=summary[draw_index],
            before_stats=before_stats,
            after_stats=after_stats,
            index_hash=index_hash,
            stream0_hash=stream0_hash,
            extra_stream_hashes=extra_hashes,
            vsconsts_hash=short_hash(uniforms.get("vsconsts_file")),
            psconsts_hash=short_hash(uniforms.get("psconsts_file")),
            ffpvs_hash=short_hash(uniforms.get("ffpvs_file")),
            ffpps_hash=short_hash(uniforms.get("ffpps_file")),
            runtime_probe=runtime_probe_rows.get(runtime_probe_key_for_draw(draw), {}),
            primitive_summary=load_primitive_summary(single_draw_dir, draw_index),
            broad_key=(),
            sparse_active_pixels=sparse_active_pixels,
        )
        item.broad_key = broad_key_for(draw, index_hash, stream0_hash, extra_hashes)
        item.broad_group_id = key_digest(item.broad_key)
        analyses.append(item)

    by_group: dict[str, list[DrawAnalysis]] = {}
    for item in analyses:
        by_group.setdefault(item.broad_group_id, []).append(item)
    for group in by_group.values():
        pass_count = sum(1 for item in group if item.semantic_status == "pass")
        fail_count = len(group) - pass_count
        status = "mixed" if pass_count and fail_count else ("all-pass" if pass_count else "all-fail")
        for item in group:
            item.broad_group_status = status
            item.broad_group_pass_draws = pass_count
            item.broad_group_fail_draws = fail_count
    return analyses


def write_csv(path: Path, analyses: list[DrawAnalysis]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=CSV_FIELDS)
        writer.writeheader()
        for item in analyses:
            writer.writerow(item.csv_row())


def selector_csv_cell(value: str) -> str:
    return value.replace("`", "")


def write_selector_csv(
    path: Path,
    analyses: list[DrawAnalysis],
    *,
    selector_max_fields: int = 2,
    selector_limit: int = 10,
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    failures = [item for item in analyses if item.semantic_status == "fail"]
    passes = [item for item in analyses if item.semantic_status == "pass"]
    fields = (
        "queue",
        "selector",
        "verdict",
        "kept_draws",
        "kept_fail",
        "lru32_delta",
        "gain_share",
        "mixed_all_fail_groups",
        "meaning",
    )
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for row in selector_candidate_rows(analyses, passes, failures):
            writer.writerow({
                "queue": "selector-candidate",
                "selector": selector_csv_cell(row[0]),
                "verdict": selector_csv_cell(row[1]),
                "kept_draws": selector_csv_cell(row[2]),
                "kept_fail": selector_csv_cell(row[3]),
                "lru32_delta": selector_csv_cell(row[4]).replace(",", ""),
                "gain_share": selector_csv_cell(row[5]),
                "mixed_all_fail_groups": "",
                "meaning": selector_csv_cell(row[6]),
            })
        for row in runtime_field_combination_rows(
            analyses,
            max_fields=selector_max_fields,
            limit=selector_limit,
        ):
            writer.writerow({
                "queue": "runtime-field-combination",
                "selector": selector_csv_cell(row[0]),
                "verdict": selector_csv_cell(row[1]),
                "kept_draws": selector_csv_cell(row[2]),
                "kept_fail": "0",
                "lru32_delta": selector_csv_cell(row[3]).replace(",", ""),
                "gain_share": selector_csv_cell(row[4]),
                "mixed_all_fail_groups": selector_csv_cell(row[5]),
                "meaning": selector_csv_cell(row[6]),
            })
        for row in final_color_runtime_selector_rows(
            analyses,
            max_fields=selector_max_fields,
            limit=selector_limit,
        ):
            writer.writerow({
                "queue": "final-color-runtime-selector",
                "selector": selector_csv_cell(row[0]),
                "verdict": selector_csv_cell(row[1]),
                "kept_draws": selector_csv_cell(row[2]),
                "kept_fail": "0",
                "lru32_delta": selector_csv_cell(row[3]).replace(",", ""),
                "gain_share": selector_csv_cell(row[4]),
                "mixed_all_fail_groups": selector_csv_cell(row[5]),
                "meaning": selector_csv_cell(row[6]),
            })
        for row in final_color_runtime_blocker_rows(analyses):
            writer.writerow({
                "queue": "final-color-runtime-blocker",
                "selector": selector_csv_cell(row[0]),
                "verdict": selector_csv_cell(row[1]),
                "kept_draws": selector_csv_cell(row[2]),
                "kept_fail": str(len([item for item in selector_csv_cell(row[3]).split(",") if item])),
                "lru32_delta": selector_csv_cell(row[4]).replace(",", ""),
                "gain_share": "",
                "mixed_all_fail_groups": f"fail_lru32={selector_csv_cell(row[5]).replace(',', '')}",
                "meaning": (
                    f"trace-local-diff={selector_csv_cell(row[6])}; "
                    f"{selector_csv_cell(row[7])}"
                ),
            })
        for row in final_writer_runtime_selector_rows(
            analyses,
            max_fields=selector_max_fields,
            limit=selector_limit,
        ):
            writer.writerow({
                "queue": "final-writer-runtime-selector",
                "selector": selector_csv_cell(row[0]),
                "verdict": selector_csv_cell(row[1]),
                "kept_draws": selector_csv_cell(row[2]),
                "kept_fail": "0",
                "lru32_delta": selector_csv_cell(row[3]).replace(",", ""),
                "gain_share": selector_csv_cell(row[4]),
                "mixed_all_fail_groups": selector_csv_cell(row[5]),
                "meaning": selector_csv_cell(row[6]),
            })


def markdown_table(headers: tuple[str, ...], rows: Iterable[tuple[str, ...]]) -> str:
    out = [
        "| " + " | ".join(headers) + " |",
        "| " + " | ".join("---" for _ in headers) + " |",
    ]
    for row in rows:
        out.append("| " + " | ".join(row) + " |")
    return "\n".join(out)


def exact_numeric_separator(
    passes: list[DrawAnalysis],
    failures: list[DrawAnalysis],
    getter: Callable[[DrawAnalysis], int],
) -> tuple[str, int, int] | None:
    if not passes or not failures:
        return None
    pass_values = [int(getter(item)) for item in passes]
    fail_values = [int(getter(item)) for item in failures]
    pass_max = max(pass_values)
    pass_min = min(pass_values)
    fail_max = max(fail_values)
    fail_min = min(fail_values)
    if fail_min > pass_max:
        return (f">= {fail_min}", fail_min, fail_min - pass_max)
    if fail_max < pass_min:
        return (f"<= {fail_max}", fail_max, pass_min - fail_max)
    return None


def selector_scout_rows(
    analyses: list[DrawAnalysis],
    passes: list[DrawAnalysis],
    failures: list[DrawAnalysis],
    mixed_groups: dict[str, list[DrawAnalysis]],
    primitive_items: list[DrawAnalysis],
) -> list[tuple[str, str, str, str]]:
    rows: list[tuple[str, str, str, str]] = []
    no_final_passes = [
        item for item in passes
        if item.visibility_class == "no-final-color-exact-pass"
    ]
    sparse_passes = [
        item for item in passes
        if item.visibility_class in ("no-final-color-exact-pass", "sparse-exact-pass")
    ]
    sparse_failures = [
        item for item in failures
        if item.visibility_class == "sparse-fail"
    ]
    if mixed_groups:
        rows.append((
            "Broad state/shader/payload",
            "reject",
            f"{len(mixed_groups)} mixed group(s)",
            "A production predicate cannot use only state, shaders, stream/index payload hashes, or broad geometry.",
        ))
    else:
        rows.append((
            "Broad state/shader/payload",
            "possible",
            "no mixed groups",
            "A broad selector may be enough, but still needs same-input semantic proof.",
        ))

    if primitive_items:
        owner_stable_passes = sum(
            1 for item in primitive_items
            if item.primitive_owner_risk == "primitive-owner-changed-color-stable"
        )
        owner_failures = sum(
            1 for item in primitive_items
            if item.primitive_owner_risk == "color-change-follows-primitive-owner"
        )
        if owner_stable_passes:
            rows.append((
                "Primitive-owner change",
                "overreject",
                f"{owner_stable_passes} exact-pass owner-changing draw(s), {owner_failures} owner-explained fail draw(s)",
                "Useful to explain failures, but too conservative as a runtime reject gate.",
            ))
        elif owner_failures:
            rows.append((
                "Primitive-owner change",
                "possible",
                f"{owner_failures} owner-explained fail draw(s)",
                "Could be a conservative gate if no exact-pass draw changes owner.",
            ))

    if sparse_passes:
        sparse_lru = sum(item.lru32_delta for item in sparse_passes)
        if sparse_failures:
            rows.append((
                "No-final-color / sparse visibility",
                "reject",
                f"{len(sparse_failures)} sparse fail draw(s), {len(sparse_passes)} sparse exact-pass draw(s)",
                "Sparse final visibility alone does not isolate correctness when sparse failures exist.",
            ))
        else:
            rows.append((
                "No-final-color / sparse visibility",
                "debug-only",
                (
                    f"{len(no_final_passes)} inactive exact-pass draw(s), "
                    f"{len(sparse_passes)} sparse exact-pass draw(s), "
                    f"LRU32 {fmt_int(sparse_lru)}"
                ),
                "Promising replay selector, but final framebuffer visibility is not yet a cheap D3D9 runtime predicate.",
            ))
    else:
        rows.append((
            "No-final-color / sparse visibility",
            "no-signal",
            "no sparse exact-pass draws",
            "This replay does not show a sparse/no-final-color harvestable subset.",
        ))

    active_sep = exact_numeric_separator(
        passes,
        failures,
        lambda item: item.before_stats.active_pixels,
    )
    if active_sep is None:
        rows.append((
            "Pre-mutation active pixels",
            "no-separator",
            "pass/fail ranges overlap",
            "Coverage area alone does not isolate the unsafe draw set.",
        ))
    else:
        expr, threshold, margin = active_sep
        rows.append((
            "Pre-mutation active pixels",
            "debug-only",
            f"failure isolated by active_pixels {expr}; margin {margin} px",
            "Framebuffer activity is a replay-derived diagnostic, not a cheap D3D9 runtime predicate.",
        ))

    fail_vs = {item.vsconsts_hash for item in failures if item.vsconsts_hash}
    pass_vs = {item.vsconsts_hash for item in passes if item.vsconsts_hash}
    if fail_vs and fail_vs.isdisjoint(pass_vs):
        all_vs = [item.vsconsts_hash for item in analyses if item.vsconsts_hash]
        unique_note = "all VS const hashes unique" if len(set(all_vs)) == len(all_vs) else "fail hash disjoint from pass hashes"
        rows.append((
            "VS constant hash",
            "trace-local",
            unique_note,
            "Constant hashes can guide bisection, but are not yet a stable semantic predicate.",
        ))
    else:
        rows.append((
            "VS constant hash",
            "no-separator",
            "fail/pass hashes overlap or are missing",
            "Constant payload identity does not isolate the unsafe draw set.",
        ))

    rows.append((
        "Encoder draw index / ordinal",
        "debug-only",
        "separates the current artifact when failing draws are known",
        "Trace-local identity is acceptable for replay proofs and Xcode mechanism runs, not production.",
    ))
    return rows


def gain_share_text(candidate_lru: int, total_lru: int) -> str:
    if total_lru == 0:
        return "n/a"
    return f"{abs(candidate_lru) / abs(total_lru) * 100.0:.2f}%"


def selector_candidate_rows(
    analyses: list[DrawAnalysis],
    passes: list[DrawAnalysis],
    failures: list[DrawAnalysis],
) -> list[tuple[str, str, str, str, str, str, str]]:
    total_lru = sum(item.lru32_delta for item in analyses)
    rows: list[tuple[str, str, str, str, str, str, str]] = []
    seen: set[str] = set()

    def add(name: str, verdict: str, scope: str, items: list[DrawAnalysis], meaning: str) -> None:
        if not items or name in seen:
            return
        seen.add(name)
        kept_failures = [item for item in items if item.semantic_status == "fail"]
        kept_lru = sum(item.lru32_delta for item in items)
        rows.append((
            name,
            verdict if not kept_failures else "reject-keeps-fail",
            f"`{','.join(str(item.draw_index) for item in items)}`",
            f"`{len(kept_failures)}`",
            f"`{fmt_int(kept_lru)}`",
            f"`{gain_share_text(kept_lru, total_lru)}`",
            f"{scope}: {meaning}",
        ))

    add(
        "all exact-pass draws",
        "trace-local-upper-bound",
        "debug-only",
        passes,
        "maximum known safe movement for this replay; not a runtime predicate",
    )

    broad_all_pass = [
        item for item in analyses
        if item.broad_group_status == "all-pass"
    ]
    add(
        "broad all-pass groups",
        "trace-local",
        "payload-hash",
        broad_all_pass,
        "state/shader/payload groups that contain no replay failures",
    )

    sparse_exact = [
        item for item in passes
        if item.visibility_class in ("no-final-color-exact-pass", "sparse-exact-pass")
    ]
    add(
        "no-final-color or sparse exact-pass",
        "debug-only",
        "replay-visibility",
        sparse_exact,
        "requires framebuffer/depth visibility knowledge that dxmt does not cheaply know at draw submit",
    )

    visible_exact = [
        item for item in passes
        if item.visibility_class == "visible-exact-pass"
    ]
    add(
        "visible exact-pass draws",
        "debug-only",
        "replay-visibility",
        visible_exact,
        "large mechanism signal, but no current runtime separator from visible failures",
    )

    primitive_owner_safe = [
        item for item in analyses
        if item.primitive_owner_risk != "color-change-follows-primitive-owner"
    ]
    if any(item.primitive_id_available for item in analyses):
        add(
            "primitive-owner not color-changing",
            "debug-only",
            "primitive-id-replay",
            primitive_owner_safe,
            "explains this failure but requires primitive-id/color replay, not a production predicate",
        )

    active_sep = exact_numeric_separator(
        passes,
        failures,
        lambda item: item.before_stats.active_pixels,
    )
    if active_sep is not None:
        _expr, threshold, _margin = active_sep
        if failures and all(item.before_stats.active_pixels >= threshold for item in failures):
            active_items = [item for item in analyses if item.before_stats.active_pixels < threshold]
            add(
                f"active pixels < {threshold}",
                "debug-only",
                "replay-visibility",
                active_items,
                "separates this replay failure by framebuffer activity only",
            )
        elif failures and all(item.before_stats.active_pixels <= threshold for item in failures):
            active_items = [item for item in analyses if item.before_stats.active_pixels > threshold]
            add(
                f"active pixels > {threshold}",
                "debug-only",
                "replay-visibility",
                active_items,
                "separates this replay failure by framebuffer activity only",
            )

    fail_vs = {item.vsconsts_hash for item in failures if item.vsconsts_hash}
    if fail_vs:
        add(
            "VS const hash excluding failures",
            "trace-local",
            "payload-hash",
            [item for item in analyses if item.vsconsts_hash not in fail_vs],
            "useful for bisection, but constant-payload identity is not a semantic runtime rule",
        )

    fail_draws = {item.draw_index for item in failures}
    if fail_draws:
        add(
            "encoder draw index excluding failures",
            "trace-local",
            "draw-identity",
            [item for item in analyses if item.draw_index not in fail_draws],
            "acceptable for a mechanism proof or replay bisect only",
        )
    return rows


def runtime_selector_values(item: DrawAnalysis) -> dict[str, tuple[str, str]]:
    draw = item.manifest_draw
    state = draw.get("state", {})
    geometry = draw.get("geometry", {})
    shaders = draw.get("shaders", {})
    values: dict[str, tuple[str, str]] = {}

    for field in STATE_SELECTOR_FIELDS:
        if field in state:
            values[f"state.{field}"] = ("runtime-state", str(state.get(field, "")))
    for field in GEOMETRY_SELECTOR_FIELDS:
        if field in geometry:
            values[f"geometry.{field}"] = ("geometry-scout", str(geometry.get(field, "")))
    for field in SHADER_SELECTOR_FIELDS:
        if field in shaders:
            values[f"shader.{field}"] = ("runtime-shader", str(shaders.get(field, "")))

    if item.index_hash:
        values["payload.index_hash"] = ("payload-hash", item.index_hash)
    if item.stream0_hash:
        values["payload.stream0_hash"] = ("payload-hash", item.stream0_hash)
    if item.extra_stream_hashes:
        values["payload.extra_stream_hashes"] = ("payload-hash", item.extra_stream_hashes)
    if item.vsconsts_hash:
        values["constant.vsconsts_hash"] = ("constant-hash", item.vsconsts_hash)
    if item.psconsts_hash:
        values["constant.psconsts_hash"] = ("constant-hash", item.psconsts_hash)
    if item.ffpvs_hash:
        values["constant.ffpvs_hash"] = ("constant-hash", item.ffpvs_hash)
    if item.ffpps_hash:
        values["constant.ffpps_hash"] = ("constant-hash", item.ffpps_hash)
    if item.runtime_probe.get("vs_constants_hash"):
        values["runtime.vs_constants_hash"] = (
            "runtime-constant-hash",
            item.runtime_probe["vs_constants_hash"],
        )
    if item.runtime_probe.get("ps_constants_hash"):
        values["runtime.ps_constants_hash"] = (
            "runtime-constant-hash",
            item.runtime_probe["ps_constants_hash"],
        )
    if item.runtime_probe.get("uniform_payload_hash"):
        values["runtime.uniform_payload_hash"] = (
            "runtime-draw-payload-hash",
            item.runtime_probe["uniform_payload_hash"],
        )
    return values


def selector_field_scopes(analyses: list[DrawAnalysis]) -> dict[str, set[str]]:
    scopes: dict[str, set[str]] = {}
    for item in analyses:
        for field, (scope, _value) in runtime_selector_values(item).items():
            scopes.setdefault(field, set()).add(scope)
    return scopes


def combo_has_trace_local_field(
    combo: tuple[str, ...],
    field_scopes: dict[str, set[str]],
) -> bool:
    trace_scopes = {"constant-hash", "payload-hash", "runtime-draw-payload-hash"}
    return any(field_scopes.get(field, set()) & trace_scopes for field in combo)


def runtime_visible_field_values(item: DrawAnalysis) -> dict[str, str]:
    trace_scopes = {"constant-hash", "payload-hash", "runtime-draw-payload-hash"}
    return {
        field: value
        for field, (scope, value) in runtime_selector_values(item).items()
        if scope not in trace_scopes
    }


def runtime_visible_group_key(item: DrawAnalysis) -> tuple[tuple[str, str], ...]:
    return tuple(sorted(runtime_visible_field_values(item).items()))


def selector_combo_verdict(scopes: set[str], group_count: int, draw_count: int) -> str:
    if "runtime-draw-payload-hash" in scopes:
        return "runtime-payload-overfit"
    if "constant-hash" in scopes:
        return "trace-local-constant"
    if "payload-hash" in scopes:
        return "trace-local-payload"
    if draw_count and group_count >= draw_count:
        return "overfit-singleton"
    if "runtime-constant-hash" in scopes:
        return "runtime-constant-scout"
    if "geometry-scout" in scopes:
        return "geometry-scout"
    return "runtime-scout"


def selector_combo_meaning(verdict: str, mixed_count: int, all_fail_count: int) -> str:
    if verdict == "trace-local-constant":
        base = "constant payload identity separates this replay, but is not a stable semantic predicate"
    elif verdict == "trace-local-payload":
        base = "payload hashes help bisect captured draws, but are not a production semantic rule"
    elif verdict == "runtime-payload-overfit":
        base = "full runtime uniform-payload hash is draw-local and overfits replay identity"
    elif verdict == "runtime-constant-scout":
        base = "runtime constant hash is observable, but still needs semantic proof before becoming a predicate"
    elif verdict == "overfit-singleton":
        base = "field tuple is as specific as draw identity and should not drive production"
    elif verdict == "geometry-scout":
        base = "geometry-derived grouping is a scout; it still needs wider semantic proof"
    else:
        base = "state/shader grouping is runtime-shaped, but current replay evidence is narrow"
    if mixed_count:
        base += f"; {mixed_count} mixed group(s) remain excluded"
    if all_fail_count:
        base += f"; {all_fail_count} all-fail group(s) remain excluded"
    return base


def final_color_selector_meaning(
    verdict: str,
    blocked_target_groups: int,
    all_fail_count: int,
    non_visible_pass_count: int,
) -> str:
    if verdict == "trace-local-constant":
        base = "constant payload identity can isolate visible exact-pass draws in this replay only"
    elif verdict == "trace-local-payload":
        base = "payload hashes can isolate visible exact-pass draws for bisection, not production"
    elif verdict == "runtime-payload-overfit":
        base = "full runtime uniform-payload hash can isolate draws but overfits identity"
    elif verdict == "runtime-constant-scout":
        base = "runtime constant hash is observable, but does not prove final-color safety"
    elif verdict == "overfit-singleton":
        base = "field tuple is as specific as draw identity and cannot be a production selector"
    elif verdict == "geometry-scout":
        base = "geometry-derived grouping may isolate visible exact-pass draws, but needs wider replay proof"
    else:
        base = "runtime-shaped grouping may isolate visible exact-pass draws, but current replay evidence is narrow"
    if blocked_target_groups:
        base += f"; {blocked_target_groups} target group(s) also contain semantic failures"
    if all_fail_count:
        base += f"; {all_fail_count} all-fail group(s) remain excluded"
    if non_visible_pass_count:
        base += f"; {non_visible_pass_count} non-visible exact-pass draw(s) would also be selected"
    return base


def runtime_field_combination_rows(
    analyses: list[DrawAnalysis],
    max_fields: int = 2,
    limit: int = 10,
) -> list[tuple[str, str, str, str, str, str, str]]:
    if not analyses:
        return []
    total_lru = sum(item.lru32_delta for item in analyses)
    field_scopes = selector_field_scopes(analyses)
    field_names = sorted(field_scopes)
    candidates: list[tuple[float, int, int, int, int, str, tuple[str, ...], list[DrawAnalysis], str]] = []

    for size in range(1, max_fields + 1):
        for combo in itertools.combinations(field_names, size):
            if size > 2 and combo_has_trace_local_field(combo, field_scopes):
                continue
            grouped: dict[tuple[str, ...], list[DrawAnalysis]] = {}
            scopes: set[str] = set()
            missing = False
            for item in analyses:
                values = runtime_selector_values(item)
                combo_values: list[str] = []
                for field in combo:
                    if field not in values:
                        missing = True
                        break
                    scope, value = values[field]
                    scopes.add(scope)
                    combo_values.append(value)
                if missing:
                    break
                grouped.setdefault(tuple(combo_values), []).append(item)
            if missing:
                continue

            safe_items: list[DrawAnalysis] = []
            mixed_count = 0
            all_fail_count = 0
            for group in grouped.values():
                pass_count = sum(1 for item in group if item.semantic_status == "pass")
                fail_count = len(group) - pass_count
                if pass_count and fail_count:
                    mixed_count += 1
                    continue
                if fail_count:
                    all_fail_count += 1
                    continue
                safe_items.extend(group)
            if not safe_items:
                continue

            lru = sum(item.lru32_delta for item in safe_items)
            gain = abs(lru) / abs(total_lru) if total_lru else 0.0
            verdict = selector_combo_verdict(scopes, len(grouped), len(analyses))
            candidates.append((
                gain,
                lru,
                len(safe_items),
                mixed_count,
                all_fail_count,
                verdict,
                combo,
                sorted(safe_items, key=lambda item: item.draw_index),
                selector_combo_meaning(verdict, mixed_count, all_fail_count),
            ))

    verdict_rank = {
        "runtime-scout": 0,
        "runtime-constant-scout": 1,
        "geometry-scout": 2,
        "trace-local-payload": 3,
        "trace-local-constant": 4,
        "runtime-payload-overfit": 5,
        "overfit-singleton": 6,
    }
    candidates.sort(
        key=lambda row: (
            -row[0],
            verdict_rank.get(row[5], 99),
            len(row[6]),
            row[6],
        )
    )

    rows: list[tuple[str, str, str, str, str, str, str]] = []
    seen_draw_sets: set[tuple[int, ...]] = set()
    for gain, lru, _count, mixed_count, all_fail_count, verdict, combo, safe_items, meaning in candidates:
        draw_ids = tuple(item.draw_index for item in safe_items)
        if draw_ids in seen_draw_sets:
            continue
        seen_draw_sets.add(draw_ids)
        rows.append((
            "`" + " + ".join(combo) + "`",
            f"`{verdict}`",
            f"`{','.join(str(draw_id) for draw_id in draw_ids)}`",
            f"`{fmt_int(lru)}`",
            f"`{gain * 100.0:.2f}%`",
            f"`{mixed_count}` / `{all_fail_count}`",
            meaning,
        ))
        if len(rows) >= limit:
            break
    return rows


def final_color_runtime_selector_rows(
    analyses: list[DrawAnalysis],
    max_fields: int = 2,
    limit: int = 10,
) -> list[tuple[str, str, str, str, str, str, str]]:
    """Rank runtime-shaped selectors by visible exact-pass gain.

    The broad reorder blocker is specifically a visible final-writer hazard:
    visible exact-pass draws have useful locality movement, but visible-fail
    draws cannot be admitted. This sweep asks whether any runtime/geometry/
    shader field tuple keeps visible exact-pass groups while excluding all
    semantic failures. It deliberately computes gain only from visible
    exact-pass draws; sparse/no-final-color exact passes are positive controls,
    not the selector value we need for row 50/2 promotion.
    """

    target_items = [
        item for item in analyses
        if item.visibility_class == "visible-exact-pass"
    ]
    target_lru = sum(item.lru32_delta for item in target_items)
    if not target_items or target_lru == 0:
        return []

    field_scopes = selector_field_scopes(analyses)
    field_names = sorted(field_scopes)
    candidates: list[
        tuple[float, int, int, int, int, str, tuple[str, ...], list[DrawAnalysis], str]
    ] = []

    for size in range(1, max_fields + 1):
        for combo in itertools.combinations(field_names, size):
            if size > 2 and combo_has_trace_local_field(combo, field_scopes):
                continue
            grouped: dict[tuple[str, ...], list[DrawAnalysis]] = {}
            scopes: set[str] = set()
            missing = False
            for item in analyses:
                values = runtime_selector_values(item)
                combo_values: list[str] = []
                for field in combo:
                    if field not in values:
                        missing = True
                        break
                    scope, value = values[field]
                    scopes.add(scope)
                    combo_values.append(value)
                if missing:
                    break
                grouped.setdefault(tuple(combo_values), []).append(item)
            if missing:
                continue

            selected_targets: list[DrawAnalysis] = []
            selected_non_visible = 0
            blocked_target_groups = 0
            all_fail_count = 0
            for group in grouped.values():
                group_targets = [
                    item for item in group
                    if item.visibility_class == "visible-exact-pass"
                ]
                if not group_targets:
                    if all(item.semantic_status != "pass" for item in group):
                        all_fail_count += 1
                    continue
                fail_count = sum(1 for item in group if item.semantic_status != "pass")
                if fail_count:
                    blocked_target_groups += 1
                    continue
                selected_targets.extend(group_targets)
                selected_non_visible += sum(
                    1 for item in group
                    if item.semantic_status == "pass"
                    and item.visibility_class != "visible-exact-pass"
                )
            if not selected_targets:
                continue

            lru = sum(item.lru32_delta for item in selected_targets)
            gain = abs(lru) / abs(target_lru) if target_lru else 0.0
            verdict = selector_combo_verdict(scopes, len(grouped), len(analyses))
            candidates.append((
                gain,
                lru,
                len(selected_targets),
                blocked_target_groups,
                all_fail_count,
                verdict,
                combo,
                sorted(selected_targets, key=lambda item: item.draw_index),
                final_color_selector_meaning(
                    verdict,
                    blocked_target_groups,
                    all_fail_count,
                    selected_non_visible,
                ),
            ))

    verdict_rank = {
        "runtime-scout": 0,
        "runtime-constant-scout": 1,
        "geometry-scout": 2,
        "trace-local-payload": 3,
        "trace-local-constant": 4,
        "runtime-payload-overfit": 5,
        "overfit-singleton": 6,
    }
    candidates.sort(
        key=lambda row: (
            -row[0],
            verdict_rank.get(row[5], 99),
            len(row[6]),
            row[6],
        )
    )

    rows: list[tuple[str, str, str, str, str, str, str]] = []
    seen_draw_sets: set[tuple[int, ...]] = set()
    for gain, lru, _count, blocked_target_groups, all_fail_count, verdict, combo, selected_targets, meaning in candidates:
        draw_ids = tuple(item.draw_index for item in selected_targets)
        if draw_ids in seen_draw_sets:
            continue
        seen_draw_sets.add(draw_ids)
        rows.append((
            "`" + " + ".join(combo) + "`",
            f"`{verdict}`",
            f"`{','.join(str(draw_id) for draw_id in draw_ids)}`",
            f"`{fmt_int(lru)}`",
            f"`{gain * 100.0:.2f}%`",
            f"`{blocked_target_groups}` / `{all_fail_count}`",
            meaning,
        ))
        if len(rows) >= limit:
            break
    return rows


def final_writer_selector_meaning(
    verdict: str,
    blocked_target_groups: int,
    all_hazard_count: int,
) -> str:
    if verdict == "runtime-payload-overfit":
        base = "full runtime uniform-payload hash can isolate owner-stable draws but overfits identity"
    elif verdict == "trace-local-constant":
        base = "trace-local constant payload can isolate owner-stable draws in this replay only"
    elif verdict == "trace-local-payload":
        base = "payload hashes can isolate owner-stable draws for bisection, not production"
    elif verdict == "runtime-constant-scout":
        base = "runtime constant hash is observable, but does not prove final-writer safety"
    elif verdict == "overfit-singleton":
        base = "field tuple is as specific as draw identity and cannot be a production selector"
    elif verdict == "geometry-scout":
        base = "geometry-derived grouping may isolate owner-stable draws, but needs wider replay proof"
    else:
        base = "runtime-shaped grouping may isolate owner-stable draws, but current replay evidence is narrow"
    if blocked_target_groups:
        base += f"; {blocked_target_groups} owner-stable target group(s) also contain final-writer hazards"
    if all_hazard_count:
        base += f"; {all_hazard_count} all-hazard group(s) remain excluded"
    return base


def final_writer_runtime_selector_rows(
    analyses: list[DrawAnalysis],
    max_fields: int = 2,
    limit: int = 10,
) -> list[tuple[str, str, str, str, str, str, str]]:
    """Rank selectors that keep owner-change/color-stable movement.

    This targets the exact-pass owner-changing bucket, because those draws
    prove that primitive-owner change alone is an over-reject. A useful
    production selector must keep that movement while excluding real
    final-writer color hazards.
    """

    target_items = [
        item for item in analyses
        if item.final_writer_oracle_status == "owner-change-color-stable"
    ]
    target_lru = sum(item.lru32_delta for item in target_items)
    if not target_items or target_lru == 0:
        return []

    field_scopes = selector_field_scopes(analyses)
    field_names = sorted(field_scopes)
    candidates: list[
        tuple[float, int, int, int, int, str, tuple[str, ...], list[DrawAnalysis], str]
    ] = []

    for size in range(1, max_fields + 1):
        for combo in itertools.combinations(field_names, size):
            if size > 2 and combo_has_trace_local_field(combo, field_scopes):
                continue
            grouped: dict[tuple[str, ...], list[DrawAnalysis]] = {}
            scopes: set[str] = set()
            missing = False
            for item in analyses:
                values = runtime_selector_values(item)
                combo_values: list[str] = []
                for field in combo:
                    if field not in values:
                        missing = True
                        break
                    scope, value = values[field]
                    scopes.add(scope)
                    combo_values.append(value)
                if missing:
                    break
                grouped.setdefault(tuple(combo_values), []).append(item)
            if missing:
                continue

            selected_targets: list[DrawAnalysis] = []
            blocked_target_groups = 0
            all_hazard_count = 0
            for group in grouped.values():
                group_targets = [
                    item for item in group
                    if item.final_writer_oracle_status == "owner-change-color-stable"
                ]
                group_hazards = [
                    item for item in group
                    if item.final_writer_oracle_status in {
                        "final-writer-color-hazard",
                        "final-color-hazard",
                        "semantic-fail-no-final-writer-proof",
                    }
                ]
                if not group_targets:
                    if group_hazards and len(group_hazards) == len(group):
                        all_hazard_count += 1
                    continue
                if group_hazards:
                    blocked_target_groups += 1
                    continue
                selected_targets.extend(group_targets)
            if not selected_targets:
                continue

            lru = sum(item.lru32_delta for item in selected_targets)
            gain = abs(lru) / abs(target_lru) if target_lru else 0.0
            verdict = selector_combo_verdict(scopes, len(grouped), len(analyses))
            candidates.append((
                gain,
                lru,
                len(selected_targets),
                blocked_target_groups,
                all_hazard_count,
                verdict,
                combo,
                sorted(selected_targets, key=lambda item: item.draw_index),
                final_writer_selector_meaning(
                    verdict,
                    blocked_target_groups,
                    all_hazard_count,
                ),
            ))

    verdict_rank = {
        "runtime-scout": 0,
        "runtime-constant-scout": 1,
        "geometry-scout": 2,
        "trace-local-payload": 3,
        "trace-local-constant": 4,
        "runtime-payload-overfit": 5,
        "overfit-singleton": 6,
    }
    candidates.sort(
        key=lambda row: (
            -row[0],
            verdict_rank.get(row[5], 99),
            len(row[6]),
            row[6],
        )
    )

    rows: list[tuple[str, str, str, str, str, str, str]] = []
    seen_draw_sets: set[tuple[str, tuple[int, ...]]] = set()
    for gain, lru, _count, blocked_target_groups, all_hazard_count, verdict, combo, selected_targets, meaning in candidates:
        draw_ids = tuple(item.draw_index for item in selected_targets)
        # Keep one row per verdict for the same draw set so the report does
        # not hide that runtime uniform-payload separation is only overfit
        # identity, distinct from trace-local replay constants.
        duplicate_key = (verdict, draw_ids)
        if duplicate_key in seen_draw_sets:
            continue
        seen_draw_sets.add(duplicate_key)
        rows.append((
            "`" + " + ".join(combo) + "`",
            f"`{verdict}`",
            f"`{','.join(str(draw_id) for draw_id in draw_ids)}`",
            f"`{fmt_int(lru)}`",
            f"`{gain * 100.0:.2f}%`",
            f"`{blocked_target_groups}` / `{all_hazard_count}`",
            meaning,
        ))
        if len(rows) >= limit:
            break
    return rows


def final_color_runtime_blocker_rows(
    analyses: list[DrawAnalysis],
) -> list[tuple[str, str, str, str, str, str, str, str]]:
    grouped: dict[tuple[tuple[str, str], ...], list[DrawAnalysis]] = {}
    for item in analyses:
        grouped.setdefault(runtime_visible_group_key(item), []).append(item)

    rows: list[tuple[str, str, str, str, str, str, str, str]] = []
    for key, group in grouped.items():
        visible_pass = [
            item for item in group
            if item.visibility_class == "visible-exact-pass"
        ]
        failures = [
            item for item in group
            if item.semantic_status != "pass"
        ]
        if not visible_pass or not failures:
            continue

        trace_diff_fields: list[str] = []
        trace_values = {
            "vsconsts_hash": {item.vsconsts_hash for item in group if item.vsconsts_hash},
            "psconsts_hash": {item.psconsts_hash for item in group if item.psconsts_hash},
            "index_hash": {item.index_hash for item in group if item.index_hash},
            "stream0_hash": {item.stream0_hash for item in group if item.stream0_hash},
            "extra_stream_hashes": {item.extra_stream_hashes for item in group if item.extra_stream_hashes},
            "runtime.uniform_payload_hash": {
                item.runtime_probe.get("uniform_payload_hash", "")
                for item in group
                if item.runtime_probe.get("uniform_payload_hash", "")
            },
        }
        for field, values in trace_values.items():
            if len(values) > 1:
                trace_diff_fields.append(field)

        sample_fields = []
        for field, value in key:
            if field in {
                "state.index_count",
                "state.primitive_count",
                "state.scissor",
                "state.depth_write",
                "state.alpha_blend",
                "geometry.unique_indices",
                "geometry.cache_miss_64",
                "shader.vs_hash",
                "shader.ps_hash",
            }:
                sample_fields.append(f"{field}={value}")
        if not sample_fields:
            sample_fields = [f"{field}={value}" for field, value in key[:6]]

        rows.append((
            f"`all-runtime-visible-fields`",
            f"`runtime-indistinguishable-target-fail`",
            f"`{','.join(str(item.draw_index) for item in visible_pass)}`",
            f"`{','.join(str(item.draw_index) for item in failures)}`",
            f"`{fmt_int(sum(item.lru32_delta for item in visible_pass))}`",
            f"`{fmt_int(sum(item.lru32_delta for item in failures))}`",
            f"`{', '.join(trace_diff_fields) or 'none'}`",
            (
                f"{len(key)} runtime/geometry/shader fields are identical "
                f"({'; '.join(sample_fields)}); only trace-local or draw-local payload fields can split this blocker"
            ),
        ))

    rows.sort(key=lambda row: abs(as_int(row[4].replace("`", "").replace(",", ""))), reverse=True)
    return rows


def final_writer_oracle_rows(
    analyses: list[DrawAnalysis],
) -> list[tuple[str, str, str, str, str, str, str, str]]:
    grouped: dict[str, list[DrawAnalysis]] = {}
    for item in analyses:
        grouped.setdefault(item.final_writer_oracle_status, []).append(item)

    priority = {
        "final-writer-color-hazard": 0,
        "final-color-hazard": 1,
        "semantic-fail-no-final-writer-proof": 2,
        "owner-change-color-stable": 3,
        "visible-final-color-stable": 4,
        "sparse-positive-control": 5,
        "no-final-color-positive-control": 6,
        "exact-pass": 7,
    }
    rows: list[tuple[str, str, str, str, str, str, str, str]] = []
    for status, group in grouped.items():
        pass_count = sum(1 for item in group if item.semantic_status == "pass")
        fail_count = len(group) - pass_count
        primitive_pixels = sum(item.primitive_identity_changed_pixels for item in group)
        color_pixels = sum(item.color_changed_pixels for item in group)
        actions = sorted({item.final_writer_oracle_action for item in group})
        rows.append((
            f"`{status}`",
            f"`{len(group)}`",
            f"`{pass_count}` / `{fail_count}`",
            f"`{fmt_int(sum(item.lru32_delta for item in group))}`",
            f"`{fmt_int(primitive_pixels)}`",
            f"`{fmt_int(color_pixels)}`",
            f"`{','.join(str(item.draw_index) for item in sorted(group, key=lambda row: row.draw_index))}`",
            "; ".join(actions),
        ))
    rows.sort(
        key=lambda row: (
            priority.get(row[0].strip("`"), 99),
            -abs(as_int(row[3].replace("`", "").replace(",", ""))),
        )
    )
    return rows


def proof_verdict_rows(
    analyses: list[DrawAnalysis],
    passes: list[DrawAnalysis],
    failures: list[DrawAnalysis],
    mixed_groups: dict[str, list[DrawAnalysis]],
    primitive_items: list[DrawAnalysis],
) -> list[tuple[str, str, str]]:
    total_lru = sum(item.lru32_delta for item in analyses)
    safe_lru = sum(item.lru32_delta for item in passes)
    safe_share = (safe_lru / total_lru * 100.0) if total_lru else None
    max_active = max((item.max_active_pixels for item in analyses), default=0)
    sparse_exact = [
        item for item in passes
        if item.visibility_class in ("no-final-color-exact-pass", "sparse-exact-pass")
    ]
    visible_exact = [
        item for item in passes
        if item.visibility_class == "visible-exact-pass"
    ]
    owner_failures = [
        item for item in primitive_items
        if item.primitive_owner_risk == "color-change-follows-primitive-owner"
    ]

    if failures:
        if owner_failures:
            semantic = "fail-visible-primitive-owner-conflict"
            production = "reject primitive reorder for this broad class"
        else:
            semantic = "fail-semantic-image-gate"
            production = "reject until failures have a stable safe selector"
        if safe_share is not None and safe_share >= 80.0:
            mechanism = (
                f"mechanism-only safe sub-window keeps {safe_share:.2f}% of "
                "LRU32 gain, but selector is not production-shaped"
            )
        else:
            mechanism = (
                "mechanism-only; exact-pass share is too small or unproven for "
                "a promotion path"
            )
        return [
            ("Semantic proof", semantic, "exact image gate does not pass for the full mutated draw set"),
            ("Production status", production, "depth-read/color-write primitive order can change final writers"),
            ("Xcode budget", "do-not-spend-production-gputrace", "spend Xcode only for mechanism runs or a newly proven selector"),
            ("Mechanism value", mechanism, f"total LRU32 delta {fmt_int(total_lru)}; exact-pass delta {fmt_int(safe_lru)}"),
            (
                "Next proof",
                "stronger semantic selector or non-reorder backend-shape A/B",
                "state/payload selectors are mixed" if mixed_groups else "current selectors still need runtime proof",
            ),
        ]

    if sparse_exact and not visible_exact:
        semantic = "exact-sparse-or-no-final-color"
        production = "positive-control-only"
        xcode = "repeat-xcode-mechanism-ok"
        next_proof = "needs cheap runtime no-final-color/occlusion predicate"
    else:
        semantic = "exact-visible-pass" if visible_exact else "exact-no-final-color"
        production = "candidate-semantic-payload"
        xcode = "xcode-candidate-after-shape-gate"
        next_proof = "require stable row/full-frame shape and target VS-inv/write gates"

    return [
        ("Semantic proof", semantic, "all single-draw same-input comparisons pass"),
        ("Production status", production, f"max active pixels {fmt_int(max_active)}"),
        ("Xcode budget", xcode, "semantic image gate is clean for this replay payload"),
        ("Mechanism value", "exact-pass payload", f"LRU32 delta {fmt_int(safe_lru)}"),
        ("Next proof", next_proof, "do not generalize beyond the replayed selector without wider evidence"),
    ]


def write_markdown(path: Path,
                   manifest_path: Path,
                   summary_path: Path,
                   analyses: list[DrawAnalysis],
                   *,
                   selector_max_fields: int = 2,
                   selector_limit: int = 10) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    failures = [item for item in analyses if item.semantic_status == "fail"]
    passes = [item for item in analyses if item.semantic_status == "pass"]
    safe_lru = sum(item.lru32_delta for item in passes)
    fail_lru = sum(item.lru32_delta for item in failures)
    total_lru = safe_lru + fail_lru
    safe_lru_share = (safe_lru / total_lru * 100.0) if total_lru else None
    mixed_groups: dict[str, list[DrawAnalysis]] = {}
    for item in analyses:
        if item.broad_group_status == "mixed":
            mixed_groups.setdefault(item.broad_group_id, []).append(item)
    primitive_items = [item for item in analyses if item.primitive_id_available]
    primitive_color_failures = [
        item for item in primitive_items
        if item.color_changed_pixels
    ]
    owner_changed_color_stable = [
        item for item in primitive_items
        if item.primitive_identity_changed_pixels and not item.color_changed_pixels
    ]
    no_final_passes = [
        item for item in passes
        if item.visibility_class == "no-final-color-exact-pass"
    ]
    sparse_exact_passes = [
        item for item in passes
        if item.visibility_class in ("no-final-color-exact-pass", "sparse-exact-pass")
    ]
    sparse_visible_passes = [
        item for item in passes
        if item.visibility_class == "sparse-exact-pass"
    ]
    visible_exact_passes = [
        item for item in passes
        if item.visibility_class == "visible-exact-pass"
    ]
    sparse_failures = [
        item for item in failures
        if item.visibility_class == "sparse-fail"
    ]
    sparse_lru = sum(item.lru32_delta for item in sparse_exact_passes)
    visible_pass_lru = sum(item.lru32_delta for item in visible_exact_passes)
    sparse_lru_share = (sparse_lru / total_lru * 100.0) if total_lru else None

    lines = [
        "# Mini Replay Semantic Bisection Analysis",
        "",
        f"- Manifest: `{manifest_path}`",
        f"- Single-draw summary: `{summary_path}`",
        "",
        "## Overview",
        "",
        markdown_table(
            ("Metric", "Value"),
            (
                ("Draws", f"`{len(analyses)}`"),
                ("Exact-pass draws", f"`{len(passes)}`"),
                ("Exact-fail draws", f"`{len(failures)}`"),
                ("Total LRU32 delta", f"`{fmt_int(total_lru)}`"),
                ("Exact-pass LRU32 delta", f"`{fmt_int(safe_lru)}`"),
                ("Exact-fail LRU32 delta", f"`{fmt_int(fail_lru)}`"),
                ("Pass share of LRU32 gain", f"`{safe_lru_share:.2f}%`" if safe_lru_share is not None else "`n/a`"),
                ("Mixed broad groups", f"`{len(mixed_groups)}`"),
                ("Primitive-id analyzed draws", f"`{len(primitive_items)}`"),
                ("Primitive-owner changed but color stable", f"`{len(owner_changed_color_stable)}`"),
                ("Color-change owner-explained draws", f"`{len(primitive_color_failures)}`"),
                ("No-final-color exact-pass draws", f"`{len(no_final_passes)}`"),
                ("Sparse exact-pass draws", f"`{len(sparse_exact_passes)}`"),
                ("Visible exact-pass draws", f"`{len(visible_exact_passes)}`"),
                ("Sparse exact-pass LRU32 delta", f"`{fmt_int(sparse_lru)}`"),
                ("Visible exact-pass LRU32 delta", f"`{fmt_int(visible_pass_lru)}`"),
                ("Sparse share of LRU32 gain", f"`{sparse_lru_share:.2f}%`" if sparse_lru_share is not None else "`n/a`"),
            ),
        ),
    ]

    lines.extend([
        "",
        "## Proof Verdict",
        "",
        "This verdict decides whether the semantic replay is ready to justify",
        "Xcode replay budget, and whether that replay would be production proof",
        "or only a mechanism/positive-control artifact.",
        "",
        markdown_table(
            (
                "Gate",
                "Verdict",
                "Evidence",
            ),
            proof_verdict_rows(analyses, passes, failures, mixed_groups, primitive_items),
        ),
    ])

    lines.extend([
        "",
        "## Final-Color Visibility",
        "",
        "This is replay evidence only. A no-final-color or sparse final-color",
        "bucket can guide which payloads deserve a same-input proof, but it",
        "is not by itself a production runtime predicate.",
        "",
        markdown_table(
            (
                "Class",
                "Draws",
                "LRU32 delta",
                "Max active px",
                "Draw indexes",
            ),
            (
                (
                    "`no-final-color-exact-pass`",
                    f"`{len(no_final_passes)}`",
                    f"`{fmt_int(sum(item.lru32_delta for item in no_final_passes))}`",
                    f"`{max((item.max_active_pixels for item in no_final_passes), default=0)}`",
                    f"`{','.join(str(item.draw_index) for item in no_final_passes) or 'none'}`",
                ),
                (
                    "`sparse-exact-pass`",
                    f"`{len(sparse_visible_passes)}`",
                    f"`{fmt_int(sum(item.lru32_delta for item in sparse_visible_passes))}`",
                    f"`{max((item.max_active_pixels for item in sparse_visible_passes), default=0)}`",
                    f"`{','.join(str(item.draw_index) for item in sparse_visible_passes) or 'none'}`",
                ),
                (
                    "`visible-exact-pass`",
                    f"`{len(visible_exact_passes)}`",
                    f"`{fmt_int(visible_pass_lru)}`",
                    f"`{max((item.max_active_pixels for item in visible_exact_passes), default=0)}`",
                    f"`{','.join(str(item.draw_index) for item in visible_exact_passes) or 'none'}`",
                ),
                (
                    "`sparse-fail`",
                    f"`{len(sparse_failures)}`",
                    f"`{fmt_int(sum(item.lru32_delta for item in sparse_failures))}`",
                    f"`{max((item.max_active_pixels for item in sparse_failures), default=0)}`",
                    f"`{','.join(str(item.draw_index) for item in sparse_failures) or 'none'}`",
                ),
            ),
        ),
    ])

    if failures:
        lines.extend([
            "",
            "## Semantic Failures",
            "",
            markdown_table(
                (
                    "Draw",
                    "Enc draw",
                    "Ordinal",
                    "Changed",
                    "Max delta",
                "Active bbox",
                "Visibility",
                "Changed bbox",
                "LRU32 delta",
                "Broad group",
            ),
                (
                    (
                        f"`{item.draw_index}`",
                        f"`{item.encoder_draw_index}`",
                        f"`{item.draw_ordinal}`",
                        f"`{item.changed_pixels}`",
                        f"`{as_int(item.summary.get('max_delta'))}`",
                        f"`{item.before_stats.active_bbox or 'none'}`",
                        f"`{item.visibility_class}`",
                        f"`{item.after_stats.changed_bbox or 'none'}`",
                        f"`{item.lru32_delta}`",
                        f"`{item.broad_group_id}`",
                    )
                    for item in failures
                ),
            ),
        ])

    if primitive_items:
        lines.extend([
            "",
            "## Primitive-Owner Predicate Check",
            "",
            "Primitive-owner changes are useful for explaining color failures,",
            "but they are too conservative as a production reject predicate when",
            "exact-pass draws also change visible primitive ownership.",
            "",
            markdown_table(
                (
                    "Draw",
                    "Status",
                    "Primitive changed px",
                    "Color changed px",
                    "Color+primitive px",
                    "Overlap",
                    "Risk",
                ),
                (
                    (
                        f"`{item.draw_index}`",
                        f"`{item.semantic_status}`",
                        f"`{item.primitive_identity_changed_pixels}`",
                        f"`{item.color_changed_pixels}`",
                        f"`{item.color_and_primitive_changed_pixels}`",
                        (
                            f"`{item.color_change_primitive_overlap_pct:.2f}%`"
                            if item.color_change_primitive_overlap_pct is not None else "`n/a`"
                        ),
                        f"`{item.primitive_owner_risk}`",
                    )
                    for item in primitive_items
                ),
            ),
        ])

    oracle_rows = final_writer_oracle_rows(analyses)
    if oracle_rows:
        lines.extend([
            "",
            "## Final-Writer Oracle",
            "",
            "This bucketizes each draw by final-color and primitive-owner replay",
            "evidence. It separates true final-writer hazards from exact-pass",
            "owner changes that keep the final color stable; the latter prove",
            "that a simple owner-change reject gate overrejects useful movement.",
            "",
            markdown_table(
                (
                    "Oracle status",
                    "Draws",
                    "Pass / fail",
                    "LRU32 delta",
                    "Primitive-owner px",
                    "Color px",
                    "Draw indexes",
                    "Next action",
                ),
                oracle_rows,
            ),
        ])

    lines.extend([
        "",
        "## Selector Scout",
        "",
        "This section classifies candidate selectors by whether they can become",
        "a production runtime predicate or should stay as replay/debug evidence.",
        "",
        markdown_table(
            (
                "Selector",
                "Verdict",
                "Evidence",
                "Meaning",
            ),
            selector_scout_rows(analyses, passes, failures, mixed_groups, primitive_items),
        ),
    ])

    candidate_rows = selector_candidate_rows(analyses, passes, failures)
    if candidate_rows:
        lines.extend([
            "",
            "## Selector Candidate Sweep",
            "",
            "This sweep ranks selectors by retained LRU32 gain before spending",
            "another Xcode replay. Rows marked debug-only or trace-local are",
            "evidence for mechanism work, not production runtime predicates.",
            "",
            markdown_table(
                (
                    "Selector",
                    "Verdict",
                    "Kept draws",
                    "Kept fail",
                    "LRU32 delta",
                    "Gain share",
                    "Scope / meaning",
                ),
                candidate_rows,
            ),
        ])

    runtime_rows = runtime_field_combination_rows(
        analyses,
        max_fields=selector_max_fields,
        limit=selector_limit,
    )
    if runtime_rows:
        lines.extend([
            "",
            "## Runtime Field Combination Sweep",
            "",
            "This sweep groups draws by runtime-shaped state/shader fields and",
            "diagnostic geometry/payload hashes, then keeps only value groups",
            "that contain no semantic failures. `runtime-scout` and",
            "`geometry-scout` rows are possible production-design leads;",
            "`trace-local-*` rows are bisection evidence only.",
            "",
            markdown_table(
                (
                    "Fields",
                    "Verdict",
                    "Kept draws",
                    "LRU32 delta",
                    "Gain share",
                    "Mixed / all-fail groups",
                    "Meaning",
                ),
                runtime_rows,
            ),
        ])

    final_color_rows = final_color_runtime_selector_rows(
        analyses,
        max_fields=selector_max_fields,
        limit=selector_limit,
    )
    if final_color_rows:
        lines.extend([
            "",
            "## Final-Color Runtime Selector Sweep",
            "",
            "This sweep targets only `visible-exact-pass` locality gain. It keeps",
            "runtime/geometry/shader field groups that contain visible exact-pass",
            "draws and no semantic failures. The gain share is relative to visible",
            "exact-pass LRU32 movement, not the broader sparse/no-final-color",
            "positive-control movement.",
            "",
            markdown_table(
                (
                    "Fields",
                    "Verdict",
                    "Visible exact draws",
                    "Visible LRU32 delta",
                    "Visible gain share",
                    "Blocked target / all-fail groups",
                    "Meaning",
                ),
                final_color_rows,
            ),
        ])

    final_writer_rows = final_writer_runtime_selector_rows(
        analyses,
        max_fields=selector_max_fields,
        limit=selector_limit,
    )
    if final_writer_rows:
        lines.extend([
            "",
            "## Final-Writer Runtime Selector Sweep",
            "",
            "This sweep targets `owner-change-color-stable` locality movement.",
            "It keeps runtime/geometry/shader field groups that contain those",
            "owner-changing exact-pass draws and no final-writer/color hazards.",
            "Rows marked `runtime-payload-overfit` can isolate replay identity",
            "but are not production predicates.",
            "",
            markdown_table(
                (
                    "Fields",
                    "Verdict",
                    "Owner-stable draws",
                    "Owner-stable LRU32 delta",
                    "Owner-stable gain share",
                    "Blocked target / all-hazard groups",
                    "Meaning",
                ),
                final_writer_rows,
            ),
        ])

    blocker_rows = final_color_runtime_blocker_rows(analyses)
    if blocker_rows:
        lines.extend([
            "",
            "## Final-Color Runtime Blockers",
            "",
            "These rows group draws by every runtime-visible state, geometry, and",
            "shader field currently available to the mini-replay manifest. If a",
            "group contains both visible exact-pass draws and semantic failures,",
            "then no combination of those runtime-visible fields can isolate the",
            "visible safe gain. Remaining separators are trace-local/debug data",
            "such as constant payload hashes or framebuffer replay output.",
            "",
            markdown_table(
                (
                    "Selector",
                    "Verdict",
                    "Visible exact draws",
                    "Fail draws",
                    "Visible LRU32 delta",
                    "Fail LRU32 delta",
                    "Trace-local differing fields",
                    "Meaning",
                ),
                blocker_rows,
            ),
        ])

    lines.extend(["", "## Broad Predicate Groups", ""])
    if not mixed_groups:
        lines.append("No broad state/shader/geometry payload group mixes pass and fail draws.")
    else:
        rows = []
        for group_id, group in sorted(mixed_groups.items()):
            pass_draws = [str(item.draw_index) for item in group if item.semantic_status == "pass"]
            fail_draws = [str(item.draw_index) for item in group if item.semantic_status == "fail"]
            vs_hashes = sorted({item.vsconsts_hash for item in group})
            rows.append((
                f"`{group_id}`",
                f"`{len(group)}`",
                f"`{','.join(pass_draws)}`",
                f"`{','.join(fail_draws)}`",
                f"`{len(vs_hashes)}`",
                f"`{group[0].index_hash}`",
                f"`{group[0].stream0_hash}`",
            ))
        lines.append(markdown_table(
            (
                "Group",
                "Draws",
                "Pass draw indexes",
                "Fail draw indexes",
                "VS const hashes",
                "Index hash",
                "Stream0 hash",
            ),
            rows,
        ))
        lines.extend([
            "",
            "Mixed groups mean the current broad predicate is too weak for a",
            "production reorder gate. A narrower selector must explain the fail draw",
            "without relying on trace-local draw index or ordinal.",
        ])

    lines.extend([
        "",
        "## Draw Details",
        "",
        markdown_table(
            (
                "Draw",
                "Enc",
                "Status",
                "Active px",
                "Active %",
                "Visibility",
                "Active bbox",
                "Changed px",
                "LRU32 delta",
                "Broad group",
                "VS const",
            ),
            (
                (
                    f"`{item.draw_index}`",
                    f"`{item.encoder_draw_index}`",
                    f"`{item.semantic_status}`",
                    f"`{item.before_stats.active_pixels}`",
                    f"`{item.before_stats.active_pct:.6f}%`",
                    f"`{item.visibility_class}`",
                    f"`{item.before_stats.active_bbox or 'none'}`",
                    f"`{item.changed_pixels}`",
                    f"`{item.lru32_delta}`",
                    f"`{item.broad_group_id}`",
                    f"`{item.vsconsts_hash}`",
                )
                for item in analyses
            ),
        ),
    ])

    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True,
                        help="Mini-replay manifest JSON")
    parser.add_argument("--single-draw-summary", type=Path, required=True,
                        help="CSV emitted by the single-draw semantic scan")
    parser.add_argument("--single-draw-dir", type=Path,
                        help="Directory containing drawNNN-original/cacheopt outputs")
    parser.add_argument("--runtime-probe-csv", type=Path,
                        help="Optional 3dmark05-perf-indexed-probe-draws.csv to attach runtime constant/payload hashes by seq/encoder/draw")
    parser.add_argument("--output", type=Path, required=True,
                        help="Markdown report path")
    parser.add_argument("--csv-output", type=Path,
                        help="Optional joined per-draw CSV path")
    parser.add_argument("--selector-csv-output", type=Path,
                        help="Optional selector candidate/runtime field sweep CSV path")
    parser.add_argument("--selector-max-fields", type=int, default=2,
                        help="Max runtime/geometry/shader fields per selector sweep combination (default: 2)")
    parser.add_argument("--selector-limit", type=int, default=10,
                        help="Max rows per runtime selector sweep section (default: 10)")
    parser.add_argument("--active-threshold", type=int, default=0,
                        help="RGB max threshold for active-pixel bounds")
    parser.add_argument("--sparse-active-pixels", type=int, default=32,
                        help="Max active pixels for sparse/no-final-color replay diagnostics")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.selector_max_fields <= 0:
        raise SystemExit("--selector-max-fields must be positive")
    if args.selector_limit <= 0:
        raise SystemExit("--selector-limit must be positive")
    single_draw_dir = args.single_draw_dir or args.single_draw_summary.parent
    analyses = analyze(
        args.manifest,
        args.single_draw_summary,
        single_draw_dir,
        runtime_probe_csv=args.runtime_probe_csv,
        active_threshold=args.active_threshold,
        sparse_active_pixels=args.sparse_active_pixels,
    )
    write_markdown(
        args.output,
        args.manifest,
        args.single_draw_summary,
        analyses,
        selector_max_fields=args.selector_max_fields,
        selector_limit=args.selector_limit,
    )
    if args.csv_output:
        write_csv(args.csv_output, analyses)
    if args.selector_csv_output:
        write_selector_csv(
            args.selector_csv_output,
            analyses,
            selector_max_fields=args.selector_max_fields,
            selector_limit=args.selector_limit,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
