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
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow({field: row.get(field, "") for field in fields})


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
    lines.extend(
        [
            "",
            "## Top Stage-Sum Encoders",
            "",
            "| Rank | Row | Stage ms | Vertex ms | Fragment ms | Draws | Tris | Vertices | RT | Depth | End |",
            "|---:|---|---:|---:|---:|---:|---:|---:|---|---|---|",
        ]
    )
    for index, row in enumerate(rows[:top], start=1):
        lines.append(
            f"| {index} | `{row['seq']}/{row['encoder']}` | "
            f"{float(row['xctrace_stage_sum_ms']):.3f} | "
            f"{float(row['xctrace_vertex_ms']):.3f} | "
            f"{float(row['xctrace_fragment_ms']):.3f} | "
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
    parser.add_argument("--run-label", default="")
    parser.add_argument("--trace", type=Path, default=Path(""))
    parser.add_argument("--top", type=int, default=20)
    args = parser.parse_args()

    rows = aggregate_gpu_intervals(parse_xctrace_rows(args.gpu_intervals))
    dxmt = load_dxmt_encoders(args.dxmt_encoders)
    join_dxmt(rows, dxmt)
    rows.sort(key=lambda row: float(row["xctrace_stage_sum_ms"]), reverse=True)
    matches = sum(1 for row in rows if (as_int(row.get("seq")), as_int(row.get("encoder"))) in dxmt)

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
