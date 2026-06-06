#!/usr/bin/env python3
"""Plan reduced A/B gates for backend escape candidates before GT1 Xcode spend."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path


CSV_FIELDS = [
    "candidate",
    "audit_verdict",
    "surface_status",
    "reduced_ab_status",
    "control",
    "treatment",
    "route_gate",
    "equality_gate",
    "counter_gate",
    "gt1_promotion_gate",
    "expansion_status",
    "expansion_evidence",
    "reason",
    "next_action",
]


READY_AUDIT_VERDICTS = {"candidate-route-present", "candidate-coverage"}


def load_csv(path: Path) -> list[dict[str, str]]:
    if not path.exists():
        raise SystemExit(f"missing CSV: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def candidate_spec(candidate: str) -> dict[str, str]:
    specs: dict[str, dict[str, str]] = {
        "mesh-object": {
            "control": "ordinary render mini-replay or synthetic draw with identical geometry/state",
            "treatment": "mesh/object route emitting the same primitives and visible outputs",
            "route_gate": "dxmt9 route and mesh/object shader emitter exist",
            "equality_gate": "same-input color equality plus primitive-owner/final-writer stability",
            "counter_gate": "VS buffer bytes or bytes/invocation decreases with draw/primitive/invocation shape held stable",
            "gt1_promotion_gate": "attach reduced equality and counter A/B before any GT1 Xcode capture",
        },
        "position-binning": {
            "control": "ordinary render route for the same row-local geometry/state",
            "treatment": "real position/binning/depth route, not source-visible VSOut trimming",
            "route_gate": "separate binning/depth/position route token exists in dxmt9",
            "equality_gate": "depth/color semantics stay stable for the reduced scene",
            "counter_gate": "hidden backend bytes/invocation or tiler/primitive storage decreases while VS invocations are stable",
            "gt1_promotion_gate": "prove bytes/invocation movement in reduced A/B before GT1 row capture",
        },
        "tile-ffp": {
            "control": "portable FFP render path under DXMT9_TILE_FFP=off",
            "treatment": "tile FFP base-colour draw plus tile post-pass under DXMT9_TILE_FFP=auto/force",
            "route_gate": "eligible hot-row primitive coverage is large enough",
            "equality_gate": "portable-vs-tile image equality for eligible draws",
            "counter_gate": "tile route reduces the relevant pass/backend bytes or GPU time at fixed coverage",
            "gt1_promotion_gate": "only promote when eligible primitives cover a meaningful GT1 hot-row share",
        },
    }
    return specs.get(candidate, {
        "control": "ordinary current route",
        "treatment": "candidate backend route",
        "route_gate": "candidate route exists",
        "equality_gate": "same-input visual and semantic equality",
        "counter_gate": "backend counter movement at stable draw/geometry shape",
        "gt1_promotion_gate": "attach reduced proof before GT1 Xcode",
    })


def tile_expansion_summary(rows: list[dict[str, str]]) -> tuple[str, str]:
    if not rows:
        return "", ""
    counts: dict[str, int] = {}
    top: list[str] = []
    for row in rows:
        verdict = row.get("verdict", "")
        counts[verdict] = counts.get(verdict, 0) + 1
        if len(top) < 3:
            top.append(
                f"{row.get('row', '')}:{verdict}/{row.get('dominant_blocker', '')}"
            )
    if counts.get("needs-programmable-tile-route"):
        return (
            "needs-programmable-tile-route",
            f"{counts['needs-programmable-tile-route']} row(s); top {'; '.join(top)}",
        )
    if counts.get("needs-unsupported-state-expansion"):
        return (
            "needs-unsupported-state-expansion",
            f"{counts['needs-unsupported-state-expansion']} row(s); top {'; '.join(top)}",
        )
    if counts.get("current-tile-ffp-coverage"):
        return (
            "current-tile-ffp-coverage",
            f"{counts['current-tile-ffp-coverage']} row(s); top {'; '.join(top)}",
        )
    return "no-meaningful-expansion", f"top {'; '.join(top)}"


def classify(row: dict[str, str]) -> tuple[str, str, str, str]:
    verdict = row.get("verdict", "")
    candidate = row.get("candidate", "")
    if verdict in READY_AUDIT_VERDICTS:
        return (
            "route-or-coverage-present",
            "ready-reduced-ab",
            f"{candidate} has enough route/coverage surface for a reduced A/B gate",
            "run the reduced equality gate, then compare Xcode counters on the reduced scene before GT1 promotion",
        )
    if verdict == "bridge-only-reduced-ab-required":
        return (
            "bridge-only",
            "blocked-missing-dxmt9-route",
            "winemetal bridge symbols exist, but dxmt9 has no route/emitter for this candidate",
            "implement a dxmt9 route/emitter or build an out-of-GT1 synthetic A/B before claiming backend escape",
        )
    if verdict == "visible-vsout-probe-only":
        return (
            "visible-probe-only",
            "blocked-real-route-missing",
            "the current probe changes source-visible VSOut only; Xcode already rejected visible width as the owner",
            "define a real binning/depth/position route before another Xcode capture",
        )
    if verdict == "rejected-current-coverage":
        return (
            "route-present-coverage-rejected",
            "blocked-hot-row-coverage",
            "the route exists, but current GT1 hot rows do not have useful eligible coverage",
            "expand eligibility into hot programmable/textured rows or keep this as a narrow architecture experiment",
        )
    if verdict in {"missing", "missing-bridge-surface"}:
        return (
            "missing-surface",
            "blocked-missing-surface",
            "required bridge or route surface is missing",
            "do not schedule reduced A/B until the candidate surface exists",
        )
    return (
        "unknown",
        "blocked-unclassified-audit",
        f"unrecognized audit verdict `{verdict}`",
        "update the reduced A/B planner when this audit verdict becomes meaningful",
    )


def plan_rows(
    audit_rows: list[dict[str, str]],
    *,
    tile_expansion_rows: list[dict[str, str]] | None = None,
) -> list[dict[str, str]]:
    planned: list[dict[str, str]] = []
    tile_expansion_status, tile_expansion_evidence = tile_expansion_summary(
        tile_expansion_rows or []
    )
    for row in audit_rows:
        candidate = row.get("candidate", "")
        spec = candidate_spec(candidate)
        surface_status, reduced_ab_status, reason, next_action = classify(row)
        expansion_status = ""
        expansion_evidence = ""
        if candidate == "tile-ffp" and tile_expansion_status:
            expansion_status = tile_expansion_status
            expansion_evidence = tile_expansion_evidence
            if tile_expansion_status == "needs-programmable-tile-route":
                reason += "; expansion analysis says hot rows require a programmable/textured route"
                next_action = (
                    "do not treat current Tile-FFP widening as the GT1 path; "
                    "define a programmable/textured tile or mesh route before reduced A/B"
                )
            elif tile_expansion_status == "needs-unsupported-state-expansion":
                reason += "; expansion analysis points at unsupported-state coverage"
                next_action = (
                    "split the unsupported-state subset and prove portable-vs-tile equality before Xcode"
                )
        planned.append({
            "candidate": candidate,
            "audit_verdict": row.get("verdict", ""),
            "surface_status": surface_status,
            "reduced_ab_status": reduced_ab_status,
            "control": spec["control"],
            "treatment": spec["treatment"],
            "route_gate": spec["route_gate"],
            "equality_gate": spec["equality_gate"],
            "counter_gate": spec["counter_gate"],
            "gt1_promotion_gate": spec["gt1_promotion_gate"],
            "expansion_status": expansion_status,
            "expansion_evidence": expansion_evidence,
            "reason": reason,
            "next_action": next_action,
        })
    return planned


def overall_verdict(rows: list[dict[str, str]]) -> tuple[str, str]:
    if not rows:
        return (
            "missing-surface-audit",
            "no backend escape audit rows were provided",
        )
    ready = [row for row in rows if row["reduced_ab_status"] == "ready-reduced-ab"]
    if ready:
        labels = ", ".join(row["candidate"] for row in ready)
        return (
            "ready-reduced-ab",
            f"{labels} can enter reduced equality/counter A/B before GT1 Xcode",
        )
    return (
        "blocked-before-reduced-ab",
        "no backend escape candidate currently clears route/coverage preconditions",
    )


def write_csv(path: Path, rows: list[dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=CSV_FIELDS)
        writer.writeheader()
        writer.writerows(rows)


def md_cell(text: str) -> str:
    return text.replace("|", "\\|")


def write_markdown(path: Path, rows: list[dict[str, str]], source: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    verdict, reason = overall_verdict(rows)
    lines = [
        "# Backend Escape Reduced A/B Plan",
        "",
        f"- Source: `{source}`",
        f"- Overall verdict: `{verdict}`",
        f"- Reason: {reason}",
        "",
        "| Candidate | Audit verdict | Reduced A/B status | Expansion status | Route gate | Equality gate | Counter gate | Next action |",
        "|---|---|---|---|---|---|---|---|",
    ]
    for row in rows:
        lines.append(
            "| "
            + " | ".join(
                [
                    f"`{md_cell(row['candidate'])}`",
                    f"`{md_cell(row['audit_verdict'])}`",
                    f"`{md_cell(row['reduced_ab_status'])}`",
                    f"`{md_cell(row['expansion_status'])}`",
                    md_cell(row["route_gate"]),
                    md_cell(row["equality_gate"]),
                    md_cell(row["counter_gate"]),
                    md_cell(row["next_action"]),
                ]
            )
            + " |"
        )
    lines.extend([
        "",
        "```mermaid",
        "flowchart TD",
        "  Audit[backend escape surface audit] --> Surface{route or coverage present?}",
        "  Surface -- No --> Block[blocked before reduced A/B]",
        "  Surface -- Yes --> Eq[reduced same-input equality gate]",
        "  Eq --> Counters[reduced Xcode counter gate]",
        "  Counters --> GT1[GT1 Xcode promotion gate]",
        "  Block --> RouteWork[implement route or expand coverage]",
        "```",
        "",
    ])
    path.write_text("\n".join(lines), encoding="utf-8")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend-escape-surface-csv", type=Path, required=True)
    parser.add_argument("--tile-ffp-expansion-csv", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--csv-output", type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    rows = plan_rows(
        load_csv(args.backend_escape_surface_csv),
        tile_expansion_rows=load_csv(args.tile_ffp_expansion_csv)
        if args.tile_ffp_expansion_csv else None,
    )
    if args.csv_output:
        write_csv(args.csv_output, rows)
    if args.output:
        write_markdown(args.output, rows, args.backend_escape_surface_csv)
    if not args.output and not args.csv_output:
        verdict, reason = overall_verdict(rows)
        print(f"{verdict}: {reason}")
        for row in rows:
            print(f"{row['candidate']}: {row['reduced_ab_status']} - {row['reason']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
