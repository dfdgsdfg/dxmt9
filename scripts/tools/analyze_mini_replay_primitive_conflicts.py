#!/usr/bin/env python3
"""Analyze geometry behind primitive-owner changes in a mini-replay draw.

This tool consumes a mini-replay manifest and the per-pixel CSV emitted by
``analyze_primitive_id_replay.py``.  For the selected draw it replays the small
subset of vertex-shader math needed by the 3DMark05 GT1 material family and
reports the screen/depth/UV relationship between the original and reordered
triangles at pixels whose primitive owner changed.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import struct
from pathlib import Path
from typing import Any, Iterable


REPO_ROOT = Path(__file__).resolve().parents[2]


def resolve_path(path: str | Path) -> Path:
    value = Path(path)
    return value if value.is_absolute() else REPO_ROOT / value


def as_int(value: Any, default: int = 0) -> int:
    text = str(value or "").strip()
    if not text:
        return default
    try:
        return int(text, 0)
    except ValueError:
        try:
            return int(float(text))
        except ValueError:
            return default


def as_float(value: Any, default: float = 0.0) -> float:
    text = str(value or "").strip()
    if not text:
        return default
    try:
        return float(text)
    except ValueError:
        return default


def fmt_float(value: float, digits: int = 6) -> str:
    if math.isnan(value) or math.isinf(value):
        return "n/a"
    return f"{value:.{digits}f}"


def fmt_int(value: int) -> str:
    return f"{value:,}"


def load_manifest(path: Path) -> dict[str, Any]:
    with path.open(encoding="utf-8") as handle:
        data = json.load(handle)
    if data.get("schema") != "dxmt9.3dmark05.mini_replay_manifest.v1":
        raise SystemExit(f"unsupported manifest schema: {data.get('schema')}")
    draws = data.get("draws")
    if not isinstance(draws, list) or not draws:
        raise SystemExit("manifest has no draws")
    return data


def uint_indices(path: Path, index_type: str) -> list[int]:
    payload = path.read_bytes()
    if index_type == "uint32":
        if len(payload) % 4:
            raise SystemExit(f"index payload is not uint32 aligned: {path}")
        return list(struct.unpack("<" + "I" * (len(payload) // 4), payload))
    if len(payload) % 2:
        raise SystemExit(f"index payload is not uint16 aligned: {path}")
    return list(struct.unpack("<" + "H" * (len(payload) // 2), payload))


def triangles(indices: list[int]) -> list[tuple[int, int, int]]:
    return [
        (indices[i * 3], indices[i * 3 + 1], indices[i * 3 + 2])
        for i in range(len(indices) // 3)
    ]


def floats_from_file(path: Path) -> list[float]:
    payload = path.read_bytes()
    if len(payload) % 4:
        raise SystemExit(f"float payload is not 4-byte aligned: {path}")
    return list(struct.unpack("<" + "f" * (len(payload) // 4), payload))


def read_f32x2(payload: bytes, offset: int) -> tuple[float, float]:
    return struct.unpack_from("<ff", payload, offset)


def read_f32x3(payload: bytes, offset: int) -> tuple[float, float, float]:
    return struct.unpack_from("<fff", payload, offset)


def dot4(a: tuple[float, float, float, float], b: tuple[float, float, float, float]) -> float:
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]


def dot3(a: tuple[float, float, float], b: tuple[float, float, float]) -> float:
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def sub3(a: tuple[float, float, float], b: tuple[float, float, float]) -> tuple[float, float, float]:
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def f4(constants: list[float], index: int) -> tuple[float, float, float, float]:
    base = index * 4
    return (
        constants[base + 0],
        constants[base + 1],
        constants[base + 2],
        constants[base + 3],
    )


def f4_xyz(value: tuple[float, float, float, float]) -> tuple[float, float, float]:
    return (value[0], value[1], value[2])


def ffp_half_pixel_fixup(path: Path | None) -> tuple[float, float]:
    if path is None or not path.exists():
        return (0.0, 0.0)
    payload = path.read_bytes()
    # See FfpVsConsts in generated MSL. halfPixelFixup starts after:
    # 4 matrices (3 groups), material/global fields, light arrays,
    # blend matrices, texture transforms, and clip planes.
    offset = 2048
    if len(payload) < offset + 8:
        return (0.0, 0.0)
    return read_f32x2(payload, offset)


def viewport_size(draw: dict[str, Any]) -> tuple[int, int]:
    color = ((draw.get("attachments") or {}).get("colors") or [{}])[0]
    depth = (draw.get("attachments") or {}).get("depth") or {}
    width = as_int(color.get("width")) or as_int(depth.get("width")) or 1024
    height = as_int(color.get("height")) or as_int(depth.get("height")) or 768
    return width, height


def first_stream_file(draw: dict[str, Any], stream_index: int) -> Path | None:
    geometry = draw.get("geometry") or {}
    for stream in geometry.get("streams") or []:
        if as_int(stream.get("stream"), -1) == stream_index:
            file_text = stream.get("file")
            return resolve_path(file_text) if file_text else None
    if stream_index == 0 and geometry.get("stream0_file"):
        return resolve_path(geometry["stream0_file"])
    return None


class VertexProjector:
    def __init__(self, draw: dict[str, Any]) -> None:
        geometry = draw.get("geometry") or {}
        uniforms = draw.get("uniforms") or {}
        state = draw.get("state") or {}
        self.stream0_stride = as_int(state.get("stream0_stride")) or 24
        self.stream1_stride = 32
        self.stream0 = resolve_path(geometry["stream0_file"]).read_bytes()
        stream1_file = first_stream_file(draw, 1)
        self.stream1 = stream1_file.read_bytes() if stream1_file else b""
        self.c = floats_from_file(resolve_path(uniforms["vsconsts_file"]))
        self.half_pixel = ffp_half_pixel_fixup(
            resolve_path(uniforms["ffpvs_file"]) if uniforms.get("ffpvs_file") else None
        )
        self.width, self.height = viewport_size(draw)

    def vertex(self, index: int) -> dict[str, Any]:
        base0 = index * self.stream0_stride
        base1 = index * self.stream1_stride
        vin0_3 = read_f32x3(self.stream0, base0 + 0)
        vin0 = (vin0_3[0], vin0_3[1], vin0_3[2], 1.0)
        vin1 = read_f32x3(self.stream1, base1 + 0) if self.stream1 else (0.0, 0.0, 0.0)
        vin2 = read_f32x3(self.stream1, base1 + 12) if self.stream1 else (0.0, 0.0, 0.0)
        vin3 = read_f32x3(self.stream0, base0 + 12)
        vin4 = read_f32x2(self.stream1, base1 + 24) if self.stream1 else (0.0, 0.0)

        clip = (
            dot4(vin0, f4(self.c, 4)),
            dot4(vin0, f4(self.c, 5)),
            dot4(vin0, f4(self.c, 6)),
            dot4(vin0, f4(self.c, 7)),
        )
        clip = (
            clip[0] + self.half_pixel[0] * clip[3],
            clip[1] + self.half_pixel[1] * clip[3],
            clip[2],
            clip[3],
        )
        tex7 = (
            dot4(vin0, f4(self.c, 0)),
            dot4(vin0, f4(self.c, 1)),
            dot4(vin0, f4(self.c, 2)),
            dot4(vin0, f4(self.c, 3)),
        )
        c8 = f4_xyz(f4(self.c, 8))
        c9 = f4_xyz(f4(self.c, 9))
        r0 = sub3(c9, vin0_3)
        tex6 = (dot3(c8, vin1), dot3(c8, vin2), dot3(c8, vin3), 1.0)
        tex1 = (dot3(r0, vin1), dot3(r0, vin2), dot3(r0, vin3), 1.0)
        uv0 = (vin4[0], vin4[1])
        w = clip[3]
        ndc = (
            clip[0] / w if w else math.nan,
            clip[1] / w if w else math.nan,
            clip[2] / w if w else math.nan,
        )
        screen = (
            (ndc[0] + 1.0) * 0.5 * self.width,
            (1.0 - ndc[1]) * 0.5 * self.height,
        )
        projected_tex7 = (
            tex7[0] / tex7[3] if tex7[3] else math.nan,
            tex7[1] / tex7[3] if tex7[3] else math.nan,
            tex7[2] / tex7[3] if tex7[3] else math.nan,
        )
        return {
            "clip": clip,
            "ndc": ndc,
            "screen": screen,
            "depth": ndc[2],
            "uv0": uv0,
            "tex7_projected": projected_tex7,
            "tex1": tex1,
            "tex6": tex6,
        }


def barycentric(
    p: tuple[float, float],
    a: tuple[float, float],
    b: tuple[float, float],
    c: tuple[float, float],
) -> tuple[float, float, float]:
    denom = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1])
    if abs(denom) < 1.0e-12:
        return (math.nan, math.nan, math.nan)
    u = ((b[1] - c[1]) * (p[0] - c[0]) + (c[0] - b[0]) * (p[1] - c[1])) / denom
    v = ((c[1] - a[1]) * (p[0] - c[0]) + (a[0] - c[0]) * (p[1] - c[1])) / denom
    w = 1.0 - u - v
    return (u, v, w)


def bary_inside(weights: tuple[float, float, float], eps: float = 1.0e-3) -> bool:
    if any(math.isnan(v) for v in weights):
        return False
    return min(weights) >= -eps and max(weights) <= 1.0 + eps


def interpolate(weights: tuple[float, float, float], values: Iterable[float]) -> float:
    vals = list(values)
    return weights[0] * vals[0] + weights[1] * vals[1] + weights[2] * vals[2]


def interpolate_vec(
    weights: tuple[float, float, float],
    values: Iterable[tuple[float, ...]],
) -> tuple[float, ...]:
    vals = list(values)
    return tuple(interpolate(weights, component) for component in zip(*vals))


def triangle_stats(
    projector: VertexProjector,
    tri: tuple[int, int, int],
    x: int,
    y: int,
) -> dict[str, Any]:
    vertices = [projector.vertex(index) for index in tri]
    p = (float(x) + 0.5, float(y) + 0.5)
    weights = barycentric(
        p,
        vertices[0]["screen"],
        vertices[1]["screen"],
        vertices[2]["screen"],
    )
    depth = interpolate(weights, (v["depth"] for v in vertices))
    uv0 = interpolate_vec(weights, (v["uv0"] for v in vertices))
    tex7 = interpolate_vec(weights, (v["tex7_projected"] for v in vertices))
    tex1 = interpolate_vec(weights, (v["tex1"][:3] for v in vertices))
    tex6 = interpolate_vec(weights, (v["tex6"][:3] for v in vertices))
    screen_x = [v["screen"][0] for v in vertices]
    screen_y = [v["screen"][1] for v in vertices]
    return {
        "triangle": tri,
        "weights": weights,
        "inside": bary_inside(weights),
        "depth": depth,
        "uv0": uv0,
        "tex7": tex7,
        "tex1": tex1,
        "tex6": tex6,
        "screen_bbox": (
            min(screen_x), min(screen_y), max(screen_x), max(screen_y),
        ),
    }


def vec_delta(a: tuple[float, ...], b: tuple[float, ...]) -> float:
    return math.sqrt(sum((x - y) * (x - y) for x, y in zip(a, b)))


def load_pixel_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def selected_draw(manifest: dict[str, Any], draw_index: int) -> dict[str, Any]:
    draws = manifest["draws"]
    if draw_index < 0 or draw_index >= len(draws):
        raise SystemExit(f"draw index out of range: {draw_index}")
    return draws[draw_index]


def analyze(args: argparse.Namespace) -> tuple[dict[str, Any], list[dict[str, str]]]:
    manifest = load_manifest(args.manifest)
    draw = selected_draw(manifest, args.draw_index)
    geometry = draw.get("geometry") or {}
    state = draw.get("state") or {}
    indices = uint_indices(resolve_path(geometry["index_file"]), str(state.get("index_type", "uint16")))
    tris = triangles(indices)
    projector = VertexProjector(draw)
    rows = [
        row for row in load_pixel_rows(args.primitive_pixel_csv)
        if not args.color_changed_only or as_int(row.get("color_changed")) != 0
    ]

    out_rows: list[dict[str, str]] = []
    depth_deltas: list[float] = []
    uv0_deltas: list[float] = []
    tex7_deltas: list[float] = []
    inside_count = 0
    for row in rows:
        x = as_int(row.get("x"))
        y = as_int(row.get("y"))
        before_tri_id = as_int(row.get("before_original_triangle"), -1)
        after_tri_id = as_int(row.get("after_original_triangle"), -1)
        if before_tri_id < 0 or before_tri_id >= len(tris):
            continue
        if after_tri_id < 0 or after_tri_id >= len(tris):
            continue
        before = triangle_stats(projector, tris[before_tri_id], x, y)
        after = triangle_stats(projector, tris[after_tri_id], x, y)
        depth_delta = after["depth"] - before["depth"]
        uv0_delta = vec_delta(before["uv0"], after["uv0"])
        tex7_delta = vec_delta(before["tex7"], after["tex7"])
        tex1_delta = vec_delta(before["tex1"], after["tex1"])
        tex6_delta = vec_delta(before["tex6"], after["tex6"])
        depth_deltas.append(abs(depth_delta))
        uv0_deltas.append(uv0_delta)
        tex7_deltas.append(tex7_delta)
        both_inside = bool(before["inside"] and after["inside"])
        if both_inside:
            inside_count += 1
        out_rows.append({
            "x": str(x),
            "y": str(y),
            "before_original_triangle": str(before_tri_id),
            "after_original_triangle": str(after_tri_id),
            "color_changed": str(as_int(row.get("color_changed"))),
            "both_triangles_cover_pixel": "1" if both_inside else "0",
            "before_depth": fmt_float(before["depth"], 9),
            "after_depth": fmt_float(after["depth"], 9),
            "depth_delta": fmt_float(depth_delta, 9),
            "abs_depth_delta": fmt_float(abs(depth_delta), 9),
            "before_uv0": ",".join(fmt_float(v, 6) for v in before["uv0"]),
            "after_uv0": ",".join(fmt_float(v, 6) for v in after["uv0"]),
            "uv0_delta": fmt_float(uv0_delta, 9),
            "before_projected_tex7": ",".join(fmt_float(v, 6) for v in before["tex7"]),
            "after_projected_tex7": ",".join(fmt_float(v, 6) for v in after["tex7"]),
            "projected_tex7_delta": fmt_float(tex7_delta, 9),
            "tex1_delta": fmt_float(tex1_delta, 9),
            "tex6_delta": fmt_float(tex6_delta, 9),
            "before_barycentric": ",".join(fmt_float(v, 6) for v in before["weights"]),
            "after_barycentric": ",".join(fmt_float(v, 6) for v in after["weights"]),
        })

    summary = {
        "draw_index": args.draw_index,
        "encoder_draw_index": as_int(draw.get("encoder_draw_index")),
        "draw_ordinal": as_int(draw.get("draw_ordinal")),
        "pixels": len(out_rows),
        "both_cover_pixels": inside_count,
        "max_abs_depth_delta": max(depth_deltas) if depth_deltas else math.nan,
        "avg_abs_depth_delta": (
            sum(depth_deltas) / len(depth_deltas) if depth_deltas else math.nan
        ),
        "max_uv0_delta": max(uv0_deltas) if uv0_deltas else math.nan,
        "max_projected_tex7_delta": max(tex7_deltas) if tex7_deltas else math.nan,
        "index_file": str(resolve_path(geometry["index_file"])),
    }
    return summary, out_rows


def write_csv(path: Path, rows: list[dict[str, str]]) -> None:
    fields = (
        "x",
        "y",
        "before_original_triangle",
        "after_original_triangle",
        "color_changed",
        "both_triangles_cover_pixel",
        "before_depth",
        "after_depth",
        "depth_delta",
        "abs_depth_delta",
        "before_uv0",
        "after_uv0",
        "uv0_delta",
        "before_projected_tex7",
        "after_projected_tex7",
        "projected_tex7_delta",
        "tex1_delta",
        "tex6_delta",
        "before_barycentric",
        "after_barycentric",
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def markdown_table(headers: tuple[str, ...], rows: Iterable[tuple[str, ...]]) -> str:
    out = [
        "| " + " | ".join(headers) + " |",
        "| " + " | ".join("---" for _ in headers) + " |",
    ]
    for row in rows:
        out.append("| " + " | ".join(row) + " |")
    return "\n".join(out)


def write_markdown(path: Path, summary: dict[str, Any], rows: list[dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "# Mini Replay Primitive Conflict Analysis",
        "",
        "## Summary",
        "",
        markdown_table(
            ("Metric", "Value"),
            (
                ("Draw index", f"`{summary['draw_index']}`"),
                ("Encoder draw index", f"`{summary['encoder_draw_index']}`"),
                ("Draw ordinal", f"`{summary['draw_ordinal']}`"),
                ("Selected pixels", f"`{fmt_int(summary['pixels'])}`"),
                ("Both triangles cover pixel", f"`{fmt_int(summary['both_cover_pixels'])}`"),
                ("Max abs depth delta", f"`{fmt_float(summary['max_abs_depth_delta'], 9)}`"),
                ("Avg abs depth delta", f"`{fmt_float(summary['avg_abs_depth_delta'], 9)}`"),
                ("Max uv0 delta", f"`{fmt_float(summary['max_uv0_delta'], 9)}`"),
                (
                    "Max projected texcoord7 delta",
                    f"`{fmt_float(summary['max_projected_tex7_delta'], 9)}`",
                ),
                ("Index file", f"`{summary['index_file']}`"),
            ),
        ),
        "",
        "## Pixel Conflicts",
        "",
    ]
    if rows:
        lines.append(markdown_table(
            (
                "Pixel",
                "Before tri",
                "After tri",
                "Depth delta",
                "UV0 delta",
                "Proj tex7 delta",
                "Both cover",
            ),
            (
                (
                    f"`{row['x']},{row['y']}`",
                    f"`{row['before_original_triangle']}`",
                    f"`{row['after_original_triangle']}`",
                    f"`{row['depth_delta']}`",
                    f"`{row['uv0_delta']}`",
                    f"`{row['projected_tex7_delta']}`",
                    f"`{row['both_triangles_cover_pixel']}`",
                )
                for row in rows[:50]
            ),
        ))
    else:
        lines.append("No selected primitive-conflict pixels.")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True, type=resolve_path)
    parser.add_argument("--draw-index", required=True, type=int)
    parser.add_argument("--primitive-pixel-csv", required=True, type=resolve_path)
    parser.add_argument("--output", required=True, type=resolve_path)
    parser.add_argument("--csv-output", required=True, type=resolve_path)
    parser.add_argument(
        "--all-owner-changes",
        action="store_true",
        help="Analyze all primitive-owner changes instead of color-changed pixels only.",
    )
    args = parser.parse_args()
    args.color_changed_only = not args.all_owner_changes
    return args


def main() -> None:
    args = parse_args()
    summary, rows = analyze(args)
    write_csv(args.csv_output, rows)
    write_markdown(args.output, summary, rows)


if __name__ == "__main__":
    main()
