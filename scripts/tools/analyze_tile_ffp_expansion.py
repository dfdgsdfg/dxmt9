#!/usr/bin/env python3
"""Rank what would be needed to make Tile-FFP cover GT1 hot rows."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path
from typing import Any, Iterable


CSV_FIELDS = [
    "row",
    "verdict",
    "dominant_blocker",
    "draws",
    "primitives",
    "eligible_primitives",
    "eligible_share_pct",
    "not_ffp_primitives",
    "not_ffp_share_pct",
    "unsupported_state_primitives",
    "unsupported_state_share_pct",
    "precision_primitives",
    "precision_share_pct",
    "gpu_family_primitives",
    "gpu_family_share_pct",
    "programmable_draws",
    "ffp_draws",
    "textured_draws",
    "potential_programmable_route_primitives",
    "potential_programmable_route_share_pct",
    "potential_unsupported_state_primitives",
    "potential_unsupported_state_share_pct",
    "potential_any_route_primitives",
    "potential_any_route_share_pct",
    "reason",
    "next_action",
]


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


def fmt_float(value: float) -> str:
    return f"{value:.3f}"


def fmt_int(value: int) -> str:
    return f"{value:,}"


def safe_pct(numer: int, denom: int) -> float:
    return float(numer) / float(denom) * 100.0 if denom else 0.0


def row_key(row: dict[str, str]) -> str:
    return f"{row.get('seq', '')}/{row.get('encoder') or row.get('enc', '')}"


def load_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def blocker_metrics(row: dict[str, str]) -> dict[str, Any]:
    primitives = as_int(row.get("primitive_count"))
    eligible = as_int(row.get("tile_ffp_eligible_primitives"))
    not_ffp = as_int(row.get("tile_ffp_fallback_not_ffp_primitives"))
    unsupported = as_int(row.get("tile_ffp_fallback_unsupported_state_primitives"))
    precision = as_int(row.get("tile_ffp_fallback_precision_primitives"))
    gpu_family = as_int(row.get("tile_ffp_fallback_gpu_family_primitives"))
    programmable_route = eligible + not_ffp
    unsupported_route = eligible + unsupported
    any_route = eligible + not_ffp + unsupported + precision
    blockers = {
        "not-ffp": not_ffp,
        "unsupported-state": unsupported,
        "precision": precision,
        "gpu-family": gpu_family,
        "eligible": eligible,
    }
    dominant = max(blockers, key=blockers.get)
    return {
        "row": row_key(row),
        "draws": as_int(row.get("draw_calls")),
        "primitives": primitives,
        "eligible_primitives": eligible,
        "not_ffp_primitives": not_ffp,
        "unsupported_state_primitives": unsupported,
        "precision_primitives": precision,
        "gpu_family_primitives": gpu_family,
        "programmable_draws": as_int(row.get("programmable_draws")),
        "ffp_draws": as_int(row.get("ffp_draws")),
        "textured_draws": as_int(row.get("textured_draws")),
        "potential_programmable_route_primitives": programmable_route,
        "potential_unsupported_state_primitives": unsupported_route,
        "potential_any_route_primitives": any_route,
        "dominant_blocker": dominant,
    }


def classify(metrics: dict[str, Any], *, min_hot_share_pct: float) -> tuple[str, str, str]:
    primitives = int(metrics["primitives"])
    if primitives == 0:
        return "no-primitives", "row has no primitives", "ignore this row"

    current_share = safe_pct(int(metrics["eligible_primitives"]), primitives)
    not_ffp_share = safe_pct(int(metrics["not_ffp_primitives"]), primitives)
    unsupported_share = safe_pct(int(metrics["unsupported_state_primitives"]), primitives)
    precision_share = safe_pct(int(metrics["precision_primitives"]), primitives)
    any_share = safe_pct(int(metrics["potential_any_route_primitives"]), primitives)
    textured_draws = int(metrics["textured_draws"])
    programmable_draws = int(metrics["programmable_draws"])

    if current_share >= min_hot_share_pct:
        return (
            "current-tile-ffp-coverage",
            f"current eligible primitives already cover {current_share:.2f}% of the row",
            "run portable-vs-tile equality and then a scoped counter A/B",
        )
    if not_ffp_share >= min_hot_share_pct:
        if programmable_draws and textured_draws:
            reason = (
                f"not-FFP fallback covers {not_ffp_share:.2f}% with programmable textured draws"
            )
        elif programmable_draws:
            reason = f"not-FFP fallback covers {not_ffp_share:.2f}% with programmable draws"
        else:
            reason = f"not-FFP fallback covers {not_ffp_share:.2f}%"
        return (
            "needs-programmable-tile-route",
            reason,
            "Tile-FFP cannot reach this row by widening FFP only; define a programmable/textured tile or mesh route first",
        )
    if unsupported_share >= min_hot_share_pct:
        if textured_draws:
            reason = (
                f"unsupported-state fallback covers {unsupported_share:.2f}% and the row is textured"
            )
        else:
            reason = f"unsupported-state fallback covers {unsupported_share:.2f}%"
        return (
            "needs-unsupported-state-expansion",
            reason,
            "identify the unsupported state subset and prove portable-vs-tile equality before Xcode",
        )
    if precision_share >= min_hot_share_pct:
        return (
            "needs-precision-expansion",
            f"precision fallback covers {precision_share:.2f}%",
            "do not expand until exact precision equality is proven",
        )
    if any_share >= min_hot_share_pct:
        return (
            "fragmented-expansion-candidate",
            f"combined non-current Tile-FFP blockers cover {any_share:.2f}%",
            "split blockers by draw-level state before implementing a route",
        )
    return (
        "no-meaningful-expansion",
        f"all potential expansion buckets stay below {min_hot_share_pct:.2f}% row coverage",
        "keep Tile-FFP out of the GT1 hot-row lane",
    )


def analyze_rows(
    rows: Iterable[dict[str, str]],
    *,
    selected_rows: set[str],
    selected_seq: set[str],
    top: int,
    min_hot_share_pct: float,
) -> list[dict[str, str]]:
    analyzed: list[dict[str, str]] = []
    for row in rows:
        key = row_key(row)
        if selected_rows and key not in selected_rows:
            continue
        if selected_seq and row.get("seq", "") not in selected_seq:
            continue
        metrics = blocker_metrics(row)
        verdict, reason, action = classify(metrics, min_hot_share_pct=min_hot_share_pct)
        primitives = int(metrics["primitives"])
        analyzed.append({
            "row": metrics["row"],
            "verdict": verdict,
            "dominant_blocker": metrics["dominant_blocker"],
            "draws": str(metrics["draws"]),
            "primitives": str(primitives),
            "eligible_primitives": str(metrics["eligible_primitives"]),
            "eligible_share_pct": fmt_float(safe_pct(int(metrics["eligible_primitives"]), primitives)),
            "not_ffp_primitives": str(metrics["not_ffp_primitives"]),
            "not_ffp_share_pct": fmt_float(safe_pct(int(metrics["not_ffp_primitives"]), primitives)),
            "unsupported_state_primitives": str(metrics["unsupported_state_primitives"]),
            "unsupported_state_share_pct": fmt_float(safe_pct(int(metrics["unsupported_state_primitives"]), primitives)),
            "precision_primitives": str(metrics["precision_primitives"]),
            "precision_share_pct": fmt_float(safe_pct(int(metrics["precision_primitives"]), primitives)),
            "gpu_family_primitives": str(metrics["gpu_family_primitives"]),
            "gpu_family_share_pct": fmt_float(safe_pct(int(metrics["gpu_family_primitives"]), primitives)),
            "programmable_draws": str(metrics["programmable_draws"]),
            "ffp_draws": str(metrics["ffp_draws"]),
            "textured_draws": str(metrics["textured_draws"]),
            "potential_programmable_route_primitives": str(metrics["potential_programmable_route_primitives"]),
            "potential_programmable_route_share_pct": fmt_float(
                safe_pct(int(metrics["potential_programmable_route_primitives"]), primitives)
            ),
            "potential_unsupported_state_primitives": str(metrics["potential_unsupported_state_primitives"]),
            "potential_unsupported_state_share_pct": fmt_float(
                safe_pct(int(metrics["potential_unsupported_state_primitives"]), primitives)
            ),
            "potential_any_route_primitives": str(metrics["potential_any_route_primitives"]),
            "potential_any_route_share_pct": fmt_float(
                safe_pct(int(metrics["potential_any_route_primitives"]), primitives)
            ),
            "reason": reason,
            "next_action": action,
        })

    analyzed.sort(
        key=lambda item: (
            as_int(item["primitives"]),
            as_int(item["potential_any_route_primitives"]),
        ),
        reverse=True,
    )
    if selected_rows or selected_seq:
        return analyzed
    return analyzed[:top]


def overall_verdict(rows: list[dict[str, str]]) -> tuple[str, str]:
    if not rows:
        return "missing-input", "no rows matched the selected filters"
    verdict_counts: dict[str, int] = {}
    for row in rows:
        verdict_counts[row["verdict"]] = verdict_counts.get(row["verdict"], 0) + 1
    if verdict_counts.get("needs-programmable-tile-route"):
        return (
            "needs-programmable-tile-route",
            f"{verdict_counts['needs-programmable-tile-route']} hot row(s) require programmable/textured route coverage",
        )
    if verdict_counts.get("needs-unsupported-state-expansion"):
        return (
            "needs-unsupported-state-expansion",
            f"{verdict_counts['needs-unsupported-state-expansion']} hot row(s) require unsupported-state expansion",
        )
    if verdict_counts.get("current-tile-ffp-coverage"):
        return (
            "current-tile-ffp-coverage",
            f"{verdict_counts['current-tile-ffp-coverage']} row(s) can use the current Tile-FFP route",
        )
    return "no-current-tile-ffp-expansion", "current expansion buckets do not create a GT1 hot-row route"


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
        "# Tile-FFP Expansion",
        "",
        f"- Source: `{source}`",
        f"- Overall verdict: `{verdict}`",
        f"- Reason: {reason}",
        "",
        "| Row | Verdict | Dominant blocker | primitives | current eligible % | not-FFP % | unsupported % | programmable draws | textured draws | next action |",
        "|---|---|---|---:|---:|---:|---:|---:|---:|---|",
    ]
    for row in rows:
        lines.append(
            "| {row} | `{verdict}` | `{blocker}` | `{primitives}` | `{eligible}` | "
            "`{notffp}` | `{unsupported}` | `{programmable}` | `{textured}` | {action} |".format(
                row=row["row"],
                verdict=row["verdict"],
                blocker=row["dominant_blocker"],
                primitives=fmt_int(as_int(row["primitives"])),
                eligible=row["eligible_share_pct"],
                notffp=row["not_ffp_share_pct"],
                unsupported=row["unsupported_state_share_pct"],
                programmable=fmt_int(as_int(row["programmable_draws"])),
                textured=fmt_int(as_int(row["textured_draws"])),
                action=row["next_action"],
            )
        )
    lines.extend([
        "",
        "```mermaid",
        "flowchart TD",
        "  Current[Current Tile-FFP route] --> Eligible{hot-row coverage?}",
        "  Eligible -- Yes --> Equality[portable-vs-tile equality]",
        "  Eligible -- No --> Blocker{dominant fallback}",
        "  Blocker -- not-FFP --> Programmable[programmable/textured tile or mesh route]",
        "  Blocker -- unsupported-state --> Unsupported[split unsupported state subset]",
        "  Blocker -- precision --> Precision[prove exact precision equality]",
        "  Equality --> Counters[scoped reduced counter A/B]",
        "```",
        "",
    ])
    path.write_text("\n".join(lines), encoding="utf-8")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("encoder_csv", type=Path)
    parser.add_argument("--row", action="append", default=[], help="Restrict to SEQ/ENC row")
    parser.add_argument("--seq", action="append", default=[], help="Restrict to frame/seq id")
    parser.add_argument("--top", type=int, default=12)
    parser.add_argument("--min-hot-share-pct", type=float, default=5.0)
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
        min_hot_share_pct=args.min_hot_share_pct,
    )
    write_csv(args.csv_output, rows)
    write_markdown(args.output, rows, args.encoder_csv)
    print(args.output)
    print(args.csv_output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
