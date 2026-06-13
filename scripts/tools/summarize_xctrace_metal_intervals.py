#!/usr/bin/env python3
"""Summarize xctrace Metal GPU intervals and join dxmt9 encoder attribution.

This is a fallback/sidecar for 3DMark05 runs where Xcode `.gputrace` capture is
blocked by the Metal capture layer. It consumes XML exported from xctrace's
`metal-gpu-intervals` table and the dxmt9 `3dmark05-perf-encoders.csv`, then
joins rows by `RenderPass[seq=...,enc=...]` labels.
"""

from __future__ import annotations

import argparse
import csv
import math
import re
import xml.etree.ElementTree as ET
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any


RENDER_PASS_LABEL_RE = re.compile(
    r"RenderPass\[seq=(\d+),enc=(\d+),rt=(0x[0-9a-fA-F]+),depth=(0x[0-9a-fA-F]+)\]"
)


def parse_duration_ms(value: str) -> float:
    text = (value or "").strip().replace(",", "")
    if not text:
        return 0.0
    parts = text.split()
    try:
        amount = float(parts[0])
    except (ValueError, IndexError):
        return 0.0
    unit = parts[1] if len(parts) > 1 else "ms"
    if unit.startswith("ns"):
        return amount / 1_000_000.0
    if unit.startswith("us") or unit.startswith("\u00b5s"):
        return amount / 1_000.0
    if unit.startswith("ms"):
        return amount
    if unit.startswith("s"):
        return amount * 1_000.0
    return amount


def as_int(value: Any) -> int:
    try:
        text = str(value).strip()
        if text.startswith(("0x", "0X")):
            return int(text, 16)
        return int(float(text))
    except (TypeError, ValueError):
        return 0


def as_float(value: Any) -> float:
    try:
        return float(str(value).strip() or 0.0)
    except (TypeError, ValueError):
        return 0.0


def ratio_ms_per_million(ms: Any, count: Any) -> float:
    denominator = as_float(count)
    if denominator <= 0.0:
        return 0.0
    return as_float(ms) / (denominator / 1_000_000.0)


def primitive_class(row: dict[str, Any]) -> str:
    opaque = as_int(row.get("indexed_triangle_opaque_depth_write_primitives"))
    alpha = as_int(row.get("indexed_triangle_alpha_blend_primitives"))
    primitives = as_int(row.get("primitive_count"))
    if opaque > 0 and alpha > 0:
        return "mixed-indexed"
    if opaque > 0:
        return "opaque-depth-indexed"
    if alpha > 0:
        return "alpha-blend-indexed"
    if primitives > 0:
        return "other-primitive"
    return "unknown"


def is_textured_probe_draw(row: dict[str, str]) -> bool:
    return as_int(row.get("texture_mask")) != 0


def is_depth_only_probe_draw(row: dict[str, str]) -> bool:
    return (
        as_int(row.get("depth_enabled")) != 0
        and as_int(row.get("depth_write")) != 0
        and as_int(row.get("color_write")) == 0
        and as_int(row.get("alpha_blend")) == 0
        and as_int(row.get("alpha_test")) == 0
    )


def probe_draw_route_bucket(row: dict[str, str]) -> str:
    if is_depth_only_probe_draw(row):
        return "depth-only"
    if is_textured_probe_draw(row):
        return "programmable-textured"
    return "programmable-color"


def classify_route_group(
    *,
    primitives: int,
    depth_only_primitives: int,
    programmable_textured_primitives: int,
    programmable_color_primitives: int,
    alpha_blend_primitives: int,
    alpha_test_primitives: int,
) -> tuple[str, str]:
    if primitives == 0:
        return ("route-unavailable", "no indexed probe primitives for this row")
    depth_share = depth_only_primitives / primitives * 100.0
    textured_share = programmable_textured_primitives / primitives * 100.0
    color_share = programmable_color_primitives / primitives * 100.0
    blend_share = alpha_blend_primitives / primitives * 100.0
    alpha_test_share = alpha_test_primitives / primitives * 100.0
    if depth_share >= 80.0:
        return (
            "candidate-depth-only-route",
            f"depth-only candidate covers {depth_share:.2f}% of primitives",
        )
    if textured_share >= 50.0:
        return (
            "needs-programmable-textured-route",
            f"programmable textured draws cover {textured_share:.2f}% of primitives",
        )
    if color_share >= 50.0:
        return (
            "needs-programmable-color-route",
            f"programmable non-textured color draws cover {color_share:.2f}% of primitives",
        )
    if blend_share >= 10.0 or alpha_test_share >= 10.0:
        return (
            "order-dependent-fragment-route",
            f"alpha blend {blend_share:.2f}% / alpha test {alpha_test_share:.2f}% of primitives",
        )
    return ("mixed-programmable-route", "no single programmable route class dominates")


def percentile(sorted_values: list[float], fraction: float) -> float:
    if not sorted_values:
        return 0.0
    index = (len(sorted_values) - 1) * fraction
    low = math.floor(index)
    high = math.ceil(index)
    if low == high:
        return sorted_values[low]
    return sorted_values[low] * (high - index) + sorted_values[high] * (index - low)


def distribution(values: list[float]) -> tuple[float, float, float, float]:
    if not values:
        return (0.0, 0.0, 0.0, 0.0)
    sorted_values = sorted(values)
    return (
        sum(sorted_values) / len(sorted_values),
        percentile(sorted_values, 0.50),
        percentile(sorted_values, 0.95),
        percentile(sorted_values, 0.99),
    )


def aggregate_by(rows: list[dict[str, Any]], key: str) -> list[dict[str, Any]]:
    groups: dict[str, dict[str, Any]] = defaultdict(
        lambda: {
            "group": "",
            "rows": 0,
            "xctrace_stage_sum_ms": 0.0,
            "xctrace_vertex_ms": 0.0,
            "xctrace_fragment_ms": 0.0,
            "xctrace_other_ms": 0.0,
            "draw_calls": 0,
            "primitive_count": 0,
            "vertex_count": 0,
        }
    )
    for row in rows:
        group_name = str(row.get(key) or "unknown")
        group = groups[group_name]
        group["group"] = group_name
        group["rows"] += 1
        for metric in (
            "xctrace_stage_sum_ms",
            "xctrace_vertex_ms",
            "xctrace_fragment_ms",
            "xctrace_other_ms",
        ):
            group[metric] += as_float(row.get(metric))
        for metric in ("draw_calls", "primitive_count", "vertex_count"):
            group[metric] += as_int(row.get(metric))
    return sorted(groups.values(), key=lambda row: as_float(row["xctrace_stage_sum_ms"]), reverse=True)


def parse_xctrace_rows(path: Path) -> list[dict[str, str]]:
    tree = ET.parse(path)
    root = tree.getroot()
    ref_map = {node.get("id"): node for node in root.iter() if node.get("id") is not None}
    rows: list[dict[str, str]] = []
    headers: list[str] | None = None

    for row in root.findall(".//row"):
        columns = list(row)
        if headers is None:
            seen: Counter[str] = Counter()
            headers = []
            for column in columns:
                seen[column.tag] += 1
                headers.append(column.tag if seen[column.tag] == 1 else f"{column.tag}_{seen[column.tag]}")

        parsed: dict[str, str] = {}
        for index, column in enumerate(columns):
            ref = column.get("ref")
            resolved = ref_map.get(ref, column) if ref else column
            key = headers[index] if headers and index < len(headers) else f"col_{index}"
            parsed[key] = resolved.get("fmt", resolved.text or "")
        rows.append(parsed)

    return rows


def parse_render_pass_label(row: dict[str, str]) -> tuple[int, int, str, str] | None:
    # `metal-gpu-intervals` exposes the label as formatted-label. Some other
    # xctrace tables put pieces in metal-object-label columns, so fall back to
    # scanning the whole row when needed.
    candidates = [row.get("formatted-label", "")]
    candidates.extend(str(value) for value in row.values())
    for candidate in candidates:
        match = RENDER_PASS_LABEL_RE.search(candidate)
        if match:
            return (
                int(match.group(1)),
                int(match.group(2)),
                match.group(3),
                match.group(4),
            )
    return None


def load_dxmt_encoders(path: Path) -> dict[tuple[int, int], dict[str, str]]:
    if not path:
        return {}
    with path.open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))
    by_key: dict[tuple[int, int], dict[str, str]] = {}
    for row in rows:
        seq_text = row.get("seq")
        enc_text = row.get("encoder")
        seq = as_int(seq_text)
        enc = as_int(enc_text)
        if seq_text not in (None, "") and enc_text not in (None, ""):
            by_key[(seq, enc)] = row
    return by_key


def load_indexed_probe_routes(path: Path | None) -> dict[tuple[int, int], dict[str, Any]]:
    if path is None or not str(path):
        return {}
    if not path.exists():
        return {}
    with path.open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))

    grouped: dict[tuple[int, int], list[dict[str, str]]] = defaultdict(list)
    for row in rows:
        seq_text = row.get("seq")
        enc_text = row.get("encoder") or row.get("enc")
        if seq_text in (None, "") or enc_text in (None, ""):
            continue
        grouped[(as_int(seq_text), as_int(enc_text))].append(row)

    result: dict[tuple[int, int], dict[str, Any]] = {}
    for key, group_rows in grouped.items():
        primitives = 0
        vertices = 0
        depth_only_primitives = 0
        programmable_textured_primitives = 0
        programmable_color_primitives = 0
        alpha_blend_primitives = 0
        alpha_test_primitives = 0
        for row in group_rows:
            primitive_count = as_int(row.get("primitive_count"))
            primitives += primitive_count
            vertices += as_int(row.get("vertex_count"))
            if as_int(row.get("alpha_blend")) != 0:
                alpha_blend_primitives += primitive_count
            if as_int(row.get("alpha_test")) != 0:
                alpha_test_primitives += primitive_count
            bucket = probe_draw_route_bucket(row)
            if bucket == "depth-only":
                depth_only_primitives += primitive_count
            elif bucket == "programmable-textured":
                programmable_textured_primitives += primitive_count
            else:
                programmable_color_primitives += primitive_count
        verdict, reason = classify_route_group(
            primitives=primitives,
            depth_only_primitives=depth_only_primitives,
            programmable_textured_primitives=programmable_textured_primitives,
            programmable_color_primitives=programmable_color_primitives,
            alpha_blend_primitives=alpha_blend_primitives,
            alpha_test_primitives=alpha_test_primitives,
        )
        result[key] = {
            "route_verdict": verdict,
            "route_reason": reason,
            "route_draws": len(group_rows),
            "route_primitives": primitives,
            "route_vertices": vertices,
            "route_depth_only_primitives": depth_only_primitives,
            "route_programmable_textured_primitives": programmable_textured_primitives,
            "route_programmable_color_primitives": programmable_color_primitives,
        }
    return result


def aggregate_gpu_intervals(rows: list[dict[str, str]]) -> list[dict[str, Any]]:
    by_key: dict[tuple[int, int], dict[str, Any]] = defaultdict(
        lambda: {
            "xctrace_vertex_ms": 0.0,
            "xctrace_fragment_ms": 0.0,
            "xctrace_other_ms": 0.0,
            "xctrace_rows": 0,
            "rt": "",
            "depth": "",
            "process": "",
            "gpu_frames": set(),
        }
    )

    for row in rows:
        parsed = parse_render_pass_label(row)
        if parsed is None:
            continue
        seq, enc, rt, depth = parsed
        current = by_key[(seq, enc)]
        current["seq"] = seq
        current["encoder"] = enc
        current["rt"] = rt
        current["depth"] = depth
        current["process"] = row.get("process", "")
        current["xctrace_rows"] += 1
        current["gpu_frames"].add(row.get("gpu-frame-number", ""))

        duration = parse_duration_ms(row.get("duration", ""))
        channel = row.get("gpu-channel-name", "")
        if channel == "Vertex":
            current["xctrace_vertex_ms"] += duration
        elif channel == "Fragment":
            current["xctrace_fragment_ms"] += duration
        else:
            current["xctrace_other_ms"] += duration

    result: list[dict[str, Any]] = []
    for row in by_key.values():
        row["xctrace_stage_sum_ms"] = (
            row["xctrace_vertex_ms"] + row["xctrace_fragment_ms"] + row["xctrace_other_ms"]
        )
        row["gpu_frames"] = ";".join(sorted(frame for frame in row["gpu_frames"] if frame))
        result.append(row)
    return result


def join_dxmt(rows: list[dict[str, Any]], dxmt_rows: dict[tuple[int, int], dict[str, str]]) -> None:
    for row in rows:
        dxmt = dxmt_rows.get((as_int(row.get("seq")), as_int(row.get("encoder"))), {})
        for key in (
            "draw_calls",
            "primitive_count",
            "vertex_count",
            "indexed_triangle_opaque_depth_write_primitives",
            "indexed_triangle_alpha_blend_primitives",
            "stream_metal_binds",
            "stream_handle_changes",
            "ib_metal_binds",
            "ib_handle_changes",
            "argbuf_table_bytes",
            "argbuf_cbuf_bytes",
            "set_vertex_bytes_bytes",
            "transient_vertex_bytes",
            "transient_index_bytes",
            "end_reason",
        ):
            row[key] = dxmt.get(key, "")
        row["primitive_class"] = primitive_class(row)
        row["xctrace_vertex_ms_per_mvertex"] = ratio_ms_per_million(
            row.get("xctrace_vertex_ms"),
            row.get("vertex_count"),
        )
        row["xctrace_stage_ms_per_mvertex"] = ratio_ms_per_million(
            row.get("xctrace_stage_sum_ms"),
            row.get("vertex_count"),
        )


def join_indexed_probe_routes(rows: list[dict[str, Any]], routes: dict[tuple[int, int], dict[str, Any]]) -> None:
    for row in rows:
        route = routes.get((as_int(row.get("seq")), as_int(row.get("encoder"))), {})
        row["route_verdict"] = route.get("route_verdict", "route-unavailable")
        row["route_reason"] = route.get("route_reason", "no indexed probe draw row joined")
        for key in (
            "route_draws",
            "route_primitives",
            "route_vertices",
            "route_depth_only_primitives",
            "route_programmable_textured_primitives",
            "route_programmable_color_primitives",
        ):
            row[key] = route.get(key, 0)


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    fields = (
        "seq",
        "encoder",
        "xctrace_stage_sum_ms",
        "xctrace_vertex_ms",
        "xctrace_fragment_ms",
        "xctrace_other_ms",
        "xctrace_rows",
        "gpu_frames",
        "rt",
        "depth",
        "process",
        "draw_calls",
        "primitive_count",
        "vertex_count",
        "indexed_triangle_opaque_depth_write_primitives",
        "indexed_triangle_alpha_blend_primitives",
        "stream_metal_binds",
        "stream_handle_changes",
        "ib_metal_binds",
        "ib_handle_changes",
        "argbuf_table_bytes",
        "argbuf_cbuf_bytes",
        "set_vertex_bytes_bytes",
        "transient_vertex_bytes",
        "transient_index_bytes",
        "end_reason",
        "primitive_class",
        "xctrace_vertex_ms_per_mvertex",
        "xctrace_stage_ms_per_mvertex",
        "route_verdict",
        "route_reason",
        "route_draws",
        "route_primitives",
        "route_vertices",
        "route_depth_only_primitives",
        "route_programmable_textured_primitives",
        "route_programmable_color_primitives",
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow({field: row.get(field, "") for field in fields})


def append_aggregate_table(lines: list[str], title: str, groups: list[dict[str, Any]], total_stage_ms: float) -> None:
    if not groups:
        return
    lines.extend(
        [
            "",
            f"## {title}",
            "",
            "| Group | Rows | Stage ms | Stage share | Vertex ms | Fragment ms | Vertex share | Vertex ms/Mvert | Draws | Tris | Vertices |",
            "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
        ]
    )
    for group in groups:
        stage_ms = as_float(group.get("xctrace_stage_sum_ms"))
        vertex_ms = as_float(group.get("xctrace_vertex_ms"))
        fragment_ms = as_float(group.get("xctrace_fragment_ms"))
        vertex_share = vertex_ms / stage_ms * 100.0 if stage_ms else 0.0
        stage_share = stage_ms / total_stage_ms * 100.0 if total_stage_ms else 0.0
        lines.append(
            f"| `{group['group']}` | {group['rows']} | {stage_ms:.3f} | "
            f"{stage_share:.2f}% | {vertex_ms:.3f} | {fragment_ms:.3f} | "
            f"{vertex_share:.2f}% | "
            f"{ratio_ms_per_million(vertex_ms, group.get('vertex_count')):.3f} | "
            f"{group['draw_calls']} | {group['primitive_count']} | {group['vertex_count']} |"
        )


def write_markdown(path: Path, *, rows: list[dict[str, Any]], run_label: str, trace_path: Path,
                   dxmt_matches: int, top: int) -> None:
    seqs = sorted(as_int(row.get("seq")) for row in rows)
    vertex_ms = sum(float(row["xctrace_vertex_ms"]) for row in rows)
    fragment_ms = sum(float(row["xctrace_fragment_ms"]) for row in rows)
    other_ms = sum(float(row["xctrace_other_ms"]) for row in rows)
    stage_ms = vertex_ms + fragment_ms + other_ms
    lines: list[str] = [
        "# xctrace Metal System Trace Summary",
        "",
        f"- Run: `{run_label}`",
        f"- Trace: `{trace_path}`",
        f"- Joined encoder rows: `{len(rows)}`",
        f"- Joined dxmt attribution coverage: `{dxmt_matches}/{len(rows)}`",
    ]
    if seqs:
        lines.append(f"- Captured seq range: `{seqs[0]}..{seqs[-1]}` (`{len(set(seqs))}` seq ids)")
    lines.extend(
        [
            f"- Stage sum: `{stage_ms:.3f} ms` (`vertex={vertex_ms:.3f} ms`, "
            f"`fragment={fragment_ms:.3f} ms`, `other={other_ms:.3f} ms`)",
        ]
    )
    if stage_ms > 0.0:
        lines.append(
            f"- Vertex share: `{vertex_ms / stage_ms * 100.0:.2f}%`; "
            f"fragment share: `{fragment_ms / stage_ms * 100.0:.2f}%`"
        )
    top_rows = rows[:top]
    normalized = [
        ratio_ms_per_million(row.get("xctrace_vertex_ms"), row.get("vertex_count"))
        for row in top_rows
        if as_int(row.get("vertex_count")) > 0
    ]
    if normalized:
        mean, median, p95, p99 = distribution(normalized)
        lines.append(
            f"- Top-{len(top_rows)} vertex ms/Mvertex: "
            f"`mean={mean:.3f}`, `p50={median:.3f}`, `p95={p95:.3f}`, `p99={p99:.3f}`"
        )
    class_counts = Counter(str(row.get("primitive_class", "unknown")) for row in top_rows)
    if class_counts:
        class_summary = ", ".join(f"`{name}={count}`" for name, count in sorted(class_counts.items()))
        lines.append(f"- Top-{len(top_rows)} primitive classes: {class_summary}")
    append_aggregate_table(
        lines,
        "Aggregate By Primitive Class",
        aggregate_by(rows, "primitive_class"),
        stage_ms,
    )
    append_aggregate_table(
        lines,
        "Aggregate By End Reason",
        aggregate_by(rows, "end_reason"),
        stage_ms,
    )
    route_groups = aggregate_by(
        [row for row in rows if row.get("route_verdict") != "route-unavailable"],
        "route_verdict",
    )
    append_aggregate_table(lines, "Aggregate By Route Verdict", route_groups, stage_ms)
    if not route_groups:
        lines.extend(
            [
                "",
                "## Aggregate By Route Verdict",
                "",
                "No indexed probe draw rows were joined. Re-run the probe with indexed draw telemetry "
                "enabled before using this sidecar for depth-only/textured/color route selection.",
            ]
        )
    lines.extend(
        [
            "",
            "## Top Stage-Sum Encoders",
            "",
            "| Rank | Row | Stage ms | Vertex ms | Fragment ms | Vertex ms/Mvert | Class | Route | Draws | Tris | Vertices | RT | Depth | End |",
            "|---:|---|---:|---:|---:|---:|---|---|---:|---:|---:|---|---|---|",
        ]
    )
    for index, row in enumerate(top_rows, start=1):
        lines.append(
            f"| {index} | `{row['seq']}/{row['encoder']}` | "
            f"{float(row['xctrace_stage_sum_ms']):.3f} | "
            f"{float(row['xctrace_vertex_ms']):.3f} | "
            f"{float(row['xctrace_fragment_ms']):.3f} | "
            f"{ratio_ms_per_million(row.get('xctrace_vertex_ms'), row.get('vertex_count')):.3f} | "
            f"`{row.get('primitive_class', '')}` | "
            f"`{row.get('route_verdict', 'route-unavailable')}` | "
            f"{row.get('draw_calls', '')} | {row.get('primitive_count', '')} | "
            f"{row.get('vertex_count', '')} | `{row.get('rt', '')}` | "
            f"`{row.get('depth', '')}` | `{row.get('end_reason', '')}` |"
        )
    lines.extend(
        [
            "",
            "## Interpretation",
            "",
            "- This is an Instruments Metal System Trace, not an Xcode `.gputrace` replay.",
            "- It does not expose Xcode replay counter fields such as "
            "`VS Buffer Device Memory Bytes Written`.",
            "- It is useful for timing/label attribution while `.gputrace` capture is blocked, "
            "because it can capture dxmt9 render-pass labels for the Wine temp child without "
            "`MTL_CAPTURE_ENABLED=1`.",
        ]
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gpu-intervals", type=Path, required=True,
                        help="xctrace export XML for schema metal-gpu-intervals")
    parser.add_argument("--dxmt-encoders", type=Path, required=True,
                        help="3dmark05-perf-encoders.csv produced by summarize_3dmark05_perf.py")
    parser.add_argument("--output-csv", type=Path, required=True)
    parser.add_argument("--output-md", type=Path, required=True)
    parser.add_argument("--indexed-probe-draws", type=Path,
                        help="Optional 3dmark05-perf-indexed-probe-draws.csv for route verdict joins")
    parser.add_argument("--require-xctrace-render-rows", action="store_true",
                        help="Fail if no RenderPass-labelled xctrace rows are found")
    parser.add_argument("--min-dxmt-join-coverage", type=float, default=0.0,
                        help="Fail if dxmt encoder attribution coverage is below this ratio, e.g. 0.99")
    parser.add_argument("--require-indexed-probe-routes", action="store_true",
                        help="Fail if indexed probe draw rows are missing or do not join to any xctrace rows")
    parser.add_argument("--run-label", default="")
    parser.add_argument("--trace", type=Path, default=Path(""))
    parser.add_argument("--top", type=int, default=20)
    args = parser.parse_args()

    rows = aggregate_gpu_intervals(parse_xctrace_rows(args.gpu_intervals))
    dxmt = load_dxmt_encoders(args.dxmt_encoders)
    join_dxmt(rows, dxmt)
    routes = load_indexed_probe_routes(args.indexed_probe_draws)
    join_indexed_probe_routes(rows, routes)
    rows.sort(key=lambda row: float(row["xctrace_stage_sum_ms"]), reverse=True)
    matches = sum(1 for row in rows if (as_int(row.get("seq")), as_int(row.get("encoder"))) in dxmt)
    route_matches = sum(1 for row in rows if row.get("route_verdict") != "route-unavailable")

    if args.require_xctrace_render_rows and not rows:
        raise SystemExit(
            "xctrace render rows required, but no RenderPass[seq=...,enc=...] "
            f"labels were found in {args.gpu_intervals}"
        )
    if rows and args.min_dxmt_join_coverage > 0.0:
        coverage = matches / len(rows)
        if coverage < args.min_dxmt_join_coverage:
            raise SystemExit(
                "dxmt encoder join coverage below required threshold: "
                f"{matches}/{len(rows)} ({coverage:.2%}) < {args.min_dxmt_join_coverage:.2%}; "
                "check xctrace label coverage and use the same-run 3DMark05 encoder CSV"
            )

    if args.require_indexed_probe_routes and route_matches == 0:
        source = args.indexed_probe_draws if args.indexed_probe_draws else "<missing>"
        raise SystemExit(
            "indexed probe route join required, but no route rows joined "
            f"(source: {source}); rerun the 3DMark05 probe with same-run "
            "--measure-index-reuse telemetry"
        )

    write_csv(args.output_csv, rows)
    write_markdown(
        args.output_md,
        rows=rows,
        run_label=args.run_label or args.dxmt_encoders.parent.name,
        trace_path=args.trace,
        dxmt_matches=matches,
        top=args.top,
    )
    print(args.output_csv)
    print(args.output_md)


if __name__ == "__main__":
    main()
