#!/usr/bin/env python3
"""Summarize Tile-FFP eligibility/routing coverage from dxmt9 encoder CSV."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path
from typing import Any, Iterable


CSV_FIELDS = [
    "row",
    "verdict",
    "draws",
    "primitives",
    "vertices",
    "routed_tile_draws",
    "routed_tile_primitives",
    "routed_tile_vertices",
    "routed_portable_draws",
    "routed_portable_primitives",
    "routed_portable_vertices",
    "eligible_draws",
    "eligible_primitives",
    "eligible_vertices",
    "eligible_primitive_share_pct",
    "eligible_vertex_share_pct",
    "fallback_gpu_family_draws",
    "fallback_gpu_family_primitives",
    "fallback_not_ffp_draws",
    "fallback_not_ffp_primitives",
    "fallback_precision_draws",
    "fallback_precision_primitives",
    "fallback_unsupported_state_draws",
    "fallback_unsupported_state_primitives",
    "ffp_draws",
    "programmable_draws",
    "textured_draws",
    "triangle_estimate",
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


def fmt_int(value: int) -> str:
    return f"{value:,}"


def fmt_float(value: float) -> str:
    return f"{value:.3f}"


def safe_pct(numer: int, denom: int) -> float:
    return (float(numer) / float(denom) * 100.0) if denom else 0.0


def row_key(row: dict[str, str]) -> str:
    return f"{row.get('seq', '')}/{row.get('encoder') or row.get('enc', '')}"


def load_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def row_metrics(row: dict[str, str]) -> dict[str, Any]:
    primitives = as_int(row.get("primitive_count"))
    vertices = as_int(row.get("vertex_count"))
    eligible_primitives = as_int(row.get("tile_ffp_eligible_primitives"))
    eligible_vertices = as_int(row.get("tile_ffp_eligible_vertices"))
    return {
        "row": row_key(row),
        "draws": as_int(row.get("draw_calls")),
        "primitives": primitives,
        "vertices": vertices,
        "routed_tile_draws": as_int(row.get("tile_ffp_routed_tile_draws")),
        "routed_tile_primitives": as_int(row.get("tile_ffp_routed_tile_primitives")),
        "routed_tile_vertices": as_int(row.get("tile_ffp_routed_tile_vertices")),
        "routed_portable_draws": as_int(row.get("tile_ffp_routed_portable_draws")),
        "routed_portable_primitives": as_int(row.get("tile_ffp_routed_portable_primitives")),
        "routed_portable_vertices": as_int(row.get("tile_ffp_routed_portable_vertices")),
        "eligible_draws": as_int(row.get("tile_ffp_eligible_draws")),
        "eligible_primitives": eligible_primitives,
        "eligible_vertices": eligible_vertices,
        "eligible_primitive_share_pct": safe_pct(eligible_primitives, primitives),
        "eligible_vertex_share_pct": safe_pct(eligible_vertices, vertices),
        "fallback_gpu_family_draws": as_int(row.get("tile_ffp_fallback_gpu_family_draws")),
        "fallback_gpu_family_primitives": as_int(row.get("tile_ffp_fallback_gpu_family_primitives")),
        "fallback_not_ffp_draws": as_int(row.get("tile_ffp_fallback_not_ffp_draws")),
        "fallback_not_ffp_primitives": as_int(row.get("tile_ffp_fallback_not_ffp_primitives")),
        "fallback_precision_draws": as_int(row.get("tile_ffp_fallback_precision_draws")),
        "fallback_precision_primitives": as_int(row.get("tile_ffp_fallback_precision_primitives")),
        "fallback_unsupported_state_draws": as_int(row.get("tile_ffp_fallback_unsupported_state_draws")),
        "fallback_unsupported_state_primitives": as_int(row.get("tile_ffp_fallback_unsupported_state_primitives")),
        "ffp_draws": as_int(row.get("ffp_draws")),
        "programmable_draws": as_int(row.get("programmable_draws")),
        "textured_draws": as_int(row.get("textured_draws")),
        "triangle_estimate": as_int(row.get("triangle_estimate")),
    }


def classify(metrics: dict[str, Any], *, min_eligible_primitive_share_pct: float) -> tuple[str, str, str]:
    draws = int(metrics["draws"])
    primitives = int(metrics["primitives"])
    eligible_primitives = int(metrics["eligible_primitives"])
    share = float(metrics["eligible_primitive_share_pct"])

    if draws == 0:
        return "no-draws", "row has no draws", "ignore this row"
    if eligible_primitives == 0:
        return (
            "no-tile-ffp-coverage",
            "no hypothetical Tile-FFP eligible primitives in this row",
            "do not spend Xcode or implementation time on Tile-FFP for this row",
        )
    if share < min_eligible_primitive_share_pct:
        return (
            "low-tile-ffp-coverage",
            f"eligible primitives are only {share:.2f}% of the row",
            "keep Tile-FFP as a narrow correctness experiment, not a GT1 FPS lever",
        )
    return (
        "candidate-tile-ffp-coverage",
        f"eligible primitives cover {share:.2f}% of the row",
        "run portable-vs-tile equality before any Xcode spend",
    )


def analyze_rows(
    rows: Iterable[dict[str, str]],
    *,
    selected_rows: set[str],
    selected_seq: set[str],
    top: int,
    min_eligible_primitive_share_pct: float,
) -> list[dict[str, str]]:
    analyzed: list[dict[str, str]] = []
    for row in rows:
        key = row_key(row)
        if selected_rows and key not in selected_rows:
            continue
        if selected_seq and row.get("seq", "") not in selected_seq:
            continue
        metrics = row_metrics(row)
        verdict, reason, action = classify(
            metrics,
            min_eligible_primitive_share_pct=min_eligible_primitive_share_pct,
        )
        analyzed.append({
            "row": str(metrics["row"]),
            "verdict": verdict,
            "draws": str(metrics["draws"]),
            "primitives": str(metrics["primitives"]),
            "vertices": str(metrics["vertices"]),
            "routed_tile_draws": str(metrics["routed_tile_draws"]),
            "routed_tile_primitives": str(metrics["routed_tile_primitives"]),
            "routed_tile_vertices": str(metrics["routed_tile_vertices"]),
            "routed_portable_draws": str(metrics["routed_portable_draws"]),
            "routed_portable_primitives": str(metrics["routed_portable_primitives"]),
            "routed_portable_vertices": str(metrics["routed_portable_vertices"]),
            "eligible_draws": str(metrics["eligible_draws"]),
            "eligible_primitives": str(metrics["eligible_primitives"]),
            "eligible_vertices": str(metrics["eligible_vertices"]),
            "eligible_primitive_share_pct": fmt_float(metrics["eligible_primitive_share_pct"]),
            "eligible_vertex_share_pct": fmt_float(metrics["eligible_vertex_share_pct"]),
            "fallback_gpu_family_draws": str(metrics["fallback_gpu_family_draws"]),
            "fallback_gpu_family_primitives": str(metrics["fallback_gpu_family_primitives"]),
            "fallback_not_ffp_draws": str(metrics["fallback_not_ffp_draws"]),
            "fallback_not_ffp_primitives": str(metrics["fallback_not_ffp_primitives"]),
            "fallback_precision_draws": str(metrics["fallback_precision_draws"]),
            "fallback_precision_primitives": str(metrics["fallback_precision_primitives"]),
            "fallback_unsupported_state_draws": str(metrics["fallback_unsupported_state_draws"]),
            "fallback_unsupported_state_primitives": str(metrics["fallback_unsupported_state_primitives"]),
            "ffp_draws": str(metrics["ffp_draws"]),
            "programmable_draws": str(metrics["programmable_draws"]),
            "textured_draws": str(metrics["textured_draws"]),
            "triangle_estimate": str(metrics["triangle_estimate"]),
            "reason": reason,
            "next_action": action,
        })
    if selected_rows or selected_seq:
        analyzed.sort(
            key=lambda item: (
                item["verdict"] == "candidate-tile-ffp-coverage",
                as_int(item["primitives"]),
                as_int(item["eligible_primitives"]),
            ),
            reverse=True,
        )
        return analyzed

    analyzed.sort(
        key=lambda item: (
            as_int(item["primitives"]),
            as_int(item["eligible_primitives"]),
        ),
        reverse=True,
    )
    return analyzed[:top]


def overall_verdict(rows: list[dict[str, str]]) -> tuple[str, str]:
    candidates = [row for row in rows if row["verdict"] == "candidate-tile-ffp-coverage"]
    if candidates:
        return (
            "candidate-tile-ffp-coverage",
            f"{len(candidates)} row(s) have enough Tile-FFP eligible primitives to justify equality testing",
        )
    low = [row for row in rows if row["verdict"] == "low-tile-ffp-coverage"]
    if low:
        return (
            "low-tile-ffp-coverage",
            "Tile-FFP eligibility exists but is too small for a current GT1 FPS lever",
        )
    return (
        "no-tile-ffp-hot-coverage",
        "analyzed rows have no hypothetical Tile-FFP eligible primitives",
    )


def write_csv(path: Path, rows: list[dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
      writer = csv.DictWriter(handle, fieldnames=CSV_FIELDS)
      writer.writeheader()
      writer.writerows(rows)


def write_markdown(path: Path, rows: list[dict[str, str]], source: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    verdict, reason = overall_verdict(rows)
    lines = [
        "# Tile-FFP Coverage",
        "",
        f"- Source: `{source}`",
        f"- Overall verdict: `{verdict}`",
        f"- Reason: {reason}",
        "",
        "| Row | Verdict | draws | primitives | eligible prim | eligible % | routed tile prim | not-FFP prim | unsupported prim | next action |",
        "|---|---|---:|---:|---:|---:|---:|---:|---:|---|",
    ]
    for row in rows:
        lines.append(
            "| {row} | `{verdict}` | `{draws}` | `{primitives}` | `{eligible}` | "
            "`{share}` | `{routed}` | `{notffp}` | `{unsupported}` | {action} |".format(
                row=row["row"],
                verdict=row["verdict"],
                draws=fmt_int(as_int(row["draws"])),
                primitives=fmt_int(as_int(row["primitives"])),
                eligible=fmt_int(as_int(row["eligible_primitives"])),
                share=row["eligible_primitive_share_pct"],
                routed=fmt_int(as_int(row["routed_tile_primitives"])),
                notffp=fmt_int(as_int(row["fallback_not_ffp_primitives"])),
                unsupported=fmt_int(as_int(row["fallback_unsupported_state_primitives"])),
                action=row["next_action"],
            )
        )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("encoder_csv", type=Path)
    parser.add_argument("--row", action="append", default=[], help="Restrict to SEQ/ENC row")
    parser.add_argument("--seq", action="append", default=[], help="Restrict to frame/seq id")
    parser.add_argument("--top", type=int, default=12)
    parser.add_argument("--min-eligible-primitive-share-pct", type=float, default=5.0)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--csv-output", type=Path, required=True)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    rows = analyze_rows(
        load_rows(args.encoder_csv),
        selected_rows=set(args.row),
        selected_seq=set(args.seq),
        top=args.top,
        min_eligible_primitive_share_pct=args.min_eligible_primitive_share_pct,
    )
    write_csv(args.csv_output, rows)
    write_markdown(args.output, rows, args.encoder_csv)
    print(args.output)
    print(args.csv_output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
