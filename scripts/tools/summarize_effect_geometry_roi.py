#!/usr/bin/env python3
"""Summarize dxmt9 effect-geometry rows that overlap screen-space ROIs."""

from __future__ import annotations

import argparse
import csv
import re
import sys
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path


PAIR_RE = re.compile(r"([A-Za-z0-9_#]+)=((?:\([^)]*\))|(?:\S+))")


@dataclass(frozen=True)
class Roi:
    left: float
    top: float
    right: float
    bottom: float
    name: str
    seq: str | None = None

    @property
    def area(self) -> float:
        return max(0.0, self.right - self.left) * max(0.0, self.bottom - self.top)


@dataclass(frozen=True)
class BBox:
    left: float
    top: float
    right: float
    bottom: float
    source: str

    @property
    def area(self) -> float:
        return max(0.0, self.right - self.left) * max(0.0, self.bottom - self.top)


@dataclass(frozen=True)
class GeometryRow:
    source: Path
    line: int
    fields: dict[str, str]
    bbox: BBox | None


@dataclass(frozen=True)
class OverlapRow:
    geometry: GeometryRow
    roi: Roi
    intersection_area: float
    roi_coverage_pct: float
    bbox_coverage_pct: float


def parse_roi(spec: str) -> Roi:
    coords, sep, name = spec.partition(":")
    parts = coords.split(",")
    if len(parts) != 4:
        raise argparse.ArgumentTypeError("ROI must be L,T,R,B or L,T,R,B:name")
    try:
        left, top, right, bottom = (float(part) for part in parts)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("ROI coordinates must be numeric") from exc
    if left < 0 or top < 0 or right <= left or bottom <= top:
        raise argparse.ArgumentTypeError("ROI must have non-negative L/T and R>L, B>T")
    area_name = name.strip() if sep and name.strip() else f"roi-{left:g}-{top:g}-{right:g}-{bottom:g}"
    return Roi(left=left, top=top, right=right, bottom=bottom, name=area_name)


def component_rois_from_csv(paths: list[Path], limit: int, match_seq: bool) -> list[Roi]:
    rois: list[Roi] = []
    for path in paths:
        with path.open(newline="", encoding="utf-8") as handle:
            reader = csv.DictReader(handle)
            for row_no, row in enumerate(reader, start=2):
                bbox = (row.get("bbox") or "").strip()
                frame = (row.get("frame") or "").strip()
                component_id = (row.get("component_id") or str(row_no - 1)).strip()
                if not bbox:
                    raise ValueError(f"{path}:{row_no}: component row is missing bbox")
                if match_seq and not frame:
                    raise ValueError(f"{path}:{row_no}: --component-roi-match-seq requires frame")
                name = f"frame{frame}-component{component_id}" if frame else f"component{component_id}"
                try:
                    roi = parse_roi(f"{bbox}:{name}")
                except argparse.ArgumentTypeError as exc:
                    raise ValueError(f"{path}:{row_no}: invalid component bbox {bbox!r}: {exc}") from exc
                rois.append(
                    Roi(
                        left=roi.left,
                        top=roi.top,
                        right=roi.right,
                        bottom=roi.bottom,
                        name=roi.name,
                        seq=frame if match_seq else None,
                    )
                )
                if limit > 0 and len(rois) >= limit:
                    return rois
    return rois


def parse_geometry_fields(line: str) -> dict[str, str]:
    return {match.group(1): match.group(2) for match in PAIR_RE.finditer(line)}


def parse_tuple(value: str | None) -> tuple[float, ...] | None:
    if not value or not value.startswith("(") or not value.endswith(")"):
        return None
    parts = value[1:-1].split(",")
    try:
        return tuple(float(part) for part in parts)
    except ValueError:
        return None


def as_int(fields: dict[str, str], key: str, default: int = 0) -> int:
    value = fields.get(key)
    if value is None:
        return default
    try:
        return int(value, 0)
    except ValueError:
        return default


def has_screen_space_position(fields: dict[str, str]) -> bool:
    pos_min = parse_tuple(fields.get("pos_min"))
    pos_max = parse_tuple(fields.get("pos_max"))
    viewport_size = parse_tuple(fields.get("viewport_size"))
    if not pos_min or not pos_max or len(pos_min) < 2 or len(pos_max) < 2:
        return False
    if as_int(fields, "pretransformed") == 1:
        return True
    if pos_max[0] > 2.0 or pos_max[1] > 2.0 or pos_min[0] < -2.0 or pos_min[1] < -2.0:
        if viewport_size and len(viewport_size) >= 2:
            return (
                pos_min[0] >= -1.0
                and pos_min[1] >= -1.0
                and pos_max[0] <= viewport_size[0] + 1.0
                and pos_max[1] <= viewport_size[1] + 1.0
            )
        return True
    return False


def bbox_from_fields(fields: dict[str, str]) -> BBox | None:
    projected_refs = as_int(fields, "projected_refs")
    if projected_refs > 0:
        screen_min = parse_tuple(fields.get("screen_min"))
        screen_max = parse_tuple(fields.get("screen_max"))
        if screen_min and screen_max and len(screen_min) >= 2 and len(screen_max) >= 2:
            return BBox(
                left=min(screen_min[0], screen_max[0]),
                top=min(screen_min[1], screen_max[1]),
                right=max(screen_min[0], screen_max[0]),
                bottom=max(screen_min[1], screen_max[1]),
                source="projected-screen",
            )

    if has_screen_space_position(fields):
        pos_min = parse_tuple(fields.get("pos_min"))
        pos_max = parse_tuple(fields.get("pos_max"))
        if pos_min and pos_max and len(pos_min) >= 2 and len(pos_max) >= 2:
            return BBox(
                left=min(pos_min[0], pos_max[0]),
                top=min(pos_min[1], pos_max[1]),
                right=max(pos_min[0], pos_max[0]),
                bottom=max(pos_min[1], pos_max[1]),
                source="screen-space-pos",
            )
    return None


def read_geometry_rows(paths: list[Path]) -> list[GeometryRow]:
    rows: list[GeometryRow] = []
    for path in paths:
        with path.open(encoding="utf-8", errors="replace") as handle:
            for line_no, line in enumerate(handle, start=1):
                if "[dxmt9-effect-geometry" not in line:
                    continue
                fields = parse_geometry_fields(line)
                rows.append(
                    GeometryRow(
                        source=path,
                        line=line_no,
                        fields=fields,
                        bbox=bbox_from_fields(fields),
                    )
                )
    return rows


def intersect_area(bbox: BBox, roi: Roi) -> float:
    left = max(bbox.left, roi.left)
    top = max(bbox.top, roi.top)
    right = min(bbox.right, roi.right)
    bottom = min(bbox.bottom, roi.bottom)
    return max(0.0, right - left) * max(0.0, bottom - top)


def build_overlaps(
    rows: list[GeometryRow],
    rois: list[Roi],
    include_zero_overlap: bool,
    min_roi_coverage_pct: float = 0.0,
    min_bbox_coverage_pct: float = 0.0,
) -> list[OverlapRow]:
    overlaps: list[OverlapRow] = []
    for row in rows:
        if row.bbox is None:
            continue
        for roi in rois:
            if roi.seq is not None and field(row, "seq") != roi.seq:
                continue
            area = intersect_area(row.bbox, roi)
            if area <= 0.0 and not include_zero_overlap:
                continue
            roi_coverage_pct = (area / roi.area * 100.0) if roi.area else 0.0
            bbox_coverage_pct = (area / row.bbox.area * 100.0) if row.bbox.area else 0.0
            if roi_coverage_pct < min_roi_coverage_pct:
                continue
            if bbox_coverage_pct < min_bbox_coverage_pct:
                continue
            overlaps.append(
                OverlapRow(
                    geometry=row,
                    roi=roi,
                    intersection_area=area,
                    roi_coverage_pct=roi_coverage_pct,
                    bbox_coverage_pct=bbox_coverage_pct,
                )
            )
    overlaps.sort(
        key=lambda item: (
            item.roi.name,
            -item.intersection_area,
            item.geometry.fields.get("texture0", ""),
            as_int(item.geometry.fields, "seq"),
            as_int(item.geometry.fields, "encoder"),
            as_int(item.geometry.fields, "encoder_draw_index"),
            item.geometry.line,
        )
    )
    return overlaps


def format_float(value: float) -> str:
    return f"{value:.3f}"


def field(row: GeometryRow, key: str, default: str = "") -> str:
    return row.fields.get(key, default)


def write_csv(path: Path, overlaps: list[OverlapRow]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    columns = [
        "roi",
        "roi_seq",
        "source",
        "line",
        "seq",
        "encoder",
        "encoder_draw_index",
        "ordinal",
        "command_index",
        "command_draw_index",
        "command_draw_count",
        "texture0",
        "texture0_width",
        "texture0_height",
        "texture0_format",
        "primitive_count",
        "index_count",
        "complete",
        "src_blend",
        "dst_blend",
        "depth_enabled",
        "depth_write",
        "vs_hash",
        "ps_hash",
        "bbox_source",
        "bbox_left",
        "bbox_top",
        "bbox_right",
        "bbox_bottom",
        "bbox_area",
        "intersection_area",
        "roi_coverage_pct",
        "bbox_coverage_pct",
    ]
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=columns)
        writer.writeheader()
        for item in overlaps:
            row = item.geometry
            assert row.bbox is not None
            writer.writerow(
                {
                    "roi": item.roi.name,
                    "roi_seq": item.roi.seq or "",
                    "source": str(row.source),
                    "line": row.line,
                    "seq": field(row, "seq"),
                    "encoder": field(row, "encoder"),
                    "encoder_draw_index": field(row, "encoder_draw_index"),
                    "ordinal": field(row, "ordinal"),
                    "command_index": field(row, "command_index"),
                    "command_draw_index": field(row, "command_draw_index"),
                    "command_draw_count": field(row, "command_draw_count"),
                    "texture0": field(row, "texture0"),
                    "texture0_width": field(row, "texture0_width"),
                    "texture0_height": field(row, "texture0_height"),
                    "texture0_format": field(row, "texture0_format"),
                    "primitive_count": field(row, "primitive_count"),
                    "index_count": field(row, "index_count"),
                    "complete": field(row, "complete"),
                    "src_blend": field(row, "src_blend"),
                    "dst_blend": field(row, "dst_blend"),
                    "depth_enabled": field(row, "depth_enabled"),
                    "depth_write": field(row, "depth_write"),
                    "vs_hash": field(row, "vs_hash"),
                    "ps_hash": field(row, "ps_hash"),
                    "bbox_source": row.bbox.source,
                    "bbox_left": format_float(row.bbox.left),
                    "bbox_top": format_float(row.bbox.top),
                    "bbox_right": format_float(row.bbox.right),
                    "bbox_bottom": format_float(row.bbox.bottom),
                    "bbox_area": format_float(row.bbox.area),
                    "intersection_area": format_float(item.intersection_area),
                    "roi_coverage_pct": format_float(item.roi_coverage_pct),
                    "bbox_coverage_pct": format_float(item.bbox_coverage_pct),
                }
            )


def aggregate_overlaps(overlaps: list[OverlapRow]) -> list[dict[str, object]]:
    buckets: dict[tuple[str, str, str, str, str], dict[str, object]] = {}
    for item in overlaps:
        row = item.geometry
        assert row.bbox is not None
        key = (
            item.roi.name,
            field(row, "texture0", "(none)"),
            row.bbox.source,
            field(row, "src_blend"),
            field(row, "dst_blend"),
        )
        bucket = buckets.setdefault(
            key,
            {
                "roi": item.roi.name,
                "texture0": field(row, "texture0", "(none)"),
                "bbox_source": row.bbox.source,
                "src_blend": field(row, "src_blend"),
                "dst_blend": field(row, "dst_blend"),
                "rows": 0,
                "complete_rows": 0,
                "primitive_count": 0,
                "intersection_area": 0.0,
                "max_roi_coverage_pct": 0.0,
                "max_bbox_coverage_pct": 0.0,
                "seqs": set(),
                "draws": set(),
            },
        )
        bucket["rows"] = int(bucket["rows"]) + 1
        bucket["complete_rows"] = int(bucket["complete_rows"]) + (1 if as_int(row.fields, "complete") else 0)
        bucket["primitive_count"] = int(bucket["primitive_count"]) + as_int(row.fields, "primitive_count")
        bucket["intersection_area"] = float(bucket["intersection_area"]) + item.intersection_area
        bucket["max_roi_coverage_pct"] = max(float(bucket["max_roi_coverage_pct"]), item.roi_coverage_pct)
        bucket["max_bbox_coverage_pct"] = max(float(bucket["max_bbox_coverage_pct"]), item.bbox_coverage_pct)
        bucket["seqs"].add(field(row, "seq"))
        bucket["draws"].add(f"{field(row, 'seq')}/{field(row, 'encoder')}/{field(row, 'encoder_draw_index')}")

    result = list(buckets.values())
    result.sort(
        key=lambda bucket: (
            str(bucket["roi"]),
            -float(bucket["intersection_area"]),
            -int(bucket["primitive_count"]),
            str(bucket["texture0"]),
        )
    )
    return result


def write_markdown(
    path: Path,
    inputs: list[Path],
    rows: list[GeometryRow],
    rois: list[Roi],
    overlaps: list[OverlapRow],
    limit: int,
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    bbox_rows = [row for row in rows if row.bbox is not None]
    aggregate = aggregate_overlaps(overlaps)
    lines = [
        "# Effect Geometry ROI Summary",
        "",
        "Inputs:",
    ]
    lines.extend(f"- `{path}`" for path in inputs)
    lines.extend(
        [
            "",
            "ROIs:",
        ]
    )
    lines.extend(
        f"- `{roi.name}`: `{roi.left:g},{roi.top:g},{roi.right:g},{roi.bottom:g}`"
        + (f" (seq `{roi.seq}`)" if roi.seq is not None else "")
        for roi in rois
    )
    lines.extend(
        [
            "",
            "Totals:",
            "",
            "| Metric | Value |",
            "|---|---:|",
            f"| Geometry rows | `{len(rows)}` |",
            f"| Rows with screen bbox | `{len(bbox_rows)}` |",
            f"| Overlap rows | `{len(overlaps)}` |",
            "",
            "## Aggregate",
            "",
            "| ROI | Texture | BBox Source | Blend | Rows | Complete | Primitives | Intersection Area | Max ROI Coverage % | Max BBox Coverage % | Distinct Seq |",
            "|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|",
        ]
    )
    for bucket in aggregate[:limit]:
        lines.append(
            "| {roi} | `{texture0}` | {bbox_source} | `{src_blend}->{dst_blend}` | "
            "`{rows}` | `{complete_rows}` | `{primitive_count}` | `{intersection_area}` | "
            "`{max_roi_coverage_pct}` | `{max_bbox_coverage_pct}` | `{seq_count}` |".format(
                roi=bucket["roi"],
                texture0=bucket["texture0"],
                bbox_source=bucket["bbox_source"],
                src_blend=bucket["src_blend"],
                dst_blend=bucket["dst_blend"],
                rows=bucket["rows"],
                complete_rows=bucket["complete_rows"],
                primitive_count=bucket["primitive_count"],
                intersection_area=format_float(float(bucket["intersection_area"])),
                max_roi_coverage_pct=format_float(float(bucket["max_roi_coverage_pct"])),
                max_bbox_coverage_pct=format_float(float(bucket["max_bbox_coverage_pct"])),
                seq_count=len(bucket["seqs"]),
            )
        )

    lines.extend(
        [
            "",
            "## Top Overlaps",
            "",
            "| ROI | Seq/Encoder/Draw | Texture | Primitives | BBox Source | BBox | Intersection Area | ROI Coverage % | BBox Coverage % |",
            "|---|---|---|---:|---|---|---:|---:|---:|",
        ]
    )
    for item in overlaps[:limit]:
        row = item.geometry
        assert row.bbox is not None
        lines.append(
            "| {roi} | `{seq}/{encoder}/{draw}` | `{texture}` | `{prims}` | {source} | "
            "`{left},{top},{right},{bottom}` | `{area}` | `{roi_pct}` | `{bbox_pct}` |".format(
                roi=item.roi.name,
                seq=field(row, "seq"),
                encoder=field(row, "encoder"),
                draw=field(row, "encoder_draw_index"),
                texture=field(row, "texture0"),
                prims=field(row, "primitive_count"),
                source=row.bbox.source,
                left=format_float(row.bbox.left),
                top=format_float(row.bbox.top),
                right=format_float(row.bbox.right),
                bottom=format_float(row.bbox.bottom),
                area=format_float(item.intersection_area),
                roi_pct=format_float(item.roi_coverage_pct),
                bbox_pct=format_float(item.bbox_coverage_pct),
            )
        )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Join dxmt9 effect-geometry projected/screen-space boxes to ROIs.",
    )
    parser.add_argument("logs", nargs="+", type=Path, help="effect-geometry log files")
    parser.add_argument(
        "--roi",
        action="append",
        type=parse_roi,
        help="Screen region L,T,R,B or L,T,R,B:name. Repeatable.",
    )
    parser.add_argument(
        "--component-roi-csv",
        action="append",
        type=Path,
        help=(
            "Read component bboxes from summarize_capture_rois.py CSV as ROIs. "
            "Repeatable."
        ),
    )
    parser.add_argument(
        "--component-roi-limit",
        type=int,
        default=0,
        help="Maximum component CSV rows to convert to ROIs. 0 means all rows.",
    )
    parser.add_argument(
        "--component-roi-match-seq",
        action="store_true",
        help="Only compare component ROIs to geometry rows with the same seq/frame.",
    )
    parser.add_argument("--output", type=Path, help="Markdown report output")
    parser.add_argument("--csv-output", type=Path, help="Detailed overlap CSV output")
    parser.add_argument(
        "--include-zero-overlap",
        action="store_true",
        help="Include rows with a screen bbox that do not overlap a ROI in the CSV/report.",
    )
    parser.add_argument(
        "--min-roi-coverage-pct",
        type=float,
        default=0.0,
        help="Drop rows whose intersection covers less than this percent of the ROI.",
    )
    parser.add_argument(
        "--min-bbox-coverage-pct",
        type=float,
        default=0.0,
        help="Drop rows whose intersection covers less than this percent of the geometry bbox.",
    )
    parser.add_argument(
        "--limit",
        type=int,
        default=40,
        help="Maximum aggregate/top-overlap rows in the Markdown report.",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_arg_parser()
    args = parser.parse_args(argv)
    rois = list(args.roi or [])
    if args.component_roi_csv:
        try:
            rois.extend(
                component_rois_from_csv(
                    args.component_roi_csv,
                    args.component_roi_limit,
                    args.component_roi_match_seq,
                )
            )
        except ValueError as exc:
            parser.error(str(exc))
    if not rois:
        parser.error("at least one --roi or --component-roi-csv is required")
    if args.min_roi_coverage_pct < 0.0 or args.min_bbox_coverage_pct < 0.0:
        parser.error("coverage thresholds must be non-negative")
    rows = read_geometry_rows(args.logs)
    overlaps = build_overlaps(
        rows,
        rois,
        args.include_zero_overlap,
        min_roi_coverage_pct=args.min_roi_coverage_pct,
        min_bbox_coverage_pct=args.min_bbox_coverage_pct,
    )
    if args.csv_output:
        write_csv(args.csv_output, overlaps)
    if args.output:
        write_markdown(args.output, args.logs, rows, rois, overlaps, args.limit)
    if not args.csv_output and not args.output:
        write_markdown(Path("/dev/stdout"), args.logs, rows, rois, overlaps, args.limit)
    return 0


if __name__ == "__main__":
    sys.exit(main())
