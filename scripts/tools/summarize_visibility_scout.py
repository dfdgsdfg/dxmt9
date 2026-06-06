#!/usr/bin/env python3
"""Summarize dxmt9 Metal visibility scout CSV output."""

from __future__ import annotations

import argparse
import csv
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable


def as_int(value: object, default: int = 0) -> int:
    if value is None:
        return default
    text = str(value).strip().replace(",", "")
    if not text:
        return default
    try:
        return int(text, 0)
    except ValueError:
        return default


def fmt_int(value: int) -> str:
    return f"{value:,}"


def fmt_pct(numer: int, denom: int) -> str:
    if denom == 0:
        return "n/a"
    return f"{numer / denom * 100.0:.2f}%"


def row_key(row: dict[str, str]) -> tuple[str, str]:
    return row.get("seq", ""), row.get("encoder", "")


def row_key_text(row: dict[str, str]) -> str:
    seq, encoder = row_key(row)
    return f"{seq}/{encoder}"


def bool_text(value: object) -> str:
    return "on" if as_int(value) != 0 else "off"


def depth_kind(row: dict[str, str]) -> str:
    if as_int(row.get("z_enable")) == 0:
        return "off"
    if as_int(row.get("z_write")) != 0:
        return "write"
    return "read"


def color_write_text(row: dict[str, str]) -> str:
    return f"0x{as_int(row.get('color_write'), 0):x}"


def class_signature(row: dict[str, str]) -> str:
    primitive_count = as_int(row.get("source_primitive_count"))
    return "|".join(
        [
            row_key_text(row),
            f"depth={depth_kind(row)}",
            f"blend={bool_text(row.get('alpha_blend'))}",
            f"scissor={bool_text(row.get('scissor'))}",
            f"textured={'yes' if as_int(row.get('texture_mask')) != 0 else 'no'}",
            f"large4096={'yes' if primitive_count >= 4096 else 'no'}",
            f"color_write={color_write_text(row)}",
        ]
    )


@dataclass
class ProbeJoin:
    rows_by_draw_ordinal: dict[tuple[str, str, str], dict[str, str]] = field(default_factory=dict)
    rows_by_encoder_draw: dict[tuple[str, str, str], dict[str, str]] = field(default_factory=dict)


def load_probe_rows(path: Path | None) -> ProbeJoin:
    join = ProbeJoin()
    if path is None:
        return join
    with path.open(newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            seq = row.get("seq", "")
            encoder = row.get("encoder", "")
            if row.get("draw_ordinal"):
                join.rows_by_draw_ordinal[(seq, encoder, row["draw_ordinal"])] = row
            if row.get("encoder_draw_index"):
                join.rows_by_encoder_draw[(seq, encoder, row["encoder_draw_index"])] = row
    return join


def attach_probe(row: dict[str, str], join: ProbeJoin) -> dict[str, str]:
    seq, encoder = row_key(row)
    by_ordinal = join.rows_by_draw_ordinal.get((seq, encoder, row.get("draw_ordinal", "")))
    if by_ordinal is not None:
        return by_ordinal
    return join.rows_by_encoder_draw.get((seq, encoder, row.get("metal_draw_index", "")), {})


@dataclass
class VisibilityGroup:
    key: str
    rows: int = 0
    zero: int = 0
    positive: int = 0
    primitives: int = 0
    submitted_elements: int = 0
    visible_samples_sum: int = 0
    visible_samples_max: int = 0
    metal_draw_min: int | None = None
    metal_draw_max: int | None = None
    zero_primitives: int = 0
    positive_primitives: int = 0
    zero_submitted_elements: int = 0
    positive_submitted_elements: int = 0
    zero_miss32_delta: int = 0
    positive_miss32_delta: int = 0
    original_miss32: int = 0
    candidate_miss32: int = 0
    probe_rows: int = 0

    def add(self, row: dict[str, str], probe: dict[str, str]) -> None:
        samples = as_int(row.get("visible_samples"))
        metal_draw = as_int(row.get("metal_draw_index"))
        primitives = as_int(row.get("source_primitive_count"))
        submitted_elements = as_int(row.get("submitted_element_count"))
        self.rows += 1
        self.zero += 1 if samples == 0 else 0
        self.positive += 1 if samples > 0 else 0
        self.primitives += primitives
        self.submitted_elements += submitted_elements
        self.visible_samples_sum += samples
        self.visible_samples_max = max(self.visible_samples_max, samples)
        self.metal_draw_min = metal_draw if self.metal_draw_min is None else min(self.metal_draw_min, metal_draw)
        self.metal_draw_max = metal_draw if self.metal_draw_max is None else max(self.metal_draw_max, metal_draw)
        if samples == 0:
            self.zero_primitives += primitives
            self.zero_submitted_elements += submitted_elements
        elif samples > 0:
            self.positive_primitives += primitives
            self.positive_submitted_elements += submitted_elements
        if probe:
            self.probe_rows += 1
            original_miss32 = as_int(probe.get("original_cache_miss32"))
            candidate_miss32 = as_int(probe.get("candidate_cache_miss32"))
            delta = candidate_miss32 - original_miss32
            self.original_miss32 += original_miss32
            self.candidate_miss32 += candidate_miss32
            if samples == 0:
                self.zero_miss32_delta += delta
            elif samples > 0:
                self.positive_miss32_delta += delta

    @property
    def miss32_delta(self) -> int:
        return self.candidate_miss32 - self.original_miss32


def read_visibility_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def filter_rows(rows: Iterable[dict[str, str]], row_selector: str | None) -> list[dict[str, str]]:
    if not row_selector:
        return list(rows)
    return [row for row in rows if row_key_text(row) == row_selector]


def parse_draw_indices(spec: str | None) -> set[int]:
    indices: set[int] = set()
    if not spec:
        return indices
    for part in spec.replace(";", ",").split(","):
        part = part.strip()
        if not part:
            continue
        if ".." in part:
            lo, hi = part.split("..", 1)
            start = as_int(lo, -1)
            end = as_int(hi, -1)
            if start >= 0 and end >= start:
                indices.update(range(start, end + 1))
            continue
        value = as_int(part, -1)
        if value >= 0:
            indices.add(value)
    return indices


def aggregate(rows: Iterable[dict[str, str]], join: ProbeJoin) -> list[VisibilityGroup]:
    groups: dict[str, VisibilityGroup] = {}
    for row in rows:
        key = class_signature(row)
        group = groups.setdefault(key, VisibilityGroup(key))
        group.add(row, attach_probe(row, join))
    return sorted(
        groups.values(),
        key=lambda group: (group.zero == 0, -group.visible_samples_sum, group.key),
    )


def aggregate_window(rows: Iterable[dict[str, str]], draw_indices: set[int], join: ProbeJoin) -> VisibilityGroup | None:
    selected = [row for row in rows if as_int(row.get("metal_draw_index"), -1) in draw_indices]
    if not selected:
        return None
    group = VisibilityGroup(f"draws={','.join(str(i) for i in sorted(draw_indices))}")
    for row in selected:
        group.add(row, attach_probe(row, join))
    return group


CSV_FIELDS = [
    "class",
    "draws",
    "zero_draws",
    "positive_draws",
    "zero_pct",
    "metal_draw_min",
    "metal_draw_max",
    "source_primitives",
    "zero_source_primitives",
    "positive_source_primitives",
    "submitted_elements",
    "zero_submitted_elements",
    "positive_submitted_elements",
    "visible_samples_sum",
    "visible_samples_max",
    "probe_rows",
    "original_miss32",
    "candidate_miss32",
    "miss32_delta",
    "zero_miss32_delta",
    "positive_miss32_delta",
]


def group_to_csv_row(group: VisibilityGroup) -> dict[str, str]:
    return {
        "class": group.key,
        "draws": str(group.rows),
        "zero_draws": str(group.zero),
        "positive_draws": str(group.positive),
        "zero_pct": fmt_pct(group.zero, group.rows),
        "metal_draw_min": "" if group.metal_draw_min is None else str(group.metal_draw_min),
        "metal_draw_max": "" if group.metal_draw_max is None else str(group.metal_draw_max),
        "source_primitives": str(group.primitives),
        "zero_source_primitives": str(group.zero_primitives),
        "positive_source_primitives": str(group.positive_primitives),
        "submitted_elements": str(group.submitted_elements),
        "zero_submitted_elements": str(group.zero_submitted_elements),
        "positive_submitted_elements": str(group.positive_submitted_elements),
        "visible_samples_sum": str(group.visible_samples_sum),
        "visible_samples_max": str(group.visible_samples_max),
        "probe_rows": str(group.probe_rows),
        "original_miss32": str(group.original_miss32),
        "candidate_miss32": str(group.candidate_miss32),
        "miss32_delta": str(group.miss32_delta),
        "zero_miss32_delta": str(group.zero_miss32_delta),
        "positive_miss32_delta": str(group.positive_miss32_delta),
    }


def md_cell(value: object) -> str:
    return str(value).replace("|", "\\|")


def write_csv(path: Path, groups: list[VisibilityGroup]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=CSV_FIELDS)
        writer.writeheader()
        for group in groups:
            writer.writerow(group_to_csv_row(group))


def markdown_table(groups: list[VisibilityGroup], limit: int) -> str:
    lines = [
        "| Class | Draws | Zero | Positive | Zero % | Draw range | Visible samples | Max | Miss32 delta |",
        "|---|---:|---:|---:|---:|---|---:|---:|---:|",
    ]
    for group in groups[:limit]:
        draw_range = (
            "n/a"
            if group.metal_draw_min is None
            else f"{group.metal_draw_min}..{group.metal_draw_max}"
        )
        lines.append(
            "| "
            + " | ".join(
                [
                    md_cell(group.key),
                    fmt_int(group.rows),
                    fmt_int(group.zero),
                    fmt_int(group.positive),
                    fmt_pct(group.zero, group.rows),
                    draw_range,
                    fmt_int(group.visible_samples_sum),
                    fmt_int(group.visible_samples_max),
                    fmt_int(group.miss32_delta),
                ]
            )
            + " |"
        )
    return "\n".join(lines)


def probe_miss32_delta(probe: dict[str, str]) -> int:
    if not probe:
        return 0
    return as_int(probe.get("candidate_cache_miss32")) - as_int(probe.get("original_cache_miss32"))


def no_sample_rows(
    rows: Iterable[dict[str, str]],
    join: ProbeJoin,
    limit: int,
) -> list[tuple[dict[str, str], dict[str, str]]]:
    selected: list[tuple[dict[str, str], dict[str, str]]] = []
    for row in rows:
        if as_int(row.get("visible_samples")) != 0:
            continue
        selected.append((row, attach_probe(row, join)))
    selected.sort(
        key=lambda pair: (
            abs(probe_miss32_delta(pair[1])),
            as_int(pair[0].get("source_primitive_count")),
            as_int(pair[0].get("submitted_element_count")),
            -as_int(pair[0].get("metal_draw_index"), 0),
        ),
        reverse=True,
    )
    return selected[:limit]


def markdown_draw_table(rows: list[tuple[dict[str, str], dict[str, str]]]) -> str:
    lines = [
        "| Row | Metal draw | Draw ordinal | Class | Primitives | Elements | Miss32 delta |",
        "|---|---:|---:|---|---:|---:|---:|",
    ]
    if not rows:
        lines.append("| n/a |  |  | no zero-sample draws |  |  |  |")
        return "\n".join(lines)
    for row, probe in rows:
        lines.append(
            "| "
            + " | ".join(
                [
                    md_cell(row_key_text(row)),
                    fmt_int(as_int(row.get("metal_draw_index"))),
                    fmt_int(as_int(row.get("draw_ordinal"))),
                    md_cell(class_signature(row)),
                    fmt_int(as_int(row.get("source_primitive_count"))),
                    fmt_int(as_int(row.get("submitted_element_count"))),
                    fmt_int(probe_miss32_delta(probe)),
                ]
            )
            + " |"
        )
    return "\n".join(lines)


def write_markdown(
    path: Path,
    visibility_path: Path,
    groups: list[VisibilityGroup],
    window: VisibilityGroup | None,
    rows: list[dict[str, str]],
    join: ProbeJoin,
    total_rows: int,
    row_selector: str | None,
    limit: int,
    no_sample_limit: int,
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    total_zero = sum(group.zero for group in groups)
    total_positive = sum(group.positive for group in groups)
    lines = [
        "# Metal Visibility Scout Summary",
        "",
        f"- Source: `{visibility_path}`",
        f"- Row filter: `{row_selector or 'all'}`",
        f"- Draws: `{fmt_int(total_rows)}`",
        f"- No-sample draws: `{fmt_int(total_zero)}` (`{fmt_pct(total_zero, total_rows)}`)",
        f"- Sample-visible draws: `{fmt_int(total_positive)}`",
        "",
    ]
    if window is not None:
        lines.extend(
            [
                "## Requested Draw Window",
                "",
                markdown_table([window], 1),
                "",
            ]
        )
    lines.extend(
        [
            "## No-Sample Draws",
            "",
            markdown_draw_table(no_sample_rows(rows, join, no_sample_limit)),
            "",
        ]
    )
    lines.extend(
        [
            "## Classes",
            "",
            markdown_table(groups, limit),
            "",
            "Positive `visible_samples` is not final-color proof; it only shows that samples passed visibility at that draw point.",
            "",
        ]
    )
    path.write_text("\n".join(lines), encoding="utf-8")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("visibility_csv", type=Path)
    parser.add_argument("--probe-draws", type=Path)
    parser.add_argument("--row", help="Optional SEQ/ENC row filter")
    parser.add_argument(
        "--draw-indices",
        help="Optional comma-separated metal_draw_index list/ranges, e.g. 36..37,71",
    )
    parser.add_argument("--output", type=Path)
    parser.add_argument("--csv-output", type=Path)
    parser.add_argument("--limit", type=int, default=12)
    parser.add_argument("--no-sample-limit", type=int, default=12)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    rows = filter_rows(read_visibility_rows(args.visibility_csv), args.row)
    join = load_probe_rows(args.probe_draws)
    groups = aggregate(rows, join)
    draw_indices = parse_draw_indices(args.draw_indices)
    window = aggregate_window(rows, draw_indices, join) if draw_indices else None

    if args.csv_output:
        write_csv(args.csv_output, groups)
    if args.output:
        write_markdown(
            args.output,
            args.visibility_csv,
            groups,
            window,
            rows,
            join,
            len(rows),
            args.row,
            args.limit,
            args.no_sample_limit,
        )
    if not args.output and not args.csv_output:
        print(markdown_table(groups, args.limit))
        if window is not None:
            print()
            print("Requested Draw Window")
            print(markdown_table([window], 1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
