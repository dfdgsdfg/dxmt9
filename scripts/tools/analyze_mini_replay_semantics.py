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

    def csv_row(self) -> dict[str, str]:
        return {
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
            "shader_key": shader_key(self.manifest_draw),
            "state_key": state_key(self.manifest_draw),
        }


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


def analyze(manifest_path: Path,
            summary_path: Path,
            single_draw_dir: Path,
            active_threshold: int = 0,
            sparse_active_pixels: int = 32) -> list[DrawAnalysis]:
    manifest = load_manifest(manifest_path)
    summary = load_summary(summary_path)
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


def write_markdown(path: Path,
                   manifest_path: Path,
                   summary_path: Path,
                   analyses: list[DrawAnalysis]) -> None:
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
    parser.add_argument("--output", type=Path, required=True,
                        help="Markdown report path")
    parser.add_argument("--csv-output", type=Path,
                        help="Optional joined per-draw CSV path")
    parser.add_argument("--active-threshold", type=int, default=0,
                        help="RGB max threshold for active-pixel bounds")
    parser.add_argument("--sparse-active-pixels", type=int, default=32,
                        help="Max active pixels for sparse/no-final-color replay diagnostics")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    single_draw_dir = args.single_draw_dir or args.single_draw_summary.parent
    analyses = analyze(
        args.manifest,
        args.single_draw_summary,
        single_draw_dir,
        active_threshold=args.active_threshold,
        sparse_active_pixels=args.sparse_active_pixels,
    )
    write_markdown(args.output, args.manifest, args.single_draw_summary, analyses)
    if args.csv_output:
        write_csv(args.csv_output, analyses)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
