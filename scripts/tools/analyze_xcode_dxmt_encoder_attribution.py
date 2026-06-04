#!/usr/bin/env python3
"""Attribute Xcode encoder writes against dxmt CPU writers and state churn.

The input is a `frame<N>-xcode-dxmt-joined-summary.csv` produced by
`summarize_xcode_encoder_counters.py`. This report is intentionally a single
capture view: comparison gates live in `compare_xcode_dxmt_bottlenecks.py`,
while this script answers which hot encoders are GPU backend dominated, dxmt
writer dominated, or CPU state-churn cleanup targets.
"""

from __future__ import annotations

import argparse
import csv
from pathlib import Path
from typing import Any


MIB = 1024.0 * 1024.0


REPORT_FIELDS = (
    "row",
    "gpu_ms",
    "gpu_share_pct",
    "buffer_write_mib",
    "vs_buffer_write_mib",
    "hidden_backend_write_mib",
    "hidden_backend_write_ratio",
    "named_tiled_buffer_mib",
    "tiled_vertex_buffer_mib",
    "tiled_primitive_block_mib",
    "tvb_pressure_proxy_mib",
    "tvb_named_to_proxy_ratio",
    "vs_buffer_write_to_tvb_proxy_ratio",
    "primitive_block_tile_intersections_pct",
    "tiling_block_utilization_pct",
    "primitives_per_tile",
    "cpu_writer_mib",
    "cpu_writer_to_buffer_write_ratio",
    "argbuf_table_mib",
    "argbuf_cbuf_mib",
    "set_vertex_bytes_mib",
    "transient_vertex_mib",
    "transient_index_mib",
    "draw_calls",
    "stream_handle_changes",
    "stream_offset_changes",
    "stream_stride_changes",
    "ib_handle_changes",
    "state_churn_events",
    "state_churn_events_per_draw",
    "stream_unique_handles",
    "ib_unique_handles",
    "stream_unique_mib",
    "ib_unique_mib",
    "classification",
    "backend_storage_class",
    "backend_probe_hint",
    "mechanism_hint",
    "next_action",
)


def as_float(value: Any) -> float:
    try:
        return float(str(value))
    except (TypeError, ValueError):
        return 0.0


def as_int(value: Any) -> int:
    try:
        text = str(value).strip()
        if text.startswith(("0x", "0X")):
            return int(text, 16)
        return int(float(text))
    except (TypeError, ValueError):
        return 0


def first(row: dict[str, str], *keys: str) -> str:
    for key in keys:
        value = row.get(key)
        if value not in (None, ""):
            return value
    return ""


def mib_from_bytes(row: dict[str, str], *keys: str) -> float:
    return as_int(first(row, *keys)) / MIB


def fmt(value: float) -> str:
    if 0.0 < abs(value) < 0.001:
        return f"{value:.6f}"
    if abs(value) >= 1000.0:
        return f"{value:,.3f}"
    return f"{value:.3f}"


def load_rows(path: Path) -> list[dict[str, str]]:
    if not path.exists():
        raise SystemExit(f"missing joined summary: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def row_label(row: dict[str, str]) -> str:
    seq = first(row, "seq")
    enc = first(row, "enc")
    if seq and enc:
        return f"{seq}/{enc}"
    return first(row, "xcode_index", "encoder_index", "command_buffer_index") or "n/a"


def cpu_writer_mib(row: dict[str, str]) -> float:
    explicit_mib = first(row, "dxmt_cpu_writer_mib", "cpu_writer_mib")
    if explicit_mib:
        return as_float(explicit_mib)
    explicit_bytes = first(row, "dxmt_cpu_writer_bytes", "cpu_writer_bytes")
    if explicit_bytes:
        return as_int(explicit_bytes) / MIB
    return (
        mib_from_bytes(row, "dxmt_argbuf_table_bytes", "argbuf_table_bytes") +
        mib_from_bytes(row, "dxmt_argbuf_cbuf_bytes", "argbuf_cbuf_bytes") +
        mib_from_bytes(row, "dxmt_set_vertex_bytes_bytes", "set_vertex_bytes_bytes") +
        mib_from_bytes(row, "dxmt_transient_vertex_bytes", "transient_vertex_bytes") +
        mib_from_bytes(row, "dxmt_transient_index_bytes", "transient_index_bytes")
    )


def hidden_backend_write_mib(row: dict[str, str]) -> float:
    explicit = first(row, "dxmt_hidden_backend_write_mib", "hidden_backend_write_mib")
    if explicit:
        return as_float(explicit)
    unexplained = first(row, "dxmt_unexplained_buffer_write_mib", "unexplained_buffer_write_mib")
    if unexplained:
        return as_float(unexplained)
    return max(as_float(row.get("buffer_write_mib")) - cpu_writer_mib(row), 0.0)


def named_tiled_buffer_mib(row: dict[str, str]) -> float:
    explicit = first(row, "dxmt_named_tiled_buffer_mib", "named_tiled_buffer_mib")
    if explicit:
        return as_float(explicit)
    return (
        as_float(row.get("tiled_vertex_buffer_mib")) +
        as_float(row.get("tiled_primitive_block_mib"))
    )


def mechanism_hint(row: dict[str, str],
                   *,
                   hidden_ratio: float,
                   named_to_proxy_ratio: float,
                   vs_to_proxy_ratio: float,
                   primitive_intersections_pct: float) -> str:
    backend_class = first(row, "dxmt_backend_storage_class", "backend_storage_class")
    if backend_class == "named_tiled_vertex_or_primitive_storage":
        return "named-tiled-storage-density"
    if hidden_ratio >= 0.80 and vs_to_proxy_ratio >= 4.0 and named_to_proxy_ratio <= 0.30:
        return "hidden-expanded-tvb-parameter-storage"
    if hidden_ratio >= 0.80 and primitive_intersections_pct >= 1.0:
        return "primitive-tile-intersection-pressure"
    if hidden_ratio >= 0.80:
        return "hidden-backend-storage-unknown-subtype"
    return "non-hidden-backend-or-low-write"


def report_row(row: dict[str, str]) -> dict[str, Any]:
    buffer_write = as_float(row.get("buffer_write_mib"))
    writer_mib = cpu_writer_mib(row)
    hidden_mib = hidden_backend_write_mib(row)
    draw_calls = as_int(first(row, "dxmt_draw_calls", "draw_calls"))
    stream_handle = as_int(first(row, "dxmt_stream_handle_changes", "stream_handle_changes"))
    stream_offset = as_int(first(row, "dxmt_stream_offset_changes", "stream_offset_changes"))
    stream_stride = as_int(first(row, "dxmt_stream_stride_changes", "stream_stride_changes"))
    ib_handle = as_int(first(row, "dxmt_ib_handle_changes", "ib_handle_changes"))
    state_churn = stream_handle + stream_offset + stream_stride + ib_handle
    state_per_draw = state_churn / draw_calls if draw_calls else 0.0
    writer_ratio = writer_mib / buffer_write if buffer_write else 0.0
    hidden_ratio = hidden_mib / buffer_write if buffer_write else 0.0
    named_tiled_mib = named_tiled_buffer_mib(row)
    tvb_proxy_mib = as_float(first(row, "dxmt_tvb_pressure_proxy_mib", "tvb_pressure_proxy_mib"))
    named_to_proxy_ratio = as_float(first(
        row,
        "dxmt_tvb_named_to_proxy_ratio",
        "tvb_named_to_proxy_ratio",
    ))
    vs_to_proxy_ratio = as_float(first(
        row,
        "dxmt_vs_buffer_write_to_tvb_proxy_ratio",
        "vs_buffer_write_to_tvb_proxy_ratio",
    ))
    primitive_intersections_pct = as_float(row.get("primitive_block_tile_intersections_pct"))
    mechanism = mechanism_hint(
        row,
        hidden_ratio=hidden_ratio,
        named_to_proxy_ratio=named_to_proxy_ratio,
        vs_to_proxy_ratio=vs_to_proxy_ratio,
        primitive_intersections_pct=primitive_intersections_pct,
    )
    classification, next_action = classify(
        buffer_write_mib=buffer_write,
        writer_ratio=writer_ratio,
        hidden_ratio=hidden_ratio,
        state_churn_events_per_draw=state_per_draw,
    )
    return {
        "row": row_label(row),
        "gpu_ms": as_float(row.get("gpu_ms")),
        "gpu_share_pct": as_float(first(row, "gpu_share_pct", "cost_pct")),
        "buffer_write_mib": buffer_write,
        "vs_buffer_write_mib": as_float(row.get("vs_buffer_write_mib")),
        "hidden_backend_write_mib": hidden_mib,
        "hidden_backend_write_ratio": hidden_ratio,
        "named_tiled_buffer_mib": named_tiled_mib,
        "tiled_vertex_buffer_mib": as_float(row.get("tiled_vertex_buffer_mib")),
        "tiled_primitive_block_mib": as_float(row.get("tiled_primitive_block_mib")),
        "tvb_pressure_proxy_mib": tvb_proxy_mib,
        "tvb_named_to_proxy_ratio": named_to_proxy_ratio,
        "vs_buffer_write_to_tvb_proxy_ratio": vs_to_proxy_ratio,
        "primitive_block_tile_intersections_pct": primitive_intersections_pct,
        "tiling_block_utilization_pct": as_float(row.get("tiling_block_utilization_pct")),
        "primitives_per_tile": as_float(row.get("primitives_per_tile")),
        "cpu_writer_mib": writer_mib,
        "cpu_writer_to_buffer_write_ratio": writer_ratio,
        "argbuf_table_mib": mib_from_bytes(row, "dxmt_argbuf_table_bytes", "argbuf_table_bytes"),
        "argbuf_cbuf_mib": mib_from_bytes(row, "dxmt_argbuf_cbuf_bytes", "argbuf_cbuf_bytes"),
        "set_vertex_bytes_mib": mib_from_bytes(
            row,
            "dxmt_set_vertex_bytes_bytes",
            "set_vertex_bytes_bytes",
        ),
        "transient_vertex_mib": mib_from_bytes(
            row,
            "dxmt_transient_vertex_bytes",
            "transient_vertex_bytes",
        ),
        "transient_index_mib": mib_from_bytes(
            row,
            "dxmt_transient_index_bytes",
            "transient_index_bytes",
        ),
        "draw_calls": draw_calls,
        "stream_handle_changes": stream_handle,
        "stream_offset_changes": stream_offset,
        "stream_stride_changes": stream_stride,
        "ib_handle_changes": ib_handle,
        "state_churn_events": state_churn,
        "state_churn_events_per_draw": state_per_draw,
        "stream_unique_handles": as_int(first(row, "dxmt_stream_unique_handles", "stream_unique_handles")),
        "ib_unique_handles": as_int(first(row, "dxmt_ib_unique_handles", "ib_unique_handles")),
        "stream_unique_mib": mib_from_bytes(row, "dxmt_stream_unique_bytes", "stream_unique_bytes"),
        "ib_unique_mib": mib_from_bytes(row, "dxmt_ib_unique_bytes", "ib_unique_bytes"),
        "classification": classification,
        "backend_storage_class": first(row, "dxmt_backend_storage_class", "backend_storage_class"),
        "backend_probe_hint": first(row, "dxmt_backend_probe_hint", "backend_probe_hint"),
        "mechanism_hint": mechanism,
        "next_action": next_action,
    }


def classify(*,
             buffer_write_mib: float,
             writer_ratio: float,
             hidden_ratio: float,
             state_churn_events_per_draw: float) -> tuple[str, str]:
    if buffer_write_mib <= 0.0:
        return "no-xcode-buffer-write", "ignore for buffer-write attribution"
    if writer_ratio >= 0.50:
        return "dxmt-writer-primary", "reduce argbuf/transient/setVertexBytes writers first"
    if writer_ratio >= 0.10:
        return "mixed-writer-backend", "separate dxmt writer cleanup from backend proof"
    if hidden_ratio >= 0.80 and state_churn_events_per_draw >= 1.0:
        return (
            "hidden-backend-primary-state-churn-secondary",
            "use TVB/tiler/backend probes; keep stream/IB coalescing on CPU track",
        )
    if hidden_ratio >= 0.80:
        return "hidden-backend-primary", "use TVB/tiler/backend mechanism probes"
    if state_churn_events_per_draw >= 1.0:
        return "state-churn-secondary", "optimize stream/IB batching as CPU encode work"
    return "low-explicit-writer", "inspect shader/backend storage before CPU writer work"


def sort_rows(rows: list[dict[str, Any]], sort_key: str) -> list[dict[str, Any]]:
    if sort_key not in REPORT_FIELDS:
        raise SystemExit(f"unknown sort key: {sort_key}")
    return sorted(rows, key=lambda row: row.get(sort_key, 0), reverse=True)


def sum_float(rows: list[dict[str, Any]], key: str) -> float:
    return sum(as_float(row.get(key)) for row in rows)


def experiment_candidate_rows(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    return [
        row for row in rows
        if row.get("mechanism_hint") == "hidden-expanded-tvb-parameter-storage"
    ]


def write_csv_report(path: Path, rows: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=REPORT_FIELDS, extrasaction="ignore")
        writer.writeheader()
        for row in rows:
            writer.writerow({key: row.get(key, "") for key in REPORT_FIELDS})


def write_markdown(path: Path,
                   input_path: Path,
                   rows: list[dict[str, Any]],
                   *,
                   top: int,
                   sort_key: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    top_rows = rows[:top]
    total_gpu = sum_float(rows, "gpu_ms")
    top_gpu = sum_float(top_rows, "gpu_ms")
    top_buffer = sum_float(top_rows, "buffer_write_mib")
    top_hidden = sum_float(top_rows, "hidden_backend_write_mib")
    top_named_tiled = sum_float(top_rows, "named_tiled_buffer_mib")
    top_tvb_proxy = sum_float(top_rows, "tvb_pressure_proxy_mib")
    top_writer = sum_float(top_rows, "cpu_writer_mib")
    top_state = sum_float(top_rows, "state_churn_events")
    top_draws = sum_float(top_rows, "draw_calls")

    lines = [
        "# Xcode/dxmt Encoder Attribution",
        "",
        f"- Input: `{input_path}`",
        f"- Sort key: `{sort_key}`",
        f"- Rows: `{len(rows)}`",
        f"- Top rows: `{len(top_rows)}`",
        "",
        "## Top Aggregate",
        "",
        "| Metric | Value |",
        "|---|---:|",
        f"| `total_gpu_ms` | `{fmt(total_gpu)}` |",
        f"| `top_gpu_ms` | `{fmt(top_gpu)}` |",
        f"| `top_gpu_share_pct` | `{fmt((top_gpu / total_gpu * 100.0) if total_gpu else 0.0)}` |",
        f"| `top_buffer_write_mib` | `{fmt(top_buffer)}` |",
        f"| `top_hidden_backend_write_mib` | `{fmt(top_hidden)}` |",
        f"| `top_hidden_backend_write_ratio` | `{fmt((top_hidden / top_buffer) if top_buffer else 0.0)}` |",
        f"| `top_named_tiled_buffer_mib` | `{fmt(top_named_tiled)}` |",
        f"| `top_tvb_pressure_proxy_mib` | `{fmt(top_tvb_proxy)}` |",
        f"| `top_named_tiled_to_proxy_ratio` | `{fmt((top_named_tiled / top_tvb_proxy) if top_tvb_proxy else 0.0)}` |",
        f"| `top_cpu_writer_mib` | `{fmt(top_writer)}` |",
        f"| `top_cpu_writer_to_buffer_write_ratio` | `{fmt((top_writer / top_buffer) if top_buffer else 0.0)}` |",
        f"| `top_state_churn_events_per_draw` | `{fmt((top_state / top_draws) if top_draws else 0.0)}` |",
        "",
        "## Top Encoder Attribution",
        "",
        "| Row | GPU ms | Buffer write MiB | VS write MiB | Hidden MiB | CPU writer MiB | Writer ratio | State churn/draw | Stream h/o/s | IB h | Class |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|",
    ]
    for row in top_rows:
        stream_tuple = (
            f"{as_int(row['stream_handle_changes'])}/"
            f"{as_int(row['stream_offset_changes'])}/"
            f"{as_int(row['stream_stride_changes'])}"
        )
        lines.append(
            "| "
            f"`{row['row']}` | "
            f"`{fmt(row['gpu_ms'])}` | "
            f"`{fmt(row['buffer_write_mib'])}` | "
            f"`{fmt(row['vs_buffer_write_mib'])}` | "
            f"`{fmt(row['hidden_backend_write_mib'])}` | "
            f"`{fmt(row['cpu_writer_mib'])}` | "
            f"`{fmt(row['cpu_writer_to_buffer_write_ratio'])}` | "
            f"`{fmt(row['state_churn_events_per_draw'])}` | "
            f"`{stream_tuple}` | "
            f"`{fmt(row['ib_handle_changes'])}` | "
            f"`{row['classification']}` |"
        )
    lines.extend([
        "",
        "## Backend Mechanism Signals",
        "",
        "| Row | Named tiled MiB | TVB proxy MiB | Named/proxy | VS write/proxy | Primitive tile intersections % | Tiling util % | Primitives/tile | Backend class | Mechanism hint | Probe hint |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---|---|---|",
    ])
    for row in top_rows:
        lines.append(
            "| "
            f"`{row['row']}` | "
            f"`{fmt(row['named_tiled_buffer_mib'])}` | "
            f"`{fmt(row['tvb_pressure_proxy_mib'])}` | "
            f"`{fmt(row['tvb_named_to_proxy_ratio'])}` | "
            f"`{fmt(row['vs_buffer_write_to_tvb_proxy_ratio'])}` | "
            f"`{fmt(row['primitive_block_tile_intersections_pct'])}` | "
            f"`{fmt(row['tiling_block_utilization_pct'])}` | "
            f"`{fmt(row['primitives_per_tile'])}` | "
            f"`{row['backend_storage_class']}` | "
            f"`{row['mechanism_hint']}` | "
            f"`{row['backend_probe_hint']}` |"
        )
    lines.extend([
        "",
        "## Writer Component Split",
        "",
        "| Row | Argbuf table MiB | Argbuf cbuf MiB | setVertexBytes MiB | Transient vertex MiB | Transient index MiB | Stream unique MiB | IB unique MiB | Next action |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---|",
    ])
    for row in top_rows:
        lines.append(
            "| "
            f"`{row['row']}` | "
            f"`{fmt(row['argbuf_table_mib'])}` | "
            f"`{fmt(row['argbuf_cbuf_mib'])}` | "
            f"`{fmt(row['set_vertex_bytes_mib'])}` | "
            f"`{fmt(row['transient_vertex_mib'])}` | "
            f"`{fmt(row['transient_index_mib'])}` | "
            f"`{fmt(row['stream_unique_mib'])}` | "
            f"`{fmt(row['ib_unique_mib'])}` | "
            f"{row['next_action']} |"
        )
    candidates = experiment_candidate_rows(top_rows)
    if candidates:
        first_candidate = candidates[0]
        row = str(first_candidate["row"])
        frame = row.split("/", 1)[0] if "/" in row else "<frame>"
        lines.extend([
            "",
            "## Mechanism Experiment Queue",
            "",
            "These rows are hidden-expanded TVB/parameter-storage candidates. "
            "Do not spend Xcode time unless a candidate changes a new backend "
            "mechanism or a same-input semantic/locality proof exists; the "
            "rejected broad axes remain rejected.",
            "",
            "| Row | Mechanism hint | Preflight gate | Xcode gate |",
            "|---|---|---|---|",
        ])
        for candidate in candidates:
            lines.append(
                "| "
                f"`{candidate['row']}` | "
                f"`{candidate['mechanism_hint']}` | "
                "no-gputrace smoke must keep row/draw/vertex/triangle shape stable "
                "and prove a new mechanism signal, not just a rejected state bit | "
                "`--require-tvb-mechanism-proof` plus target/top geometry gates; "
                "target hidden backend write, VS write, VS invocations, and GPU time must decrease |"
            )
        lines.extend([
            "",
            "No-gputrace shape smoke template:",
            "",
            "```bash",
            "scripts/tools/run_3dmark05_perf_probe.sh \\",
            "  --suffix <hidden-tvb-mechanism-smoke> \\",
            f"  --frame {frame} \\",
            f"  --encoder-breakdown-seq {frame} \\",
            "  --no-gputrace \\",
            "  --timeout 240 \\",
            "  <primitive-order-preserving-backend-mechanism-option>",
            "```",
            "",
            "Gputrace/Xcode proof template:",
            "",
            "```bash",
            "scripts/tools/run_3dmark05_perf_probe.sh \\",
            "  --suffix <hidden-tvb-mechanism-gputrace> \\",
            f"  --frame {frame} \\",
            f"  --encoder-breakdown-seq {frame} \\",
            f"  --baseline-joined {input_path} \\",
            f"  --target-row-key {row} \\",
            "  --require-tvb-mechanism-proof \\",
            "  --require-top-row-key-match \\",
            "  --max-top-draw-call-delta-ratio 0.05 \\",
            "  --max-top-vertex-count-delta-ratio 0.05 \\",
            "  --max-top-triangle-delta-ratio 0.05 \\",
            "  --timeout 420 \\",
            "  <primitive-order-preserving-backend-mechanism-option>",
            "```",
        ])
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("joined_summary", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--csv-output", type=Path)
    parser.add_argument("--top", type=int, default=10)
    parser.add_argument("--sort", default="gpu_ms", choices=REPORT_FIELDS)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.top <= 0:
        raise SystemExit("--top must be positive")
    source_rows = load_rows(args.joined_summary)
    rows = sort_rows([report_row(row) for row in source_rows], args.sort)
    write_markdown(args.output, args.joined_summary, rows, top=args.top, sort_key=args.sort)
    if args.csv_output:
        write_csv_report(args.csv_output, rows)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
