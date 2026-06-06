#!/usr/bin/env python3
"""Preflight stream/IB churn as a 3DMark05 backend-storage experiment target."""

from __future__ import annotations

import argparse
from collections import Counter
import csv
from pathlib import Path
from typing import Any, Iterable


CSV_FIELDS = [
    "row",
    "verdict",
    "draws",
    "triangles",
    "vertices",
    "stream_metal_binds",
    "stream_handle_changes",
    "stream_offset_changes",
    "stream_stride_changes",
    "stream_handle_changes_per_draw",
    "stream_offset_stride_per_draw",
    "ib_metal_binds",
    "ib_handle_changes",
    "ib_handle_changes_per_draw",
    "combined_handle_changes_per_draw",
    "stream_unique_handles",
    "ib_unique_handles",
    "stream_unique_bytes",
    "ib_unique_bytes",
    "explicit_writer_bytes",
    "explicit_writer_bytes_per_draw",
    "explicit_writer_bytes_per_vertex",
    "lru32_delta",
    "pso_changes",
    "draw_stream0_handle_changes",
    "draw_index_buffer_changes",
    "draw_extra_stream_binding_changes",
    "draw_extra_stream_binding_unique",
    "draw_extra_streams_observed",
    "draw_binding_tuple_changes",
    "draw_binding_tuple_unique",
    "draw_binding_tuple_max_run",
    "draw_binding_tuple_avg_run",
    "draw_stream0_handle_unique",
    "draw_index_buffer_unique",
    "draw_first_extra_handle_unique",
    "draw_consecutive_pair_count",
    "draw_consecutive_pair_samples",
    "draw_consecutive_pair_ratio",
    "draw_stream0_ib_delta_top",
    "draw_consecutive_triplet_count",
    "draw_consecutive_triplet_samples",
    "draw_consecutive_triplet_ratio",
    "draw_triplet_delta_top",
    "draw_transition_classes",
    "draw_top_binding_tuples",
    "stream_breakdown",
    "reason",
    "next_action",
]


def as_int(value: Any) -> int:
    text = str(value or "").strip().replace(",", "")
    if not text:
        return 0
    try:
        return int(float(text))
    except ValueError:
        return 0


def as_float(value: Any) -> float:
    text = str(value or "").strip().replace(",", "")
    if not text:
        return 0.0
    try:
        return float(text)
    except ValueError:
        return 0.0


def fmt_int(value: int) -> str:
    return f"{value:,}"


def fmt_float(value: float) -> str:
    return f"{value:.3f}"


def safe_ratio(numer: float, denom: float) -> float:
    return numer / denom if denom else 0.0


def row_key(row: dict[str, str]) -> str:
    return f"{row.get('seq', '')}/{row.get('encoder') or row.get('enc', '')}"


def load_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def load_stream_rows(path: Path | None) -> dict[str, list[dict[str, str]]]:
    if not path or not path.exists():
        return {}
    grouped: dict[str, list[dict[str, str]]] = {}
    for row in load_rows(path):
        grouped.setdefault(row_key(row), []).append(row)
    for rows in grouped.values():
        rows.sort(key=lambda item: as_int(item.get("stream")))
    return grouped


def load_probe_draw_rows(path: Path | None) -> dict[str, list[dict[str, str]]]:
    if not path or not path.exists():
        return {}
    grouped: dict[str, list[dict[str, str]]] = {}
    for row in load_rows(path):
        grouped.setdefault(row_key(row), []).append(row)
    for rows in grouped.values():
        rows.sort(key=lambda item: as_int(item.get("encoder_draw_index")))
    return grouped


def explicit_writer_bytes(row: dict[str, str]) -> int:
    return sum(
        as_int(row.get(name))
        for name in (
            "argbuf_table_bytes",
            "argbuf_cbuf_bytes",
            "set_vertex_bytes_bytes",
            "transient_vertex_bytes",
            "transient_index_bytes",
        )
    )


def stream_breakdown(rows: list[dict[str, str]]) -> str:
    parts = []
    for row in rows:
        parts.append(
            "s{stream}:h{handle}/o{offset}/s{stride}/u{unique}".format(
                stream=as_int(row.get("stream")),
                handle=as_int(row.get("metal_bind_handle_changes")),
                offset=as_int(row.get("metal_bind_offset_changes")),
                stride=as_int(row.get("stride_changes")),
                unique=as_int(row.get("unique_handles")),
            )
        )
    return ";".join(parts)


def count_changes(values: list[str]) -> int:
    changes = 0
    have_previous = False
    previous = ""
    for value in values:
        if value == "":
            continue
        if have_previous and value != previous:
            changes += 1
        previous = value
        have_previous = True
    return changes


def parse_handle(value: str) -> int | None:
    text = (value or "").strip()
    if not text:
        return None
    try:
        return int(text, 16 if text.lower().startswith("0x") else 10)
    except ValueError:
        return None


def first_extra_handle(value: str) -> str:
    for part in (value or "").split(";"):
        if ":" not in part:
            continue
        handle = part.split(":", 1)[1].split("@", 1)[0].strip()
        if handle:
            return handle
    return ""


def format_counts(counter: Counter[str], limit: int = 3) -> str:
    return ";".join(f"{key}x{count}" for key, count in counter.most_common(limit))


def binding_tuple_key(value: tuple[str, str, str]) -> str:
    stream0, ib, extra = value
    return f"s0={stream0 or '-'},ib={ib or '-'},extra={extra or '-'}"


def transition_classes(values: list[tuple[str, str, str]]) -> str:
    counts: Counter[str] = Counter()
    previous: tuple[str, str, str] | None = None
    for value in values:
        if previous is None:
            previous = value
            continue
        changed = []
        if value[0] != previous[0]:
            changed.append("s0")
        if value[1] != previous[1]:
            changed.append("ib")
        if value[2] != previous[2]:
            changed.append("extra")
        counts["+".join(changed) if changed else "same"] += 1
        previous = value
    return format_counts(counts, limit=5)


def triplet_delta_metrics(
    stream0_values: list[str],
    ib_values: list[str],
    first_extra_values: list[str],
) -> dict[str, Any]:
    pair_samples = 0
    consecutive_pairs = 0
    pair_deltas: Counter[str] = Counter()
    samples = 0
    consecutive = 0
    deltas: Counter[str] = Counter()
    for stream0, ib, extra in zip(stream0_values, ib_values, first_extra_values):
        s0_handle = parse_handle(stream0)
        ib_handle = parse_handle(ib)
        if s0_handle is None or ib_handle is None or s0_handle == 0 or ib_handle == 0:
            continue
        pair_samples += 1
        ib_delta = ib_handle - s0_handle
        if ib_delta == 2:
            consecutive_pairs += 1
        pair_deltas[f"{ib_delta:+d}"] += 1
        extra_handle = parse_handle(extra)
        if extra_handle is None or extra_handle == 0:
            continue
        samples += 1
        extra_delta = extra_handle - s0_handle
        if extra_delta == 1 and ib_delta == 2:
            consecutive += 1
        deltas[f"{extra_delta:+d}/{ib_delta:+d}"] += 1
    return {
        "draw_consecutive_pair_count": consecutive_pairs,
        "draw_consecutive_pair_samples": pair_samples,
        "draw_consecutive_pair_ratio": safe_ratio(consecutive_pairs, pair_samples),
        "draw_stream0_ib_delta_top": format_counts(pair_deltas, limit=3),
        "draw_consecutive_triplet_count": consecutive,
        "draw_consecutive_triplet_samples": samples,
        "draw_consecutive_triplet_ratio": safe_ratio(consecutive, samples),
        "draw_triplet_delta_top": format_counts(deltas, limit=3),
    }


def probe_draw_metrics(rows: list[dict[str, str]]) -> dict[str, Any]:
    stream0_values = [row.get("stream0_handle", "") for row in rows]
    ib_values = [row.get("index_buffer", "") for row in rows]
    extra_values = [row.get("stream_extra_bindings", "") for row in rows]
    first_extra_values = [first_extra_handle(value) for value in extra_values]
    extra_nonempty = [value for value in extra_values if value]
    tuples = list(zip(stream0_values, ib_values, extra_values))
    tuple_run_lengths: list[int] = []
    previous_tuple: tuple[str, str, str] | None = None
    current_run = 0
    for value in tuples:
        if previous_tuple is not None and value == previous_tuple:
            current_run += 1
            continue
        if previous_tuple is not None:
            tuple_run_lengths.append(current_run)
        previous_tuple = value
        current_run = 1
    if previous_tuple is not None:
        tuple_run_lengths.append(current_run)
    top_tuples = Counter(binding_tuple_key(value) for value in tuples)
    return {
        "draw_stream0_handle_changes": count_changes(stream0_values),
        "draw_index_buffer_changes": count_changes(ib_values),
        "draw_extra_stream_binding_changes": count_changes(extra_values),
        "draw_extra_stream_binding_unique": len(set(extra_nonempty)),
        "draw_extra_streams_observed": 1 if extra_nonempty else 0,
        "draw_binding_tuple_changes": count_changes(["|".join(value) for value in tuples]),
        "draw_binding_tuple_unique": len(set(tuples)) if tuples else 0,
        "draw_binding_tuple_max_run": max(tuple_run_lengths) if tuple_run_lengths else 0,
        "draw_binding_tuple_avg_run": (
            sum(tuple_run_lengths) / len(tuple_run_lengths)
            if tuple_run_lengths
            else 0.0
        ),
        "draw_stream0_handle_unique": len({value for value in stream0_values if value}),
        "draw_index_buffer_unique": len({value for value in ib_values if value}),
        "draw_first_extra_handle_unique": len({value for value in first_extra_values if value}),
        "draw_transition_classes": transition_classes(tuples),
        "draw_top_binding_tuples": format_counts(top_tuples, limit=3),
        **triplet_delta_metrics(stream0_values, ib_values, first_extra_values),
    }


def row_metrics(
    row: dict[str, str],
    stream_rows: dict[str, list[dict[str, str]]],
    probe_draw_rows: dict[str, list[dict[str, str]]],
) -> dict[str, Any]:
    draws = as_int(row.get("draw_calls"))
    vertices = as_int(row.get("vertex_count"))
    stream_handle_changes = as_int(row.get("stream_metal_bind_handle_changes"))
    stream_offset_changes = as_int(row.get("stream_metal_bind_offset_changes"))
    stream_stride_changes = as_int(row.get("stream_stride_changes"))
    ib_handle_changes = as_int(row.get("ib_handle_changes"))
    writers = explicit_writer_bytes(row)
    draw_metrics = probe_draw_metrics(probe_draw_rows.get(row_key(row), []))
    return {
        "row": row_key(row),
        "draws": draws,
        "triangles": as_int(row.get("triangle_estimate")),
        "vertices": vertices,
        "stream_metal_binds": as_int(row.get("stream_metal_binds")),
        "stream_handle_changes": stream_handle_changes,
        "stream_offset_changes": stream_offset_changes,
        "stream_stride_changes": stream_stride_changes,
        "stream_handle_changes_per_draw": safe_ratio(stream_handle_changes, draws),
        "stream_offset_stride_per_draw": safe_ratio(
            stream_offset_changes + stream_stride_changes, draws
        ),
        "ib_metal_binds": as_int(row.get("ib_metal_binds")),
        "ib_handle_changes": ib_handle_changes,
        "ib_handle_changes_per_draw": safe_ratio(ib_handle_changes, draws),
        "combined_handle_changes_per_draw": safe_ratio(
            stream_handle_changes + ib_handle_changes, draws
        ),
        "stream_unique_handles": as_int(row.get("stream_unique_handles")),
        "ib_unique_handles": as_int(row.get("ib_unique_handles")),
        "stream_unique_bytes": as_int(row.get("stream_unique_bytes")),
        "ib_unique_bytes": as_int(row.get("ib_unique_bytes")),
        "explicit_writer_bytes": writers,
        "explicit_writer_bytes_per_draw": safe_ratio(writers, draws),
        "explicit_writer_bytes_per_vertex": safe_ratio(writers, vertices),
        "lru32_delta": as_int(row.get("indexed_cache_opt_candidate_miss_delta_32")),
        "pso_changes": as_int(row.get("pso_handle_changes")),
        **draw_metrics,
        "stream_breakdown": stream_breakdown(stream_rows.get(row_key(row), [])),
    }


def classify(
    metrics: dict[str, Any],
    *,
    min_triangles: int,
    min_combined_handle_changes_per_draw: float,
    offset_stride_dominance_ratio: float,
) -> tuple[str, str, str]:
    draws = int(metrics["draws"])
    triangles = int(metrics["triangles"])
    if draws == 0:
        return "no-draws", "row has no draws", "ignore this row"
    if triangles < min_triangles:
        return (
            "low-geometry",
            f"triangle count is below the hot-row threshold ({triangles} < {min_triangles})",
            "ignore for backend-storage Xcode budgeting",
        )

    combined_handle_per_draw = float(metrics["combined_handle_changes_per_draw"])
    offset_stride_per_draw = float(metrics["stream_offset_stride_per_draw"])
    if (
        offset_stride_per_draw > 0
        and offset_stride_per_draw >= combined_handle_per_draw * offset_stride_dominance_ratio
    ):
        return (
            "offset-stride-dominant",
            "offset/stride churn dominates handle churn",
            "inspect offset normalization before handle-stabilizing experiments",
        )

    if combined_handle_per_draw >= min_combined_handle_changes_per_draw:
        return (
            "handle-churn-dominant",
            "stream/IB handle changes dominate the hot-row binding motion",
            "build a no-gputrace handle-stable A/B before spending Xcode counters",
        )

    return (
        "stream-ib-not-dominant",
        "stream/IB handle churn is below the hot-row threshold",
        "do not schedule a stream/IB backend-storage experiment for this row",
    )


def analyze_rows(
    rows: Iterable[dict[str, str]],
    stream_rows: dict[str, list[dict[str, str]]],
    probe_draw_rows: dict[str, list[dict[str, str]]],
    *,
    selected_rows: set[str],
    min_triangles: int,
    min_combined_handle_changes_per_draw: float,
    offset_stride_dominance_ratio: float,
    top: int,
) -> list[dict[str, str]]:
    analyzed: list[dict[str, str]] = []
    for row in rows:
        metrics = row_metrics(row, stream_rows, probe_draw_rows)
        if selected_rows and metrics["row"] not in selected_rows:
            continue
        verdict, reason, action = classify(
            metrics,
            min_triangles=min_triangles,
            min_combined_handle_changes_per_draw=min_combined_handle_changes_per_draw,
            offset_stride_dominance_ratio=offset_stride_dominance_ratio,
        )
        analyzed.append({
            "row": str(metrics["row"]),
            "verdict": verdict,
            "draws": str(metrics["draws"]),
            "triangles": str(metrics["triangles"]),
            "vertices": str(metrics["vertices"]),
            "stream_metal_binds": str(metrics["stream_metal_binds"]),
            "stream_handle_changes": str(metrics["stream_handle_changes"]),
            "stream_offset_changes": str(metrics["stream_offset_changes"]),
            "stream_stride_changes": str(metrics["stream_stride_changes"]),
            "stream_handle_changes_per_draw": fmt_float(metrics["stream_handle_changes_per_draw"]),
            "stream_offset_stride_per_draw": fmt_float(metrics["stream_offset_stride_per_draw"]),
            "ib_metal_binds": str(metrics["ib_metal_binds"]),
            "ib_handle_changes": str(metrics["ib_handle_changes"]),
            "ib_handle_changes_per_draw": fmt_float(metrics["ib_handle_changes_per_draw"]),
            "combined_handle_changes_per_draw": fmt_float(metrics["combined_handle_changes_per_draw"]),
            "stream_unique_handles": str(metrics["stream_unique_handles"]),
            "ib_unique_handles": str(metrics["ib_unique_handles"]),
            "stream_unique_bytes": str(metrics["stream_unique_bytes"]),
            "ib_unique_bytes": str(metrics["ib_unique_bytes"]),
            "explicit_writer_bytes": str(metrics["explicit_writer_bytes"]),
            "explicit_writer_bytes_per_draw": fmt_float(metrics["explicit_writer_bytes_per_draw"]),
            "explicit_writer_bytes_per_vertex": fmt_float(metrics["explicit_writer_bytes_per_vertex"]),
            "lru32_delta": str(metrics["lru32_delta"]),
            "pso_changes": str(metrics["pso_changes"]),
            "draw_stream0_handle_changes": str(metrics["draw_stream0_handle_changes"]),
            "draw_index_buffer_changes": str(metrics["draw_index_buffer_changes"]),
            "draw_extra_stream_binding_changes": str(metrics["draw_extra_stream_binding_changes"]),
            "draw_extra_stream_binding_unique": str(metrics["draw_extra_stream_binding_unique"]),
            "draw_extra_streams_observed": str(metrics["draw_extra_streams_observed"]),
            "draw_binding_tuple_changes": str(metrics["draw_binding_tuple_changes"]),
            "draw_binding_tuple_unique": str(metrics["draw_binding_tuple_unique"]),
            "draw_binding_tuple_max_run": str(metrics["draw_binding_tuple_max_run"]),
            "draw_binding_tuple_avg_run": fmt_float(metrics["draw_binding_tuple_avg_run"]),
            "draw_stream0_handle_unique": str(metrics["draw_stream0_handle_unique"]),
            "draw_index_buffer_unique": str(metrics["draw_index_buffer_unique"]),
            "draw_first_extra_handle_unique": str(metrics["draw_first_extra_handle_unique"]),
            "draw_consecutive_pair_count": str(metrics["draw_consecutive_pair_count"]),
            "draw_consecutive_pair_samples": str(metrics["draw_consecutive_pair_samples"]),
            "draw_consecutive_pair_ratio": fmt_float(metrics["draw_consecutive_pair_ratio"]),
            "draw_stream0_ib_delta_top": str(metrics["draw_stream0_ib_delta_top"]),
            "draw_consecutive_triplet_count": str(metrics["draw_consecutive_triplet_count"]),
            "draw_consecutive_triplet_samples": str(metrics["draw_consecutive_triplet_samples"]),
            "draw_consecutive_triplet_ratio": fmt_float(metrics["draw_consecutive_triplet_ratio"]),
            "draw_triplet_delta_top": str(metrics["draw_triplet_delta_top"]),
            "draw_transition_classes": str(metrics["draw_transition_classes"]),
            "draw_top_binding_tuples": str(metrics["draw_top_binding_tuples"]),
            "stream_breakdown": str(metrics["stream_breakdown"]),
            "reason": reason,
            "next_action": action,
        })
    analyzed.sort(
        key=lambda item: (
            item["verdict"] == "handle-churn-dominant",
            as_int(item["triangles"]),
            abs(as_int(item["lru32_delta"])),
        ),
        reverse=True,
    )
    return analyzed[:top] if not selected_rows else analyzed


def overall_verdict(rows: list[dict[str, str]]) -> tuple[str, str]:
    hot = [row for row in rows if row["verdict"] == "handle-churn-dominant"]
    if hot:
        return (
            "stream-ib-isolation-required",
            f"{len(hot)} hot row(s) are handle-churn-dominant; this names an A/B target, not a direct Xcode spend",
        )
    offset = [row for row in rows if row["verdict"] == "offset-stride-dominant"]
    if offset:
        return (
            "offset-stride-preflight-required",
            "offset/stride churn dominates at least one hot row; normalize that before handle experiments",
        )
    return (
        "no-stream-ib-xcode-candidate",
        "no hot row has stream/IB churn high enough to justify an isolated backend-storage preflight",
    )


def write_csv(path: Path, rows: list[dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=CSV_FIELDS)
        writer.writeheader()
        writer.writerows(rows)


def write_markdown(
    path: Path,
    encoder_csv: Path,
    streams_csv: Path | None,
    probe_draws_csv: Path | None,
    rows: list[dict[str, str]],
) -> None:
    verdict, reason = overall_verdict(rows)
    lines = [
        "# Stream/IB Backend Churn Preflight",
        "",
        f"- Encoder source: `{encoder_csv}`",
        f"- Stream source: `{streams_csv}`" if streams_csv else "- Stream source: `(not provided)`",
        f"- Probe draws source: `{probe_draws_csv}`" if probe_draws_csv else "- Probe draws source: `(not provided)`",
        f"- Overall: `{verdict}`",
        f"- Reason: {reason}",
        "",
        "| Row | Verdict | Draws | Triangles | Stream h/draw | IB h/draw | Combined h/draw | Offset+stride/draw | Stream unique | IB unique | Draw tuple changes | Unique tuples | Max tuple run | Avg tuple run | Draw extra changes | Explicit B/vertex | LRU32 delta | Stream breakdown | Next action |",
        "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---|",
    ]
    for row in rows:
        lines.append(
            "| "
            + " | ".join([
                f"`{row['row']}`",
                f"`{row['verdict']}`",
                fmt_int(as_int(row["draws"])),
                fmt_int(as_int(row["triangles"])),
                row["stream_handle_changes_per_draw"],
                row["ib_handle_changes_per_draw"],
                row["combined_handle_changes_per_draw"],
                row["stream_offset_stride_per_draw"],
                fmt_int(as_int(row["stream_unique_handles"])),
                fmt_int(as_int(row["ib_unique_handles"])),
                fmt_int(as_int(row["draw_binding_tuple_changes"])),
                fmt_int(as_int(row["draw_binding_tuple_unique"])),
                fmt_int(as_int(row["draw_binding_tuple_max_run"])),
                row["draw_binding_tuple_avg_run"],
                fmt_int(as_int(row["draw_extra_stream_binding_changes"])),
                row["explicit_writer_bytes_per_vertex"],
                fmt_int(as_int(row["lru32_delta"])),
                f"`{row['stream_breakdown']}`",
                row["next_action"],
            ])
            + " |"
        )
    lines.append("")
    lines.extend([
        "## Draw Tuple Detail",
        "",
        "| Row | Stream0 unique | IB unique | First extra unique | Consecutive s0/IB pairs | Consecutive triplets | Pair delta | Triplet delta | Transition classes | Top binding tuples |",
        "|---|---:|---:|---:|---:|---:|---|---|---|---|",
    ])
    for row in rows:
        pair = (
            f"{fmt_int(as_int(row['draw_consecutive_pair_count']))}/"
            f"{fmt_int(as_int(row['draw_consecutive_pair_samples']))}"
            f" ({row['draw_consecutive_pair_ratio']})"
        )
        triplet = (
            f"{fmt_int(as_int(row['draw_consecutive_triplet_count']))}/"
            f"{fmt_int(as_int(row['draw_consecutive_triplet_samples']))}"
            f" ({row['draw_consecutive_triplet_ratio']})"
        )
        lines.append(
            "| "
            + " | ".join([
                f"`{row['row']}`",
                fmt_int(as_int(row["draw_stream0_handle_unique"])),
                fmt_int(as_int(row["draw_index_buffer_unique"])),
                fmt_int(as_int(row["draw_first_extra_handle_unique"])),
                pair,
                triplet,
                f"`{row['draw_stream0_ib_delta_top']}`",
                f"`{row['draw_triplet_delta_top']}`",
                f"`{row['draw_transition_classes']}`",
                f"`{row['draw_top_binding_tuples']}`",
            ])
            + " |"
        )
    lines.append("")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines), encoding="utf-8")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("encoder_csv", type=Path)
    parser.add_argument("--streams-csv", type=Path)
    parser.add_argument("--probe-draws", type=Path)
    parser.add_argument("--row", action="append", default=[], help="Optional SEQ/ENC filter")
    parser.add_argument("--top", type=int, default=12)
    parser.add_argument("--min-triangles", type=int, default=4096)
    parser.add_argument("--min-combined-handle-changes-per-draw", type=float, default=1.0)
    parser.add_argument("--offset-stride-dominance-ratio", type=float, default=1.0)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--csv-output", type=Path)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    streams_csv = args.streams_csv
    if streams_csv is None:
        candidate = args.encoder_csv.with_name("3dmark05-perf-encoder-streams.csv")
        streams_csv = candidate if candidate.exists() else None
    probe_draws_csv = args.probe_draws
    if probe_draws_csv is None:
        candidate = args.encoder_csv.with_name("3dmark05-perf-indexed-probe-draws.csv")
        probe_draws_csv = candidate if candidate.exists() else None
    rows = analyze_rows(
        load_rows(args.encoder_csv),
        load_stream_rows(streams_csv),
        load_probe_draw_rows(probe_draws_csv),
        selected_rows=set(args.row),
        min_triangles=args.min_triangles,
        min_combined_handle_changes_per_draw=args.min_combined_handle_changes_per_draw,
        offset_stride_dominance_ratio=args.offset_stride_dominance_ratio,
        top=args.top,
    )
    if args.output:
        write_markdown(args.output, args.encoder_csv, streams_csv, probe_draws_csv, rows)
    if args.csv_output:
        write_csv(args.csv_output, rows)
    if not args.output and not args.csv_output:
        for row in rows:
            print(
                f"{row['row']} {row['verdict']} draws={row['draws']} "
                f"tri={row['triangles']} combined-h/draw={row['combined_handle_changes_per_draw']} "
                f"stream-h/draw={row['stream_handle_changes_per_draw']} "
                f"ib-h/draw={row['ib_handle_changes_per_draw']}"
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
