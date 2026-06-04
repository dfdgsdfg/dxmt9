#!/usr/bin/env python3
"""Summarize 3DMark05 reordered-index-cache runtime telemetry.

The Xcode comparison/finalizer path proves whether selected rows reduce
VS invocations and VS buffer writes. This script covers the complementary
runtime question from dxmt encoder CSVs: did the opt-in cache actually serve
draws, create entries, and reject unsafe/non-target lookups?
"""

from __future__ import annotations

import argparse
import csv
from pathlib import Path
from typing import Any


def as_float(value: Any) -> float:
    try:
        return float(str(value))
    except (TypeError, ValueError):
        return 0.0


def as_int(value: Any) -> int:
    return int(as_float(value))


def pct(numerator: float, denominator: float) -> float:
    return (numerator / denominator * 100.0) if denominator else 0.0


def load_csv(path: Path) -> list[dict[str, str]]:
    if not path.exists():
        raise SystemExit(f"missing CSV: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def sum_field(rows: list[dict[str, str]], name: str) -> float:
    return sum(as_float(row.get(name)) for row in rows)


def parse_run(value: str) -> tuple[str, Path, Path | None]:
    label, sep, rest = value.partition("=")
    if not sep or not label or not rest:
        raise argparse.ArgumentTypeError("run must be LABEL=ENCODERS_CSV[,PROBE_DRAWS_CSV]")
    encoder_text, sep, probe_text = rest.partition(",")
    return label, Path(encoder_text), Path(probe_text) if sep and probe_text else None


def probe_draw_summary(path: Path | None) -> dict[str, Any]:
    if path is None:
        return {
            "probe_draws_csv": "",
            "probe_draw_logging": "not-provided",
            "probe_draw_rows": 0,
            "probe_eligible_rows": 0,
            "probe_applied_rows": 0,
            "probe_rejected_rows": 0,
            "probe_all_original_miss32": 0,
            "probe_all_effective_miss32": 0,
            "probe_all_delta32": 0,
            "probe_applied_original_miss32": 0,
            "probe_applied_effective_miss32": 0,
            "probe_applied_delta32": 0,
        }

    rows = load_csv(path)
    eligible = sum(as_int(row.get("eligible")) for row in rows)
    applied = sum(as_int(row.get("applied")) for row in rows)
    rejected = max(0, len(rows) - applied)
    all_original = sum(as_int(row.get("original_cache_miss32")) for row in rows)
    all_effective = sum(as_int(row.get("effective_cache_miss32")) for row in rows)
    applied_rows = [row for row in rows if as_int(row.get("applied")) > 0]
    applied_original = sum(as_int(row.get("original_cache_miss32")) for row in applied_rows)
    applied_effective = sum(as_int(row.get("effective_cache_miss32")) for row in applied_rows)
    return {
        "probe_draws_csv": str(path),
        "probe_draw_logging": "per-draw-present" if rows else "empty-or-scoped-out",
        "probe_draw_rows": len(rows),
        "probe_eligible_rows": eligible,
        "probe_applied_rows": applied,
        "probe_rejected_rows": rejected,
        "probe_all_original_miss32": all_original,
        "probe_all_effective_miss32": all_effective,
        "probe_all_delta32": all_effective - all_original,
        "probe_applied_original_miss32": applied_original,
        "probe_applied_effective_miss32": applied_effective,
        "probe_applied_delta32": applied_effective - applied_original,
    }


def summarize_run(label: str, encoders_csv: Path, probe_csv: Path | None) -> dict[str, Any]:
    rows = load_csv(encoders_csv)
    lookups = sum_field(rows, "reordered_index_cache_lookups")
    hits = sum_field(rows, "reordered_index_cache_hits")
    rejected_hits = sum_field(rows, "reordered_index_cache_rejected_hits")
    misses = sum_field(rows, "reordered_index_cache_misses")
    created = sum_field(rows, "reordered_index_cache_created")
    created_bytes = sum_field(rows, "reordered_index_cache_created_bytes")
    miss_not_created = max(0.0, misses - created)
    runtime_applied = hits + created
    runtime_skipped = rejected_hits + miss_not_created
    indexed_order_applied = sum_field(rows, "indexed_order_probe_draws")
    indexed_order_skipped = sum_field(rows, "indexed_order_probe_skipped")

    candidate_original32 = sum_field(rows, "indexed_cache_opt_candidate_original_miss32")
    candidate_effective32 = sum_field(rows, "indexed_cache_opt_candidate_miss32")
    candidate_delta32 = candidate_effective32 - candidate_original32

    seq_keys = sorted({row.get("seq", "") for row in rows if row.get("seq", "")})
    enc_keys = sorted({
        f"{row.get('seq', '')}/{row.get('encoder', '')}"
        for row in rows
        if row.get("seq", "") or row.get("encoder", "")
    })
    if lookups <= 0:
        verdict = "no-cache-runtime-activity"
    elif runtime_applied > 0 and runtime_skipped > 0:
        verdict = "active-with-rejections"
    elif runtime_applied > 0:
        verdict = "active"
    else:
        verdict = "lookup-only"

    out: dict[str, Any] = {
        "run": label,
        "encoders_csv": str(encoders_csv),
        "encoder_rows": len(rows),
        "unique_seq_count": len(seq_keys),
        "seq_scope": ",".join(seq_keys) if len(seq_keys) <= 12 else f"{len(seq_keys)} seqs",
        "unique_seq_enc_count": len(enc_keys),
        "seq_enc_scope": ",".join(enc_keys) if len(enc_keys) <= 12 else f"{len(enc_keys)} seq/enc rows",
        "draw_calls": int(sum_field(rows, "draw_calls")),
        "cache_lookups": int(lookups),
        "cache_hits": int(hits),
        "cache_rejected_hits": int(rejected_hits),
        "cache_misses": int(misses),
        "cache_created": int(created),
        "cache_created_bytes": int(created_bytes),
        "cache_miss_not_created": int(miss_not_created),
        "runtime_applied_draws": int(runtime_applied),
        "runtime_skipped_draws": int(runtime_skipped),
        "runtime_apply_rate_pct": pct(runtime_applied, lookups),
        "runtime_skip_rate_pct": pct(runtime_skipped, lookups),
        "cache_hit_rate_pct": pct(hits, lookups),
        "cache_rejected_hit_rate_pct": pct(rejected_hits, lookups),
        "cache_miss_rate_pct": pct(misses, lookups),
        "cache_create_rate_pct": pct(created, lookups),
        "indexed_order_probe_draws": int(indexed_order_applied),
        "indexed_order_probe_skipped": int(indexed_order_skipped),
        "indexed_order_probe_bytes": int(sum_field(rows, "indexed_order_probe_bytes")),
        "indexed_order_accounting_delta": int(indexed_order_applied - runtime_applied),
        "indexed_skip_accounting_delta": int(indexed_order_skipped - runtime_skipped),
        "candidate_draws": int(sum_field(rows, "indexed_cache_opt_candidate_draws")),
        "candidate_skipped": int(sum_field(rows, "indexed_cache_opt_candidate_skipped")),
        "candidate_bytes": int(sum_field(rows, "indexed_cache_opt_candidate_bytes")),
        "candidate_original_miss32": int(candidate_original32),
        "candidate_effective_miss32": int(candidate_effective32),
        "candidate_miss_delta32": int(candidate_delta32),
        "candidate_miss_delta32_pct": pct(candidate_delta32, candidate_original32),
        "verdict": verdict,
    }
    out.update(probe_draw_summary(probe_csv))
    return out


CSV_FIELDS = [
    "run",
    "encoder_rows",
    "unique_seq_count",
    "seq_scope",
    "unique_seq_enc_count",
    "seq_enc_scope",
    "draw_calls",
    "cache_lookups",
    "runtime_applied_draws",
    "runtime_skipped_draws",
    "runtime_apply_rate_pct",
    "runtime_skip_rate_pct",
    "cache_hits",
    "cache_rejected_hits",
    "cache_misses",
    "cache_created",
    "cache_created_bytes",
    "cache_miss_not_created",
    "cache_hit_rate_pct",
    "cache_rejected_hit_rate_pct",
    "cache_miss_rate_pct",
    "cache_create_rate_pct",
    "indexed_order_probe_draws",
    "indexed_order_probe_skipped",
    "indexed_order_probe_bytes",
    "indexed_order_accounting_delta",
    "indexed_skip_accounting_delta",
    "candidate_draws",
    "candidate_skipped",
    "candidate_bytes",
    "candidate_original_miss32",
    "candidate_effective_miss32",
    "candidate_miss_delta32",
    "candidate_miss_delta32_pct",
    "probe_draw_logging",
    "probe_draw_rows",
    "probe_eligible_rows",
    "probe_applied_rows",
    "probe_rejected_rows",
    "probe_all_original_miss32",
    "probe_all_effective_miss32",
    "probe_all_delta32",
    "probe_applied_original_miss32",
    "probe_applied_effective_miss32",
    "probe_applied_delta32",
    "verdict",
    "encoders_csv",
    "probe_draws_csv",
]


def write_csv(path: Path, summaries: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=CSV_FIELDS)
        writer.writeheader()
        for row in summaries:
            writer.writerow({field: row.get(field, "") for field in CSV_FIELDS})


def write_markdown(path: Path, summaries: list[dict[str, Any]]) -> None:
    lines = [
        "# Reordered Index Cache Runtime Summary",
        "",
        "This report summarizes dxmt encoder telemetry for the production-shaped",
        "reordered index-cache path. It complements Xcode counter comparison:",
        "`result.json` can miss these encoder-only runtime counters, while the",
        "encoder CSV shows lookup, hit, creation, and rejection behavior.",
        "",
        "## Overview",
        "",
        "| Run | Scope | Lookups | Applied | Skipped | Hits | Created | Rejected hits | Misses | Probe rows | Verdict |",
        "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|",
    ]
    for row in summaries:
        lines.append(
            "| "
            f"`{row['run']}` | "
            f"`{row['seq_enc_scope']}` | "
            f"`{row['cache_lookups']}` | "
            f"`{row['runtime_applied_draws']}` | "
            f"`{row['runtime_skipped_draws']}` | "
            f"`{row['cache_hits']}` | "
            f"`{row['cache_created']}` | "
            f"`{row['cache_rejected_hits']}` | "
            f"`{row['cache_misses']}` | "
            f"`{row['probe_draw_rows']}` | "
            f"`{row['verdict']}` |"
        )
    lines.extend([
        "",
        "## Runtime Accounting",
        "",
        "| Run | Apply rate | Skip rate | Hit rate | Create rate | Miss-not-created | Accounting delta |",
        "|---|---:|---:|---:|---:|---:|---:|",
    ])
    for row in summaries:
        lines.append(
            "| "
            f"`{row['run']}` | "
            f"`{row['runtime_apply_rate_pct']:.2f}%` | "
            f"`{row['runtime_skip_rate_pct']:.2f}%` | "
            f"`{row['cache_hit_rate_pct']:.2f}%` | "
            f"`{row['cache_create_rate_pct']:.4f}%` | "
            f"`{row['cache_miss_not_created']}` | "
            f"`applied {row['indexed_order_accounting_delta']}, skipped {row['indexed_skip_accounting_delta']}` |"
        )
    lines.extend([
        "",
        "## Candidate Measurement",
        "",
        "| Run | Candidate draws | Candidate bytes | Original LRU32 | Effective LRU32 | Delta |",
        "|---|---:|---:|---:|---:|---:|",
    ])
    for row in summaries:
        lines.append(
            "| "
            f"`{row['run']}` | "
            f"`{row['candidate_draws']}` | "
            f"`{row['candidate_bytes']}` | "
            f"`{row['candidate_original_miss32']}` | "
            f"`{row['candidate_effective_miss32']}` | "
            f"`{row['candidate_miss_delta32']} ({row['candidate_miss_delta32_pct']:.2f}%)` |"
        )
    lines.extend([
        "",
        "## Probe Draw Evidence",
        "",
        "| Run | Probe logging | Rows | Eligible | Applied | Rejected | Applied LRU32 delta | All LRU32 delta |",
        "|---|---|---:|---:|---:|---:|---:|---:|",
    ])
    for row in summaries:
        lines.append(
            "| "
            f"`{row['run']}` | "
            f"`{row['probe_draw_logging']}` | "
            f"`{row['probe_draw_rows']}` | "
            f"`{row['probe_eligible_rows']}` | "
            f"`{row['probe_applied_rows']}` | "
            f"`{row['probe_rejected_rows']}` | "
            f"`{row['probe_applied_delta32']}` | "
            f"`{row['probe_all_delta32']}` |"
        )
    lines.extend([
        "",
        "## Interpretation",
        "",
        "- Positive `runtime_applied_draws` means the production cache path was not a",
        "  no-op; `hits + created` accounts for applied reordered index buffers.",
        "- Positive `runtime_skipped_draws` is expected for a conservative opt-in",
        "  policy: rejected hits and miss-not-created entries are non-target or",
        "  unsafe lookups that were left in original order.",
        "- `probe_draw_rows=0` means there is no per-draw proof in that run, usually",
        "  because diagnostics were scoped or suppressed for a full no-gputrace",
        "  smoke. Use those rows for runtime health, not semantic correctness.",
    ])
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--run",
        action="append",
        required=True,
        type=parse_run,
        help="LABEL=ENCODERS_CSV[,PROBE_DRAWS_CSV]; repeat for multiple runs",
    )
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--csv-output", type=Path)
    args = parser.parse_args()

    summaries = [summarize_run(label, encoders_csv, probe_csv) for label, encoders_csv, probe_csv in args.run]
    write_markdown(args.output, summaries)
    if args.csv_output is not None:
        write_csv(args.csv_output, summaries)
    print(args.output)
    if args.csv_output is not None:
        print(args.csv_output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
