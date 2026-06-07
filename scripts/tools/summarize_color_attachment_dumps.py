#!/usr/bin/env python3
"""Summarize dxmt9 color attachment dump sidecars."""

from __future__ import annotations

import argparse
import csv
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence


@dataclass(frozen=True)
class Roi:
    name: str
    left: int
    top: int
    right: int
    bottom: int


def parse_roi(value: str) -> Roi:
    parts = value.split(":")
    coords = parts[0].split(",")
    if len(coords) != 4 or len(parts) > 2:
        raise argparse.ArgumentTypeError("ROI must be L,T,R,B or L,T,R,B:name")
    try:
        left, top, right, bottom = (int(part) for part in coords)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("ROI coordinates must be integers") from exc
    if left < 0 or top < 0 or right <= left or bottom <= top:
        raise argparse.ArgumentTypeError("ROI must have non-negative L/T and R>L, B>T")
    name = parts[1] if len(parts) == 2 and parts[1] else f"{left},{top},{right},{bottom}"
    return Roi(name=name, left=left, top=top, right=right, bottom=bottom)


def iter_metadata(paths: Sequence[Path]) -> list[Path]:
    result: list[Path] = []
    for path in paths:
        if path.is_dir():
            result.extend(sorted(path.glob("*.bin.json")))
        elif path.name.endswith(".bin.json"):
            result.append(path)
        elif path.suffix == ".json" and path.with_suffix("").exists():
            result.append(path)
        elif path.suffix == ".bin" and Path(f"{path}.json").exists():
            result.append(Path(f"{path}.json"))
        else:
            raise FileNotFoundError(f"not a color dump .bin/.json path: {path}")
    return sorted(dict.fromkeys(result))


def clamp_roi(roi: Roi, width: int, height: int) -> Roi:
    return Roi(
        name=roi.name,
        left=min(roi.left, width),
        top=min(roi.top, height),
        right=min(roi.right, width),
        bottom=min(roi.bottom, height),
    )


def summarize_region(
    data: bytes,
    row_bytes: int,
    width: int,
    height: int,
    roi: Roi,
    bright_threshold: int,
    white_threshold: int,
    warm_red_threshold: int,
    warm_green_threshold: int,
    warm_blue_margin: int,
) -> dict[str, object]:
    clipped = clamp_roi(roi, width, height)
    pixel_count = max(0, clipped.right - clipped.left) * max(0, clipped.bottom - clipped.top)
    sums = [0, 0, 0]
    max_rgb = [0, 0, 0]
    bright = 0
    white = 0
    warm = 0
    hot = [0, 0, 0, 0, 0, 0]
    warm_hot = [0, 0, 0, 0, 0, 0]
    for y in range(clipped.top, clipped.bottom):
        offset = y * row_bytes + clipped.left * 4
        for x in range(clipped.left, clipped.right):
            b = data[offset]
            g = data[offset + 1]
            r = data[offset + 2]
            sums[0] += r
            sums[1] += g
            sums[2] += b
            max_rgb[0] = max(max_rgb[0], r)
            max_rgb[1] = max(max_rgb[1], g)
            max_rgb[2] = max(max_rgb[2], b)
            channel_max = max(r, g, b)
            if channel_max > bright_threshold:
                bright += 1
            if r > white_threshold and g > white_threshold and b > white_threshold:
                white += 1
            if (
                r >= warm_red_threshold
                and g >= warm_green_threshold
                and b <= r + warm_blue_margin
            ):
                warm += 1
                warm_score = r + g + b
                if warm_score > warm_hot[5]:
                    warm_hot = [x, y, r, g, b, warm_score]
            if channel_max > hot[5]:
                hot = [x, y, r, g, b, channel_max]
            offset += 4
    avg = [0.0, 0.0, 0.0]
    if pixel_count:
        avg = [value / pixel_count for value in sums]
    return {
        "roi": clipped,
        "pixels": pixel_count,
        "avg": avg,
        "max": max_rgb,
        "bright": bright,
        "white": white,
        "warm": warm,
        "hot": hot,
        "warm_hot": warm_hot,
    }


def summarize_dump(
    metadata_path: Path,
    rois: Sequence[Roi],
    bright_threshold: int,
    white_threshold: int,
    warm_red_threshold: int,
    warm_green_threshold: int,
    warm_blue_margin: int,
) -> list[dict[str, object]]:
    metadata = json.loads(metadata_path.read_text())
    bin_path = Path(str(metadata_path).removesuffix(".json"))
    data = bin_path.read_bytes()
    width = int(metadata["width"])
    height = int(metadata["height"])
    row_bytes = int(metadata["rowBytes"])
    rows: list[dict[str, object]] = []
    for roi in rois:
        region = summarize_region(
            data,
            row_bytes,
            width,
            height,
            roi,
            bright_threshold,
            white_threshold,
            warm_red_threshold,
            warm_green_threshold,
            warm_blue_margin,
        )
        clipped: Roi = region["roi"]  # type: ignore[assignment]
        avg = region["avg"]  # type: ignore[assignment]
        max_rgb = region["max"]  # type: ignore[assignment]
        hot = region["hot"]  # type: ignore[assignment]
        warm_hot = region["warm_hot"]  # type: ignore[assignment]
        rows.append(
            {
                "file": str(bin_path),
                "handle": metadata.get("handle", ""),
                "seq": metadata.get("seq", ""),
                "enc": metadata.get("enc", ""),
                "after_draw": metadata.get("afterDraw", 0),
                "draw": metadata.get("draw", ""),
                "command_index": metadata.get("commandIndex", ""),
                "command_draw_index": metadata.get("commandDrawIndex", ""),
                "command_draw_count": metadata.get("commandDrawCount", ""),
                "texture0": metadata.get("texture0", ""),
                "format_name": metadata.get("formatName", ""),
                "width": width,
                "height": height,
                "roi": clipped.name,
                "roi_rect": f"{clipped.left},{clipped.top},{clipped.right},{clipped.bottom}",
                "pixels": region["pixels"],
                "avg_r": f"{avg[0]:.3f}",
                "avg_g": f"{avg[1]:.3f}",
                "avg_b": f"{avg[2]:.3f}",
                "max_r": max_rgb[0],
                "max_g": max_rgb[1],
                "max_b": max_rgb[2],
                "bright_pixels": region["bright"],
                "white_pixels": region["white"],
                "warm_pixels": region["warm"],
                "hot_x": hot[0],
                "hot_y": hot[1],
                "hot_r": hot[2],
                "hot_g": hot[3],
                "hot_b": hot[4],
                "warm_hot_x": warm_hot[0],
                "warm_hot_y": warm_hot[1],
                "warm_hot_r": warm_hot[2],
                "warm_hot_g": warm_hot[3],
                "warm_hot_b": warm_hot[4],
            }
        )
    return rows


CSV_FIELDS = [
    "file",
    "handle",
    "seq",
    "enc",
    "after_draw",
    "draw",
    "command_index",
    "command_draw_index",
    "command_draw_count",
    "texture0",
    "format_name",
    "width",
    "height",
    "roi",
    "roi_rect",
    "pixels",
    "avg_r",
    "avg_g",
    "avg_b",
    "max_r",
    "max_g",
    "max_b",
    "bright_pixels",
    "white_pixels",
    "warm_pixels",
    "hot_x",
    "hot_y",
    "hot_r",
    "hot_g",
    "hot_b",
    "warm_hot_x",
    "warm_hot_y",
    "warm_hot_r",
    "warm_hot_g",
    "warm_hot_b",
]


def write_csv(rows: Sequence[dict[str, object]], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=CSV_FIELDS)
        writer.writeheader()
        writer.writerows(rows)


def write_markdown(rows: Sequence[dict[str, object]], path: Path, limit: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    shown = rows[:limit] if limit > 0 else rows
    lines = [
        "# Color Attachment Dump Summary",
        "",
        f"- Rows: `{len(rows)}`",
        "",
        "| File | Seq/Enc/Draw | Cmd | Texture0 | ROI | Max RGB | Bright | White | Warm | Hot | Warm Hot |",
        "|---|---:|---:|---|---|---:|---:|---:|---:|---|---|",
    ]
    for row in shown:
        lines.append(
            "| "
            + " | ".join(
                [
                    Path(str(row["file"])).name,
                    f"{row['seq']}/{row['enc']}/{row['draw']}",
                    f"{row['command_index']}:{row['command_draw_index']}/{row['command_draw_count']}",
                    str(row["texture0"]),
                    f"{row['roi']} `{row['roi_rect']}`",
                    f"[{row['max_r']},{row['max_g']},{row['max_b']}]",
                    str(row["bright_pixels"]),
                    str(row["white_pixels"]),
                    str(row["warm_pixels"]),
                    f"({row['hot_x']},{row['hot_y']}) [{row['hot_r']},{row['hot_g']},{row['hot_b']}]",
                    f"({row['warm_hot_x']},{row['warm_hot_y']}) [{row['warm_hot_r']},{row['warm_hot_g']},{row['warm_hot_b']}]",
                ]
            )
            + " |"
        )
    path.write_text("\n".join(lines) + "\n")


def int_sort_key(value: object) -> tuple[int, int | str]:
    text = str(value)
    if not text:
        return (1, "")
    try:
        return (0, int(text, 0))
    except ValueError:
        return (1, text)


def row_sort_key(row: dict[str, object]) -> tuple[object, ...]:
    return (
        int_sort_key(row["seq"]),
        int_sort_key(row["command_index"]),
        int_sort_key(row["command_draw_index"]),
        int_sort_key(row["enc"]),
        int_sort_key(row["draw"]),
        str(row["roi"]),
        str(row["file"]),
    )


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="+", type=Path, help="Color dump dirs or .bin/.json paths")
    parser.add_argument("--roi", action="append", type=parse_roi, default=[], help="ROI L,T,R,B[:name]")
    parser.add_argument("--bright-threshold", type=int, default=220)
    parser.add_argument("--white-threshold", type=int, default=240)
    parser.add_argument("--warm-red-threshold", type=int, default=180)
    parser.add_argument("--warm-green-threshold", type=int, default=110)
    parser.add_argument("--warm-blue-margin", type=int, default=32)
    parser.add_argument("--output", type=Path, required=True, help="Markdown output")
    parser.add_argument("--csv-output", type=Path, help="CSV output")
    parser.add_argument("--limit", type=int, default=80, help="Markdown row limit; 0 means all")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_arg_parser().parse_args(argv)
    rois = args.roi or [Roi("full", 0, 0, 1 << 30, 1 << 30)]
    rows: list[dict[str, object]] = []
    for metadata_path in iter_metadata(args.paths):
        rows.extend(
            summarize_dump(
                metadata_path,
                rois,
                bright_threshold=args.bright_threshold,
                white_threshold=args.white_threshold,
                warm_red_threshold=args.warm_red_threshold,
                warm_green_threshold=args.warm_green_threshold,
                warm_blue_margin=args.warm_blue_margin,
            )
        )
    rows.sort(key=row_sort_key)
    write_markdown(rows, args.output, args.limit)
    if args.csv_output:
        write_csv(rows, args.csv_output)
    print(args.output)
    if args.csv_output:
        print(args.csv_output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
