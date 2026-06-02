#!/usr/bin/env python3
"""Build a row-local 3DMark05 mini replay manifest.

The manifest joins reduced dxmt9/Xcode artifacts into a single JSON input for
the next mini replay harness:

* shader dump summary rows for hot-row shader source files;
* indexed probe draw rows for state, shader hashes, and draw identity;
* indexed geometry payload triplets for raw index/stream0 bytes.
"""

from __future__ import annotations

import argparse
import csv
import json
import re
from pathlib import Path
from typing import Any


def as_int(value: Any) -> int:
    try:
        text = str(value)
        if text.startswith(("0x", "0X")):
            return int(text, 16)
        return int(float(text))
    except (TypeError, ValueError):
        return 0


def load_csv(path: Path) -> list[dict[str, str]]:
    if not path.exists():
        raise SystemExit(f"missing CSV: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def row_key(row: dict[str, str]) -> str:
    seq = row.get("seq", "")
    enc = row.get("enc", row.get("encoder", ""))
    if seq == "" or enc == "":
        return ""
    return f"{seq}/{enc}"


def read_meta(path: Path) -> dict[str, str]:
    row: dict[str, str] = {"meta_file": str(path)}
    for line in path.read_text(encoding="utf-8").splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        row[key] = value
    stem = path.with_suffix("")
    index_path = Path(str(stem) + ".index.bin")
    stream_path = Path(str(stem) + ".stream0.bin")
    row["index_file"] = str(index_path)
    row["stream0_file"] = str(stream_path)
    row["index_file_bytes"] = str(index_path.stat().st_size if index_path.exists() else 0)
    row["stream0_file_bytes"] = str(stream_path.stat().st_size if stream_path.exists() else 0)
    row["index_file_exists"] = "1" if index_path.exists() else "0"
    row["stream0_file_exists"] = "1" if stream_path.exists() else "0"
    return row


def geometry_payload_valid(row: dict[str, str]) -> bool:
    return (
        row.get("index_range_valid") == "1"
        and row.get("stream0_range_valid") == "1"
        and row.get("wrote_index") == "1"
        and row.get("wrote_stream0") == "1"
        and row.get("index_file_exists") == "1"
        and row.get("stream0_file_exists") == "1"
        and as_int(row.get("index_file_bytes")) == as_int(row.get("index_byte_count"))
        and as_int(row.get("stream0_file_bytes")) == as_int(row.get("stream0_byte_count"))
    )


def geometry_draw_ordinal(row: dict[str, str]) -> int:
    if row.get("draw_ordinal"):
        return as_int(row.get("draw_ordinal"))
    # Older payload smoke artifacts wrote the global draw ordinal under this
    # name before encoder_draw_index/draw_ordinal were split.
    return as_int(row.get("encoder_draw"))


def geometry_encoder_draw_index(row: dict[str, str]) -> int | None:
    if row.get("encoder_draw_index"):
        return as_int(row.get("encoder_draw_index"))
    return None


def load_geometry_payloads(path: Path) -> list[dict[str, str]]:
    if not path.exists():
        raise SystemExit(f"missing geometry dir: {path}")
    rows = [read_meta(meta) for meta in sorted(path.glob("*.meta"))]
    if not rows:
        raise SystemExit(f"no geometry metadata files in: {path}")
    return rows


def shader_index(rows: list[dict[str, str]]) -> dict[str, dict[str, str]]:
    return {row_key(row): row for row in rows if row_key(row)}


def probe_index(rows: list[dict[str, str]]) -> dict[tuple[str, int], dict[str, str]]:
    out: dict[tuple[str, int], dict[str, str]] = {}
    for row in rows:
        key = row_key(row)
        ordinal = as_int(row.get("draw_ordinal"))
        if key and ordinal:
            out[(key, ordinal)] = row
    return out


def resolve_shader_file(shader_dir: Path, filename: str) -> str:
    if not filename:
        return ""
    path = Path(filename)
    if path.is_absolute():
        return str(path)
    return str(shader_dir / filename)


def shader_hash_to_decimal_text(value: str) -> str:
    if not value:
        return ""
    return str(as_int(value))


def scan_shader_dump_files(shader_dir: Path) -> dict[tuple[str, str], Path]:
    if not shader_dir.exists():
        return {}
    pattern = re.compile(r"^(?:translated|ffp)-(?P<stage>vs|fs)-shader-(?P<hash>\d+)-source-\d+\.metal$")
    out: dict[tuple[str, str], Path] = {}
    for path in sorted(shader_dir.glob("*.metal")):
        match = pattern.match(path.name)
        if not match:
            continue
        out[(match.group("stage"), match.group("hash"))] = path
    return out


def resolve_draw_shader_file(
    shader_files: dict[tuple[str, str], Path],
    stage: str,
    draw_hash: str,
    shader_dir: Path,
    row_filename: str,
) -> tuple[str, str]:
    direct = shader_files.get((stage, shader_hash_to_decimal_text(draw_hash)))
    if direct is not None:
        return str(direct), "draw-hash"
    fallback = resolve_shader_file(shader_dir, row_filename)
    return fallback, "row-fallback" if fallback else "missing"


def selected_payloads(
    geometries: list[dict[str, str]],
    target_row: str | None,
    target_vs: str | None,
    target_ps: str | None,
    max_draws: int,
) -> list[dict[str, str]]:
    rows = [
        row for row in geometries
        if geometry_payload_valid(row)
        and (target_row is None or row_key(row) == target_row)
        and (target_vs is None or row.get("vs", "").lower() == target_vs.lower())
        and (target_ps is None or row.get("ps", "").lower() == target_ps.lower())
    ]
    rows.sort(key=lambda row: (row_key(row), geometry_encoder_draw_index(row) or 10**18,
                               geometry_draw_ordinal(row), as_int(row.get("slot"))))
    return rows[:max_draws] if max_draws > 0 else rows


def build_manifest(args: argparse.Namespace) -> dict[str, Any]:
    shaders = shader_index(load_csv(args.shader_summary))
    probes = probe_index(load_csv(args.probe_draws))
    geometries = load_geometry_payloads(args.geometry_dir)
    shader_files = scan_shader_dump_files(args.shader_msl_dir)
    payloads = selected_payloads(geometries, args.row, args.vs, args.ps, args.max_draws)
    if not payloads:
        raise SystemExit("no valid geometry payloads matched the requested filters")

    manifest_draws: list[dict[str, Any]] = []
    missing_probe_rows = 0
    missing_shader_rows = 0
    missing_draw_shader_files = 0
    row_shader_fallbacks = 0
    for payload in payloads:
        key = row_key(payload)
        draw_ordinal = geometry_draw_ordinal(payload)
        probe = probes.get((key, draw_ordinal), {})
        shader = shaders.get(key, {})
        if not probe:
            missing_probe_rows += 1
        if not shader:
            missing_shader_rows += 1
        draw_vs_hash = probe.get("vs", payload.get("vs", ""))
        draw_ps_hash = probe.get("ps", payload.get("ps", ""))
        vs_file, vs_file_source = resolve_draw_shader_file(
            shader_files,
            "vs",
            draw_vs_hash,
            args.shader_msl_dir,
            shader.get("vs_file", ""),
        )
        ps_file, ps_file_source = resolve_draw_shader_file(
            shader_files,
            "fs",
            draw_ps_hash,
            args.shader_msl_dir,
            shader.get("ps_file", ""),
        )
        if vs_file_source == "missing" or ps_file_source == "missing":
            missing_draw_shader_files += 1
        if vs_file_source == "row-fallback" or ps_file_source == "row-fallback":
            row_shader_fallbacks += 1

        manifest_draws.append({
            "row": key,
            "seq": as_int(payload.get("seq")),
            "encoder": as_int(payload.get("encoder")),
            "encoder_draw_index": (
                geometry_encoder_draw_index(payload)
                if geometry_encoder_draw_index(payload) is not None
                else as_int(probe.get("encoder_draw_index"))
            ),
            "draw_ordinal": draw_ordinal,
            "state": {
                "primitive_type": as_int(probe.get("primitive_type")),
                "primitive_count": as_int(payload.get("primitive_count")),
                "index_count": as_int(payload.get("index_count")),
                "base_vertex": as_int(payload.get("base_vertex")),
                "start_index": as_int(payload.get("start_index")),
                "index_type": payload.get("index_type", probe.get("index_type", "")),
                "stream0_offset": as_int(payload.get("stream0_offset")),
                "stream0_stride": as_int(payload.get("stream0_stride")),
                "alpha_blend": as_int(probe.get("alpha_blend")),
                "src_blend": as_int(probe.get("src_blend")),
                "dst_blend": as_int(probe.get("dst_blend")),
                "blend_op": as_int(probe.get("blend_op")),
                "separate_alpha": as_int(probe.get("separate_alpha")),
                "src_blend_alpha": as_int(probe.get("src_blend_alpha")),
                "dst_blend_alpha": as_int(probe.get("dst_blend_alpha")),
                "blend_op_alpha": as_int(probe.get("blend_op_alpha")),
                "alpha_test": as_int(probe.get("alpha_test")),
                "depth_enabled": as_int(probe.get("depth_enabled")),
                "depth_write": as_int(probe.get("depth_write")),
                "depth_func": as_int(probe.get("depth_func")),
                "scissor": as_int(probe.get("scissor")),
                "scissor_l": as_int(probe.get("scissor_l")),
                "scissor_t": as_int(probe.get("scissor_t")),
                "scissor_r": as_int(probe.get("scissor_r")),
                "scissor_b": as_int(probe.get("scissor_b")),
                "cull": as_int(probe.get("cull")),
                "fill": as_int(probe.get("fill")),
                "texture_mask": probe.get("texture_mask", ""),
                "color_write": probe.get("color_write", ""),
            },
            "shaders": {
                "vs_hash": draw_vs_hash,
                "ps_hash": draw_ps_hash,
                "vsout": probe.get("vsout", ""),
                "row_vs_hash": shader.get("dxmt_vertex_shader_last", ""),
                "row_ps_hash": shader.get("dxmt_pixel_shader_last", ""),
                "vs_file": vs_file,
                "ps_file": ps_file,
                "vs_file_source": vs_file_source,
                "ps_file_source": ps_file_source,
                "vsout_fields": shader.get("vsout_fields", ""),
                "ps_vsout_read_fields": shader.get("ps_vsout_read_fields", ""),
            },
            "geometry": {
                "meta_file": payload.get("meta_file", ""),
                "index_file": payload.get("index_file", ""),
                "stream0_file": payload.get("stream0_file", ""),
                "index_bytes": as_int(payload.get("index_file_bytes")),
                "stream0_bytes": as_int(payload.get("stream0_file_bytes")),
                "min_index": as_int(payload.get("min_index")),
                "max_index": as_int(payload.get("max_index")),
                "unique_indices": as_int(payload.get("unique_indices")),
                "cache_miss_64": as_int(payload.get("cache_miss_64")),
            },
        })

    rows = sorted({draw["row"] for draw in manifest_draws})
    total_index_bytes = sum(draw["geometry"]["index_bytes"] for draw in manifest_draws)
    total_stream0_bytes = sum(draw["geometry"]["stream0_bytes"] for draw in manifest_draws)
    return {
        "schema": "dxmt9.3dmark05.mini_replay_manifest.v1",
        "sources": {
            "shader_summary": str(args.shader_summary),
            "probe_draws": str(args.probe_draws),
            "geometry_dir": str(args.geometry_dir),
            "shader_msl_dir": str(args.shader_msl_dir),
            "row_filter": args.row or "",
            "vs_filter": args.vs or "",
            "ps_filter": args.ps or "",
        },
        "summary": {
            "rows": rows,
            "draw_count": len(manifest_draws),
            "total_index_bytes": total_index_bytes,
            "total_stream0_bytes": total_stream0_bytes,
            "missing_probe_rows": missing_probe_rows,
            "missing_shader_rows": missing_shader_rows,
            "missing_draw_shader_files": missing_draw_shader_files,
            "row_shader_fallbacks": row_shader_fallbacks,
        },
        "draws": manifest_draws,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--shader-summary", type=Path, required=True)
    parser.add_argument("--probe-draws", type=Path, required=True)
    parser.add_argument("--geometry-dir", type=Path, required=True)
    parser.add_argument("--shader-msl-dir", type=Path)
    parser.add_argument("--row", help="Optional seq/encoder filter, e.g. 60/2")
    parser.add_argument("--vs", help="Optional vertex shader hash filter, e.g. 0x7836...")
    parser.add_argument("--ps", help="Optional pixel shader hash filter, e.g. 0x11cc...")
    parser.add_argument("--max-draws", type=int, default=0)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.shader_msl_dir is None:
        args.shader_msl_dir = args.shader_summary.parent / "shaders" / "msl"

    manifest = build_manifest(args)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(manifest, indent=2, sort_keys=True), encoding="utf-8")
    print(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
