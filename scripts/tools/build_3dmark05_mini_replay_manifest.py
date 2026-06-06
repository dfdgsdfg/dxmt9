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

from analyze_shader_dumps import parse_vsout_fields, stage_in_read_fields


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
    for stream in range(1, 16):
        cbuf_path = Path(str(stem) + f".stream{stream}.bin")
        row[f"stream{stream}_file"] = str(cbuf_path)
        row[f"stream{stream}_file_bytes"] = str(cbuf_path.stat().st_size if cbuf_path.exists() else 0)
        row[f"stream{stream}_file_exists"] = "1" if cbuf_path.exists() else "0"
    for key, suffix in [
        ("vsconsts", ".vsconsts.bin"),
        ("psconsts", ".psconsts.bin"),
        ("ffpvs", ".ffpvs.bin"),
        ("ffpps", ".ffpps.bin"),
    ]:
        cbuf_path = Path(str(stem) + suffix)
        row[f"{key}_file"] = str(cbuf_path)
        row[f"{key}_file_bytes"] = str(cbuf_path.stat().st_size if cbuf_path.exists() else 0)
        row[f"{key}_file_exists"] = "1" if cbuf_path.exists() else "0"
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


def stream_payload_metadata(payload: dict[str, str]) -> list[dict[str, Any]]:
    streams: list[dict[str, Any]] = [{
        "stream": 0,
        "metal_slot": 1,
        "handle": payload.get("stream0_handle", ""),
        "file": payload.get("stream0_file", ""),
        "bytes": as_int(payload.get("stream0_file_bytes")),
        "offset": as_int(payload.get("stream0_offset")),
        "stride": as_int(payload.get("stream0_stride")),
        "start_byte": as_int(payload.get("stream0_start_byte")),
        "byte_count": as_int(payload.get("stream0_byte_count")),
        "range_valid": as_int(payload.get("stream0_range_valid")),
        "wrote": as_int(payload.get("wrote_stream0")),
    }]
    for stream in range(1, 16):
        if as_int(payload.get(f"wrote_stream{stream}")) == 0:
            continue
        streams.append({
            "stream": stream,
            "metal_slot": as_int(payload.get(f"stream{stream}_metal_slot")),
            "handle": payload.get(f"stream{stream}_handle", ""),
            "file": payload.get(f"stream{stream}_file", ""),
            "bytes": as_int(payload.get(f"stream{stream}_file_bytes")),
            "offset": as_int(payload.get(f"stream{stream}_offset")),
            "stride": as_int(payload.get(f"stream{stream}_stride")),
            "start_byte": as_int(payload.get(f"stream{stream}_start_byte")),
            "byte_count": as_int(payload.get(f"stream{stream}_byte_count")),
            "range_valid": as_int(payload.get(f"stream{stream}_range_valid")),
            "wrote": as_int(payload.get(f"wrote_stream{stream}")),
        })
    return streams


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


def geometry_encoder_draw_sort_index(row: dict[str, str]) -> int:
    index = geometry_encoder_draw_index(row)
    return index if index is not None else 10**18


def load_geometry_payloads(path: Path) -> list[dict[str, str]]:
    if not path.exists():
        raise SystemExit(f"missing geometry dir: {path}")
    rows = [read_meta(meta) for meta in sorted(path.glob("*.meta"))]
    if not rows:
        raise SystemExit(f"no geometry metadata files in: {path}")
    return rows


def parse_int_filter(value: str | None) -> set[int]:
    if not value:
        return set()
    out: set[int] = set()
    for item in re.split(r"[,;\s]+", value.strip()):
        if item:
            out.add(as_int(item))
    return out


def parse_draw_ordinals(value: str | None) -> set[int]:
    return parse_int_filter(value)


def apply_payload_selection(args: argparse.Namespace) -> None:
    if args.payload_selection is None:
        return
    if not args.payload_selection.exists():
        raise SystemExit(f"missing payload selection JSON: {args.payload_selection}")
    with args.payload_selection.open(encoding="utf-8") as handle:
        selection = json.load(handle)
    if selection.get("schema") != "dxmt9.3dmark05.payload_window.v1":
        raise SystemExit(f"unsupported payload selection schema: {selection.get('schema')}")
    data = selection.get("selection", {})
    window = data.get("window", {})
    selected_row = str(data.get("row", ""))
    if selected_row:
        if args.row and args.row != selected_row:
            raise SystemExit(f"--row {args.row} conflicts with selection row {selected_row}")
        args.row = selected_row
    if args.encoder_draw_min is None and "encoder_draw_min" in window:
        args.encoder_draw_min = as_int(window.get("encoder_draw_min"))
    if args.encoder_draw_max is None and "encoder_draw_max" in window:
        args.encoder_draw_max = as_int(window.get("encoder_draw_max"))
    if not args.draw_ordinals and window.get("draw_ordinals"):
        args.draw_ordinals = ",".join(str(as_int(value)) for value in window.get("draw_ordinals", []))


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


def shader_layout_metadata(
    vs_file: str,
    ps_file: str,
    fallback_vsout_fields: str,
    fallback_ps_read_fields: str,
) -> tuple[str, str]:
    vs_path = Path(vs_file)
    ps_path = Path(ps_file)
    if not vs_path.is_file():
        return fallback_vsout_fields, fallback_ps_read_fields

    vs_fields = parse_vsout_fields(vs_path.read_text(encoding="utf-8"))
    if not vs_fields:
        return fallback_vsout_fields, fallback_ps_read_fields

    vsout_fields = ",".join(name for _, name in vs_fields)
    if not ps_path.is_file():
        return vsout_fields, fallback_ps_read_fields

    ps_reads, _ = stage_in_read_fields(ps_path.read_text(encoding="utf-8"), vs_fields)
    return vsout_fields, ",".join(ps_reads) if ps_reads else fallback_ps_read_fields


def selected_payloads(
    geometries: list[dict[str, str]],
    target_row: str | None,
    target_vs: str | None,
    target_ps: str | None,
    encoder_draw_min: int | None,
    encoder_draw_max: int | None,
    target_encoder_draw_indices: set[int],
    target_draw_ordinals: set[int],
    max_draws: int,
) -> list[dict[str, str]]:
    rows = [
        row for row in geometries
        if geometry_payload_valid(row)
        and (target_row is None or row_key(row) == target_row)
        and (target_vs is None or row.get("vs", "").lower() == target_vs.lower())
        and (target_ps is None or row.get("ps", "").lower() == target_ps.lower())
        and (
            encoder_draw_min is None
            or (
                geometry_encoder_draw_index(row) is not None
                and geometry_encoder_draw_index(row) >= encoder_draw_min
            )
        )
        and (
            encoder_draw_max is None
            or (
                geometry_encoder_draw_index(row) is not None
                and geometry_encoder_draw_index(row) <= encoder_draw_max
            )
        )
        and (
            not target_encoder_draw_indices
            or geometry_encoder_draw_index(row) in target_encoder_draw_indices
        )
        and (not target_draw_ordinals or geometry_draw_ordinal(row) in target_draw_ordinals)
    ]
    rows.sort(key=lambda row: (row_key(row), geometry_encoder_draw_sort_index(row),
                               geometry_draw_ordinal(row), as_int(row.get("slot"))))
    return rows[:max_draws] if max_draws > 0 else rows


def texture_metadata(payload: dict[str, str]) -> list[dict[str, Any]]:
    textures: list[dict[str, Any]] = []
    for stage in range(8):
        handle = payload.get(f"texture{stage}_handle", "")
        if as_int(handle) == 0:
            continue
        textures.append({
            "stage": stage,
            "handle": handle,
            "lod": as_int(payload.get(f"texture{stage}_lod")),
            "format": as_int(payload.get(f"texture{stage}_format")),
            "type": as_int(payload.get(f"texture{stage}_type")),
            "pool": as_int(payload.get(f"texture{stage}_pool")),
            "usage": payload.get(f"texture{stage}_usage", ""),
            "width": as_int(payload.get(f"texture{stage}_width")),
            "height": as_int(payload.get(f"texture{stage}_height")),
            "depth": as_int(payload.get(f"texture{stage}_depth")),
            "levels": as_int(payload.get(f"texture{stage}_levels")),
            "has_metal_texture": as_int(payload.get(f"texture{stage}_has_metal_texture")),
            "has_shader_read_texture": as_int(payload.get(f"texture{stage}_has_shader_read_texture")),
            "has_srgb_shader_read_texture": as_int(payload.get(f"texture{stage}_has_srgb_shader_read_texture")),
            "missing_record": as_int(payload.get(f"texture{stage}_missing_record")),
        })
    return textures


def surface_metadata(payload: dict[str, str], prefix: str) -> dict[str, Any]:
    return {
        "handle": payload.get(f"{prefix}_handle", ""),
        "level": as_int(payload.get(f"{prefix}_level")),
        "sample_count": as_int(payload.get(f"{prefix}_sample_count")),
        "format": as_int(payload.get(f"{prefix}_format")),
        "pool": as_int(payload.get(f"{prefix}_pool")),
        "usage": payload.get(f"{prefix}_usage", ""),
        "width": as_int(payload.get(f"{prefix}_width")),
        "height": as_int(payload.get(f"{prefix}_height")),
        "bytes_per_pixel": as_int(payload.get(f"{prefix}_bytes_per_pixel")),
        "render_target": as_int(payload.get(f"{prefix}_render_target")),
        "depth_stencil": as_int(payload.get(f"{prefix}_depth_stencil")),
        "has_metal_texture": as_int(payload.get(f"{prefix}_has_metal_texture")),
        "has_srgb_texture": as_int(payload.get(f"{prefix}_has_srgb_texture")),
        "has_resolve_texture": as_int(payload.get(f"{prefix}_has_resolve_texture")),
        "alias_texture": payload.get(f"{prefix}_alias_texture", ""),
        "alias_level": as_int(payload.get(f"{prefix}_alias_level")),
        "alias_slice": as_int(payload.get(f"{prefix}_alias_slice")),
        "alias_texture_format": as_int(payload.get(f"{prefix}_alias_texture_format")),
        "alias_texture_type": as_int(payload.get(f"{prefix}_alias_texture_type")),
        "alias_texture_usage": payload.get(f"{prefix}_alias_texture_usage", ""),
        "alias_texture_width": as_int(payload.get(f"{prefix}_alias_texture_width")),
        "alias_texture_height": as_int(payload.get(f"{prefix}_alias_texture_height")),
        "alias_texture_levels": as_int(payload.get(f"{prefix}_alias_texture_levels")),
        "missing_surface": as_int(payload.get(f"{prefix}_missing_surface")),
    }


def attachment_metadata(payload: dict[str, str]) -> dict[str, Any]:
    colors = []
    for index in range(4):
        prefix = f"attachment_color{index}"
        if as_int(payload.get(f"{prefix}_handle")) == 0:
            continue
        item = surface_metadata(payload, prefix)
        item["index"] = index
        colors.append(item)
    depth = {}
    if as_int(payload.get("attachment_depth_handle")) != 0:
        depth = surface_metadata(payload, "attachment_depth")
    return {
        "colors": colors,
        "depth": depth,
    }


def normalized_texture_handle(value: str) -> str:
    handle = as_int(value)
    return f"0x{handle:x}" if handle else ""


def texture_sidecar_ready(texture: dict[str, Any]) -> bool:
    return (
        as_int(texture.get("missing_record")) == 0
        and as_int(texture.get("handle")) != 0
        and (
            as_int(texture.get("has_metal_texture")) != 0
            or as_int(texture.get("has_shader_read_texture")) != 0
            or as_int(texture.get("has_srgb_shader_read_texture")) != 0
        )
    )


def texture_handles_summary(manifest_draws: list[dict[str, Any]]) -> dict[str, Any]:
    all_handles: set[str] = set()
    capture_handles: set[str] = set()
    for draw in manifest_draws:
        for texture in draw.get("textures", []):
            handle = normalized_texture_handle(str(texture.get("handle", "")))
            if not handle:
                continue
            all_handles.add(handle)
            if texture_sidecar_ready(texture):
                capture_handles.add(handle)

    ordered_all = sorted(all_handles, key=as_int)
    ordered_capture = sorted(capture_handles, key=as_int)
    capture_arg = ",".join(ordered_capture)
    flags: list[str] = []
    if capture_arg:
        flags = ["--dump-draw-texture-handles", capture_arg]
        seqs = sorted({as_int(draw.get("seq")) for draw in manifest_draws if as_int(draw.get("seq"))})
        encoders = sorted({as_int(draw.get("encoder")) for draw in manifest_draws if as_int(draw.get("encoder"))})
        if len(seqs) == 1:
            flags.extend(["--dump-draw-texture-seq", str(seqs[0])])
        if len(encoders) == 1:
            flags.extend(["--dump-draw-texture-enc", str(encoders[0])])

    return {
        "texture_handles": ordered_all,
        "texture_handle_count": len(ordered_all),
        "texture_capture_handles": ordered_capture,
        "texture_capture_handle_count": len(ordered_capture),
        "texture_capture_handles_arg": capture_arg,
        "texture_capture_flags": flags,
    }


def build_manifest(args: argparse.Namespace) -> dict[str, Any]:
    apply_payload_selection(args)
    shaders = shader_index(load_csv(args.shader_summary))
    probes = probe_index(load_csv(args.probe_draws))
    geometries = load_geometry_payloads(args.geometry_dir)
    shader_files = scan_shader_dump_files(args.shader_msl_dir)
    target_encoder_draw_indices = parse_int_filter(args.encoder_draw_indices)
    target_draw_ordinals = parse_draw_ordinals(args.draw_ordinals)
    payloads = selected_payloads(
        geometries,
        args.row,
        args.vs,
        args.ps,
        args.encoder_draw_min,
        args.encoder_draw_max,
        target_encoder_draw_indices,
        target_draw_ordinals,
        args.max_draws,
    )
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
        vsout_fields, ps_vsout_read_fields = shader_layout_metadata(
            vs_file,
            ps_file,
            shader.get("vsout_fields", ""),
            shader.get("ps_vsout_read_fields", ""),
        )

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
                "vsout_fields": vsout_fields,
                "ps_vsout_read_fields": ps_vsout_read_fields,
            },
            "geometry": {
                "meta_file": payload.get("meta_file", ""),
                "index_file": payload.get("index_file", ""),
                "stream0_file": payload.get("stream0_file", ""),
                "index_bytes": as_int(payload.get("index_file_bytes")),
                "stream0_bytes": as_int(payload.get("stream0_file_bytes")),
                "streams": stream_payload_metadata(payload),
                "min_index": as_int(payload.get("min_index")),
                "max_index": as_int(payload.get("max_index")),
                "unique_indices": as_int(payload.get("unique_indices")),
                "cache_miss_64": as_int(payload.get("cache_miss_64")),
            },
            "uniforms": {
                "vsconsts_file": payload.get("vsconsts_file", ""),
                "psconsts_file": payload.get("psconsts_file", ""),
                "ffpvs_file": payload.get("ffpvs_file", ""),
                "ffpps_file": payload.get("ffpps_file", ""),
                "vsconsts_bytes": as_int(payload.get("vsconsts_file_bytes")),
                "psconsts_bytes": as_int(payload.get("psconsts_file_bytes")),
                "ffpvs_bytes": as_int(payload.get("ffpvs_file_bytes")),
                "ffpps_bytes": as_int(payload.get("ffpps_file_bytes")),
                "wrote_vsconsts": as_int(payload.get("wrote_vsconsts")),
                "wrote_psconsts": as_int(payload.get("wrote_psconsts")),
                "wrote_ffpvs": as_int(payload.get("wrote_ffpvs")),
                "wrote_ffpps": as_int(payload.get("wrote_ffpps")),
            },
            "textures": texture_metadata(payload),
            "attachments": attachment_metadata(payload),
        })

    rows = sorted({draw["row"] for draw in manifest_draws})
    total_index_bytes = sum(draw["geometry"]["index_bytes"] for draw in manifest_draws)
    total_stream0_bytes = sum(draw["geometry"]["stream0_bytes"] for draw in manifest_draws)
    encoder_draw_indices = [
        draw["encoder_draw_index"]
        for draw in manifest_draws
        if draw["encoder_draw_index"] is not None
    ]
    texture_summary = texture_handles_summary(manifest_draws)
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
            "payload_selection": str(args.payload_selection or ""),
            "encoder_draw_min": "" if args.encoder_draw_min is None else str(args.encoder_draw_min),
            "encoder_draw_max": "" if args.encoder_draw_max is None else str(args.encoder_draw_max),
            "encoder_draw_indices_filter": ",".join(
                str(value) for value in sorted(target_encoder_draw_indices)
            ),
            "draw_ordinals_filter": ",".join(str(value) for value in sorted(target_draw_ordinals)),
        },
        "summary": {
            "rows": rows,
            "draw_count": len(manifest_draws),
            "total_index_bytes": total_index_bytes,
            "total_stream0_bytes": total_stream0_bytes,
            "encoder_draw_min": min(encoder_draw_indices) if encoder_draw_indices else None,
            "encoder_draw_max": max(encoder_draw_indices) if encoder_draw_indices else None,
            "draw_ordinals": [draw["draw_ordinal"] for draw in manifest_draws],
            "missing_probe_rows": missing_probe_rows,
            "missing_shader_rows": missing_shader_rows,
            "missing_draw_shader_files": missing_draw_shader_files,
            "row_shader_fallbacks": row_shader_fallbacks,
            **texture_summary,
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
    parser.add_argument("--payload-selection", type=Path,
                        help="Selection JSON from select_3dmark05_payload_window.py")
    parser.add_argument("--encoder-draw-min", type=int,
                        help="Optional inclusive encoder-local draw index filter")
    parser.add_argument("--encoder-draw-max", type=int,
                        help="Optional inclusive encoder-local draw index filter")
    parser.add_argument("--encoder-draw-indices",
                        help="Optional comma/space separated encoder-local draw index filter")
    parser.add_argument("--draw-ordinals",
                        help="Optional comma/space separated global draw ordinal filter")
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
