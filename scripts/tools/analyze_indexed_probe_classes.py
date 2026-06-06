#!/usr/bin/env python3
"""Aggregate indexed probe draw CSV rows by visibility/cache-risk classes."""

from __future__ import annotations

import argparse
import csv
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable


BLEND_FACTOR_NAMES = {
    1: "zero",
    2: "one",
    3: "src-color",
    4: "inv-src-color",
    5: "src-alpha",
    6: "inv-src-alpha",
    7: "dst-alpha",
    8: "inv-dst-alpha",
    9: "dst-color",
    10: "inv-dst-color",
    11: "src-alpha-sat",
}

BLEND_OP_NAMES = {
    1: "add",
    2: "subtract",
    3: "rev-subtract",
    4: "min",
    5: "max",
}

D3D_COMPARE_FUNC_NAMES = {
    1: "never",
    2: "less",
    3: "equal",
    4: "less-equal",
    5: "greater",
    6: "not-equal",
    7: "greater-equal",
    8: "always",
}

WMT_CULL_MODE_NAMES = {
    0: "none",
    1: "front",
    2: "back",
}

WMT_FILL_MODE_NAMES = {
    0: "fill",
    1: "lines",
}


def as_int(value: object, default: int = 0) -> int:
    if value is None:
        return default
    text = str(value).strip()
    if not text:
        return default
    try:
        return int(text, 0)
    except ValueError:
        return default


def as_float(value: object, default: float = 0.0) -> float:
    if value is None:
        return default
    text = str(value).strip().replace(",", "")
    if not text:
        return default
    try:
        return float(text)
    except ValueError:
        return default


def as_bool(value: object) -> bool:
    return as_int(value) != 0


def fmt_int(value: int) -> str:
    return f"{value:,}"


def fmt_pct(value: float | None) -> str:
    if value is None:
        return "n/a"
    return f"{value:+.2f}%"


def pct_delta(after: int, before: int) -> float | None:
    if before == 0:
        return None
    return (after - before) / before * 100.0


def row_key(row: dict[str, str]) -> str:
    return f"{row.get('seq', '')}/{row.get('encoder', '')}"


def blend_kind(row: dict[str, str]) -> str:
    if not as_bool(row.get("alpha_blend")):
        return "blend=off"

    src = as_int(row.get("src_blend"))
    dst = as_int(row.get("dst_blend"))
    op = as_int(row.get("blend_op"))
    separate_alpha = as_bool(row.get("separate_alpha"))
    if not separate_alpha and src == 10 and dst == 2 and op == 1:
        return "blend=screen"
    if not separate_alpha and src == 5 and dst == 6 and op == 1:
        return "blend=standard-alpha"
    if not separate_alpha and src == 5 and dst == 2 and op == 1:
        return "blend=additive-alpha"

    src_name = BLEND_FACTOR_NAMES.get(src, str(src))
    dst_name = BLEND_FACTOR_NAMES.get(dst, str(dst))
    op_name = BLEND_OP_NAMES.get(op, str(op))
    suffix = "+sep-alpha" if separate_alpha else ""
    return f"blend={src_name}:{dst_name}:{op_name}{suffix}"


def class_signature(row: dict[str, str]) -> str:
    depth_enabled = as_bool(row.get("depth_enabled"))
    depth_write = depth_enabled and as_bool(row.get("depth_write"))
    if depth_write:
        depth = "depth=write"
    elif depth_enabled:
        depth = "depth=read"
    else:
        depth = "depth=off"

    scissor = "scissor=on" if as_bool(row.get("scissor")) else "scissor=off"
    texture = "textured=yes" if as_int(row.get("texture_mask")) != 0 else "textured=no"
    large = "large4096=yes" if as_int(row.get("primitive_count")) >= 4096 else "large4096=no"
    color_write = f"color_write={row.get('color_write', '')}"
    return "|".join((depth, blend_kind(row), scissor, texture, large, color_write))


def named_value(row: dict[str, str], field: str, names: dict[int, str]) -> str:
    value = as_int(row.get(field))
    return names.get(value, str(value))


def state_signature(row: dict[str, str]) -> str:
    alpha_test = "alpha_test=on" if as_bool(row.get("alpha_test")) else "alpha_test=off"
    stencil = "stencil=on" if as_bool(row.get("stencil")) else "stencil=off"
    clip = "clip=on" if as_bool(row.get("clip_plane")) else "clip=off"
    depth_func = f"depth_func={named_value(row, 'depth_func', D3D_COMPARE_FUNC_NAMES)}"
    cull = f"cull={named_value(row, 'cull', WMT_CULL_MODE_NAMES)}"
    fill = f"fill={named_value(row, 'fill', WMT_FILL_MODE_NAMES)}"
    return "|".join((alpha_test, depth_func, stencil, clip, cull, fill))


def group_key(row: dict[str, str], mode: str) -> str:
    if mode == "row":
        return row_key(row)
    if mode == "class":
        return class_signature(row)
    if mode == "state-class":
        return f"{class_signature(row)}|{state_signature(row)}"
    if mode == "row-class":
        return f"{row_key(row)}|{class_signature(row)}"
    if mode == "row-state-class":
        return f"{row_key(row)}|{class_signature(row)}|{state_signature(row)}"
    if mode == "blend":
        return blend_kind(row)
    if mode == "row-blend":
        return f"{row_key(row)}|{blend_kind(row)}"
    if mode == "pso":
        return f"pso={row.get('pso', '')}|vs={row.get('vs', '')}|ps={row.get('ps', '')}"
    raise ValueError(f"unsupported group mode: {mode}")


@dataclass
class Aggregate:
    key: str
    rows: int = 0
    original_index_rows: int = 0
    eligible: int = 0
    applied: int = 0
    optimized_eligible: int = 0
    optimized_applied: int = 0
    primitives: int = 0
    vertices: int = 0
    reorder_bytes: int = 0
    original_unique: int = 0
    original_miss16: int = 0
    original_miss32: int = 0
    original_miss64: int = 0
    effective_miss16: int = 0
    effective_miss32: int = 0
    effective_miss64: int = 0
    candidate_rows: int = 0
    candidate_gate_passed: int = 0
    candidate_miss16: int = 0
    candidate_miss32: int = 0
    candidate_miss64: int = 0
    index_buffers: set[str] = field(default_factory=set)
    stream0_handles: set[str] = field(default_factory=set)
    pso_handles: set[str] = field(default_factory=set)
    shader_pairs: set[tuple[str, str]] = field(default_factory=set)
    effective_index_sources: Counter[str] = field(default_factory=Counter)
    row_original_miss32: Counter[str] = field(default_factory=Counter)
    row_effective_miss32: Counter[str] = field(default_factory=Counter)
    row_candidate_miss32: Counter[str] = field(default_factory=Counter)
    row_vertices: Counter[str] = field(default_factory=Counter)
    row_primitives: Counter[str] = field(default_factory=Counter)
    xcode_proxy_gpu_ms: float = 0.0
    xcode_proxy_vs_invocations: float = 0.0
    xcode_proxy_vs_write_mib: float = 0.0
    xcode_proxy_hidden_backend_mib: float = 0.0

    def add(self, row: dict[str, str]) -> None:
        self.rows += 1
        self.original_index_rows += 1 if as_bool(row.get("original_index_available")) else 0
        self.eligible += 1 if as_bool(row.get("eligible")) else 0
        self.applied += 1 if as_bool(row.get("applied")) else 0
        self.optimized_eligible += 1 if as_bool(row.get("optimized_eligible")) else 0
        self.optimized_applied += 1 if as_bool(row.get("optimized_applied")) else 0
        self.primitives += as_int(row.get("primitive_count"))
        self.vertices += as_int(row.get("vertex_count"))
        self.reorder_bytes += as_int(row.get("reorder_bytes"))
        self.original_unique += as_int(row.get("original_index_unique"))
        self.original_miss16 += as_int(row.get("original_cache_miss16"))
        self.original_miss32 += as_int(row.get("original_cache_miss32"))
        self.original_miss64 += as_int(row.get("original_cache_miss64"))
        self.effective_miss16 += as_int(row.get("effective_cache_miss16"))
        self.effective_miss32 += as_int(row.get("effective_cache_miss32"))
        self.effective_miss64 += as_int(row.get("effective_cache_miss64"))
        if as_bool(row.get("candidate_index_available")):
            self.candidate_rows += 1
            self.candidate_gate_passed += 1 if as_bool(row.get("candidate_gate_passed")) else 0
            self.candidate_miss16 += as_int(row.get("candidate_cache_miss16"))
            self.candidate_miss32 += as_int(row.get("candidate_cache_miss32"))
            self.candidate_miss64 += as_int(row.get("candidate_cache_miss64"))
        key = row_key(row)
        self.row_original_miss32[key] += as_int(row.get("original_cache_miss32"))
        self.row_effective_miss32[key] += as_int(row.get("effective_cache_miss32"))
        if as_bool(row.get("candidate_index_available")):
            self.row_candidate_miss32[key] += as_int(row.get("candidate_cache_miss32"))
        self.row_vertices[key] += as_int(row.get("vertex_count"))
        self.row_primitives[key] += as_int(row.get("primitive_count"))
        if row.get("index_buffer"):
            self.index_buffers.add(row["index_buffer"])
        if row.get("stream0_handle"):
            self.stream0_handles.add(row["stream0_handle"])
        if row.get("pso"):
            self.pso_handles.add(row["pso"])
        self.shader_pairs.add((row.get("vs", ""), row.get("ps", "")))
        if as_bool(row.get("applied")) or as_bool(row.get("optimized_applied")):
            source = (row.get("effective_index_source") or "").strip()
            if source:
                self.effective_index_sources[source] += 1

    @property
    def miss32_delta(self) -> int:
        return self.effective_miss32 - self.original_miss32

    @property
    def miss64_delta(self) -> int:
        return self.effective_miss64 - self.original_miss64

    @property
    def miss32_delta_pct(self) -> float | None:
        return pct_delta(self.effective_miss32, self.original_miss32)

    @property
    def miss64_delta_pct(self) -> float | None:
        return pct_delta(self.effective_miss64, self.original_miss64)

    @property
    def candidate_miss32_delta(self) -> int:
        return self.candidate_miss32 - self.original_miss32

    @property
    def candidate_miss64_delta(self) -> int:
        return self.candidate_miss64 - self.original_miss64

    @property
    def candidate_miss32_delta_pct(self) -> float | None:
        if self.candidate_rows == 0:
            return None
        return pct_delta(self.candidate_miss32, self.original_miss32)

    @property
    def candidate_miss64_delta_pct(self) -> float | None:
        if self.candidate_rows == 0:
            return None
        return pct_delta(self.candidate_miss64, self.original_miss64)

    @property
    def ranking_delta32(self) -> int:
        return self.candidate_miss32_delta if self.candidate_rows else self.miss32_delta


def load_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def load_joined_rows(path: Path) -> dict[str, dict[str, str]]:
    rows: dict[str, dict[str, str]] = {}
    with path.open(newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            seq = (row.get("seq") or "").strip()
            enc = (row.get("enc") or row.get("encoder") or "").strip()
            if seq and enc:
                rows[f"{seq}/{enc}"] = row
    return rows


def filter_rows(rows: Iterable[dict[str, str]], row_filters: set[str]) -> list[dict[str, str]]:
    if not row_filters:
        return list(rows)
    return [row for row in rows if row_key(row) in row_filters]


def aggregate_rows(rows: Iterable[dict[str, str]], mode: str) -> list[Aggregate]:
    by_key: dict[str, Aggregate] = {}
    for row in rows:
        key = group_key(row, mode)
        by_key.setdefault(key, Aggregate(key)).add(row)
    return sorted(
        by_key.values(),
        key=lambda agg: (abs(agg.ranking_delta32), agg.original_miss32, agg.primitives, agg.rows),
        reverse=True,
    )


def xcode_hidden_backend_mib(row: dict[str, str]) -> float:
    explicit = row.get("dxmt_hidden_backend_write_mib")
    if explicit not in (None, ""):
        return as_float(explicit)
    vs_write = as_float(row.get("vs_buffer_write_mib"))
    named_tiled = as_float(row.get("dxmt_named_tiled_buffer_mib"))
    cpu_writer = as_float(row.get("dxmt_cpu_writer_mib"))
    return max(vs_write - named_tiled - cpu_writer, 0.0)


def row_weight_counter(agg: Aggregate, mode: str) -> Counter[str]:
    if mode == "effective-miss32":
        return agg.row_effective_miss32
    if mode == "candidate-miss32":
        return agg.row_candidate_miss32 if agg.candidate_rows else agg.row_effective_miss32
    if mode == "original-miss32":
        return agg.row_original_miss32
    if mode == "vertices":
        return agg.row_vertices
    if mode == "primitives":
        return agg.row_primitives
    raise ValueError(f"unsupported Xcode proxy weight mode: {mode}")


def assign_xcode_proxies(
    aggregates: list[Aggregate],
    joined_rows: dict[str, dict[str, str]],
    *,
    weight_mode: str,
) -> None:
    totals: Counter[str] = Counter()
    for agg in aggregates:
        totals.update(row_weight_counter(agg, weight_mode))
    for agg in aggregates:
        for key, weight in row_weight_counter(agg, weight_mode).items():
            total = totals.get(key, 0)
            row = joined_rows.get(key)
            if not row or total <= 0:
                continue
            share = weight / total
            agg.xcode_proxy_gpu_ms += as_float(row.get("gpu_ms")) * share
            agg.xcode_proxy_vs_invocations += as_float(row.get("vs_invocations")) * share
            agg.xcode_proxy_vs_write_mib += as_float(row.get("vs_buffer_write_mib")) * share
            agg.xcode_proxy_hidden_backend_mib += xcode_hidden_backend_mib(row) * share


def write_csv(path: Path, aggregates: list[Aggregate], *, include_xcode_proxy: bool) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fieldnames = [
        "group",
        "draws",
        "original_index_rows",
        "eligible",
        "applied",
        "optimized_eligible",
        "optimized_applied",
        "primitives",
        "vertices",
        "reorder_bytes",
        "original_unique",
        "original_miss16",
        "effective_miss16",
        "miss16_delta",
        "miss16_delta_pct",
        "original_miss32",
        "effective_miss32",
        "miss32_delta",
        "miss32_delta_pct",
        "original_miss64",
        "effective_miss64",
        "miss64_delta",
        "miss64_delta_pct",
        "candidate_rows",
        "candidate_gate_passed",
        "candidate_miss32",
        "candidate_miss32_delta",
        "candidate_miss32_delta_pct",
        "candidate_miss64",
        "candidate_miss64_delta",
        "candidate_miss64_delta_pct",
        "unique_index_buffers",
        "unique_stream0_handles",
        "unique_pso_handles",
        "unique_shader_pairs",
        "applied_effective_index_sources",
        "semantic_risk",
        "proof_family",
        "candidate_action",
        "preflight_gate",
        "xcode_replay_gate",
    ]
    if include_xcode_proxy:
        fieldnames.extend([
            "xcode_proxy_gpu_ms",
            "xcode_proxy_vs_invocations",
            "xcode_proxy_vs_write_mib",
            "xcode_proxy_hidden_backend_mib",
        ])
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for agg in aggregates:
            record = {
                "group": agg.key,
                "draws": agg.rows,
                "original_index_rows": agg.original_index_rows,
                "eligible": agg.eligible,
                "applied": agg.applied,
                "optimized_eligible": agg.optimized_eligible,
                "optimized_applied": agg.optimized_applied,
                "primitives": agg.primitives,
                "vertices": agg.vertices,
                "reorder_bytes": agg.reorder_bytes,
                "original_unique": agg.original_unique,
                "original_miss16": agg.original_miss16,
                "effective_miss16": agg.effective_miss16,
                "miss16_delta": agg.effective_miss16 - agg.original_miss16,
                "miss16_delta_pct": "" if agg.original_miss16 == 0 else f"{pct_delta(agg.effective_miss16, agg.original_miss16):.6f}",
                "original_miss32": agg.original_miss32,
                "effective_miss32": agg.effective_miss32,
                "miss32_delta": agg.miss32_delta,
                "miss32_delta_pct": "" if agg.miss32_delta_pct is None else f"{agg.miss32_delta_pct:.6f}",
                "original_miss64": agg.original_miss64,
                "effective_miss64": agg.effective_miss64,
                "miss64_delta": agg.miss64_delta,
                "miss64_delta_pct": "" if agg.miss64_delta_pct is None else f"{agg.miss64_delta_pct:.6f}",
                "candidate_rows": agg.candidate_rows,
                "candidate_gate_passed": agg.candidate_gate_passed,
                "candidate_miss32": agg.candidate_miss32,
                "candidate_miss32_delta": agg.candidate_miss32_delta if agg.candidate_rows else "",
                "candidate_miss32_delta_pct": "" if agg.candidate_miss32_delta_pct is None else f"{agg.candidate_miss32_delta_pct:.6f}",
                "candidate_miss64": agg.candidate_miss64,
                "candidate_miss64_delta": agg.candidate_miss64_delta if agg.candidate_rows else "",
                "candidate_miss64_delta_pct": "" if agg.candidate_miss64_delta_pct is None else f"{agg.candidate_miss64_delta_pct:.6f}",
                "unique_index_buffers": len(agg.index_buffers),
                "unique_stream0_handles": len(agg.stream0_handles),
                "unique_pso_handles": len(agg.pso_handles),
                "unique_shader_pairs": len(agg.shader_pairs),
                "applied_effective_index_sources": format_source_counter(
                    agg.effective_index_sources),
                "semantic_risk": semantic_risk(agg),
                "proof_family": proof_family(agg),
                "candidate_action": candidate_action(agg),
                "preflight_gate": preflight_gate(agg),
                "xcode_replay_gate": xcode_replay_gate(agg),
            }
            if include_xcode_proxy:
                record.update({
                    "xcode_proxy_gpu_ms": f"{agg.xcode_proxy_gpu_ms:.9f}",
                    "xcode_proxy_vs_invocations": f"{agg.xcode_proxy_vs_invocations:.9f}",
                    "xcode_proxy_vs_write_mib": f"{agg.xcode_proxy_vs_write_mib:.9f}",
                    "xcode_proxy_hidden_backend_mib": f"{agg.xcode_proxy_hidden_backend_mib:.9f}",
                })
            writer.writerow(record)


def format_source_counter(counter: Counter[str]) -> str:
    if not counter:
        return ""
    return ",".join(f"{source}:{count}" for source, count in counter.most_common())


def semantic_risk(agg: Aggregate) -> str:
    key = agg.key
    opaque_depth = (
        "depth=write" in key and
        "blend=off" in key and
        "alpha_test=off" in key and
        "stencil=off" in key and
        "clip=off" in key and
        ("depth_func=less-equal" in key or "depth_func=less" in key)
    )
    if opaque_depth:
        return "low-opaque-depth-write"
    if "blend=screen" in key:
        return "screen-blend-tolerance"
    if "blend=standard-alpha" in key or "blend=additive-alpha" in key:
        return "high-alpha-order-dependent"
    if "depth=read" in key and "blend=off" in key:
        return "medium-depth-read-order-sensitive"
    if "depth=read" in key:
        return "medium-depth-read-visibility"
    if "blend=off" in key:
        return "low-no-alpha-blend"
    return "unknown"


def candidate_action(agg: Aggregate) -> str:
    risk = semantic_risk(agg)
    if risk == "low-opaque-depth-write":
        return "production opt-in candidate; require stable row and Xcode VS-inv/write movement"
    if risk == "screen-blend-tolerance":
        return "profiling ceiling; require same-input exact or explicit lsb1 semantic policy"
    if risk == "high-alpha-order-dependent":
        return "do not promote primitive reorder; prefer non-reorder backend-shape A/B"
    if risk == "medium-depth-read-order-sensitive":
        return "mechanism only until real-depth exact semantic replay passes; otherwise non-reorder"
    if risk == "medium-depth-read-visibility":
        return "visibility-sensitive; require same-input semantic proof before Xcode proof"
    if risk == "low-no-alpha-blend":
        return "check depth/write state before treating as reorder-safe"
    return "inspect state and add a narrower proof gate"


def proof_family(agg: Aggregate) -> str:
    risk = semantic_risk(agg)
    if risk == "low-opaque-depth-write":
        return "production-opaque-reorder"
    if risk == "screen-blend-tolerance":
        return "explicit-tolerance-reorder"
    if risk in ("medium-depth-read-order-sensitive", "medium-depth-read-visibility"):
        return "semantic-proof-or-non-reorder"
    if risk == "high-alpha-order-dependent":
        return "non-reorder-backend-shape-or-semantic-proof"
    if risk == "low-no-alpha-blend":
        return "state-audit-before-reorder"
    return "manual-inspection"


def preflight_gate(agg: Aggregate) -> str:
    risk = semantic_risk(agg)
    if risk == "low-opaque-depth-write":
        return (
            "stable row shape; cached-reordered hits; cache-opt LRU32 decrease; "
            "no color/blend visibility mutation"
        )
    if risk == "screen-blend-tolerance":
        return (
            "same-input mini replay; exact pass or explicit lsb1 policy; "
            "stable row shape; cached-reordered hits"
        )
    if risk == "medium-depth-read-order-sensitive":
        return (
            "real-depth same-input mini replay exact pass; reject primitive-owner "
            "color conflicts; otherwise use non-reorder smoke"
        )
    if risk == "medium-depth-read-visibility":
        return (
            "same-input semantic replay with real depth and textures; stable row "
            "shape before any Xcode replay"
        )
    if risk == "high-alpha-order-dependent":
        return (
            "do not reorder by default; require exact/tolerance replay or choose "
            "primitive-order-preserving backend-shape smoke"
        )
    if risk == "low-no-alpha-blend":
        return (
            "confirm depth/write/color-write state; stable row shape; image gate "
            "before treating as reorder-safe"
        )
    return "manual state audit and a narrow proof gate"


def xcode_replay_gate(agg: Aggregate) -> str:
    risk = semantic_risk(agg)
    common = "stable top-row geometry; target VS invocations/write decrease"
    if risk == "low-opaque-depth-write":
        return (
            f"{common}; target reordered-cache hits; no non-target hot-row "
            "regression"
        )
    if risk == "screen-blend-tolerance":
        return (
            f"{common}; target cache-opt decrease; semantic image policy recorded "
            "as explicit proof"
        )
    if risk in ("medium-depth-read-order-sensitive", "medium-depth-read-visibility"):
        return (
            "spend Xcode only after semantic preflight passes or after a "
            "primitive-order-preserving smoke shows backend movement; add "
            f"--require-tvb-mechanism-proof for backend-shape A/B; {common}"
        )
    if risk == "high-alpha-order-dependent":
        return (
            "spend Xcode only for non-reorder backend-shape A/B or an explicit "
            "semantic proof artifact; backend-shape runs require "
            f"--require-tvb-mechanism-proof; {common}"
        )
    if risk == "low-no-alpha-blend":
        return f"{common}; include output/image gate and depth-state proof"
    return f"{common}; document custom proof gate"


def row_key_from_group(group: str) -> str | None:
    first = group.split("|", 1)[0]
    if "/" not in first:
        return None
    seq, enc = first.split("/", 1)
    if seq.isdigit() and enc.isdigit():
        return first
    return None


def is_backend_shape_candidate(agg: Aggregate) -> bool:
    return proof_family(agg) in (
        "semantic-proof-or-non-reorder",
        "non-reorder-backend-shape-or-semantic-proof",
    )


def backend_shape_candidates(aggregates: list[Aggregate], top: int) -> list[Aggregate]:
    return sorted(
        [agg for agg in aggregates if is_backend_shape_candidate(agg)],
        key=lambda item: (
            item.xcode_proxy_hidden_backend_mib,
            item.xcode_proxy_vs_write_mib,
            item.primitives,
        ),
        reverse=True,
    )[:top]


def rejected_backend_shape_axes() -> list[tuple[str, str, str]]:
    return [
        (
            "depth-write/depth-func/scissor/cull broad probes",
            "Prior scoped probes did not materially move Xcode VS write.",
            "Do not spend another gputrace unless the candidate also changes a new backend/compiler mechanism.",
        ),
        (
            "split-large-indexed-draws",
            "Draw partitioning changed shape or left VS write unchanged in the relevant hot rows.",
            "Use only as a diagnostic, not as the next backend-shape proof candidate.",
        ),
        (
            "force-expand indexed",
            "Flattening destroys indexed reuse and raises VS invocations/backend write.",
            "Reject as an optimization path; preserve indexed submission.",
        ),
        (
            "texture-white / material-source mutation",
            "It moves secondary counters but does not reduce target GPU enough.",
            "Keep as a classifier, not a TVB mechanism proof candidate.",
        ),
        (
            "visible VSOut trimming / position-only / compiler-visible scratch",
            "Prior Xcode and mini-replay A/B left hidden VS write mostly unchanged.",
            "Do not rerun without a new backend payload or invocation-count hypothesis.",
        ),
        (
            "X8/RT PixelFormatView suppression",
            "Attachment/view probes did not affect the dominant hot encoder VS write.",
            "Reject as the primary GT1 bottleneck fix.",
        ),
    ]


def markdown_report(input_path: Path,
                    aggregates: list[Aggregate],
                    *,
                    group_mode: str,
                    rows: list[str],
                    top: int,
                    joined_summary: Path | None,
                    xcode_proxy_weight: str) -> str:
    lines = [
        "# Indexed Probe Class Breakdown",
        "",
        f"- Input: `{input_path}`",
        f"- Group mode: `{group_mode}`",
    ]
    if rows:
        lines.append(f"- Row filter: `{', '.join(rows)}`")
    if joined_summary:
        lines.append(f"- Xcode proxy summary: `{joined_summary}`")
        lines.append(f"- Xcode proxy weight: `{xcode_proxy_weight}`")
    lines.extend([
        "",
        "| Group | draws | applied | primitives | original LRU32 | effective LRU32 | effective Δ | candidate rows | candidate LRU32 | candidate Δ | reorder bytes | applied sources | unique IB/stream/PSO/shader |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---:|",
    ])
    for agg in aggregates[:top]:
        unique_summary = (
            f"{len(agg.index_buffers)}/"
            f"{len(agg.stream0_handles)}/"
            f"{len(agg.pso_handles)}/"
            f"{len(agg.shader_pairs)}"
        )
        lines.append(
            "| "
            f"`{agg.key}` | "
            f"`{fmt_int(agg.rows)}` | "
            f"`{fmt_int(agg.applied)}` | "
            f"`{fmt_int(agg.primitives)}` | "
            f"`{fmt_int(agg.original_miss32)}` | "
            f"`{fmt_int(agg.effective_miss32)}` | "
            f"`{fmt_int(agg.miss32_delta)}` ({fmt_pct(agg.miss32_delta_pct)}) | "
            f"`{fmt_int(agg.candidate_rows)}` | "
            f"`{fmt_int(agg.candidate_miss32) if agg.candidate_rows else 'n/a'}` | "
            f"`{fmt_int(agg.candidate_miss32_delta) if agg.candidate_rows else 'n/a'}` "
            f"({fmt_pct(agg.candidate_miss32_delta_pct)}) | "
            f"`{fmt_int(agg.reorder_bytes)}` | "
            f"`{format_source_counter(agg.effective_index_sources) or 'n/a'}` | "
            f"`{unique_summary}` |"
        )
    if joined_summary:
        lines.extend([
            "",
            "## Xcode Proxy",
            "",
            "The proxy allocates each row's Xcode counters across class groups by "
            f"`{xcode_proxy_weight}`. Treat this as a ranking signal, not a "
            "replacement for a class-scoped gputrace.",
            "",
            "| Group | proxy GPU ms | proxy VS invocations | proxy VS write MiB | proxy hidden backend MiB |",
            "|---|---:|---:|---:|---:|",
        ])
        for agg in sorted(
            aggregates,
            key=lambda item: (
                item.xcode_proxy_hidden_backend_mib,
                item.xcode_proxy_vs_write_mib,
                item.effective_miss32,
            ),
            reverse=True,
        )[:top]:
            lines.append(
                "| "
                f"`{agg.key}` | "
                f"`{agg.xcode_proxy_gpu_ms:.3f}` | "
                f"`{agg.xcode_proxy_vs_invocations:,.0f}` | "
                f"`{agg.xcode_proxy_vs_write_mib:.3f}` | "
            f"`{agg.xcode_proxy_hidden_backend_mib:.3f}` |"
            )
        lines.extend([
            "",
            "## Candidate Advice",
            "",
            "This advice classifies primitive-order candidates by semantic risk. "
            "It does not replace image gates or Xcode replay; it selects which "
            "proof path should be used next.",
            "",
            "| Group | Semantic risk | Proxy hidden backend MiB | Recommended next action |",
            "|---|---|---:|---|",
        ])
        for agg in sorted(
            aggregates,
            key=lambda item: (
                item.xcode_proxy_hidden_backend_mib,
                item.original_miss32,
                item.primitives,
            ),
            reverse=True,
        )[:top]:
            lines.append(
                "| "
                f"`{agg.key}` | "
                f"`{semantic_risk(agg)}` | "
                f"`{agg.xcode_proxy_hidden_backend_mib:.3f}` | "
                f"{candidate_action(agg)} |"
            )
        lines.extend([
            "",
            "## Next Experiment Queue",
            "",
            "Rows are sorted by proxy hidden-backend size. `Proof family` decides "
            "whether this is a production-shaped reorder, an explicit tolerance "
            "artifact, or a non-reorder backend-shape candidate before spending "
            "Xcode capture/export time.",
            "",
            "| Group | Proof family | Preflight gate | Xcode replay gate |",
            "|---|---|---|---|",
        ])
        for agg in sorted(
            aggregates,
            key=lambda item: (
                item.xcode_proxy_hidden_backend_mib,
                abs(item.ranking_delta32),
                item.primitives,
            ),
            reverse=True,
        )[:top]:
            lines.append(
                "| "
                f"`{agg.key}` | "
                f"`{proof_family(agg)}` | "
                f"{preflight_gate(agg)} | "
                f"{xcode_replay_gate(agg)} |"
            )
        backend_candidates = backend_shape_candidates(aggregates, min(top, 5))
        if backend_candidates:
            first_candidate = backend_candidates[0]
            target_row = row_key_from_group(first_candidate.key) or "<seq/enc>"
            frame = target_row.split("/", 1)[0] if "/" in target_row else "<frame>"
            lines.extend([
                "",
                "## Backend-Shape A/B Command Template",
                "",
                "Use this only for primitive-order-preserving candidates. The smoke "
                "run should keep row/draw/vertex/triangle shape stable before a "
                "`.gputrace` replay is worth Xcode export time.",
                "",
                "Top backend-shape candidates by proxy hidden-backend size:",
                "",
                "| Group | Row | Proxy hidden backend MiB | Proof family |",
                "|---|---|---:|---|",
            ])
            for agg in backend_candidates:
                lines.append(
                    "| "
                    f"`{agg.key}` | "
                    f"`{row_key_from_group(agg.key) or 'n/a'}` | "
                    f"`{agg.xcode_proxy_hidden_backend_mib:.3f}` | "
                    f"`{proof_family(agg)}` |"
                )
            lines.extend([
                "",
                "No-gputrace shape smoke:",
                "",
                "```bash",
                "scripts/tools/run_3dmark05_perf_probe.sh \\",
                "  --suffix <backend-shape-smoke> \\",
                f"  --frame {frame} \\",
                f"  --encoder-breakdown-seq {frame} \\",
                "  --no-gputrace \\",
                "  --timeout 240 \\",
                "  <primitive-order-preserving-backend-shape-option>",
                "```",
                "",
                "Gputrace/Xcode proof candidate:",
                "",
                "```bash",
                "scripts/tools/run_3dmark05_perf_probe.sh \\",
                "  --suffix <backend-shape-gputrace> \\",
                f"  --frame {frame} \\",
                f"  --encoder-breakdown-seq {frame} \\",
                f"  --baseline-joined {joined_summary} \\",
                f"  --target-row-key {target_row} \\",
                "  --require-tvb-mechanism-proof \\",
                "  --require-top-row-key-match \\",
                "  --max-top-draw-call-delta-ratio 0.05 \\",
                "  --max-top-vertex-count-delta-ratio 0.05 \\",
                "  --max-top-triangle-delta-ratio 0.05 \\",
                "  --timeout 420 \\",
                "  <primitive-order-preserving-backend-shape-option>",
                "```",
                "",
                "Known rejected backend-shape axes:",
                "",
                "| Axis | Evidence | Decision |",
                "|---|---|---|",
            ])
            for axis, evidence, decision in rejected_backend_shape_axes():
                lines.append(f"| `{axis}` | {evidence} | {decision} |")
    lines.append("")
    return "\n".join(lines)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe_draws_csv", type=Path)
    parser.add_argument("--row", action="append", default=[], help="Only include a seq/encoder row such as 50/2")
    parser.add_argument(
        "--group",
        choices=(
            "row",
            "class",
            "state-class",
            "row-class",
            "row-state-class",
            "blend",
            "row-blend",
            "pso",
        ),
        default="row-class",
    )
    parser.add_argument("--top", type=int, default=20)
    parser.add_argument("--min-draws", type=int, default=1)
    parser.add_argument(
        "--joined-summary",
        type=Path,
        help="Optional frameN-xcode-dxmt-joined-summary.csv for row-level Xcode proxy attribution",
    )
    parser.add_argument(
        "--xcode-proxy-weight",
        choices=("effective-miss32", "candidate-miss32", "original-miss32", "vertices", "primitives"),
        default="effective-miss32",
        help="Per-row weight used to allocate Xcode row counters across groups",
    )
    parser.add_argument("--output", type=Path)
    parser.add_argument("--csv-output", type=Path)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    rows = filter_rows(load_rows(args.probe_draws_csv), set(args.row))
    aggregates = [
        agg for agg in aggregate_rows(rows, args.group)
        if agg.rows >= args.min_draws
    ]
    if args.joined_summary:
        if not args.joined_summary.is_file():
            raise SystemExit(f"missing joined summary: {args.joined_summary}")
        assign_xcode_proxies(
            aggregates,
            load_joined_rows(args.joined_summary),
            weight_mode=args.xcode_proxy_weight,
        )

    if args.csv_output:
        write_csv(args.csv_output, aggregates, include_xcode_proxy=bool(args.joined_summary))

    report = markdown_report(
        args.probe_draws_csv,
        aggregates,
        group_mode=args.group,
        rows=args.row,
        top=args.top,
        joined_summary=args.joined_summary,
        xcode_proxy_weight=args.xcode_proxy_weight,
    )
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(report, encoding="utf-8")
    else:
        print(report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
