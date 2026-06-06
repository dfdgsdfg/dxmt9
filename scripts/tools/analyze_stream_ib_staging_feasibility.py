#!/usr/bin/env python3
"""Estimate row-stable stream/IB staging cost before an Xcode counter run."""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import csv
import re
from pathlib import Path
from typing import Any, Iterable


CSV_FIELDS = [
    "row",
    "verdict",
    "draws",
    "triangles",
    "vertices",
    "tuple_changes",
    "tuple_unique",
    "stream_slots",
    "stream0_unique_handles",
    "extra_stream_unique_handles",
    "ib_unique_handles",
    "source_range_count",
    "merged_range_count",
    "staging_copy_bytes",
    "stream0_copy_bytes",
    "extra_stream_copy_bytes",
    "ib_copy_bytes",
    "staging_copy_bytes_per_draw",
    "staging_copy_bytes_per_vertex",
    "existing_explicit_writer_bytes",
    "staging_to_existing_writer_ratio",
    "expected_stream_offset_changes",
    "expected_ib_offset_changes",
    "expected_offset_changes_per_draw",
    "non_original_effective_index_sources",
    "negative_base_vertex_draws",
    "top_binding_tuples",
    "reason",
    "next_action",
]

EXTRA_BINDING_RE = re.compile(r"s(\d+):(0x[0-9a-fA-F]+|\d+)@(\d+)/(\d+)")


def as_int(value: Any) -> int:
    text = str(value or "").strip().replace(",", "")
    if not text:
        return 0
    try:
        return int(text, 0)
    except ValueError:
        try:
            return int(float(text))
        except ValueError:
            return 0


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


def group_rows(rows: Iterable[dict[str, str]]) -> dict[str, list[dict[str, str]]]:
    grouped: dict[str, list[dict[str, str]]] = defaultdict(list)
    for row in rows:
        grouped[row_key(row)].append(row)
    for grouped_rows in grouped.values():
        grouped_rows.sort(key=lambda item: as_int(item.get("encoder_draw_index")))
    return dict(grouped)


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


def load_encoder_writers(path: Path | None) -> dict[str, int]:
    if not path or not path.exists():
        return {}
    return {row_key(row): explicit_writer_bytes(row) for row in load_rows(path)}


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


def merge_ranges(ranges: Iterable[tuple[int, int]]) -> list[tuple[int, int]]:
    merged: list[list[int]] = []
    for begin, end in sorted((begin, end) for begin, end in ranges if end > begin):
        if not merged or begin > merged[-1][1]:
            merged.append([begin, end])
        else:
            merged[-1][1] = max(merged[-1][1], end)
    return [(begin, end) for begin, end in merged]


def parse_extra_bindings(value: str) -> list[tuple[int, str, int, int]]:
    bindings: list[tuple[int, str, int, int]] = []
    for part in (value or "").split(";"):
        match = EXTRA_BINDING_RE.fullmatch(part.strip())
        if not match:
            continue
        bindings.append((
            int(match.group(1)),
            match.group(2),
            int(match.group(3)),
            int(match.group(4)),
        ))
    return bindings


def tuple_key(row: dict[str, str]) -> str:
    return (
        f"s0={row.get('stream0_handle') or '-'},"
        f"ib={row.get('index_buffer') or '-'},"
        f"extra={row.get('stream_extra_bindings') or '-'}"
    )


def format_counts(counter: Counter[str], limit: int = 3) -> str:
    return ";".join(f"{key}x{count}" for key, count in counter.most_common(limit))


def add_stream0_range(
    ranges: dict[tuple[str, str], list[tuple[int, int]]],
    row: dict[str, str],
) -> None:
    handle = row.get("stream0_handle", "")
    stride = as_int(row.get("stream0_stride"))
    if not handle or stride <= 0:
        return
    begin = as_int(row.get("original_stream0_byte_min"))
    max_byte = as_int(row.get("original_stream0_byte_max"))
    # The probe field is the byte address of the max referenced vertex, not an
    # exclusive end, so include one stride for the staged copy estimate.
    ranges[("s0", handle)].append((begin, max_byte + stride))


def add_extra_stream_ranges(
    ranges: dict[tuple[str, str], list[tuple[int, int]]],
    row: dict[str, str],
) -> None:
    base_vertex = as_int(row.get("base_vertex"))
    min_index = as_int(row.get("original_index_min"))
    max_index = as_int(row.get("original_index_max"))
    min_vertex = base_vertex + min_index
    max_vertex = base_vertex + max_index
    if min_vertex < 0 or max_vertex < 0:
        return
    for stream, handle, offset, stride in parse_extra_bindings(
        row.get("stream_extra_bindings", "")
    ):
        if not handle or stride <= 0:
            continue
        begin = offset + min_vertex * stride
        end = offset + (max_vertex + 1) * stride
        ranges[(f"s{stream}", handle)].append((begin, end))


def add_index_range(
    ranges: dict[tuple[str, str], list[tuple[int, int]]],
    row: dict[str, str],
) -> None:
    handle = row.get("index_buffer", "")
    if not handle or handle == "0x0" or handle == "0":
        return
    begin = as_int(row.get("effective_index_offset"))
    byte_count = as_int(row.get("effective_index_bytes"))
    if byte_count <= 0:
        return
    ranges[("ib", handle)].append((begin, begin + byte_count))


def index_source_preserves_original_order(source: str) -> bool:
    return source in {"", "original", "staged-original"}


def classify(
    *,
    draws: int,
    triangles: int,
    staging_copy_bytes: int,
    expected_offset_changes_per_draw: float,
    non_original_sources: int,
    negative_base_vertex_draws: int,
    min_triangles: int,
    max_copy_bytes: int,
) -> tuple[str, str, str]:
    if draws == 0:
        return "no-draws", "row has no indexed probe draws", "ignore"
    if triangles < min_triangles:
        return (
            "low-geometry",
            f"triangle count is below the hot-row threshold ({triangles} < {min_triangles})",
            "ignore for stream/IB staging",
        )
    if non_original_sources:
        return (
            "shape-mutated",
            "effective index source is already non-original on some draws",
            "rerun without index-order mutation before staging",
        )
    if negative_base_vertex_draws:
        return (
            "negative-base-risk",
            "some draws have negative baseVertex; staging offset remap needs a stronger guard",
            "inspect geometry before implementing staging",
        )
    if staging_copy_bytes > max_copy_bytes:
        return (
            "copy-cost-high",
            f"estimated staging copy bytes exceed the configured cap ({staging_copy_bytes} > {max_copy_bytes})",
            "prefer allocation-time coalescing or a narrower row/window",
        )
    if expected_offset_changes_per_draw > 1.0:
        return (
            "staging-ab-candidate-offset-risk",
            "handle churn can be replaced with stable handles, but offset churn will remain high",
            "run no-gputrace staging A/B first; promote to Xcode only if denominator fields stay stable",
        )
    return (
        "staging-ab-candidate",
        "copy cost and shape fields are suitable for a no-gputrace staging A/B",
        "implement row-scoped stable staging and validate before Xcode",
    )


def analyze_group(
    row: str,
    rows: list[dict[str, str]],
    encoder_writers: dict[str, int],
    *,
    min_triangles: int,
    max_copy_bytes: int,
) -> dict[str, str]:
    ranges: dict[tuple[str, str], list[tuple[int, int]]] = defaultdict(list)
    stream0_values: list[str] = []
    ib_values: list[str] = []
    extra_values: list[str] = []
    stream_slots: set[str] = set()
    extra_handles: set[str] = set()
    tuple_counter: Counter[str] = Counter()
    non_original_sources = 0
    negative_base_vertex_draws = 0
    triangles = 0
    vertices = 0

    for item in rows:
        triangles += as_int(item.get("primitive_count"))
        vertices += as_int(item.get("vertex_count"))
        if not index_source_preserves_original_order(
            item.get("effective_index_source", "original")
        ):
            non_original_sources += 1
        if as_int(item.get("base_vertex")) < 0:
            negative_base_vertex_draws += 1
        stream0_values.append(item.get("stream0_handle", ""))
        ib_values.append(item.get("index_buffer", ""))
        extra_values.append(item.get("stream_extra_bindings", ""))
        tuple_counter[tuple_key(item)] += 1
        if item.get("stream0_handle"):
            stream_slots.add("s0")
        for stream, handle, _offset, _stride in parse_extra_bindings(
            item.get("stream_extra_bindings", "")
        ):
            stream_slots.add(f"s{stream}")
            extra_handles.add(handle)
        add_stream0_range(ranges, item)
        add_extra_stream_ranges(ranges, item)
        add_index_range(ranges, item)

    merged_by_source = {key: merge_ranges(value) for key, value in ranges.items()}
    bytes_by_slot: Counter[str] = Counter()
    source_range_count = 0
    merged_range_count = 0
    for (slot, _handle), merged in merged_by_source.items():
        source_range_count += len(ranges[(slot, _handle)])
        merged_range_count += len(merged)
        bytes_by_slot[slot] += sum(end - begin for begin, end in merged)

    stream0_copy_bytes = bytes_by_slot["s0"]
    ib_copy_bytes = bytes_by_slot["ib"]
    extra_stream_copy_bytes = sum(
        byte_count for slot, byte_count in bytes_by_slot.items()
        if slot not in {"s0", "ib"}
    )
    staging_copy_bytes = stream0_copy_bytes + ib_copy_bytes + extra_stream_copy_bytes
    stream_offset_changes = count_changes(stream0_values) + count_changes(extra_values)
    ib_offset_changes = count_changes(ib_values)
    expected_offset_changes_per_draw = safe_ratio(
        stream_offset_changes + ib_offset_changes,
        len(rows),
    )
    existing_writers = encoder_writers.get(row, 0)
    verdict, reason, action = classify(
        draws=len(rows),
        triangles=triangles,
        staging_copy_bytes=staging_copy_bytes,
        expected_offset_changes_per_draw=expected_offset_changes_per_draw,
        non_original_sources=non_original_sources,
        negative_base_vertex_draws=negative_base_vertex_draws,
        min_triangles=min_triangles,
        max_copy_bytes=max_copy_bytes,
    )
    return {
        "row": row,
        "verdict": verdict,
        "draws": str(len(rows)),
        "triangles": str(triangles),
        "vertices": str(vertices),
        "tuple_changes": str(count_changes([tuple_key(item) for item in rows])),
        "tuple_unique": str(len(tuple_counter)),
        "stream_slots": str(len(stream_slots)),
        "stream0_unique_handles": str(len({value for value in stream0_values if value})),
        "extra_stream_unique_handles": str(len(extra_handles)),
        "ib_unique_handles": str(len({value for value in ib_values if value})),
        "source_range_count": str(source_range_count),
        "merged_range_count": str(merged_range_count),
        "staging_copy_bytes": str(staging_copy_bytes),
        "stream0_copy_bytes": str(stream0_copy_bytes),
        "extra_stream_copy_bytes": str(extra_stream_copy_bytes),
        "ib_copy_bytes": str(ib_copy_bytes),
        "staging_copy_bytes_per_draw": fmt_float(
            safe_ratio(staging_copy_bytes, len(rows))
        ),
        "staging_copy_bytes_per_vertex": fmt_float(
            safe_ratio(staging_copy_bytes, vertices)
        ),
        "existing_explicit_writer_bytes": str(existing_writers),
        "staging_to_existing_writer_ratio": fmt_float(
            safe_ratio(staging_copy_bytes, existing_writers)
        ),
        "expected_stream_offset_changes": str(stream_offset_changes),
        "expected_ib_offset_changes": str(ib_offset_changes),
        "expected_offset_changes_per_draw": fmt_float(expected_offset_changes_per_draw),
        "non_original_effective_index_sources": str(non_original_sources),
        "negative_base_vertex_draws": str(negative_base_vertex_draws),
        "top_binding_tuples": format_counts(tuple_counter, limit=3),
        "reason": reason,
        "next_action": action,
    }


def analyze(
    probe_draw_rows: Iterable[dict[str, str]],
    encoder_writers: dict[str, int],
    *,
    selected_rows: set[str],
    min_triangles: int,
    max_copy_bytes: int,
    top: int,
) -> list[dict[str, str]]:
    analyzed = [
        analyze_group(
            key,
            rows,
            encoder_writers,
            min_triangles=min_triangles,
            max_copy_bytes=max_copy_bytes,
        )
        for key, rows in group_rows(probe_draw_rows).items()
        if not selected_rows or key in selected_rows
    ]
    analyzed.sort(
        key=lambda item: (
            item["verdict"].startswith("staging-ab-candidate"),
            as_int(item["triangles"]),
            as_int(item["staging_copy_bytes"]),
        ),
        reverse=True,
    )
    return analyzed if selected_rows else analyzed[:top]


def overall_verdict(rows: list[dict[str, str]]) -> tuple[str, str]:
    candidates = [
        row for row in rows
        if row["verdict"].startswith("staging-ab-candidate")
    ]
    if candidates:
        return (
            "staging-ab-preflight-required",
            f"{len(candidates)} row(s) can be tried as no-gputrace stable-staging A/B candidates",
        )
    if any(row["verdict"] == "copy-cost-high" for row in rows):
        return (
            "coalescing-preferred",
            "hot rows exceed the copy cap; prefer allocation-time coalescing or narrower windows",
        )
    return (
        "no-staging-candidate",
        "no hot row is currently suitable for row-stable staging",
    )


def write_csv(path: Path, rows: list[dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=CSV_FIELDS)
        writer.writeheader()
        writer.writerows(rows)


def write_markdown(
    path: Path,
    probe_draws_csv: Path,
    encoder_csv: Path | None,
    rows: list[dict[str, str]],
) -> None:
    verdict, reason = overall_verdict(rows)
    lines = [
        "# Stream/IB Stable Staging Feasibility",
        "",
        f"- Probe draws source: `{probe_draws_csv}`",
        f"- Encoder source: `{encoder_csv}`" if encoder_csv else "- Encoder source: `(not provided)`",
        f"- Overall: `{verdict}`",
        f"- Reason: {reason}",
        "",
        "| Row | Verdict | Draws | Triangles | Tuples | Stream slots | Handles s0/extra/IB | Copy bytes | Copy B/vertex | Existing writers | Offset changes/draw | Ranges | Next action |",
        "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|",
    ]
    for row in rows:
        handles = (
            f"{fmt_int(as_int(row['stream0_unique_handles']))}/"
            f"{fmt_int(as_int(row['extra_stream_unique_handles']))}/"
            f"{fmt_int(as_int(row['ib_unique_handles']))}"
        )
        tuples = (
            f"{fmt_int(as_int(row['tuple_changes']))}/"
            f"{fmt_int(as_int(row['tuple_unique']))}"
        )
        ranges = (
            f"{fmt_int(as_int(row['source_range_count']))}/"
            f"{fmt_int(as_int(row['merged_range_count']))}"
        )
        lines.append(
            "| "
            + " | ".join([
                f"`{row['row']}`",
                f"`{row['verdict']}`",
                fmt_int(as_int(row["draws"])),
                fmt_int(as_int(row["triangles"])),
                tuples,
                fmt_int(as_int(row["stream_slots"])),
                handles,
                fmt_int(as_int(row["staging_copy_bytes"])),
                row["staging_copy_bytes_per_vertex"],
                fmt_int(as_int(row["existing_explicit_writer_bytes"])),
                row["expected_offset_changes_per_draw"],
                ranges,
                row["next_action"],
            ])
            + " |"
        )
    lines.extend([
        "",
        "## Byte Breakdown",
        "",
        "| Row | Stream0 copy | Extra stream copy | IB copy | Writer ratio | Top binding tuples |",
        "|---|---:|---:|---:|---:|---|",
    ])
    for row in rows:
        lines.append(
            "| "
            + " | ".join([
                f"`{row['row']}`",
                fmt_int(as_int(row["stream0_copy_bytes"])),
                fmt_int(as_int(row["extra_stream_copy_bytes"])),
                fmt_int(as_int(row["ib_copy_bytes"])),
                row["staging_to_existing_writer_ratio"],
                f"`{row['top_binding_tuples']}`",
            ])
            + " |"
        )
    lines.append("")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines), encoding="utf-8")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe_draws_csv", type=Path)
    parser.add_argument("--encoder-csv", type=Path)
    parser.add_argument("--row", action="append", default=[], help="Optional SEQ/ENC filter")
    parser.add_argument("--top", type=int, default=12)
    parser.add_argument("--min-triangles", type=int, default=4096)
    parser.add_argument("--max-copy-bytes", type=int, default=16 * 1024 * 1024)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--csv-output", type=Path)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    encoder_csv = args.encoder_csv
    if encoder_csv is None:
        candidate = args.probe_draws_csv.with_name("3dmark05-perf-encoders.csv")
        encoder_csv = candidate if candidate.exists() else None
    rows = analyze(
        load_rows(args.probe_draws_csv),
        load_encoder_writers(encoder_csv),
        selected_rows=set(args.row),
        min_triangles=args.min_triangles,
        max_copy_bytes=args.max_copy_bytes,
        top=args.top,
    )
    if args.output:
        write_markdown(args.output, args.probe_draws_csv, encoder_csv, rows)
    if args.csv_output:
        write_csv(args.csv_output, rows)
    if not args.output and not args.csv_output:
        for row in rows:
            print(
                f"{row['row']} {row['verdict']} draws={row['draws']} "
                f"tri={row['triangles']} copy={row['staging_copy_bytes']} "
                f"offset-changes/draw={row['expected_offset_changes_per_draw']}"
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
