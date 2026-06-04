#!/usr/bin/env python3
"""Compare primitive-id mini-replay images in original triangle identity space."""

from __future__ import annotations

import argparse
import csv
import struct
import sys
from collections import defaultdict, deque
from pathlib import Path
from typing import Iterable

import numpy as np
from PIL import Image


REPO_ROOT = Path(__file__).resolve().parents[2]
TOOLS_DIR = REPO_ROOT / "scripts" / "tools"
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

from run_3dmark05_mini_replay import optimize_triangle_order_for_vertex_cache  # noqa: E402


def uint16_indices(path: Path) -> list[int]:
    payload = path.read_bytes()
    if len(payload) % 2:
        raise SystemExit(f"index payload is not uint16 aligned: {path}")
    return list(struct.unpack("<" + "H" * (len(payload) // 2), payload))


def triangles(indices: list[int]) -> list[tuple[int, int, int]]:
    triangle_count = len(indices) // 3
    return [
        (indices[i * 3], indices[i * 3 + 1], indices[i * 3 + 2])
        for i in range(triangle_count)
    ]


def transformed_indices(indices: list[int], primitive_order: str) -> list[int]:
    tris = triangles(indices)
    tail = indices[len(tris) * 3:]
    if primitive_order == "original":
        return indices
    if primitive_order == "reverse-triangles":
        ordered = list(reversed(tris))
    elif primitive_order == "sort-min-index":
        ordered = sorted(tris, key=lambda tri: (min(tri), max(tri), tri[0], tri[1], tri[2]))
    elif primitive_order == "sort-max-index":
        ordered = sorted(tris, key=lambda tri: (max(tri), min(tri), tri[0], tri[1], tri[2]))
    elif primitive_order == "cache-opt-lru32":
        return optimize_triangle_order_for_vertex_cache(indices, 32)
    elif primitive_order == "cache-opt-lru64":
        return optimize_triangle_order_for_vertex_cache(indices, 64)
    else:
        raise SystemExit(f"unsupported primitive order: {primitive_order}")
    return [index for tri in ordered for index in tri] + tail


def order_to_original_triangle_map(indices: list[int], primitive_order: str) -> list[int]:
    original = triangles(indices)
    transformed = triangles(transformed_indices(indices, primitive_order))
    queues: dict[tuple[int, int, int], deque[int]] = defaultdict(deque)
    for ordinal, tri in enumerate(original):
        queues[tri].append(ordinal)
    mapping: list[int] = []
    for tri in transformed:
        if not queues[tri]:
            raise SystemExit(f"could not map transformed triangle back to original: {tri}")
        mapping.append(queues[tri].popleft())
    return mapping


def load_rgb(path: Path) -> np.ndarray:
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.uint8)


def decode_primitive_ids(path: Path) -> np.ndarray:
    rgb = load_rgb(path).astype(np.int64)
    encoded = rgb[:, :, 0] + (rgb[:, :, 1] << 8) + (rgb[:, :, 2] << 16)
    return encoded - 1


def canonicalize_ids(ids: np.ndarray, mapping: list[int]) -> np.ndarray:
    canonical = np.full_like(ids, -1)
    valid = (ids >= 0) & (ids < len(mapping))
    if np.any(valid):
        map_array = np.asarray(mapping, dtype=np.int64)
        canonical[valid] = map_array[ids[valid]]
    return canonical


def load_color_pair(before: Path | None, after: Path | None) -> tuple[np.ndarray, np.ndarray] | None:
    if before is None or after is None:
        return None
    before_rgb = load_rgb(before)
    after_rgb = load_rgb(after)
    if before_rgb.shape != after_rgb.shape:
        raise SystemExit(f"color image size mismatch: {before} vs {after}")
    return before_rgb, after_rgb


def color_changed_mask(before_rgb: np.ndarray, after_rgb: np.ndarray) -> np.ndarray:
    return np.max(np.abs(before_rgb.astype(np.int16) - after_rgb.astype(np.int16)), axis=2) > 0


def rgb_text(rgb: np.ndarray | None) -> str:
    if rgb is None:
        return ""
    return ",".join(str(int(v)) for v in rgb)


def bbox_text(mask: np.ndarray) -> str:
    ys, xs = np.nonzero(mask)
    if len(xs) == 0:
        return ""
    return f"{int(xs.min())},{int(ys.min())}-{int(xs.max())},{int(ys.max())}"


def fmt_int(value: int) -> str:
    return f"{value:,}"


def markdown_table(headers: tuple[str, ...], rows: Iterable[tuple[str, ...]]) -> str:
    out = [
        "| " + " | ".join(headers) + " |",
        "| " + " | ".join("---" for _ in headers) + " |",
    ]
    for row in rows:
        out.append("| " + " | ".join(row) + " |")
    return "\n".join(out)


def selected_pixel_mask(
    primitive_changed: np.ndarray,
    color_mask: np.ndarray | None,
    scope: str,
) -> np.ndarray:
    if scope == "primitive-changed":
        return primitive_changed
    if scope == "color-or-primitive-changed":
        return primitive_changed if color_mask is None else (primitive_changed | color_mask)
    if scope == "color-changed":
        if color_mask is None:
            raise SystemExit("--pixel-scope color-changed requires --before-color and --after-color")
        return color_mask
    if color_mask is not None:
        return color_mask
    return primitive_changed


def analyze(args: argparse.Namespace) -> tuple[dict[str, str], list[dict[str, str]]]:
    indices = uint16_indices(args.index_file)
    before_mapping = order_to_original_triangle_map(indices, args.before_primitive_order)
    after_mapping = order_to_original_triangle_map(indices, args.after_primitive_order)
    before_ids = canonicalize_ids(decode_primitive_ids(args.before_primitive_id), before_mapping)
    after_ids = canonicalize_ids(decode_primitive_ids(args.after_primitive_id), after_mapping)
    if before_ids.shape != after_ids.shape:
        raise SystemExit("primitive-id image size mismatch")

    primitive_changed = before_ids != after_ids
    color_pair = load_color_pair(args.before_color, args.after_color)
    color_mask = color_changed_mask(*color_pair) if color_pair is not None else None
    if color_mask is not None and color_mask.shape != primitive_changed.shape:
        raise SystemExit("color image size does not match primitive-id images")

    pixel_rows: list[dict[str, str]] = []
    color_delta_max_values: list[int] = []
    color_delta_l1_values: list[int] = []
    ys, xs = np.nonzero(selected_pixel_mask(primitive_changed, color_mask, args.pixel_scope))
    for y, x in zip(ys, xs):
        before_rgb = color_pair[0][y, x] if color_pair is not None else None
        after_rgb = color_pair[1][y, x] if color_pair is not None else None
        color_delta = (
            np.abs(before_rgb.astype(np.int16) - after_rgb.astype(np.int16))
            if before_rgb is not None and after_rgb is not None else None
        )
        color_delta_max = int(np.max(color_delta)) if color_delta is not None else 0
        color_delta_l1 = int(np.sum(color_delta)) if color_delta is not None else 0
        if color_delta is not None:
            color_delta_max_values.append(color_delta_max)
            color_delta_l1_values.append(color_delta_l1)
        pixel_rows.append({
            "x": str(int(x)),
            "y": str(int(y)),
            "before_original_triangle": str(int(before_ids[y, x])),
            "after_original_triangle": str(int(after_ids[y, x])),
            "primitive_identity_changed": "1" if bool(primitive_changed[y, x]) else "0",
            "color_changed": "1" if color_mask is not None and bool(color_mask[y, x]) else "0",
            "before_color_rgb": rgb_text(before_rgb),
            "after_color_rgb": rgb_text(after_rgb),
            "color_delta_max": str(color_delta_max),
            "color_delta_l1": str(color_delta_l1),
        })

    primitive_changed_pixels = int(np.count_nonzero(primitive_changed))
    color_changed_pixels = int(np.count_nonzero(color_mask)) if color_mask is not None else 0
    color_and_primitive_changed = (
        int(np.count_nonzero(primitive_changed & color_mask))
        if color_mask is not None else 0
    )
    summary = {
        "index_file": str(args.index_file),
        "before_primitive_order": args.before_primitive_order,
        "after_primitive_order": args.after_primitive_order,
        "triangle_count": str(len(before_mapping)),
        "primitive_identity_changed_pixels": str(primitive_changed_pixels),
        "primitive_identity_changed_bbox": bbox_text(primitive_changed),
        "color_changed_pixels": str(color_changed_pixels),
        "color_and_primitive_changed_pixels": str(color_and_primitive_changed),
        "max_color_delta": str(max(color_delta_max_values, default=0)),
        "max_color_delta_l1": str(max(color_delta_l1_values, default=0)),
    }
    return summary, pixel_rows


def write_csv(path: Path, rows: list[dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fields = (
        "x",
        "y",
        "before_original_triangle",
        "after_original_triangle",
        "primitive_identity_changed",
        "color_changed",
        "before_color_rgb",
        "after_color_rgb",
        "color_delta_max",
        "color_delta_l1",
    )
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def write_summary_csv(path: Path, summary: dict[str, str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fields = (
        "index_file",
        "before_primitive_order",
        "after_primitive_order",
        "triangle_count",
        "primitive_identity_changed_pixels",
        "primitive_identity_changed_bbox",
        "color_changed_pixels",
        "color_and_primitive_changed_pixels",
        "max_color_delta",
        "max_color_delta_l1",
    )
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerow({field: summary.get(field, "") for field in fields})


def write_markdown(path: Path, summary: dict[str, str], pixel_rows: list[dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "# Primitive ID Replay Analysis",
        "",
        "## Summary",
        "",
        markdown_table(
            ("Metric", "Value"),
            (
                ("Index file", f"`{summary['index_file']}`"),
                ("Before order", f"`{summary['before_primitive_order']}`"),
                ("After order", f"`{summary['after_primitive_order']}`"),
                ("Triangles", f"`{fmt_int(int(summary['triangle_count']))}`"),
                (
                    "Primitive identity changed pixels",
                    f"`{fmt_int(int(summary['primitive_identity_changed_pixels']))}`",
                ),
                ("Primitive identity changed bbox", f"`{summary['primitive_identity_changed_bbox'] or 'none'}`"),
                ("Color changed pixels", f"`{fmt_int(int(summary['color_changed_pixels']))}`"),
                (
                    "Color + primitive changed pixels",
                    f"`{fmt_int(int(summary['color_and_primitive_changed_pixels']))}`",
                ),
                ("Max color delta", f"`{fmt_int(int(summary['max_color_delta']))}`"),
                ("Max color delta L1", f"`{fmt_int(int(summary['max_color_delta_l1']))}`"),
            ),
        ),
        "",
        "## Pixel Samples",
        "",
    ]
    if not pixel_rows:
        lines.append("No selected pixels.")
    else:
        lines.append(markdown_table(
            (
                "x",
                "y",
                "Before original tri",
                "After original tri",
                "Primitive changed",
                "Color changed",
                "Color delta",
            ),
            (
                (
                    f"`{row['x']}`",
                    f"`{row['y']}`",
                    f"`{row['before_original_triangle']}`",
                    f"`{row['after_original_triangle']}`",
                    f"`{row['primitive_identity_changed']}`",
                    f"`{row['color_changed']}`",
                    f"`{row.get('color_delta_max', '')}`",
                )
                for row in pixel_rows[:64]
            ),
        ))
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--index-file", type=Path, required=True)
    parser.add_argument("--before-primitive-id", type=Path, required=True)
    parser.add_argument("--after-primitive-id", type=Path, required=True)
    parser.add_argument("--before-primitive-order", default="original")
    parser.add_argument("--after-primitive-order", default="cache-opt-lru32")
    parser.add_argument("--before-color", type=Path)
    parser.add_argument("--after-color", type=Path)
    parser.add_argument(
        "--pixel-scope",
        choices=(
            "color-if-available",
            "color-changed",
            "primitive-changed",
            "color-or-primitive-changed",
        ),
        default="color-if-available",
        help=(
            "Which pixels to emit in --pixel-csv-output. The default preserves "
            "existing behavior: color-changed pixels when color images are "
            "provided, otherwise primitive-changed pixels."
        ),
    )
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--pixel-csv-output", type=Path)
    parser.add_argument("--summary-csv-output", type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    summary, pixel_rows = analyze(args)
    write_markdown(args.output, summary, pixel_rows)
    if args.pixel_csv_output:
        write_csv(args.pixel_csv_output, pixel_rows)
    if args.summary_csv_output:
        write_summary_csv(args.summary_csv_output, summary)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
