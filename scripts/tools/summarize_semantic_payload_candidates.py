#!/usr/bin/env python3
"""Summarize ranked 3DMark05 semantic payload replay outcomes."""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
from typing import Any


def as_int(value: Any) -> int:
    try:
        return int(float(str(value)))
    except (TypeError, ValueError):
        return 0


def as_float(value: Any) -> float:
    try:
        return float(str(value))
    except (TypeError, ValueError):
        return 0.0


def load_csv(path: Path) -> list[dict[str, str]]:
    if not path.exists():
        raise SystemExit(f"missing CSV: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def read_compare_summary(path: Path) -> dict[str, str]:
    rows = load_csv(path)
    if not rows:
        raise SystemExit(f"empty compare CSV: {path}")
    for row in rows:
        if row.get("area") == "full":
            return row
    return rows[0]


def read_mini_replay_summary(path: Path) -> dict[str, Any]:
    if not path.exists():
        raise SystemExit(f"missing mini replay summary: {path}")
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


def parse_rank_outcome(value: str) -> tuple[int, Path, Path]:
    rank_text, sep, rest = value.partition("=")
    if not sep:
        raise argparse.ArgumentTypeError("rank outcome must be RANK=COMPARE_CSV,SUMMARY_JSON")
    compare_text, sep, summary_text = rest.partition(",")
    if not sep:
        raise argparse.ArgumentTypeError("rank outcome must be RANK=COMPARE_CSV,SUMMARY_JSON")
    rank = as_int(rank_text)
    if rank <= 0:
        raise argparse.ArgumentTypeError("rank must be positive")
    return rank, Path(compare_text), Path(summary_text)


def classify_exact(changed_pixels: int, active_pixels: int, sparse_active_pixels: int) -> str:
    if changed_pixels > 0:
        return "visible-fail" if active_pixels > sparse_active_pixels else "sparse-fail"
    if active_pixels <= 0:
        return "no-final-color-exact-pass"
    if active_pixels <= sparse_active_pixels:
        return "sparse-exact-pass"
    return "visible-exact-pass"


def lru32_bucket_fields(verdict: str, lru32_delta: int) -> dict[str, int]:
    fields = {
        "visible_exact_lru32_delta": 0,
        "sparse_exact_lru32_delta": 0,
        "no_final_color_lru32_delta": 0,
        "visible_fail_lru32_delta": 0,
        "sparse_fail_lru32_delta": 0,
    }
    if verdict == "visible-exact-pass":
        fields["visible_exact_lru32_delta"] = lru32_delta
    elif verdict == "sparse-exact-pass":
        fields["sparse_exact_lru32_delta"] = lru32_delta
    elif verdict == "no-final-color-exact-pass":
        fields["no_final_color_lru32_delta"] = lru32_delta
    elif verdict == "visible-fail":
        fields["visible_fail_lru32_delta"] = lru32_delta
    elif verdict == "sparse-fail":
        fields["sparse_fail_lru32_delta"] = lru32_delta
    return fields


def load_candidates(path: Path) -> dict[int, dict[str, Any]]:
    if not path.exists():
        raise SystemExit(f"missing candidate JSON: {path}")
    with path.open(encoding="utf-8") as handle:
        data = json.load(handle)
    if data.get("schema") != "dxmt9.3dmark05.payload_window_list.v1":
        raise SystemExit(f"unsupported candidate schema: {data.get('schema')}")
    out: dict[int, dict[str, Any]] = {}
    for item in data.get("selections", []):
        rank = as_int(item.get("rank"))
        if rank > 0:
            out[rank] = item
    return out


def rank1_from_analysis(
    candidates: dict[int, dict[str, Any]],
    analysis_csv: Path,
    sparse_active_pixels: int,
) -> dict[str, Any]:
    rows = load_csv(analysis_csv)
    if not rows:
        raise SystemExit(f"empty rank1 analysis CSV: {analysis_csv}")
    fail_rows = [row for row in rows if row.get("semantic_status") != "pass"]
    changed_pixels = sum(as_int(row.get("changed_pixels")) for row in fail_rows)
    max_active = max((as_int(row.get("max_active_pixels")) for row in rows), default=0)
    active_fail = max((as_int(row.get("max_active_pixels")) for row in fail_rows), default=0)
    total_lru32 = sum(as_int(row.get("lru32_delta")) for row in rows)
    pass_lru32 = sum(as_int(row.get("lru32_delta")) for row in rows if row.get("semantic_status") == "pass")
    fail_lru32 = sum(as_int(row.get("lru32_delta")) for row in fail_rows)
    lru32_buckets = {
        "visible_exact_lru32_delta": 0,
        "sparse_exact_lru32_delta": 0,
        "no_final_color_lru32_delta": 0,
        "visible_fail_lru32_delta": 0,
        "sparse_fail_lru32_delta": 0,
    }
    for row in rows:
        visibility = row.get("visibility_class", "")
        if not visibility:
            visibility = classify_exact(
                as_int(row.get("changed_pixels")),
                as_int(row.get("max_active_pixels")),
                sparse_active_pixels,
            )
        for key, value in lru32_bucket_fields(
            visibility,
            as_int(row.get("lru32_delta")),
        ).items():
            lru32_buckets[key] += value
    candidate = candidates.get(1, {})
    verdict = classify_exact(0, max_active, sparse_active_pixels)
    if fail_rows:
        verdict = "visible-fail" if active_fail > sparse_active_pixels else "sparse-fail"
    return {
        "rank": 1,
        "row": candidate.get("row", ""),
        "encoder_draws": window_text(candidate),
        "group_draws": as_int(candidate.get("group", {}).get("draws")),
        "group_primitives": as_int(candidate.get("group", {}).get("primitive_count")),
        "group_lru64": as_int(candidate.get("group", {}).get("cache_miss64")),
        "vs": candidate.get("group", {}).get("vs", ""),
        "ps": candidate.get("group", {}).get("ps", ""),
        "verdict": verdict,
        "changed_pixels": changed_pixels,
        "active_pixels": active_fail if fail_rows else max_active,
        "lru32_delta": total_lru32,
        "safe_lru32_delta": pass_lru32,
        "unsafe_lru32_delta": fail_lru32,
        "exact_pass_draws": len(rows) - len(fail_rows),
        "exact_fail_draws": len(fail_rows),
        "source": str(analysis_csv),
        **lru32_buckets,
    }


def window_text(candidate: dict[str, Any]) -> str:
    window = candidate.get("window", {})
    low = window.get("encoder_draw_min")
    high = window.get("encoder_draw_max")
    if low is None or high is None:
        return ""
    return f"{low}..{high}"


def outcome_from_replay(
    candidates: dict[int, dict[str, Any]],
    rank: int,
    compare_csv: Path,
    summary_json: Path,
    sparse_active_pixels: int,
) -> dict[str, Any]:
    compare = read_compare_summary(compare_csv)
    summary = read_mini_replay_summary(summary_json)
    estimate = summary.get("index_cache_estimate", {})
    changed_pixels = as_int(compare.get("changed_pixels"))
    active_pixels = max(as_int(compare.get("before_active_pixels")), as_int(compare.get("after_active_pixels")))
    candidate = candidates.get(rank, {})
    verdict = classify_exact(changed_pixels, active_pixels, sparse_active_pixels)
    safe_lru32 = as_int(estimate.get("replay_lru32_miss_delta")) if changed_pixels == 0 else 0
    unsafe_lru32 = 0 if changed_pixels == 0 else as_int(estimate.get("replay_lru32_miss_delta"))
    return {
        "rank": rank,
        "row": candidate.get("row", ""),
        "encoder_draws": window_text(candidate),
        "group_draws": as_int(candidate.get("group", {}).get("draws")),
        "group_primitives": as_int(candidate.get("group", {}).get("primitive_count")),
        "group_lru64": as_int(candidate.get("group", {}).get("cache_miss64")),
        "vs": candidate.get("group", {}).get("vs", ""),
        "ps": candidate.get("group", {}).get("ps", ""),
        "verdict": classify_exact(changed_pixels, active_pixels, sparse_active_pixels),
        "changed_pixels": changed_pixels,
        "active_pixels": active_pixels,
        "changed_pct": as_float(compare.get("changed_pct")),
        "active_pct": max(as_float(compare.get("before_active_pct")), as_float(compare.get("after_active_pct"))),
        "lru32_delta": as_int(estimate.get("replay_lru32_miss_delta")),
        "lru32_delta_pct": as_float(estimate.get("replay_lru32_miss_delta_pct")),
        "lru64_delta": as_int(estimate.get("replay_lru64_miss_delta")),
        "lru64_delta_pct": as_float(estimate.get("replay_lru64_miss_delta_pct")),
        "index_count": as_int(estimate.get("original_index_count")),
        "unique_indices_per_draw": as_int(estimate.get("original_unique_indices_per_draw")),
        "safe_lru32_delta": safe_lru32,
        "unsafe_lru32_delta": unsafe_lru32,
        "exact_pass_draws": as_int(summary.get("draw_count")) if changed_pixels == 0 else 0,
        "exact_fail_draws": 0 if changed_pixels == 0 else as_int(summary.get("draw_count")),
        "source": f"{compare_csv}; {summary_json}",
        **lru32_bucket_fields(verdict, as_int(estimate.get("replay_lru32_miss_delta"))),
    }


def aggregate_verdict(outcomes: list[dict[str, Any]]) -> tuple[str, str]:
    has_visible_fail = any(row["verdict"] == "visible-fail" for row in outcomes)
    has_visible_pass = any(row["verdict"] == "visible-exact-pass" for row in outcomes)
    sparse_or_hidden = [
        row for row in outcomes
        if row["verdict"] in {"sparse-exact-pass", "no-final-color-exact-pass"}
    ]
    if has_visible_fail and sparse_or_hidden:
        return (
            "reject-broad-reorder",
            "visible final-writer failure exists, while safe ranked payloads are sparse or no-final-color",
        )
    if has_visible_fail:
        return (
            "reject-broad-reorder",
            "visible final-writer failure exists",
        )
    if has_visible_pass:
        return (
            "needs-visible-selector-proof",
            "visible exact-pass payloads exist but require a runtime selector",
        )
    return (
        "sparse-only-positive-controls",
        "ranked exact-pass payloads have no or sparse final-color contribution",
    )


def sum_int(outcomes: list[dict[str, Any]], key: str) -> int:
    return sum(as_int(row.get(key)) for row in outcomes)


def gain_share(delta: int, total_delta: int) -> float:
    total = abs(total_delta)
    return (abs(delta) / total * 100.0) if total else 0.0


def oracle_status(row: dict[str, Any]) -> str:
    verdict = str(row.get("verdict", ""))
    if verdict == "visible-exact-pass":
        return "candidate-final-color-selector"
    if verdict == "visible-fail":
        return "blocks-broad-reorder"
    if verdict == "sparse-exact-pass":
        return "sparse-positive-control"
    if verdict == "no-final-color-exact-pass":
        return "no-final-color-positive-control"
    if verdict == "sparse-fail":
        return "exact-fail"
    return "unknown"


def oracle_next_action(row: dict[str, Any]) -> str:
    status = oracle_status(row)
    if status == "candidate-final-color-selector":
        return "validate a runtime final-color/final-writer selector on a wider replay set"
    if status == "blocks-broad-reorder":
        return "reject broad reorder unless a selector excludes this visible final-writer hazard"
    if status == "sparse-positive-control":
        return "keep as exact positive control; coverage is too sparse for production promotion"
    if status == "no-final-color-positive-control":
        return "requires a runtime no-final-color/occlusion predicate before promotion"
    if status == "exact-fail":
        return "reject exact policy for this payload or keep it diagnostic-only"
    return "inspect replay outcome"


def oracle_priority(row: dict[str, Any]) -> tuple[int, int]:
    status_order = {
        "candidate-final-color-selector": 0,
        "blocks-broad-reorder": 1,
        "sparse-positive-control": 2,
        "no-final-color-positive-control": 3,
        "exact-fail": 4,
    }
    return (
        status_order.get(oracle_status(row), 99),
        -abs(as_int(row.get("lru32_delta"))),
    )


def oracle_bucket_rows(
    visible_exact_lru32: int,
    sparse_exact_lru32: int,
    no_final_color_lru32: int,
    visible_fail_lru32: int,
    sparse_fail_lru32: int,
) -> list[dict[str, Any]]:
    buckets = [
        {
            "bucket": "visible-exact-pass",
            "oracle_status": "candidate-final-color-selector",
            "lru32_delta": visible_exact_lru32,
            "next_action": "validate a runtime final-color/final-writer selector on a wider replay set",
        },
        {
            "bucket": "visible-fail",
            "oracle_status": "blocks-broad-reorder",
            "lru32_delta": visible_fail_lru32,
            "next_action": "reject broad reorder unless a selector excludes this visible final-writer hazard",
        },
        {
            "bucket": "sparse-exact-pass",
            "oracle_status": "sparse-positive-control",
            "lru32_delta": sparse_exact_lru32,
            "next_action": "keep as exact positive control; coverage is too sparse for production promotion",
        },
        {
            "bucket": "no-final-color-exact-pass",
            "oracle_status": "no-final-color-positive-control",
            "lru32_delta": no_final_color_lru32,
            "next_action": "requires a runtime no-final-color/occlusion predicate before promotion",
        },
        {
            "bucket": "sparse-fail",
            "oracle_status": "exact-fail",
            "lru32_delta": sparse_fail_lru32,
            "next_action": "reject exact policy for this payload or keep it diagnostic-only",
        },
    ]
    return sorted(
        [row for row in buckets if as_int(row["lru32_delta"]) != 0],
        key=lambda row: (
            {
                "candidate-final-color-selector": 0,
                "blocks-broad-reorder": 1,
                "sparse-positive-control": 2,
                "no-final-color-positive-control": 3,
                "exact-fail": 4,
            }.get(str(row["oracle_status"]), 99),
            -abs(as_int(row["lru32_delta"])),
        ),
    )


def write_csv_output(path: Path, outcomes: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fieldnames = [
        "rank",
        "row",
        "encoder_draws",
        "group_draws",
        "group_primitives",
        "group_lru64",
        "vs",
        "ps",
        "verdict",
        "changed_pixels",
        "active_pixels",
        "lru32_delta",
        "lru32_delta_pct",
        "lru64_delta",
        "lru64_delta_pct",
        "safe_lru32_delta",
        "unsafe_lru32_delta",
        "visible_exact_lru32_delta",
        "sparse_exact_lru32_delta",
        "no_final_color_lru32_delta",
        "visible_fail_lru32_delta",
        "sparse_fail_lru32_delta",
        "exact_pass_draws",
        "exact_fail_draws",
        "oracle_status",
        "oracle_next_action",
        "source",
    ]
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in outcomes:
            enriched = dict(row)
            enriched["oracle_status"] = oracle_status(row)
            enriched["oracle_next_action"] = oracle_next_action(row)
            writer.writerow({name: enriched.get(name, "") for name in fieldnames})


def md_table(outcomes: list[dict[str, Any]]) -> list[str]:
    lines = [
        "| Rank | Draws | Shader pair | Verdict | Active px | Changed px | LRU32 delta | Meaning |",
        "|---:|---|---|---|---:|---:|---:|---|",
    ]
    meanings = {
        "visible-fail": "Primitive order can change a visible final writer.",
        "sparse-fail": "Failure is small but still fails exact replay.",
        "visible-exact-pass": "Visible safe payload; needs a runtime selector.",
        "sparse-exact-pass": "Positive control only; final-color contribution is sparse.",
        "no-final-color-exact-pass": "Positive control only; payload contributes no final color.",
    }
    for row in outcomes:
        shader_pair = f"`{row.get('vs', '')}` / `{row.get('ps', '')}`"
        lru32 = row.get("lru32_delta", 0)
        if row.get("lru32_delta_pct", "") != "":
            lru32 = f"{lru32} ({as_float(row.get('lru32_delta_pct')):.2f}%)"
        lines.append(
            "| "
            f"`{row.get('rank')}` | "
            f"`{row.get('encoder_draws')}` | "
            f"{shader_pair} | "
            f"`{row.get('verdict')}` | "
            f"`{row.get('active_pixels', 0)}` | "
            f"`{row.get('changed_pixels', 0)}` | "
            f"`{lru32}` | "
            f"{meanings.get(str(row.get('verdict')), '')} |"
        )
    return lines


def write_markdown(path: Path, outcomes: list[dict[str, Any]], candidates_path: Path) -> None:
    verdict, reason = aggregate_verdict(outcomes)
    exact_pass = sum(as_int(row.get("exact_pass_draws")) for row in outcomes)
    exact_fail = sum(as_int(row.get("exact_fail_draws")) for row in outcomes)
    total_lru32 = sum(as_int(row.get("lru32_delta")) for row in outcomes)
    safe_lru32 = sum_int(outcomes, "safe_lru32_delta")
    unsafe_lru32 = sum_int(outcomes, "unsafe_lru32_delta")
    visible_exact_lru32 = sum_int(outcomes, "visible_exact_lru32_delta")
    sparse_exact_lru32 = sum_int(outcomes, "sparse_exact_lru32_delta")
    no_final_color_lru32 = sum_int(outcomes, "no_final_color_lru32_delta")
    visible_fail_lru32 = sum_int(outcomes, "visible_fail_lru32_delta")
    sparse_fail_lru32 = sum_int(outcomes, "sparse_fail_lru32_delta")
    lines = [
        "# Semantic Payload Candidate Summary",
        "",
        f"- Candidates: `{candidates_path}`",
        f"- Aggregate verdict: `{verdict}`",
        f"- Reason: {reason}.",
        "",
        "## Overview",
        "",
        "| Metric | Value |",
        "|---|---:|",
        f"| Ranked outcomes | `{len(outcomes)}` |",
        f"| Exact-pass draws | `{exact_pass}` |",
        f"| Exact-fail draws | `{exact_fail}` |",
        f"| Summed LRU32 delta | `{total_lru32}` |",
        f"| Replay exact-safe LRU32 delta | `{safe_lru32}` |",
        f"| Replay unsafe/fail LRU32 delta | `{unsafe_lru32}` |",
        "",
        "## Ranked Outcomes",
        "",
        *md_table(outcomes),
        "",
        "## Promotion Boundary",
        "",
        "| Bucket | LRU32 delta | Broad-gain share | Promotion meaning |",
        "|---|---:|---:|---|",
        (
            f"| Visible exact-pass | `{visible_exact_lru32}` | "
            f"`{gain_share(visible_exact_lru32, total_lru32):.2f}%` | "
            "Potentially useful, but needs a runtime final-color/final-writer selector. |"
        ),
        (
            f"| Sparse exact-pass | `{sparse_exact_lru32}` | "
            f"`{gain_share(sparse_exact_lru32, total_lru32):.2f}%` | "
            "Positive control; too little final-color coverage to justify broad promotion alone. |"
        ),
        (
            f"| No-final-color exact-pass | `{no_final_color_lru32}` | "
            f"`{gain_share(no_final_color_lru32, total_lru32):.2f}%` | "
            "Safe replay evidence, but no visible contribution in this capture. |"
        ),
        (
            f"| Visible fail | `{visible_fail_lru32}` | "
            f"`{gain_share(visible_fail_lru32, total_lru32):.2f}%` | "
            "Correctness hazard; primitive order changes a visible final writer. |"
        ),
        (
            f"| Sparse fail | `{sparse_fail_lru32}` | "
            f"`{gain_share(sparse_fail_lru32, total_lru32):.2f}%` | "
            "Exact replay hazard even if the visible footprint is small. |"
        ),
        "",
        "## Decision",
        "",
    ]
    if verdict == "reject-broad-reorder":
        lines.extend([
            "Do not promote this shader/state class to broad production primitive",
            "reordering. The ranked evidence contains a visible final-writer",
            "failure, while the exact-safe ranked payloads are sparse or contribute",
            "no final color under the captured depth input.",
            "",
            "Next useful work is either a real final-color/occlusion classifier or a",
            "non-reorder backend-shape experiment that keeps primitive order",
            "unchanged.",
        ])
    else:
        lines.append("Use the aggregate verdict above to decide the next proof gate.")

    lines.extend([
        "",
        "## Final-Color Oracle Queue",
        "",
        "This queue separates selector candidates from positive controls and",
        "correctness blockers. It is still replay evidence: promotion requires a",
        "runtime predicate that can identify the same class before submitting",
        "reordered primitives.",
        "",
        "| Rank | Verdict | Oracle status | LRU32 delta | Active px | Next action |",
        "|---:|---|---|---:|---:|---|",
    ])
    for row in sorted(outcomes, key=oracle_priority):
        lines.append(
            "| "
            f"`{row.get('rank')}` | "
            f"`{row.get('verdict')}` | "
            f"`{oracle_status(row)}` | "
            f"`{row.get('lru32_delta', 0)}` | "
            f"`{row.get('active_pixels', 0)}` | "
            f"{oracle_next_action(row)} |"
        )
    lines.extend([
        "",
        "## Final-Color Oracle Bucket Queue",
        "",
        "Rank-level verdicts can hide mixed payloads. This bucket queue splits",
        "the summed LRU32 movement by replay visibility class so selector design",
        "can target visible exact-pass value without ignoring visible failures.",
        "",
        "| Bucket | Oracle status | LRU32 delta | Next action |",
        "|---|---|---:|---|",
    ])
    for row in oracle_bucket_rows(
        visible_exact_lru32,
        sparse_exact_lru32,
        no_final_color_lru32,
        visible_fail_lru32,
        sparse_fail_lru32,
    ):
        lines.append(
            "| "
            f"`{row['bucket']}` | "
            f"`{row['oracle_status']}` | "
            f"`{row['lru32_delta']}` | "
            f"{row['next_action']} |"
        )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--candidates", type=Path, required=True)
    parser.add_argument("--rank1-analysis", type=Path)
    parser.add_argument(
        "--rank-outcome",
        action="append",
        default=[],
        type=parse_rank_outcome,
        help="RANK=COMPARE_CSV,SUMMARY_JSON; repeat for rank2+ mini replay outcomes",
    )
    parser.add_argument("--sparse-active-pixels", type=int, default=64)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--csv-output", type=Path)
    args = parser.parse_args()

    candidates = load_candidates(args.candidates)
    outcomes: list[dict[str, Any]] = []
    if args.rank1_analysis is not None:
        outcomes.append(rank1_from_analysis(candidates, args.rank1_analysis, args.sparse_active_pixels))
    for rank, compare_csv, summary_json in args.rank_outcome:
        outcomes.append(outcome_from_replay(candidates, rank, compare_csv, summary_json, args.sparse_active_pixels))
    if not outcomes:
        raise SystemExit("no outcomes were provided")
    outcomes.sort(key=lambda row: as_int(row.get("rank")))

    write_markdown(args.output, outcomes, args.candidates)
    if args.csv_output is not None:
        write_csv_output(args.csv_output, outcomes)
    print(args.output)
    if args.csv_output is not None:
        print(args.csv_output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
