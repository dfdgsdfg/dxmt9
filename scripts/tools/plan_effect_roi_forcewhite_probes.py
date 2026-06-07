#!/usr/bin/env python3
"""Build draw-local force-white probe queues from effect ROI geometry CSVs."""

from __future__ import annotations

import argparse
import csv
import shlex
import sys
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path


@dataclass
class Candidate:
    roi: str
    texture0: str
    texture0_width: str
    texture0_height: str
    texture0_format: str
    seq: str
    encoder: str
    ordinal: str
    command_index: str
    command_draw_index: str
    command_draw_count: str
    encoder_draw_index: int
    probe_draw_index: int
    bbox_source: str
    src_blend: str
    dst_blend: str
    rows: int = 0
    complete_rows: int = 0
    primitive_count: int = 0
    intersection_area: float = 0.0
    max_roi_coverage_pct: float = 0.0
    max_bbox_coverage_pct: float = 0.0


def parse_float(row: dict[str, str], key: str, default: float = 0.0) -> float:
    try:
        return float(row.get(key, "") or default)
    except ValueError:
        return default


def parse_int(row: dict[str, str], key: str, default: int = 0) -> int:
    try:
        return int(row.get(key, "") or default, 0)
    except ValueError:
        return default


def include_row(
    row: dict[str, str],
    rois: set[str],
    textures: set[str],
    bbox_sources: set[str],
    min_roi_coverage_pct: float,
    min_bbox_coverage_pct: float,
    min_intersection_area: float,
) -> bool:
    if rois and row.get("roi", "") not in rois:
        return False
    if textures and row.get("texture0", "") not in textures:
        return False
    if bbox_sources and row.get("bbox_source", "") not in bbox_sources:
        return False
    if parse_float(row, "roi_coverage_pct") < min_roi_coverage_pct:
        return False
    if parse_float(row, "bbox_coverage_pct") < min_bbox_coverage_pct:
        return False
    if parse_float(row, "intersection_area") < min_intersection_area:
        return False
    return True


def load_candidates(
    paths: list[Path],
    rois: set[str],
    textures: set[str],
    bbox_sources: set[str],
    min_roi_coverage_pct: float,
    min_bbox_coverage_pct: float,
    min_intersection_area: float,
) -> list[Candidate]:
    buckets: dict[tuple[str, str, str, str, str, str, str, int, str], Candidate] = {}
    for path in paths:
        with path.open(newline="", encoding="utf-8") as handle:
            for row in csv.DictReader(handle):
                if not include_row(
                    row,
                    rois,
                    textures,
                    bbox_sources,
                    min_roi_coverage_pct,
                    min_bbox_coverage_pct,
                    min_intersection_area,
                ):
                    continue
                encoder_draw_index = parse_int(row, "encoder_draw_index")
                probe_draw_index = max(0, encoder_draw_index - 1)
                command_index = row.get("command_index", "")
                command_draw_index = row.get("command_draw_index", "")
                ordinal_key = "" if command_index else row.get("ordinal", "")
                key = (
                    row.get("roi", ""),
                    row.get("texture0", ""),
                    row.get("seq", ""),
                    row.get("encoder", ""),
                    ordinal_key,
                    command_index,
                    command_draw_index,
                    encoder_draw_index,
                    row.get("bbox_source", ""),
                )
                candidate = buckets.get(key)
                if candidate is None:
                    candidate = Candidate(
                        roi=row.get("roi", ""),
                        texture0=row.get("texture0", ""),
                        texture0_width=row.get("texture0_width", ""),
                        texture0_height=row.get("texture0_height", ""),
                        texture0_format=row.get("texture0_format", ""),
                        seq=row.get("seq", ""),
                        encoder=row.get("encoder", ""),
                        ordinal=row.get("ordinal", ""),
                        command_index=command_index,
                        command_draw_index=command_draw_index,
                        command_draw_count=row.get("command_draw_count", ""),
                        encoder_draw_index=encoder_draw_index,
                        probe_draw_index=probe_draw_index,
                        bbox_source=row.get("bbox_source", ""),
                        src_blend=row.get("src_blend", ""),
                        dst_blend=row.get("dst_blend", ""),
                    )
                    buckets[key] = candidate
                candidate.rows += 1
                candidate.complete_rows += 1 if parse_int(row, "complete") else 0
                candidate.primitive_count += parse_int(row, "primitive_count")
                candidate.intersection_area += parse_float(row, "intersection_area")
                candidate.max_roi_coverage_pct = max(
                    candidate.max_roi_coverage_pct,
                    parse_float(row, "roi_coverage_pct"),
                )
                candidate.max_bbox_coverage_pct = max(
                    candidate.max_bbox_coverage_pct,
                    parse_float(row, "bbox_coverage_pct"),
                )
    candidates = list(buckets.values())
    candidates.sort(
        key=lambda item: (
            item.roi,
            -item.intersection_area,
            -item.primitive_count,
            item.texture0,
            int(item.seq or 0),
            int(item.encoder or 0),
            int(item.ordinal or 0),
            int(item.command_index or 0),
            int(item.command_draw_index or 0),
            item.encoder_draw_index,
        )
    )
    return candidates


def suffix_for_candidate(prefix: str, rank: int, candidate: Candidate) -> str:
    texture = candidate.texture0.lower().replace("0x2000001000000", "tex")
    texture = texture.replace("0x", "tex")
    return (
        f"{prefix}-r{rank:02d}-{candidate.roi}-{texture}-"
        f"s{candidate.seq}-e{candidate.encoder}-d{candidate.encoder_draw_index}"
        f"-ci{candidate.command_index or 'na'}"
    )


def wrapper_args(candidate: Candidate, suffix: str, capture_range: str) -> list[str]:
    args = [
        "scripts/tools/run_3dmark05_perf_probe.sh",
        "--suffix",
        suffix,
        "--no-gputrace",
        "--encoder-breakdown-seq",
        candidate.seq,
        "--timeout",
        "180",
    ]
    if capture_range:
        args.extend(["--capture-range", capture_range])
    args.extend(
        [
            "--probe-force-texture-white-row",
            f"{candidate.seq}/{candidate.encoder}",
            "--probe-force-texture-white-texture0",
            candidate.texture0,
        ]
    )
    if candidate.command_draw_index:
        args.extend(
            [
                "--probe-force-texture-white-command-draw-index",
                candidate.command_draw_index,
            ]
        )
    else:
        args.extend(
            [
                "--probe-indexed-triangle-encoder-draw-min",
                str(candidate.probe_draw_index),
                "--probe-indexed-triangle-encoder-draw-max",
                str(candidate.probe_draw_index),
            ]
        )
    if candidate.command_index:
        args.extend(["--probe-force-texture-white-command-index", candidate.command_index])
    elif candidate.ordinal:
        args.extend(["--probe-force-texture-white-draw-ordinal", candidate.ordinal])
    if candidate.texture0_width:
        args.extend(["--probe-force-texture-white-texture0-width", candidate.texture0_width])
    if candidate.texture0_height:
        args.extend(["--probe-force-texture-white-texture0-height", candidate.texture0_height])
    if candidate.texture0_format:
        args.extend(["--probe-force-texture-white-texture0-format", candidate.texture0_format])
    return args


def shell_join(args: list[str]) -> str:
    return " ".join(shlex.quote(arg) for arg in args)


def write_csv(path: Path, candidates: list[Candidate], suffix_prefix: str, capture_range: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    columns = [
        "rank",
        "roi",
        "texture0",
        "texture0_width",
        "texture0_height",
        "texture0_format",
        "seq",
        "encoder",
        "ordinal",
        "command_index",
        "command_draw_index",
        "command_draw_count",
        "encoder_draw_index",
        "probe_draw_index",
        "bbox_source",
        "blend",
        "rows",
        "complete_rows",
        "primitive_count",
        "intersection_area",
        "max_roi_coverage_pct",
        "max_bbox_coverage_pct",
        "suffix",
        "command",
    ]
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=columns)
        writer.writeheader()
        for rank, candidate in enumerate(candidates, start=1):
            suffix = suffix_for_candidate(suffix_prefix, rank, candidate)
            writer.writerow(
                {
                    "rank": rank,
                    "roi": candidate.roi,
                    "texture0": candidate.texture0,
                    "texture0_width": candidate.texture0_width,
                    "texture0_height": candidate.texture0_height,
                    "texture0_format": candidate.texture0_format,
                    "seq": candidate.seq,
                    "encoder": candidate.encoder,
                    "ordinal": candidate.ordinal,
                    "command_index": candidate.command_index,
                    "command_draw_index": candidate.command_draw_index,
                    "command_draw_count": candidate.command_draw_count,
                    "encoder_draw_index": candidate.encoder_draw_index,
                    "probe_draw_index": candidate.probe_draw_index,
                    "bbox_source": candidate.bbox_source,
                    "blend": f"{candidate.src_blend}->{candidate.dst_blend}",
                    "rows": candidate.rows,
                    "complete_rows": candidate.complete_rows,
                    "primitive_count": candidate.primitive_count,
                    "intersection_area": f"{candidate.intersection_area:.3f}",
                    "max_roi_coverage_pct": f"{candidate.max_roi_coverage_pct:.3f}",
                    "max_bbox_coverage_pct": f"{candidate.max_bbox_coverage_pct:.3f}",
                    "suffix": suffix,
                    "command": shell_join(wrapper_args(candidate, suffix, capture_range)),
                }
            )


def write_markdown(
    path: Path,
    inputs: list[Path],
    candidates: list[Candidate],
    suffix_prefix: str,
    capture_range: str,
    limit: int,
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "# Effect ROI Force-White Probe Queue",
        "",
        "Inputs:",
    ]
    lines.extend(f"- `{input_path}`" for input_path in inputs)
    lines.extend(
        [
            "",
            "Important index convention: `dxmt9-effect-geometry` logs "
            "`encoder_draw_index` as the next 1-based draw number, while "
            "`DXMT9_PROBE_INDEXED_TRIANGLE_ENCODER_DRAW_MIN/MAX` matches the "
            "pre-increment 0-based `stats.drawCalls` value. This queue therefore "
            "uses `probe_draw_index = encoder_draw_index - 1` only when a "
            "command-local subdraw index is unavailable. If `command_index` and "
            "`command_draw_index` columns are present, commands prefer "
            "`--probe-force-texture-white-command-index` plus "
            "`--probe-force-texture-white-command-draw-index` because that is "
            "the narrowest available selector. Independent replays can still "
            "move the frame or command namespace, so every queued command must "
            "prove it hit with nonzero `probe_force_texture_white_draws` before "
            "any image delta is treated as evidence. If only `ordinal` is "
            "present, commands fall back to "
            "`--probe-force-texture-white-draw-ordinal`. Commands also include "
            "`--encoder-breakdown-seq <seq>` for scoped diagnostics.",
            "",
            "| Rank | ROI | Texture | Seq/Encoder | Command Index | Cmd Draw | Ordinal | Geometry Draw | Probe Draw | Source | Blend | Rows | Primitives | Intersection Area | Max ROI Coverage % | Max BBox Coverage % | Command |",
            "|---:|---|---|---|---:|---:|---:|---:|---:|---|---|---:|---:|---:|---:|---:|---|",
        ]
    )
    for rank, candidate in enumerate(candidates[:limit], start=1):
        suffix = suffix_for_candidate(suffix_prefix, rank, candidate)
        command = shell_join(wrapper_args(candidate, suffix, capture_range))
        command_draw = (
            f"{candidate.command_draw_index}/{candidate.command_draw_count}"
            if candidate.command_draw_index or candidate.command_draw_count
            else ""
        )
        lines.append(
            "| {rank} | {roi} | `{texture}` | `{seq}/{encoder}` | `{command_index}` | "
            "`{command_draw}` | `{ordinal}` | `{geom_draw}` | `{probe_draw}` | {source} | `{blend}` | `{rows}` | `{prims}` | "
            "`{area:.3f}` | `{roi_pct:.3f}` | `{bbox_pct:.3f}` | `{command}` |".format(
                rank=rank,
                roi=candidate.roi,
                texture=candidate.texture0,
                seq=candidate.seq,
                encoder=candidate.encoder,
                command_index=candidate.command_index or "",
                command_draw=command_draw,
                ordinal=candidate.ordinal or "",
                geom_draw=candidate.encoder_draw_index,
                probe_draw=candidate.probe_draw_index,
                source=candidate.bbox_source,
                blend=f"{candidate.src_blend}->{candidate.dst_blend}",
                rows=candidate.rows,
                prims=candidate.primitive_count,
                area=candidate.intersection_area,
                roi_pct=candidate.max_roi_coverage_pct,
                bbox_pct=candidate.max_bbox_coverage_pct,
                command=command.replace("|", "\\|"),
            )
        )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Create draw-local force-white probe queues from ROI geometry CSVs.",
    )
    parser.add_argument("csv", nargs="+", type=Path, help="ROI geometry CSV inputs")
    parser.add_argument("--roi", action="append", default=[], help="ROI name to include")
    parser.add_argument("--texture0", action="append", default=[], help="Texture handle to include")
    parser.add_argument(
        "--bbox-source",
        action="append",
        default=[],
        help="BBox source to include, e.g. projected-screen or screen-space-pos",
    )
    parser.add_argument("--min-roi-coverage-pct", type=float, default=0.0)
    parser.add_argument(
        "--min-bbox-coverage-pct",
        type=float,
        default=0.0,
        help=(
            "Require the ROI intersection to cover at least this percent of "
            "the geometry bbox. Use this to reject huge projected/fullscreen "
            "quads that only broadly contain a small visual component."
        ),
    )
    parser.add_argument("--min-intersection-area", type=float, default=0.0)
    parser.add_argument("--suffix-prefix", default="effect-roi-forcewhite")
    parser.add_argument("--capture-range", default="820:900:20")
    parser.add_argument("--limit", type=int, default=20)
    parser.add_argument("--output", type=Path, help="Markdown queue output")
    parser.add_argument("--csv-output", type=Path, help="CSV queue output")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_arg_parser().parse_args(argv)
    candidates = load_candidates(
        args.csv,
        set(args.roi),
        set(args.texture0),
        set(args.bbox_source),
        args.min_roi_coverage_pct,
        args.min_bbox_coverage_pct,
        args.min_intersection_area,
    )[: args.limit]
    if args.csv_output:
        write_csv(args.csv_output, candidates, args.suffix_prefix, args.capture_range)
    if args.output:
        write_markdown(args.output, args.csv, candidates, args.suffix_prefix, args.capture_range, args.limit)
    if not args.output and not args.csv_output:
        write_markdown(Path("/dev/stdout"), args.csv, candidates, args.suffix_prefix, args.capture_range, args.limit)
    return 0


if __name__ == "__main__":
    sys.exit(main())
