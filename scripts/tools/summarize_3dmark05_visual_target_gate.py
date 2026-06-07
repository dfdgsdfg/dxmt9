#!/usr/bin/env python3
"""Summarize whether a 3DMark05 visual target is ready for gputrace spend."""

from __future__ import annotations

import argparse
import csv
from collections import Counter
from pathlib import Path


def load_csv(path: Path | None) -> list[dict[str, str]]:
    if path is None:
        return []
    if not path.exists():
        raise SystemExit(f"missing CSV: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def as_float(value: str | None) -> float:
    try:
        return float(value or 0.0)
    except ValueError:
        return 0.0


def texture_counter(rows: list[dict[str, str]]) -> Counter[str]:
    return Counter(row.get("texture0", "") or "(none)" for row in rows)


def frame_counter(rows: list[dict[str, str]]) -> Counter[str]:
    return Counter(row.get("frame", "") or row.get("seq", "") or "(none)" for row in rows)


def filtered_source_rows(rows: list[dict[str, str]], source_textures: set[str]) -> list[dict[str, str]]:
    if not source_textures:
        return rows
    return [row for row in rows if row.get("texture0", "") in source_textures]


def best_rows(rows: list[dict[str, str]], limit: int) -> list[dict[str, str]]:
    return sorted(
        rows,
        key=lambda row: (
            as_float(row.get("max_bbox_coverage_pct") or row.get("bbox_coverage_pct")),
            as_float(row.get("max_roi_coverage_pct") or row.get("roi_coverage_pct")),
            as_float(row.get("intersection_area")),
        ),
        reverse=True,
    )[:limit]


def decide_gate(
    component_rows: list[dict[str, str]],
    local_overlap_rows: list[dict[str, str]],
    source_queue_rows: list[dict[str, str]],
    source_textures: set[str],
) -> dict[str, str]:
    source_queue = filtered_source_rows(source_queue_rows, source_textures)
    local_source = filtered_source_rows(local_overlap_rows, source_textures)
    if source_queue:
        return {
            "gate": "visual-target-gputrace",
            "verdict": "promote-candidate",
            "evidence": f"{len(source_queue)} source queue candidate(s) survive local coverage gates",
            "next_action": "run the queued same-frame probe or schedule a targeted gputrace/Xcode replay",
        }
    if local_source:
        return {
            "gate": "visual-target-gputrace",
            "verdict": "blocked-no-forcewhite-queue",
            "evidence": (
                f"{len(local_source)} source overlap row(s), but no draw-local force-white queue "
                "candidate survived the stricter source gate"
            ),
            "next_action": "tighten bbox/command targeting before Xcode; do not spend gputrace on broad overlap alone",
        }
    if local_overlap_rows:
        textures = ", ".join(
            f"{texture}:{count}" for texture, count in texture_counter(local_overlap_rows).most_common(4)
        )
        return {
            "gate": "visual-target-gputrace",
            "verdict": "blocked-local-non-source",
            "evidence": f"{len(local_overlap_rows)} local overlap row(s), but only non-source textures remain ({textures})",
            "next_action": "do not spend gputrace on this visual target; find a frame with a local source-texture writer",
        }
    if component_rows:
        frames = ", ".join(f"{frame}:{count}" for frame, count in frame_counter(component_rows).most_common(4))
        return {
            "gate": "visual-target-gputrace",
            "verdict": "blocked-components-no-local-writer",
            "evidence": f"{len(component_rows)} component row(s), but no local effect-geometry writer survived ({frames})",
            "next_action": "inspect component montage or run same-run effect geometry before Xcode",
        }
    return {
        "gate": "visual-target-gputrace",
        "verdict": "blocked-no-visual-oracle",
        "evidence": "no component rows were supplied",
        "next_action": "capture a new frame/window and run the component scanner before Xcode",
    }


def write_csv(path: Path, gate: dict[str, str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=("gate", "verdict", "evidence", "next_action"))
        writer.writeheader()
        writer.writerow(gate)


def write_markdown(
    path: Path,
    gate: dict[str, str],
    component_rows: list[dict[str, str]],
    local_overlap_rows: list[dict[str, str]],
    source_queue_rows: list[dict[str, str]],
    source_textures: set[str],
    limit: int,
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    source_queue = filtered_source_rows(source_queue_rows, source_textures)
    local_source = filtered_source_rows(local_overlap_rows, source_textures)
    lines = [
        "# 3DMark05 Visual Target Gate",
        "",
        "This gate decides whether a visual target is ready for draw-local",
        "force-white or Xcode/gputrace spend. It is intentionally stricter than",
        "plain ROI overlap: source textures must survive local bbox coverage and",
        "queue construction.",
        "",
        "## Verdict",
        "",
        "| Gate | Verdict | Evidence | Next action |",
        "|---|---|---|---|",
        f"| `{gate['gate']}` | `{gate['verdict']}` | {gate['evidence']} | {gate['next_action']} |",
        "",
        "## Counts",
        "",
        "| Metric | Value |",
        "|---|---:|",
        f"| Component rows | `{len(component_rows)}` |",
        f"| Local overlap rows | `{len(local_overlap_rows)}` |",
        f"| Local source overlap rows | `{len(local_source)}` |",
        f"| Source queue rows | `{len(source_queue)}` |",
        "",
        "Source textures:",
        "",
    ]
    if source_textures:
        lines.extend(f"- `{texture}`" for texture in sorted(source_textures))
    else:
        lines.append("- `(all textures)`")

    if local_overlap_rows:
        lines.extend([
            "",
            "Local overlap texture counts:",
            "",
        ])
        lines.extend(
            f"- `{texture}`: `{count}`" for texture, count in texture_counter(local_overlap_rows).most_common(8)
        )

    if source_queue:
        lines.extend([
            "",
            "## Source Queue Candidates",
            "",
            "| Rank | ROI | Texture | Seq | Command | Max ROI Coverage % | Max BBox Coverage % |",
            "|---:|---|---|---:|---:|---:|---:|",
        ])
        for rank, row in enumerate(best_rows(source_queue, limit), start=1):
            lines.append(
                "| {rank} | {roi} | `{texture}` | `{seq}` | `{command}` | `{roi_pct}` | `{bbox_pct}` |".format(
                    rank=rank,
                    roi=row.get("roi", ""),
                    texture=row.get("texture0", ""),
                    seq=row.get("seq", ""),
                    command=row.get("command_index", ""),
                    roi_pct=row.get("max_roi_coverage_pct", ""),
                    bbox_pct=row.get("max_bbox_coverage_pct", ""),
                )
            )
    elif local_overlap_rows:
        lines.extend([
            "",
            "## Top Local Non-Promoted Rows",
            "",
            "| Rank | ROI | Texture | Seq | Command | ROI Coverage % | BBox Coverage % |",
            "|---:|---|---|---:|---:|---:|---:|",
        ])
        for rank, row in enumerate(best_rows(local_overlap_rows, limit), start=1):
            lines.append(
                "| {rank} | {roi} | `{texture}` | `{seq}` | `{command}` | `{roi_pct}` | `{bbox_pct}` |".format(
                    rank=rank,
                    roi=row.get("roi", ""),
                    texture=row.get("texture0", ""),
                    seq=row.get("seq", ""),
                    command=row.get("command_index", ""),
                    roi_pct=row.get("roi_coverage_pct", ""),
                    bbox_pct=row.get("bbox_coverage_pct", ""),
                )
            )

    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--component-csv", type=Path, help="summarize_capture_rois.py component CSV")
    parser.add_argument("--local-overlap-csv", type=Path, help="local filtered summarize_effect_geometry_roi.py CSV")
    parser.add_argument("--source-queue-csv", type=Path, help="plan_effect_roi_forcewhite_probes.py CSV")
    parser.add_argument("--source-texture", action="append", default=[], help="Expected source texture handle")
    parser.add_argument("--limit", type=int, default=8)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--csv-output", type=Path)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    source_textures = set(args.source_texture)
    component_rows = load_csv(args.component_csv)
    local_overlap_rows = load_csv(args.local_overlap_csv)
    source_queue_rows = load_csv(args.source_queue_csv)
    gate = decide_gate(component_rows, local_overlap_rows, source_queue_rows, source_textures)
    write_markdown(
        args.output,
        gate,
        component_rows,
        local_overlap_rows,
        source_queue_rows,
        source_textures,
        args.limit,
    )
    if args.csv_output is not None:
        write_csv(args.csv_output, gate)
    print(args.output)
    if args.csv_output is not None:
        print(args.csv_output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
