#!/usr/bin/env python3
"""Analyze D3DDECLUSAGE_BLENDINDICES ranges from dxmt9 geometry dumps.

`run_3dmark05_perf_probe.sh --dump-indexed-geometry` writes per-draw `.meta`,
`.index.bin`, and `.streamN.bin` files. This tool uses the vertex declaration
metadata and the referenced index range to measure the actual blend-index values
feeding programmable VS `mova a0, vN` patterns. It is an offline proof aid for
whether indexed constant access like `c[a0.x + 0..2]` can be bounded.
"""

from __future__ import annotations

import argparse
import csv
import struct
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable


D3DDECLUSAGE_BLENDINDICES = 2

D3DDECLTYPE_FLOAT1 = 0
D3DDECLTYPE_FLOAT2 = 1
D3DDECLTYPE_FLOAT3 = 2
D3DDECLTYPE_FLOAT4 = 3
D3DDECLTYPE_D3DCOLOR = 4
D3DDECLTYPE_UBYTE4 = 5
D3DDECLTYPE_SHORT2 = 6
D3DDECLTYPE_SHORT4 = 7
D3DDECLTYPE_UBYTE4N = 8
D3DDECLTYPE_SHORT2N = 9
D3DDECLTYPE_SHORT4N = 10
D3DDECLTYPE_USHORT2N = 11
D3DDECLTYPE_USHORT4N = 12
D3DDECLTYPE_UDEC3 = 13
D3DDECLTYPE_DEC3N = 14
D3DDECLTYPE_FLOAT16_2 = 15
D3DDECLTYPE_FLOAT16_4 = 16

COMPONENTS = ("x", "y", "z", "w")

CSV_FIELDS = (
    "meta",
    "seq",
    "encoder",
    "draw_ordinal",
    "slot",
    "vs",
    "ps",
    "element",
    "stream",
    "offset",
    "type",
    "stride",
    "indices",
    "unique_indices",
    "vertices_sampled",
    "missing_vertices",
    "component_count",
    "min_x",
    "max_x",
    "unique_x",
    "min_y",
    "max_y",
    "unique_y",
    "min_z",
    "max_z",
    "unique_z",
    "min_w",
    "max_w",
    "unique_w",
    "global_min",
    "global_max",
    "global_unique",
    "c_window_min",
    "c_window_max_plus_2",
    "c_window_regs",
    "status",
)


@dataclass
class Element:
    index: int
    stream: int
    offset: int
    type: int
    usage: int
    usage_index: int


@dataclass
class BlendRange:
    values: list[set[int]] = field(default_factory=lambda: [set() for _ in range(4)])
    sampled: int = 0
    missing: int = 0


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


def read_meta(path: Path) -> dict[str, str]:
    row: dict[str, str] = {"meta": str(path)}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        row[key] = value
    return row


def vertex_elements(meta: dict[str, str]) -> list[Element]:
    count = as_int(meta.get("vertex_decl_element_count"))
    elements: list[Element] = []
    for index in range(count):
        prefix = f"vertex_decl_element{index}_"
        usage = as_int(meta.get(prefix + "usage"), -1)
        elements.append(
            Element(
                index=index,
                stream=as_int(meta.get(prefix + "stream")),
                offset=as_int(meta.get(prefix + "offset")),
                type=as_int(meta.get(prefix + "type"), -1),
                usage=usage,
                usage_index=as_int(meta.get(prefix + "usage_index")),
            )
        )
    return elements


def stem_path(meta_path: Path) -> Path:
    if meta_path.suffix == ".meta":
        return meta_path.with_suffix("")
    return Path(str(meta_path).removesuffix(".meta"))


def stream_file(meta_path: Path, stream: int) -> Path:
    return Path(str(stem_path(meta_path)) + f".stream{stream}.bin")


def index_file(meta_path: Path) -> Path:
    return Path(str(stem_path(meta_path)) + ".index.bin")


def read_indices(path: Path, index_type: str) -> list[int]:
    data = path.read_bytes()
    if index_type == "uint32":
        count = len(data) // 4
        return list(struct.unpack("<" + "I" * count, data[:count * 4]))
    count = len(data) // 2
    return list(struct.unpack("<" + "H" * count, data[:count * 2]))


def decode_decl_values(data: bytes, offset: int, decl_type: int) -> list[int] | None:
    if offset < 0 or offset >= len(data):
        return None
    remaining = len(data) - offset
    if decl_type == D3DDECLTYPE_UBYTE4 or decl_type == D3DDECLTYPE_UBYTE4N:
        if remaining < 4:
            return None
        return list(data[offset:offset + 4])
    if decl_type == D3DDECLTYPE_D3DCOLOR:
        if remaining < 4:
            return None
        # D3DCOLOR memory order is BGRA; for blend indices the useful fact is
        # the integer byte range, so keep the four stored bytes.
        return list(data[offset:offset + 4])
    if decl_type in (D3DDECLTYPE_SHORT2, D3DDECLTYPE_SHORT2N):
        if remaining < 4:
            return None
        return list(struct.unpack_from("<hh", data, offset))
    if decl_type in (D3DDECLTYPE_SHORT4, D3DDECLTYPE_SHORT4N):
        if remaining < 8:
            return None
        return list(struct.unpack_from("<hhhh", data, offset))
    if decl_type == D3DDECLTYPE_USHORT2N:
        if remaining < 4:
            return None
        return list(struct.unpack_from("<HH", data, offset))
    if decl_type == D3DDECLTYPE_USHORT4N:
        if remaining < 8:
            return None
        return list(struct.unpack_from("<HHHH", data, offset))
    if decl_type == D3DDECLTYPE_UDEC3:
        if remaining < 4:
            return None
        packed = struct.unpack_from("<I", data, offset)[0]
        return [packed & 0x3ff, (packed >> 10) & 0x3ff, (packed >> 20) & 0x3ff]
    if decl_type == D3DDECLTYPE_DEC3N:
        if remaining < 4:
            return None
        packed = struct.unpack_from("<I", data, offset)[0]
        values = []
        for shift in (0, 10, 20):
            value = (packed >> shift) & 0x3ff
            if value & 0x200:
                value -= 0x400
            values.append(value)
        return values
    if decl_type in (
        D3DDECLTYPE_FLOAT1,
        D3DDECLTYPE_FLOAT2,
        D3DDECLTYPE_FLOAT3,
        D3DDECLTYPE_FLOAT4,
    ):
        count = decl_type + 1
        byte_count = count * 4
        if remaining < byte_count:
            return None
        floats = struct.unpack_from("<" + "f" * count, data, offset)
        return [int(round(value)) for value in floats]
    if decl_type in (D3DDECLTYPE_FLOAT16_2, D3DDECLTYPE_FLOAT16_4):
        count = 2 if decl_type == D3DDECLTYPE_FLOAT16_2 else 4
        byte_count = count * 2
        if remaining < byte_count:
            return None
        halves = struct.unpack_from("<" + "H" * count, data, offset)
        return [int(value) for value in halves]
    return None


def measure_element(meta_path: Path, meta: dict[str, str], element: Element) -> dict[str, Any]:
    idx_path = index_file(meta_path)
    stream_path = stream_file(meta_path, element.stream)
    if not idx_path.exists() or not stream_path.exists():
        return base_row(meta_path, meta, element, "missing_payload")

    indices = read_indices(idx_path, meta.get("index_type", "uint16"))
    unique_indices = sorted(set(indices))
    stream_bytes = stream_path.read_bytes()
    stream_offset = as_int(meta.get(f"stream{element.stream}_offset"))
    stream_start = as_int(meta.get(f"stream{element.stream}_start_byte"))
    stride = as_int(meta.get(f"hot_stream{element.stream}_stride"))
    if stride == 0:
        stride = as_int(meta.get(f"stream{element.stream}_stride"))
    if stride == 0:
        stride = as_int(meta.get(f"vertex_decl_stream{element.stream}_stride"))
    if stride == 0:
        stride = as_int(meta.get(f"vertex_decl_stream{element.stream}_computed_stride"))
    if stride == 0:
        return base_row(meta_path, meta, element, "missing_stride")

    base_vertex = as_int(meta.get("base_vertex"))
    result = BlendRange()
    component_count = 0
    for index in unique_indices:
        vertex = base_vertex + index
        byte_offset = stream_offset + vertex * stride + element.offset - stream_start
        values = decode_decl_values(stream_bytes, byte_offset, element.type)
        if values is None:
            result.missing += 1
            continue
        component_count = max(component_count, len(values))
        result.sampled += 1
        for component, value in enumerate(values[:4]):
            result.values[component].add(value)

    row = base_row(meta_path, meta, element, "ok" if result.sampled else "no_samples")
    row.update({
        "stride": stride,
        "indices": len(indices),
        "unique_indices": len(unique_indices),
        "vertices_sampled": result.sampled,
        "missing_vertices": result.missing,
        "component_count": component_count,
    })
    all_values = set().union(*result.values)
    for component, name in enumerate(COMPONENTS):
        values = result.values[component]
        row[f"min_{name}"] = min(values) if values else ""
        row[f"max_{name}"] = max(values) if values else ""
        row[f"unique_{name}"] = len(values)
    row["global_min"] = min(all_values) if all_values else ""
    row["global_max"] = max(all_values) if all_values else ""
    row["global_unique"] = len(all_values)
    if all_values:
        row["c_window_min"] = min(all_values)
        row["c_window_max_plus_2"] = max(all_values) + 2
        row["c_window_regs"] = max(all_values) - min(all_values) + 3
    return row


def base_row(meta_path: Path, meta: dict[str, str], element: Element | None, status: str) -> dict[str, Any]:
    return {
        "meta": str(meta_path),
        "seq": meta.get("seq", ""),
        "encoder": meta.get("encoder", ""),
        "draw_ordinal": meta.get("draw_ordinal", ""),
        "slot": meta.get("slot", ""),
        "vs": meta.get("vs", ""),
        "ps": meta.get("ps", ""),
        "element": element.index if element else "",
        "stream": element.stream if element else "",
        "offset": element.offset if element else "",
        "type": element.type if element else "",
        "stride": "",
        "indices": "",
        "unique_indices": "",
        "vertices_sampled": 0,
        "missing_vertices": 0,
        "component_count": 0,
        "min_x": "",
        "max_x": "",
        "unique_x": 0,
        "min_y": "",
        "max_y": "",
        "unique_y": 0,
        "min_z": "",
        "max_z": "",
        "unique_z": 0,
        "min_w": "",
        "max_w": "",
        "unique_w": 0,
        "global_min": "",
        "global_max": "",
        "global_unique": 0,
        "c_window_min": "",
        "c_window_max_plus_2": "",
        "c_window_regs": "",
        "status": status,
    }


def iter_rows(geometry_dir: Path) -> Iterable[dict[str, Any]]:
    for meta_path in sorted(geometry_dir.glob("*.meta")):
        meta = read_meta(meta_path)
        elements = [
            element for element in vertex_elements(meta)
            if element.usage == D3DDECLUSAGE_BLENDINDICES
        ]
        if not elements:
            yield base_row(meta_path, meta, None, "no_blendindices_decl")
            continue
        for element in elements:
            yield measure_element(meta_path, meta, element)


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=CSV_FIELDS, extrasaction="ignore")
        writer.writeheader()
        for row in rows:
            writer.writerow({field: row.get(field, "") for field in CSV_FIELDS})


def fmt(value: Any) -> str:
    text = str(value)
    return text if text else "-"


def write_markdown(path: Path, geometry_dir: Path, rows: list[dict[str, Any]], top: int) -> None:
    ok_rows = [row for row in rows if row.get("status") == "ok"]
    status_counts = Counter(str(row.get("status", "")) for row in rows)
    sampled = sum(as_int(row.get("vertices_sampled")) for row in ok_rows)
    global_values: set[int] = set()
    for row in ok_rows:
        for component in COMPONENTS:
            min_value = row.get(f"min_{component}")
            max_value = row.get(f"max_{component}")
            if min_value != "" and max_value != "":
                global_values.update(range(as_int(min_value), as_int(max_value) + 1))
    lines = [
        "# Blend Indices Geometry Report",
        "",
        f"- Geometry dir: `{geometry_dir}`",
        f"- Meta rows: `{len(rows)}`",
        f"- OK rows: `{len(ok_rows)}`",
        f"- Vertices sampled: `{sampled}`",
        f"- Status counts: `{dict(status_counts)}`",
    ]
    if global_values:
        lines.extend([
            f"- Observed global blend-index min/max: `{min(global_values)}` / `{max(global_values)}`",
            f"- Observed `c[a0 + 0..2]` window: `{min(global_values)}..{max(global_values) + 2}` (`{max(global_values) - min(global_values) + 3}` regs)",
        ])
    lines.extend([
        "",
        "| Seq/Enc | Draw | VS | stream | type | sampled | x | y | z | w | c-window | status |",
        "|---|---:|---|---:|---:|---:|---|---|---|---|---|---|",
    ])
    for row in ok_rows[:top]:
        lines.append(
            "| {seq}/{enc} | {draw} | `{vs}` | {stream} | {type} | {sampled} | "
            "{x} | {y} | {z} | {w} | {window} | {status} |".format(
                seq=row.get("seq", ""),
                enc=row.get("encoder", ""),
                draw=row.get("draw_ordinal", ""),
                vs=row.get("vs", ""),
                stream=row.get("stream", ""),
                type=row.get("type", ""),
                sampled=row.get("vertices_sampled", ""),
                x=f"{fmt(row.get('min_x'))}..{fmt(row.get('max_x'))}",
                y=f"{fmt(row.get('min_y'))}..{fmt(row.get('max_y'))}",
                z=f"{fmt(row.get('min_z'))}..{fmt(row.get('max_z'))}",
                w=f"{fmt(row.get('min_w'))}..{fmt(row.get('max_w'))}",
                window=f"{fmt(row.get('c_window_min'))}..{fmt(row.get('c_window_max_plus_2'))}",
                status=row.get("status", ""),
            )
        )
    if not ok_rows:
        lines.append("| - | - | - | - | - | - | - | - | - | - | - | no usable BLENDINDICES payload |")
    lines.extend([
        "",
        "## Interpretation",
        "",
        "- The reported c-window assumes the current indexed shader shape `c[a0.x/y + 0..2]`.",
        "- This is an offline observation over dumped draws, not a proof for every draw using the shader.",
        "- A production trim still needs either runtime validation/counters or a conservative per-resource bound before changing the cbuf ABI.",
        "",
    ])
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--geometry-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--csv-output", type=Path, required=True)
    parser.add_argument("--top", type=int, default=20)
    args = parser.parse_args()

    rows = list(iter_rows(args.geometry_dir))
    write_csv(args.csv_output, rows)
    write_markdown(args.output, args.geometry_dir, rows, args.top)
    print(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
