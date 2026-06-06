#!/usr/bin/env python3
"""Prepare and optionally run a standalone Metal mini replay from a manifest.

The 3DMark05 mini replay manifest contains raw stream0/index payloads and the
draw-hash MSL files for a small hot draw set. dxmt9's dumped MSL uses a
buffer(30) argument-buffer layout. This helper rewrites that signature into
plain constant-buffer slots so a standalone Metal process can bind zeroed/dummy
constant buffers without recreating dxmt9's full argument-buffer machinery.
"""

from __future__ import annotations

import argparse
import copy
import json
import os
import re
import shlex
import shutil
import struct
import subprocess
from pathlib import Path
from typing import Any

from analyze_shader_dumps import parse_vsout_fields, stage_in_read_fields


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_MIN_CAPTURE_FREE_MB = 2048
VSOUT_RE = re.compile(r"struct\s+VSOut\s*\{(?P<body>.*?)\};", re.S)


def load_manifest(path: Path) -> dict[str, Any]:
    if not path.exists():
        raise SystemExit(f"missing manifest: {path}")
    with path.open(encoding="utf-8") as handle:
        data = json.load(handle)
    if data.get("schema") != "dxmt9.3dmark05.mini_replay_manifest.v1":
        raise SystemExit(f"unsupported manifest schema: {data.get('schema')}")
    if not data.get("draws"):
        raise SystemExit("manifest has no draws")
    return data


def resolve_path(path: str) -> Path:
    value = Path(path)
    return value if value.is_absolute() else REPO_ROOT / value


def validate_payloads(draws: list[dict[str, Any]]) -> None:
    for index, draw in enumerate(draws):
        geometry = draw.get("geometry", {})
        for key in ("index_file", "stream0_file"):
            path = resolve_path(str(geometry.get(key, "")))
            if not path.exists():
                raise SystemExit(f"draw {index}: missing {key}: {path}")
        if int(geometry.get("index_bytes", 0)) <= 0:
            raise SystemExit(f"draw {index}: index payload is empty")
        if int(geometry.get("stream0_bytes", 0)) <= 0:
            raise SystemExit(f"draw {index}: stream0 payload is empty")
        uniforms = draw.get("uniforms", {})
        for key in ("vsconsts", "psconsts", "ffpvs", "ffpps"):
            byte_count = int(uniforms.get(f"{key}_bytes", 0))
            path_text = str(uniforms.get(f"{key}_file", ""))
            if byte_count == 0 and not path_text:
                continue
            if byte_count > 0 and not path_text:
                raise SystemExit(f"draw {index}: missing {key} payload path")
            path = resolve_path(path_text)
            if byte_count > 0 and not path.exists():
                raise SystemExit(f"draw {index}: missing {key} payload: {path}")


def optimize_triangle_order_for_vertex_cache(indices: list[int], cache_size: int) -> list[int]:
    triangle_count = len(indices) // 3
    triangles = []
    vertex_triangles: dict[int, list[int]] = {}
    remaining_vertex_use: dict[int, int] = {}
    for triangle in range(triangle_count):
        values = tuple(indices[triangle * 3:(triangle + 1) * 3])
        triangles.append({
            "indices": values,
            "min_index": min(values),
            "original": triangle,
        })
        for index in values:
            vertex_triangles.setdefault(index, []).append(triangle)
            remaining_vertex_use[index] = remaining_vertex_use.get(index, 0) + 1

    cache: list[int] = []
    candidates: list[int] = []
    emitted = [False] * triangle_count
    in_candidates = [False] * triangle_count
    order: list[int] = []
    next_original = 0

    def cache_position(index: int) -> int | None:
        try:
            return cache.index(index)
        except ValueError:
            return None

    def add_candidate(triangle: int) -> None:
        if triangle < 0 or triangle >= triangle_count:
            return
        if emitted[triangle] or in_candidates[triangle]:
            return
        candidates.append(triangle)
        in_candidates[triangle] = True

    def add_vertex_neighbors(index: int) -> None:
        for triangle in vertex_triangles.get(index, []):
            add_candidate(triangle)

    def choose_best_candidate() -> int | None:
        best_slot: int | None = None
        best_score: int | None = None
        for slot, candidate in enumerate(candidates):
            if candidate >= triangle_count or emitted[candidate]:
                continue
            tri = triangles[candidate]
            cached_vertices = 0
            cache_distance = 0
            remaining_use = 0
            for index in tri["indices"]:
                pos = cache_position(index)
                if pos is not None:
                    cached_vertices += 1
                    cache_distance += pos
                remaining_use += remaining_vertex_use.get(index, 0)
            score = (
                cached_vertices * 1_000_000
                - cache_distance * 1_000
                - remaining_use * 10
                - tri["min_index"] // 256
            )
            if (
                best_score is None
                or score > best_score
                or (
                    score == best_score
                    and (
                        best_slot is None
                        or tri["original"] < triangles[candidates[best_slot]]["original"]
                    )
                )
            ):
                best_score = score
                best_slot = slot
        if best_slot is None:
            return None
        chosen = candidates[best_slot]
        in_candidates[chosen] = False
        candidates[best_slot] = candidates[-1]
        candidates.pop()
        return chosen

    def choose_next_original() -> int | None:
        nonlocal next_original
        while next_original < triangle_count and emitted[next_original]:
            next_original += 1
        if next_original >= triangle_count:
            return None
        return next_original

    def touch_cache_vertex(index: int) -> None:
        pos = cache_position(index)
        if pos is not None:
            cache.pop(pos)
            cache.insert(0, index)
            return
        cache.insert(0, index)
        if len(cache) > cache_size:
            cache.pop()

    while len(order) < triangle_count:
        candidate = choose_best_candidate()
        chosen = candidate if candidate is not None else choose_next_original()
        if chosen is None:
            break
        if chosen >= triangle_count or emitted[chosen]:
            continue
        emitted[chosen] = True
        in_candidates[chosen] = False
        order.append(chosen)
        for index in triangles[chosen]["indices"]:
            if remaining_vertex_use.get(index, 0) > 0:
                remaining_vertex_use[index] -= 1
            touch_cache_vertex(index)
        for index in triangles[chosen]["indices"]:
            add_vertex_neighbors(index)

    if len(order) != triangle_count:
        return indices
    if all(triangle == original for original, triangle in enumerate(order)):
        return indices
    ordered = [index for triangle in order for index in triangles[triangle]["indices"]]
    ordered.extend(indices[triangle_count * 3:])
    return ordered


def transform_index_payload(payload: bytes, primitive_order: str) -> bytes:
    if primitive_order == "original":
        return payload
    if len(payload) % 2:
        raise SystemExit("index payload byte length is not uint16-aligned")
    indices = list(struct.unpack(f"<{len(payload) // 2}H", payload))
    triangle_count = len(indices) // 3
    triangles = [indices[i * 3:(i + 1) * 3] for i in range(triangle_count)]
    tail = indices[triangle_count * 3:]
    if primitive_order == "reverse-triangles":
        triangles.reverse()
    elif primitive_order == "sort-min-index":
        triangles.sort(key=lambda tri: (min(tri), max(tri), tri[0], tri[1], tri[2]))
    elif primitive_order == "sort-max-index":
        triangles.sort(key=lambda tri: (max(tri), min(tri), tri[0], tri[1], tri[2]))
    elif primitive_order == "cache-opt-lru32":
        ordered = optimize_triangle_order_for_vertex_cache(indices, 32)
        return struct.pack(f"<{len(ordered)}H", *ordered)
    elif primitive_order == "cache-opt-lru64":
        ordered = optimize_triangle_order_for_vertex_cache(indices, 64)
        return struct.pack(f"<{len(ordered)}H", *ordered)
    else:
        raise SystemExit(f"unsupported primitive order: {primitive_order}")
    ordered = [index for tri in triangles for index in tri] + tail
    return struct.pack(f"<{len(ordered)}H", *ordered)


def uint16_indices(payload: bytes) -> list[int]:
    if len(payload) % 2:
        raise SystemExit("index payload byte length is not uint16-aligned")
    return list(struct.unpack(f"<{len(payload) // 2}H", payload))


def lru_cache_misses(indices: list[int], cache_size: int) -> int:
    cache: list[int] = []
    misses = 0
    for index in indices:
        if index in cache:
            cache.remove(index)
            cache.insert(0, index)
            continue
        misses += 1
        cache.insert(0, index)
        if len(cache) > cache_size:
            cache.pop()
    return misses


def index_cache_estimate(draws: list[dict[str, Any]],
                         cache_sizes: tuple[int, ...] = (16, 32, 64)) -> dict[str, Any]:
    result: dict[str, Any] = {
        "draws": len(draws),
        "cache_sizes": list(cache_sizes),
        "original_index_count": 0,
        "replay_index_count": 0,
        "original_unique_indices_per_draw": 0,
        "replay_unique_indices_per_draw": 0,
    }
    for cache_size in cache_sizes:
        result[f"original_lru{cache_size}_misses"] = 0
        result[f"replay_lru{cache_size}_misses"] = 0

    for draw in draws:
        geometry = draw["geometry"]
        replay_indices = uint16_indices(resolve_path(str(geometry["index_file"])).read_bytes())
        original_path = resolve_path(str(geometry.get("index_order_source_file", geometry["index_file"])))
        original_indices = uint16_indices(original_path.read_bytes())
        result["original_index_count"] += len(original_indices)
        result["replay_index_count"] += len(replay_indices)
        result["original_unique_indices_per_draw"] += len(set(original_indices))
        result["replay_unique_indices_per_draw"] += len(set(replay_indices))
        for cache_size in cache_sizes:
            result[f"original_lru{cache_size}_misses"] += lru_cache_misses(original_indices, cache_size)
            result[f"replay_lru{cache_size}_misses"] += lru_cache_misses(replay_indices, cache_size)

    for cache_size in cache_sizes:
        original = int(result[f"original_lru{cache_size}_misses"])
        replay = int(result[f"replay_lru{cache_size}_misses"])
        result[f"replay_lru{cache_size}_miss_delta"] = replay - original
        result[f"replay_lru{cache_size}_miss_delta_pct"] = (
            ((replay - original) / original) * 100.0 if original else 0.0
        )
    return result


def materialize_replay_draws(draws: list[dict[str, Any]],
                             output_dir: Path,
                             primitive_order: str,
                             draw_order: str) -> list[dict[str, Any]]:
    replay_draws = copy.deepcopy(draws)
    if primitive_order != "original":
        index_dir = output_dir / "index-order"
        index_dir.mkdir(parents=True, exist_ok=True)
        for ordinal, draw in enumerate(replay_draws):
            geometry = draw["geometry"]
            source = resolve_path(str(geometry["index_file"]))
            transformed = transform_index_payload(source.read_bytes(), primitive_order)
            target = index_dir / f"draw{ordinal:03d}-{primitive_order}.index.bin"
            target.write_bytes(transformed)
            geometry["index_file"] = str(target)
            geometry["index_order_source_file"] = str(source)
            geometry["index_order"] = primitive_order
            geometry["index_bytes"] = len(transformed)
    if draw_order == "reverse":
        replay_draws.reverse()
    elif draw_order != "original":
        raise SystemExit(f"unsupported draw order: {draw_order}")
    return replay_draws


def optional_payload_path(uniforms: dict[str, Any], key: str) -> str:
    if int(uniforms.get(f"{key}_bytes", 0)) <= 0:
        return ""
    return str(resolve_path(str(uniforms.get(f"{key}_file", ""))))


def stage_bindings(source: str) -> dict[str, list[int]]:
    bindings: dict[str, list[int]] = {"buffer": [], "texture": [], "sampler": []}
    for kind in bindings:
        values = sorted({int(match) for match in re.findall(rf"\[\[{kind}\((\d+)\)\]\]", source)})
        bindings[kind] = values
    return bindings


def used_buffer_indices(source: str) -> set[int]:
    return {int(match) for match in re.findall(r"\[\[buffer\((\d+)\)\]\]", source)}


def allocate_cbuf_slots(source: str, count: int) -> list[int]:
    used = used_buffer_indices(source)
    slots: list[int] = []
    for candidate in range(29, -1, -1):
        if candidate == 30 or candidate in used:
            continue
        slots.append(candidate)
        if len(slots) == count:
            return slots
    raise SystemExit("not enough free Metal buffer slots for mini replay cbuf rewrite")


def replace_argbuf_parameter(source: str, replacement: str) -> str:
    source, count = re.subn(
        r"constant\s+ArgbufLayout&\s+abuf\s+\[\[buffer\(30\)\]\]",
        replacement,
        source,
        count=1,
    )
    if count != 1:
        raise SystemExit("mini replay cbuf rewrite could not find buffer(30) argbuf parameter")
    return source


def transform_msl(source: str, stage: str) -> tuple[str, dict[str, int]]:
    if stage == "vs":
        vs_slot, ffp_slot = allocate_cbuf_slots(source, 2)
        source = replace_argbuf_parameter(
            source,
            f"constant VsConsts& vsConsts [[buffer({vs_slot})]],\n"
            f"                     constant FfpVsConsts& ffpVs [[buffer({ffp_slot})]]",
        )
        source = re.sub(r"\s*constant VsConsts& vsConsts = \*abuf\.vsConsts;\n", "\n", source)
        source = re.sub(r"\s*constant FfpVsConsts& ffpVs = \*abuf\.ffpVs;\n", "", source)
        return source, {"vsconsts": vs_slot, "ffpvs": ffp_slot}
    elif stage == "fs":
        ps_slot, ffp_slot = allocate_cbuf_slots(source, 2)
        source = replace_argbuf_parameter(
            source,
            f"constant PsConsts& psConsts [[buffer({ps_slot})]],\n"
            f"                     constant FfpPsConsts& ffpPs [[buffer({ffp_slot})]]",
        )
        source = re.sub(r"\s*constant PsConsts& psConsts = \*abuf\.psConsts;\n", "\n", source)
        source = re.sub(r"\s*constant FfpPsConsts& ffpPs = \*abuf\.ffpPs;\n", "", source)
        return source, {"psconsts": ps_slot, "ffpps": ffp_slot}
    else:
        raise ValueError(stage)


def vsout_field_name_from_line(line: str) -> str:
    text = line.strip()
    if not text or text.startswith("//"):
        return ""
    text = text.split("//", 1)[0].strip().rstrip(";")
    if not text:
        return ""
    if "[[" in text:
        text = text.split("[[", 1)[0].strip()
    parts = text.split()
    return parts[-1].strip("*&") if len(parts) >= 2 else ""


def trim_vsout_source(source: str, keep_fields: set[str]) -> tuple[str, list[str]]:
    match = VSOUT_RE.search(source)
    if not match:
        return source, []
    removed: list[str] = []
    kept_lines: list[str] = []
    for raw in match.group("body").splitlines():
        name = vsout_field_name_from_line(raw)
        if name and name not in keep_fields:
            removed.append(name)
            continue
        kept_lines.append(raw)
    if not removed:
        return source, []

    replacement = "struct VSOut {\n" + "\n".join(kept_lines).strip("\n") + "\n};"
    out = source[:match.start()] + replacement + source[match.end():]
    for field in removed:
        out = re.sub(rf"(?m)^\s*out\.{re.escape(field)}\s*=.*;\s*\n", "", out)
    for field in removed:
        if not field.startswith("texcoord"):
            continue
        suffix = field[len("texcoord"):]
        if not suffix.isdigit():
            continue
        out = re.sub(
            rf"(?m)^(\s*case\s+{int(suffix)}u:\s*)return\s+in\.{re.escape(field)}\s*;",
            r"\1return in.texcoord0;",
            out,
        )
    return out, removed


def apply_vsout_read_trim(vs_source: str, fs_source: str) -> tuple[str, str, dict[str, Any]]:
    vs_fields = parse_vsout_fields(vs_source)
    if not vs_fields:
        return vs_source, fs_source, {
            "enabled": False,
            "reason": "missing-vsout",
            "keep_fields": [],
            "removed_fields": [],
        }

    fs_reads, _ = stage_in_read_fields(fs_source, vs_fields)
    keep_fields = {"position", "texcoord0", *fs_reads}
    vs_source, removed = trim_vsout_source(vs_source, keep_fields)
    fs_source, _ = trim_vsout_source(fs_source, keep_fields)
    return vs_source, fs_source, {
        "enabled": True,
        "reason": "",
        "keep_fields": sorted(keep_fields),
        "removed_fields": removed,
    }


def replace_function_body(source: str, function_name: str, replacement_body: str) -> str:
    name_index = source.find(function_name)
    if name_index < 0:
        raise SystemExit(f"mini replay rewrite could not find {function_name}")
    open_index = source.find("{", name_index)
    if open_index < 0:
        raise SystemExit(f"mini replay rewrite could not find {function_name} body")
    depth = 0
    close_index: int | None = None
    for index in range(open_index, len(source)):
        char = source[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                close_index = index
                break
    if close_index is None:
        raise SystemExit(f"mini replay rewrite could not close {function_name} body")
    return source[:open_index] + replacement_body + source[close_index + 1:]


def force_fragment_color_source(source: str) -> str:
    return replace_function_body(
        source,
        "dxmt9_fs",
        "{\n  return float4(1.0f, 0.0f, 1.0f, 1.0f);\n}",
    )


def add_fragment_primitive_id_parameter(source: str) -> str:
    name_index = source.find("dxmt9_fs")
    if name_index < 0:
        raise SystemExit("mini replay rewrite could not find dxmt9_fs")
    open_index = source.find("{", name_index)
    if open_index < 0:
        raise SystemExit("mini replay rewrite could not find dxmt9_fs body")
    close_paren = source.rfind(")", 0, open_index)
    if close_paren < name_index:
        raise SystemExit("mini replay rewrite could not find dxmt9_fs parameter list")
    if "[[primitive_id]]" in source[name_index:open_index]:
        return source
    return (
        source[:close_paren]
        + ",\n                     uint primitiveId [[primitive_id]]"
        + source[close_paren:]
    )


def force_fragment_primitive_id_source(source: str) -> str:
    source = add_fragment_primitive_id_parameter(source)
    return replace_function_body(
        source,
        "dxmt9_fs",
        "{\n"
        "  uint value = primitiveId + 1u;\n"
        "  return float4(float(value & 255u) / 255.0f,\n"
        "                float((value >> 8u) & 255u) / 255.0f,\n"
        "                float((value >> 16u) & 255u) / 255.0f,\n"
        "                1.0f);\n"
        "}",
    )


def cxx_string(value: str) -> str:
    return json.dumps(value)


def shader_key(draw: dict[str, Any]) -> tuple[str, str]:
    shaders = draw.get("shaders", {})
    return (
        str(resolve_path(str(shaders.get("vs_file", "")))),
        str(resolve_path(str(shaders.get("ps_file", "")))),
    )


def first_color_attachment(draws: list[dict[str, Any]]) -> dict[str, Any]:
    attachments = draws[0].get("attachments", {}) if draws else {}
    colors = attachments.get("colors", [])
    return colors[0] if colors else {}


def first_depth_attachment(draws: list[dict[str, Any]]) -> dict[str, Any]:
    attachments = draws[0].get("attachments", {}) if draws else {}
    depth = attachments.get("depth", {})
    return depth if isinstance(depth, dict) else {}


def color_pixel_format(format_value: int) -> str:
    # dxmt9 core::Format values. Keep this narrow: unknown formats preserve the
    # previous mini-replay fallback instead of guessing an incompatible RT.
    if format_value in (1, 2):  # A8R8G8B8 / X8R8G8B8
        return "MTLPixelFormatBGRA8Unorm"
    if format_value in (3, 4):  # A8B8G8R8 / X8B8G8R8
        return "MTLPixelFormatRGBA8Unorm"
    return "MTLPixelFormatRGBA8Unorm"


def depth_pixel_format(format_value: int) -> str:
    if format_value in (40, 41, 49):  # D24S8 / D24X8 / D24FS8
        return "MTLPixelFormatDepth32Float_Stencil8"
    if format_value in (42, 46):  # D16 / D16_LOCKABLE
        return "MTLPixelFormatDepth16Unorm"
    return "MTLPixelFormatDepth32Float"


def depth_format_has_stencil(format_value: int) -> bool:
    return format_value in (40, 49)


def depth_bytes_per_pixel(format_value: int) -> int:
    if format_value in (42, 46):  # D16 / D16_LOCKABLE
        return 2
    if format_value == 49:  # D24FS8 -> Depth32Float_Stencil8 dump sidecar
        return 8
    return 4


def metal_pixel_format_name(value: int) -> str:
    names = {
        10: "MTLPixelFormatR8Unorm",
        11: "MTLPixelFormatR8Unorm_sRGB",
        20: "MTLPixelFormatR16Unorm",
        55: "MTLPixelFormatR32Float",
        80: "MTLPixelFormatBGRA8Unorm",
        81: "MTLPixelFormatBGRA8Unorm_sRGB",
        130: "MTLPixelFormatBC1_RGBA",
        131: "MTLPixelFormatBC1_RGBA_sRGB",
        134: "MTLPixelFormatBC3_RGBA",
        135: "MTLPixelFormatBC3_RGBA_sRGB",
    }
    return names.get(value, "MTLPixelFormatInvalid")


def metal_texture_type_name(value: int) -> str:
    if value == 1:
        return "MTLTextureTypeCube"
    if value == 2:
        return "MTLTextureType3D"
    return "MTLTextureType2D"


def normalize_texture_handle(value: Any) -> str:
    text = str(value)
    try:
        return f"0x{int(text, 0):x}"
    except ValueError:
        return text.lower()


def load_texture_sidecars(texture_input_dir: Path | None) -> tuple[list[dict[str, Any]], dict[tuple[str, bool], int]]:
    if texture_input_dir is None:
        return [], {}
    directory = resolve_path(str(texture_input_dir))
    if not directory.exists():
        raise SystemExit(f"missing texture input dir: {directory}")
    entries: list[dict[str, Any]] = []
    by_key: dict[tuple[str, bool], int] = {}
    for path in sorted(directory.glob("texture-*.json")):
        data = json.loads(path.read_text(encoding="utf-8"))
        pixel_format = metal_pixel_format_name(int(data.get("shaderMetalPixelFormat", 0)))
        texture_type = metal_texture_type_name(int(data.get("type", 0)))
        if pixel_format == "MTLPixelFormatInvalid":
            continue
        subresources = []
        for subresource in data.get("subresources", []):
            sub_path = path.parent / str(subresource.get("path", ""))
            if not sub_path.exists():
                raise SystemExit(f"missing texture subresource: {sub_path}")
            byte_count = int(subresource.get("byteCount", 0))
            if sub_path.stat().st_size != byte_count:
                raise SystemExit(
                    f"texture subresource size mismatch: {sub_path} "
                    f"has {sub_path.stat().st_size}, expected {byte_count}"
                )
            subresources.append({
                "level": int(subresource.get("level", 0)),
                "slice": int(subresource.get("slice", 0)),
                "width": int(subresource.get("width", 1)),
                "height": int(subresource.get("height", 1)),
                "depth": int(subresource.get("depth", 1)),
                "rowBytes": int(subresource.get("rowBytes", 0)),
                "bytesPerImage": int(subresource.get("bytesPerImage", 0)),
                "byteCount": byte_count,
                "path": str(sub_path),
            })
        if not subresources:
            continue
        entry = {
            "handle": normalize_texture_handle(data.get("handle", "")),
            "srgb": bool(int(data.get("srgb", 0))),
            "format": int(data.get("format", 0)),
            "formatName": str(data.get("formatName", "")),
            "pixelFormat": pixel_format,
            "textureType": texture_type,
            "width": int(data.get("width", 1)),
            "height": int(data.get("height", 1)),
            "depth": int(data.get("depth", 1)),
            "levels": int(data.get("levels", 1)),
            "source": str(path),
            "subresources": subresources,
        }
        index = len(entries)
        entries.append(entry)
        by_key[(entry["handle"], entry["srgb"])] = index
    return entries, by_key


def draw_fragment_texture_indices(draw: dict[str, Any],
                                  texture_sidecar_by_key: dict[tuple[str, bool], int]) -> list[int]:
    indices = [-1] * 16
    if not texture_sidecar_by_key:
        return indices
    for texture in draw.get("textures", []):
        stage = int(texture.get("stage", -1))
        if stage < 0 or stage >= len(indices):
            continue
        handle = normalize_texture_handle(texture.get("handle", "0"))
        want_srgb = bool(int(texture.get("srgb", texture.get("sampler_srgb", 0))))
        exact = texture_sidecar_by_key.get((handle, want_srgb))
        alternate = texture_sidecar_by_key.get((handle, not want_srgb))
        indices[stage] = exact if exact is not None else (alternate if alternate is not None else -1)
    return indices


def render_source(draws: list[dict[str, Any]],
                  shader_variants: list[dict[str, Any]],
                  dummy_vertex_buffer_slots: list[int],
                  width: int,
                  height: int,
                  depth_clear: float,
                  depth_input: Path | None,
                  texture_sidecars: list[dict[str, Any]],
                  texture_sidecar_by_key: dict[tuple[str, bool], int]) -> str:
    first_state = draws[0]["state"]
    alpha_blend = int(first_state.get("alpha_blend", 0))
    src_blend = int(first_state.get("src_blend", 2))
    dst_blend = int(first_state.get("dst_blend", 1))
    blend_op = int(first_state.get("blend_op", 1))
    separate_alpha = int(first_state.get("separate_alpha", 0))
    src_blend_alpha = int(first_state.get("src_blend_alpha", src_blend))
    dst_blend_alpha = int(first_state.get("dst_blend_alpha", dst_blend))
    blend_op_alpha = int(first_state.get("blend_op_alpha", blend_op))
    color_write = int(str(first_state.get("color_write", "0xf")), 0)
    depth_enabled = int(first_state.get("depth_enabled", 0))
    depth_write = int(first_state.get("depth_write", 0))
    depth_func = int(first_state.get("depth_func", 8 if not depth_enabled else 4))
    cull = int(first_state.get("cull", 1))
    scissor = int(first_state.get("scissor", 0))
    scissor_l = int(first_state.get("scissor_l", 0))
    scissor_t = int(first_state.get("scissor_t", 0))
    scissor_r = int(first_state.get("scissor_r", width))
    scissor_b = int(first_state.get("scissor_b", height))
    color_attachment = first_color_attachment(draws)
    depth_attachment = first_depth_attachment(draws)
    color_format = color_pixel_format(int(color_attachment.get("format", 0)))
    depth_format_value = int(depth_attachment.get("format", 0))
    depth_format = depth_pixel_format(depth_format_value)
    stencil_format = depth_format if depth_format_has_stencil(depth_format_value) else "MTLPixelFormatInvalid"
    pass_width = int(color_attachment.get("width", 0)) or int(depth_attachment.get("width", 0)) or width
    pass_height = int(color_attachment.get("height", 0)) or int(depth_attachment.get("height", 0)) or height
    color_row_bytes = ((pass_width * 4 + 255) // 256) * 256
    color_byte_count = color_row_bytes * pass_height
    color_is_bgra = "true" if color_format == "MTLPixelFormatBGRA8Unorm" else "false"
    depth_bpp = depth_bytes_per_pixel(depth_format_value)
    depth_row_bytes = pass_width * depth_bpp
    depth_byte_count = depth_row_bytes * pass_height
    depth_input_path = cxx_string(str(depth_input)) if depth_input else '""'
    depth_load_action = "MTLLoadActionLoad" if depth_input else "MTLLoadActionClear"
    draw_entries = []
    for draw in draws:
        geometry = draw["geometry"]
        uniforms = draw.get("uniforms", {})
        state = draw["state"]
        extra_stream_paths = ['""'] * 16
        extra_stream_slots = ["0"] * 16
        fragment_texture_indices = [
            str(index)
            for index in draw_fragment_texture_indices(draw, texture_sidecar_by_key)
        ]
        for stream in geometry.get("streams", []):
            stream_index = int(stream.get("stream", 0))
            if stream_index <= 0 or stream_index >= 16:
                continue
            stream_file = str(stream.get("file", ""))
            if not stream_file or int(stream.get("bytes", 0)) <= 0:
                continue
            extra_stream_paths[stream_index] = cxx_string(str(resolve_path(stream_file)))
            extra_stream_slots[stream_index] = str(int(stream.get("metal_slot", 0)))
        draw_entries.append(
            "  {"
            + ", ".join([
                cxx_string(str(resolve_path(geometry["index_file"]))),
                cxx_string(str(resolve_path(geometry["stream0_file"]))),
                cxx_string(optional_payload_path(uniforms, "vsconsts")),
                cxx_string(optional_payload_path(uniforms, "psconsts")),
                cxx_string(optional_payload_path(uniforms, "ffpvs")),
                cxx_string(optional_payload_path(uniforms, "ffpps")),
                "{" + ", ".join(extra_stream_paths) + "}",
                "{" + ", ".join(extra_stream_slots) + "}",
                "{" + ", ".join(fragment_texture_indices) + "}",
                str(int(draw.get("_shader_variant", 0))),
                str(int(state.get("index_count", 0))),
                str(int(state.get("base_vertex", 0))),
                str(int(state.get("stream0_stride", 0))),
                str(int(state.get("stream0_offset", 0))),
                str(int(state.get("scissor", 0))),
                str(int(state.get("scissor_l", 0))),
                str(int(state.get("scissor_t", 0))),
                str(int(state.get("scissor_r", pass_width))),
                str(int(state.get("scissor_b", pass_height))),
            ])
            + "}"
        )
    draw_array = ",\n".join(draw_entries)
    shader_entries = ",\n".join(
        "  {"
        + ", ".join([
            cxx_string(str(variant["vs_replay"])),
            cxx_string(str(variant["fs_replay"])),
            str(int(variant["vs_cbuf_slots"]["vsconsts"])),
            str(int(variant["vs_cbuf_slots"]["ffpvs"])),
            str(int(variant["fs_cbuf_slots"]["psconsts"])),
            str(int(variant["fs_cbuf_slots"]["ffpps"])),
        ])
        + "}"
        for variant in shader_variants
    )
    texture_subresource_entries: list[str] = []
    texture_entries: list[str] = []
    texture_subresource_offset = 0
    for texture in texture_sidecars:
        subresources = texture["subresources"]
        texture_entries.append(
            "  {"
            + ", ".join([
                cxx_string(str(texture["handle"])),
                str(int(texture["format"])),
                cxx_string(str(texture["formatName"])),
                str(texture["pixelFormat"]),
                str(texture["textureType"]),
                str(int(texture["width"])),
                str(int(texture["height"])),
                str(int(texture["depth"])),
                str(int(texture["levels"])),
                str(texture_subresource_offset),
                str(len(subresources)),
            ])
            + "}"
        )
        for subresource in subresources:
            texture_subresource_entries.append(
                "  {"
                + ", ".join([
                    cxx_string(str(subresource["path"])),
                    str(int(subresource["level"])),
                    str(int(subresource["slice"])),
                    str(int(subresource["width"])),
                    str(int(subresource["height"])),
                    str(int(subresource["depth"])),
                    str(int(subresource["rowBytes"])),
                    str(int(subresource["bytesPerImage"])),
                    str(int(subresource["byteCount"])),
                ])
                + "}"
            )
        texture_subresource_offset += len(subresources)
    texture_entry_array = ",\n".join(texture_entries)
    texture_subresource_array = ",\n".join(texture_subresource_entries)
    texture_input_count = len(texture_sidecars)
    texture_entry_array_for_source = texture_entry_array or (
        '  {"", 0, "", MTLPixelFormatInvalid, MTLTextureType2D, 1, 1, 1, 1, 0, 0}'
    )
    texture_subresource_array_for_source = texture_subresource_array or (
        '  {"", 0, 0, 0, 0, 0, 0, 0, 0}'
    )
    dummy_vertex_buffer_binds = "\n".join(
        f"    [encoder setVertexBuffer:dummyVertexStream offset:0 atIndex:{slot}];"
        for slot in dummy_vertex_buffer_slots
    )
    fragment_texture_slots = sorted({
        slot
        for variant in shader_variants
        for slot in variant.get("fs_bindings", {}).get("texture", [])
    })
    if texture_sidecars:
        fragment_texture_binds = ""
        per_draw_fragment_texture_binds = "\n".join(
            f"""        {{
          int textureIndex = draw.fragmentTextures[{slot}];
          [encoder setFragmentTexture:(textureIndex >= 0 ? textureInputs[textureIndex] : whiteTexture)
                              atIndex:{slot}];
        }}"""
            for slot in fragment_texture_slots
        )
    else:
        fragment_texture_binds = "\n".join(
            f"    [encoder setFragmentTexture:whiteTexture atIndex:{slot}];"
            for slot in fragment_texture_slots
        )
        per_draw_fragment_texture_binds = ""
    fragment_sampler_binds = "\n".join(
        f"    [encoder setFragmentSamplerState:sampler atIndex:{slot}];"
        for slot in sorted({
            slot
            for variant in shader_variants
            for slot in variant.get("fs_bindings", {}).get("sampler", [])
        })
    )
    vertex_texture_binds = "\n".join(
        f"    [encoder setVertexTexture:whiteTexture atIndex:{slot}];"
        for slot in sorted({
            slot
            for variant in shader_variants
            for slot in variant.get("vs_bindings", {}).get("texture", [])
        })
    )
    vertex_sampler_binds = "\n".join(
        f"    [encoder setVertexSamplerState:sampler atIndex:{slot}];"
        for slot in sorted({
            slot
            for variant in shader_variants
            for slot in variant.get("vs_bindings", {}).get("sampler", [])
        })
    )
    stencil_pass_attachment = ""
    if stencil_format != "MTLPixelFormatInvalid":
        stencil_pass_attachment = """    pass.stencilAttachment.texture = depth;
    pass.stencilAttachment.loadAction = MTLLoadActionClear;
    pass.stencilAttachment.storeAction = MTLStoreActionDontCare;
    pass.stencilAttachment.clearStencil = 0;
"""
    return f"""#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <vector>

struct ShaderEntry {{
  const char* vsPath;
  const char* fsPath;
  unsigned vsConstsSlot;
  unsigned ffpVsSlot;
  unsigned psConstsSlot;
  unsigned ffpPsSlot;
}};

struct TextureSubresourceEntry {{
  const char* path;
  unsigned level;
  unsigned slice;
  unsigned width;
  unsigned height;
  unsigned depth;
  unsigned rowBytes;
  unsigned long long bytesPerImage;
  unsigned long long byteCount;
}};

struct TextureEntry {{
  const char* handle;
  unsigned format;
  const char* formatName;
  MTLPixelFormat pixelFormat;
  MTLTextureType textureType;
  unsigned width;
  unsigned height;
  unsigned depth;
  unsigned levels;
  unsigned subresourceStart;
  unsigned subresourceCount;
}};

struct DrawEntry {{
  const char* indexPath;
  const char* streamPath;
  const char* vsConstsPath;
  const char* psConstsPath;
  const char* ffpVsPath;
  const char* ffpPsPath;
  const char* extraStreamPaths[16];
  unsigned extraStreamSlots[16];
  int fragmentTextures[16];
  unsigned shaderIndex;
  unsigned indexCount;
  int baseVertex;
  unsigned streamStride;
  unsigned streamOffset;
  unsigned scissorEnabled;
  unsigned scissorL;
  unsigned scissorT;
  unsigned scissorR;
  unsigned scissorB;
}};

struct DrawVolatile {{
  int vertexBaseIndex;
  unsigned vertexStreamOffset;
  unsigned vertexStreamStride;
  unsigned pad;
}};

static NSData* readData(const char* path) {{
  return [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:path]];
}}

static NSString* readString(const char* path) {{
  NSError* error = nil;
  NSString* value = [NSString stringWithContentsOfFile:[NSString stringWithUTF8String:path]
                                              encoding:NSUTF8StringEncoding
                                                 error:&error];
  if (!value) {{
    std::cerr << "failed to read " << path << "\\n";
  }}
  return value;
}}

static bool writePpm(const char* path,
                     const unsigned char* pixels,
                     unsigned width,
                     unsigned height,
                     unsigned rowBytes,
                     bool bgra) {{
  std::ofstream out(path, std::ios::binary);
  if (!out) {{
    std::cerr << "failed to open color output " << path << "\\n";
    return false;
  }}
  out << "P6\\n" << width << " " << height << "\\n255\\n";
  for (unsigned y = 0; y < height; ++y) {{
    const unsigned char* row = pixels + static_cast<size_t>(y) * rowBytes;
    for (unsigned x = 0; x < width; ++x) {{
      const unsigned char* p = row + static_cast<size_t>(x) * 4;
      unsigned char rgb[3] = {{
        bgra ? p[2] : p[0],
        p[1],
        bgra ? p[0] : p[2],
      }};
      out.write(reinterpret_cast<const char*>(rgb), sizeof(rgb));
    }}
  }}
  return true;
}}

static id<MTLLibrary> makeLibrary(id<MTLDevice> device, const char* path) {{
  NSError* error = nil;
  id<MTLLibrary> library = [device newLibraryWithSource:readString(path)
                                                options:nil
                                                  error:&error];
  if (!library) {{
    std::cerr << "failed to compile MSL " << path << ": "
              << [[error localizedDescription] UTF8String] << "\\n";
  }}
  return library;
}}

static id<MTLBuffer> zeroBuffer(id<MTLDevice> device, NSUInteger size) {{
  id<MTLBuffer> buffer = [device newBufferWithLength:size options:MTLResourceStorageModeShared];
  std::memset([buffer contents], 0, size);
  return buffer;
}}

static id<MTLBuffer> bufferFromFileOrDefault(id<MTLDevice> device,
                                             const char* path,
                                             id<MTLBuffer> fallback) {{
  if (!path || path[0] == '\\0') {{
    return fallback;
  }}
  NSData* data = readData(path);
  if (!data) {{
    std::cerr << "failed to read cbuf " << path << "\\n";
    return nil;
  }}
  return [device newBufferWithBytes:[data bytes]
                              length:[data length]
                             options:MTLResourceStorageModeShared];
}}

static MTLTextureSwizzleChannels shaderReadSwizzle(unsigned format) {{
  switch (format) {{
    case 2:   // X8R8G8B8
    case 4:   // X8B8G8R8
      return MTLTextureSwizzleChannelsMake(
          MTLTextureSwizzleRed, MTLTextureSwizzleGreen,
          MTLTextureSwizzleBlue, MTLTextureSwizzleOne);
    case 16:  // R32F
      return MTLTextureSwizzleChannelsMake(
          MTLTextureSwizzleRed, MTLTextureSwizzleOne,
          MTLTextureSwizzleOne, MTLTextureSwizzleOne);
    case 22:  // L8
    case 23:  // L16
      return MTLTextureSwizzleChannelsMake(
          MTLTextureSwizzleRed, MTLTextureSwizzleRed,
          MTLTextureSwizzleRed, MTLTextureSwizzleOne);
    default:
      return MTLTextureSwizzleChannelsDefault;
  }}
}}

static id<MTLTexture> makeTextureInput(id<MTLDevice> device,
                                       const TextureEntry& entry) {{
  MTLTextureDescriptor* desc = [MTLTextureDescriptor new];
  desc.textureType = entry.textureType;
  desc.pixelFormat = entry.pixelFormat;
  desc.width = entry.width;
  desc.height = entry.height;
  desc.depth = entry.textureType == MTLTextureType3D ? entry.depth : 1;
  desc.mipmapLevelCount = std::max(1u, entry.levels);
  desc.arrayLength = 1;
  desc.sampleCount = 1;
  desc.storageMode = MTLStorageModePrivate;
  desc.usage = MTLTextureUsageShaderRead;
  desc.swizzle = shaderReadSwizzle(entry.format);
  id<MTLTexture> texture = [device newTextureWithDescriptor:desc];
  if (!texture) {{
    std::cerr << "failed to create texture input " << entry.handle
              << " format=" << entry.formatName << "\\n";
  }}
  return texture;
}}

static bool uploadTextureInputs(id<MTLDevice> device,
                                id<MTLCommandQueue> queue,
                                std::vector<id<MTLTexture>>& textureInputs,
                                const TextureEntry* textureEntries,
                                unsigned textureCount,
                                const TextureSubresourceEntry* subresources) {{
  if (textureCount == 0) {{
    return true;
  }}
  id<MTLCommandBuffer> uploadCommandBuffer = [queue commandBuffer];
  id<MTLBlitCommandEncoder> blit = [uploadCommandBuffer blitCommandEncoder];
  if (!blit) {{
    std::cerr << "failed to create texture upload blit encoder\\n";
    return false;
  }}
  for (unsigned i = 0; i < textureCount; ++i) {{
    const TextureEntry& entry = textureEntries[i];
    textureInputs[i] = makeTextureInput(device, entry);
    if (!textureInputs[i]) {{
      return false;
    }}
    for (unsigned s = 0; s < entry.subresourceCount; ++s) {{
      const TextureSubresourceEntry& sub = subresources[entry.subresourceStart + s];
      NSData* data = readData(sub.path);
      if (!data || [data length] < sub.byteCount) {{
        std::cerr << "failed to read texture input " << sub.path << "\\n";
        return false;
      }}
      id<MTLBuffer> upload =
          [device newBufferWithBytes:[data bytes]
                              length:sub.byteCount
                             options:MTLResourceStorageModeShared];
      [blit copyFromBuffer:upload
              sourceOffset:0
         sourceBytesPerRow:sub.rowBytes
       sourceBytesPerImage:sub.bytesPerImage
                sourceSize:MTLSizeMake(sub.width, sub.height, sub.depth)
                 toTexture:textureInputs[i]
          destinationSlice:entry.textureType == MTLTextureType3D ? 0 : sub.slice
          destinationLevel:sub.level
         destinationOrigin:MTLOriginMake(0, 0, 0)];
    }}
  }}
  [blit endEncoding];
  [uploadCommandBuffer commit];
  [uploadCommandBuffer waitUntilCompleted];
  if ([uploadCommandBuffer status] == MTLCommandBufferStatusError) {{
    std::cerr << "texture upload command buffer failed\\n";
    return false;
  }}
  return true;
}}

static void initVsConsts(id<MTLBuffer> buffer) {{
  float* f = static_cast<float*>([buffer contents]);
  f[0] = 1.0f;
  f[5] = 1.0f;
  f[10] = 1.0f;
  f[15] = 1.0f;
  f[16 + 3] = 1.0f;
  f[32 + 3] = 1.0f;
  f[48 + 3] = 1.0f;
}}

static MTLCompareFunction compareFunction(unsigned value) {{
  switch (value) {{
    case 1: return MTLCompareFunctionNever;
    case 2: return MTLCompareFunctionLess;
    case 3: return MTLCompareFunctionEqual;
    case 4: return MTLCompareFunctionLessEqual;
    case 5: return MTLCompareFunctionGreater;
    case 6: return MTLCompareFunctionNotEqual;
    case 7: return MTLCompareFunctionGreaterEqual;
    case 8: return MTLCompareFunctionAlways;
    default: return MTLCompareFunctionAlways;
  }}
}}

static MTLBlendFactor blendFactor(unsigned value, bool alphaLane) {{
  switch (value) {{
    case 1: return MTLBlendFactorZero;
    case 2: return MTLBlendFactorOne;
    case 3: return MTLBlendFactorSourceColor;
    case 4: return MTLBlendFactorOneMinusSourceColor;
    case 5: return MTLBlendFactorSourceAlpha;
    case 6: return MTLBlendFactorOneMinusSourceAlpha;
    case 7: return MTLBlendFactorDestinationAlpha;
    case 8: return MTLBlendFactorOneMinusDestinationAlpha;
    case 9: return MTLBlendFactorDestinationColor;
    case 10: return MTLBlendFactorOneMinusDestinationColor;
    case 11: return alphaLane ? MTLBlendFactorOne : MTLBlendFactorSourceAlphaSaturated;
    case 12: return MTLBlendFactorSourceAlpha;
    case 13: return MTLBlendFactorOneMinusSourceAlpha;
    case 14: return alphaLane ? MTLBlendFactorBlendAlpha : MTLBlendFactorBlendColor;
    case 15: return alphaLane ? MTLBlendFactorOneMinusBlendAlpha : MTLBlendFactorOneMinusBlendColor;
    default: return MTLBlendFactorOne;
  }}
}}

static MTLBlendOperation blendOperation(unsigned value) {{
  switch (value) {{
    case 1: return MTLBlendOperationAdd;
    case 2: return MTLBlendOperationSubtract;
    case 3: return MTLBlendOperationReverseSubtract;
    case 4: return MTLBlendOperationMin;
    case 5: return MTLBlendOperationMax;
    default: return MTLBlendOperationAdd;
  }}
}}

static MTLCullMode cullMode(unsigned value) {{
  switch (value) {{
    case 1: return MTLCullModeNone;
    case 2: return MTLCullModeFront;
    case 3: return MTLCullModeBack;
    default: return MTLCullModeNone;
  }}
}}

static MTLColorWriteMask colorWriteMask(unsigned value) {{
  MTLColorWriteMask mask = 0;
  if (value & 0x1) mask |= MTLColorWriteMaskRed;
  if (value & 0x2) mask |= MTLColorWriteMaskGreen;
  if (value & 0x4) mask |= MTLColorWriteMaskBlue;
  if (value & 0x8) mask |= MTLColorWriteMaskAlpha;
  return mask;
}}

static MTLScissorRect scissorRect(const DrawEntry& draw, unsigned fullWidth, unsigned fullHeight) {{
  int left = draw.scissorEnabled ? static_cast<int>(draw.scissorL) : 0;
  int top = draw.scissorEnabled ? static_cast<int>(draw.scissorT) : 0;
  int right = draw.scissorEnabled ? static_cast<int>(draw.scissorR) : static_cast<int>(fullWidth);
  int bottom = draw.scissorEnabled ? static_cast<int>(draw.scissorB) : static_cast<int>(fullHeight);
  left = std::max(0, left);
  top = std::max(0, top);
  right = std::max(left, right);
  bottom = std::max(top, bottom);
  return {{
    static_cast<NSUInteger>(left),
    static_cast<NSUInteger>(top),
    static_cast<NSUInteger>(right - left),
    static_cast<NSUInteger>(bottom - top)
  }};
}}

int main() {{
  @autoreleasepool {{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {{
      std::cerr << "no Metal device\\n";
      return 2;
    }}

    id<MTLCommandQueue> queue = [device newCommandQueue];
    ShaderEntry shaders[] = {{
{shader_entries}
    }};
    const unsigned shaderCount = sizeof(shaders) / sizeof(shaders[0]);
    TextureSubresourceEntry textureSubresources[] = {{
{texture_subresource_array_for_source}
    }};
    TextureEntry textureEntries[] = {{
{texture_entry_array_for_source}
    }};
    const unsigned textureInputCount = {texture_input_count};
    std::vector<id<MTLTexture>> textureInputs(textureInputCount);
    if (!uploadTextureInputs(device, queue, textureInputs,
                             textureEntries, textureInputCount,
                             textureSubresources)) {{
      return 2;
    }}
    std::vector<id<MTLRenderPipelineState>> psos(shaderCount);
    std::vector<id<MTLLibrary>> vsLibs(shaderCount);
    std::vector<id<MTLLibrary>> fsLibs(shaderCount);

    MTLRenderPipelineDescriptor* psoDesc = [MTLRenderPipelineDescriptor new];
    psoDesc.colorAttachments[0].pixelFormat = {color_format};
    psoDesc.colorAttachments[0].writeMask = colorWriteMask({color_write});
    psoDesc.colorAttachments[0].blendingEnabled = {alpha_blend};
    psoDesc.colorAttachments[0].sourceRGBBlendFactor = blendFactor({src_blend}, false);
    psoDesc.colorAttachments[0].destinationRGBBlendFactor = blendFactor({dst_blend}, false);
    psoDesc.colorAttachments[0].rgbBlendOperation = blendOperation({blend_op});
    psoDesc.colorAttachments[0].sourceAlphaBlendFactor =
        blendFactor({src_blend_alpha if separate_alpha else src_blend}, true);
    psoDesc.colorAttachments[0].destinationAlphaBlendFactor =
        blendFactor({dst_blend_alpha if separate_alpha else dst_blend}, true);
    psoDesc.colorAttachments[0].alphaBlendOperation =
        blendOperation({blend_op_alpha if separate_alpha else blend_op});
    psoDesc.depthAttachmentPixelFormat = {depth_format};
    psoDesc.stencilAttachmentPixelFormat = {stencil_format};
    NSError* error = nil;
    for (unsigned i = 0; i < shaderCount; ++i) {{
      vsLibs[i] = makeLibrary(device, shaders[i].vsPath);
      fsLibs[i] = makeLibrary(device, shaders[i].fsPath);
      if (!vsLibs[i] || !fsLibs[i]) return 2;
      psoDesc.vertexFunction = [vsLibs[i] newFunctionWithName:@"dxmt9_vs"];
      psoDesc.fragmentFunction = [fsLibs[i] newFunctionWithName:@"dxmt9_fs"];
      psos[i] = [device newRenderPipelineStateWithDescriptor:psoDesc error:&error];
      if (!psos[i]) {{
        std::cerr << "failed to create PSO " << i << ": "
                  << [[error localizedDescription] UTF8String] << "\\n";
        return 2;
      }}
    }}
    MTLDepthStencilDescriptor* depthStateDesc = [MTLDepthStencilDescriptor new];
    depthStateDesc.depthCompareFunction =
        {depth_enabled} ? compareFunction({depth_func}) : MTLCompareFunctionAlways;
    depthStateDesc.depthWriteEnabled = {1 if depth_enabled and depth_write else 0};
    id<MTLDepthStencilState> depthState = [device newDepthStencilStateWithDescriptor:depthStateDesc];

    MTLTextureDescriptor* colorDesc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:{color_format}
                                                           width:{pass_width}
                                                          height:{pass_height}
                                                       mipmapped:NO];
    colorDesc.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> color = [device newTextureWithDescriptor:colorDesc];
    MTLTextureDescriptor* depthDesc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:{depth_format}
                                                           width:{pass_width}
                                                          height:{pass_height}
                                                       mipmapped:NO];
    depthDesc.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> depth = [device newTextureWithDescriptor:depthDesc];
    if ({depth_input_path}[0] != '\\0') {{
      NSData* depthData = readData({depth_input_path});
      if (!depthData || [depthData length] < {depth_byte_count}) {{
        std::cerr << "failed to read depth input or size is too small\\n";
        return 2;
      }}
      id<MTLBuffer> depthUpload =
          [device newBufferWithBytes:[depthData bytes]
                              length:{depth_byte_count}
                             options:MTLResourceStorageModeShared];
      id<MTLCommandBuffer> depthUploadCommandBuffer = [queue commandBuffer];
      id<MTLBlitCommandEncoder> depthUploadBlit =
          [depthUploadCommandBuffer blitCommandEncoder];
      [depthUploadBlit copyFromBuffer:depthUpload
                         sourceOffset:0
                    sourceBytesPerRow:{depth_row_bytes}
                  sourceBytesPerImage:0
                           sourceSize:MTLSizeMake({pass_width}, {pass_height}, 1)
                            toTexture:depth
                     destinationSlice:0
                     destinationLevel:0
                    destinationOrigin:MTLOriginMake(0, 0, 0)];
      [depthUploadBlit endEncoding];
      [depthUploadCommandBuffer commit];
      [depthUploadCommandBuffer waitUntilCompleted];
    }}

    id<MTLBuffer> vsConsts = zeroBuffer(device, 16384);
    initVsConsts(vsConsts);
    id<MTLBuffer> ffpVs = zeroBuffer(device, 16384);
    id<MTLBuffer> psConsts = zeroBuffer(device, 16384);
    id<MTLBuffer> ffpPs = zeroBuffer(device, 16384);
    id<MTLBuffer> dummyVertexStream = zeroBuffer(device, 16 * 1024 * 1024);
    MTLTextureDescriptor* whiteDesc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:1
                                                          height:1
                                                       mipmapped:NO];
    whiteDesc.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> whiteTexture = [device newTextureWithDescriptor:whiteDesc];
    const unsigned char whitePixel[4] = {{255, 255, 255, 255}};
    [whiteTexture replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
                     mipmapLevel:0
                       withBytes:whitePixel
                     bytesPerRow:4];
    MTLSamplerDescriptor* samplerDesc = [MTLSamplerDescriptor new];
    samplerDesc.minFilter = MTLSamplerMinMagFilterLinear;
    samplerDesc.magFilter = MTLSamplerMinMagFilterLinear;
    id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:samplerDesc];

    DrawEntry draws[] = {{
{draw_array}
    }};

    unsigned repeat = 1;
    if (const char* env = std::getenv("DXMT9_MINI_REPLAY_REPEAT")) {{
      repeat = std::max(1, std::atoi(env));
    }}

    if (const char* capturePath = std::getenv("DXMT9_MINI_REPLAY_CAPTURE_PATH")) {{
      MTLCaptureDescriptor* capture = [MTLCaptureDescriptor new];
      capture.captureObject = device;
      capture.destination = MTLCaptureDestinationGPUTraceDocument;
      capture.outputURL = [NSURL fileURLWithPath:[NSString stringWithUTF8String:capturePath]];
      [[MTLCaptureManager sharedCaptureManager] startCaptureWithDescriptor:capture error:&error];
      if (error) {{
        std::cerr << "capture start failed: " << [[error localizedDescription] UTF8String] << "\\n";
      }}
    }}

    id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = color;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
    pass.depthAttachment.texture = depth;
    pass.depthAttachment.loadAction = {depth_load_action};
    pass.depthAttachment.storeAction = MTLStoreActionDontCare;
    pass.depthAttachment.clearDepth = {depth_clear:.9g};
{stencil_pass_attachment.rstrip()}
    id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];
    [encoder setDepthStencilState:depthState];
    [encoder setCullMode:cullMode({cull})];
{fragment_texture_binds}
{fragment_sampler_binds}
{vertex_texture_binds}
{vertex_sampler_binds}
{dummy_vertex_buffer_binds}

    for (unsigned r = 0; r < repeat; ++r) {{
      for (const DrawEntry& draw : draws) {{
        NSData* streamData = readData(draw.streamPath);
        NSData* indexData = readData(draw.indexPath);
        if (!streamData || !indexData) return 2;
        id<MTLBuffer> drawVsConsts = bufferFromFileOrDefault(device, draw.vsConstsPath, vsConsts);
        id<MTLBuffer> drawPsConsts = bufferFromFileOrDefault(device, draw.psConstsPath, psConsts);
        id<MTLBuffer> drawFfpVs = bufferFromFileOrDefault(device, draw.ffpVsPath, ffpVs);
        id<MTLBuffer> drawFfpPs = bufferFromFileOrDefault(device, draw.ffpPsPath, ffpPs);
        if (!drawVsConsts || !drawPsConsts || !drawFfpVs || !drawFfpPs) return 2;
        id<MTLBuffer> stream =
            [device newBufferWithBytes:[streamData bytes]
                                length:[streamData length]
                               options:MTLResourceStorageModeShared];
        id<MTLBuffer> extraStreams[16] = {{}};
        for (unsigned s = 1; s < 16; ++s) {{
          if (!draw.extraStreamPaths[s] || draw.extraStreamPaths[s][0] == '\\0' ||
              draw.extraStreamSlots[s] == 0) {{
            continue;
          }}
          NSData* extraData = readData(draw.extraStreamPaths[s]);
          if (!extraData) return 2;
          extraStreams[s] = [device newBufferWithBytes:[extraData bytes]
                                                length:[extraData length]
                                               options:MTLResourceStorageModeShared];
        }}
        id<MTLBuffer> index =
            [device newBufferWithBytes:[indexData bytes]
                                length:[indexData length]
                               options:MTLResourceStorageModeShared];
        if (draw.shaderIndex >= shaderCount) return 2;
        const ShaderEntry& shader = shaders[draw.shaderIndex];
        DrawVolatile dv = {{draw.baseVertex, draw.streamOffset, draw.streamStride, 0}};
        [encoder setScissorRect:scissorRect(draw, {pass_width}, {pass_height})];
        [encoder setRenderPipelineState:psos[draw.shaderIndex]];
{per_draw_fragment_texture_binds}
        [encoder setVertexBuffer:drawVsConsts offset:0 atIndex:shader.vsConstsSlot];
        [encoder setVertexBuffer:drawFfpVs offset:0 atIndex:shader.ffpVsSlot];
        [encoder setFragmentBuffer:drawPsConsts offset:0 atIndex:shader.psConstsSlot];
        [encoder setFragmentBuffer:drawFfpPs offset:0 atIndex:shader.ffpPsSlot];
        [encoder setVertexBuffer:stream offset:0 atIndex:1];
        for (unsigned s = 1; s < 16; ++s) {{
          if (extraStreams[s]) {{
            [encoder setVertexBuffer:extraStreams[s] offset:0 atIndex:draw.extraStreamSlots[s]];
          }}
        }}
        [encoder setVertexBytes:&dv length:sizeof(dv) atIndex:5];
        [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                            indexCount:draw.indexCount
                             indexType:MTLIndexTypeUInt16
                           indexBuffer:index
                     indexBufferOffset:0];
      }}
    }}
    [encoder endEncoding];
    const char* colorOutputPath = std::getenv("DXMT9_MINI_REPLAY_COLOR_OUTPUT_PATH");
    id<MTLBuffer> colorReadback = nil;
    if (colorOutputPath && colorOutputPath[0] != '\\0') {{
      colorReadback =
          [device newBufferWithLength:{color_byte_count}
                              options:MTLResourceStorageModeShared];
      id<MTLBlitCommandEncoder> colorBlit = [commandBuffer blitCommandEncoder];
      [colorBlit copyFromTexture:color
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake({pass_width}, {pass_height}, 1)
                        toBuffer:colorReadback
               destinationOffset:0
          destinationBytesPerRow:{color_row_bytes}
        destinationBytesPerImage:0];
      [colorBlit endEncoding];
    }}
    [commandBuffer commit];
    [commandBuffer waitUntilCompleted];
    if (colorOutputPath && colorOutputPath[0] != '\\0' && colorReadback) {{
      const unsigned char* pixels =
          static_cast<const unsigned char*>([colorReadback contents]);
      if (!writePpm(colorOutputPath, pixels, {pass_width}, {pass_height},
                    {color_row_bytes}, {color_is_bgra})) {{
        return 2;
      }}
    }}
    if ([[MTLCaptureManager sharedCaptureManager] isCapturing]) {{
      [[MTLCaptureManager sharedCaptureManager] stopCapture];
    }}
    std::cout << "mini replay draws=" << (sizeof(draws) / sizeof(draws[0]))
              << " repeat=" << repeat << "\\n";
  }}
  return 0;
}}
"""


def prepare(args: argparse.Namespace) -> dict[str, Any]:
    manifest = load_manifest(args.manifest)
    draws = manifest["draws"]
    validate_payloads(draws)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    depth_input = resolve_path(str(args.depth_input)) if args.depth_input else None
    if depth_input and not depth_input.exists():
        raise FileNotFoundError(depth_input)
    texture_sidecars, texture_sidecar_by_key = load_texture_sidecars(args.texture_input_dir)
    replay_draws = materialize_replay_draws(
        draws,
        args.output_dir,
        args.primitive_order,
        args.draw_order,
    )
    shader_variants: list[dict[str, Any]] = []
    shader_variant_by_key: dict[tuple[str, str], int] = {}
    all_vs_bindings: list[dict[str, list[int]]] = []
    all_fs_bindings: list[dict[str, list[int]]] = []
    dummy_vertex_buffer_slot_set: set[int] = set()
    for draw in replay_draws:
        key = shader_key(draw)
        variant_index = shader_variant_by_key.get(key)
        if variant_index is None:
            vs_file = Path(key[0])
            fs_file = Path(key[1])
            if not vs_file.exists() or not fs_file.exists():
                raise SystemExit("manifest shader files are missing")
            variant_index = len(shader_variants)
            shader_variant_by_key[key] = variant_index
            if variant_index == 0:
                vs_replay = args.output_dir / "dxmt9_vs.replay.metal"
                fs_replay = args.output_dir / "dxmt9_fs.replay.metal"
            else:
                vs_replay = args.output_dir / f"dxmt9_vs_{variant_index:02d}.replay.metal"
                fs_replay = args.output_dir / f"dxmt9_fs_{variant_index:02d}.replay.metal"
            vs_source = vs_file.read_text(encoding="utf-8")
            fs_source = fs_file.read_text(encoding="utf-8")
            vsout_trim = {
                "enabled": False,
                "reason": "disabled",
                "keep_fields": [],
                "removed_fields": [],
            }
            if args.trim_vsout_to_fs_reads:
                vs_source, fs_source, vsout_trim = apply_vsout_read_trim(vs_source, fs_source)
            vs_source, vs_cbuf_slots = transform_msl(vs_source, "vs")
            fs_source, fs_cbuf_slots = transform_msl(fs_source, "fs")
            if args.force_fragment_color:
                fs_source = force_fragment_color_source(fs_source)
            if args.force_fragment_primitive_id:
                fs_source = force_fragment_primitive_id_source(fs_source)
            vs_replay.write_text(vs_source, encoding="utf-8")
            fs_replay.write_text(fs_source, encoding="utf-8")
            vs_bindings = stage_bindings(vs_source)
            fs_bindings = stage_bindings(fs_source)
            all_vs_bindings.append(vs_bindings)
            all_fs_bindings.append(fs_bindings)
            reserved_vs_buffers = {1, 5, *vs_cbuf_slots.values()}
            dummy_vertex_buffer_slot_set.update(
                slot for slot in vs_bindings["buffer"]
                if slot not in reserved_vs_buffers
            )
            shader_variants.append({
                "vs_source": str(vs_file),
                "fs_source": str(fs_file),
                "vs_replay": str(vs_replay),
                "fs_replay": str(fs_replay),
                "vs_cbuf_slots": vs_cbuf_slots,
                "fs_cbuf_slots": fs_cbuf_slots,
                "vs_bindings": vs_bindings,
                "fs_bindings": fs_bindings,
                "vsout_trim": vsout_trim,
            })
        draw["_shader_variant"] = variant_index
    dummy_vertex_buffer_slots = sorted(dummy_vertex_buffer_slot_set)
    actual_extra_vertex_buffer_slots = sorted({
        int(stream.get("metal_slot", 0))
        for draw in replay_draws
        for stream in draw.get("geometry", {}).get("streams", [])
        if int(stream.get("stream", 0)) > 0 and int(stream.get("bytes", 0)) > 0
    })

    source_path = args.output_dir / "dxmt9_3dmark05_mini_replay.mm"
    source_path.write_text(
        render_source(
            replay_draws,
            shader_variants,
            dummy_vertex_buffer_slots,
            args.width,
            args.height,
            args.depth_clear,
            depth_input,
            texture_sidecars,
            texture_sidecar_by_key,
        ),
        encoding="utf-8",
    )
    binary_path = args.output_dir / "dxmt9-3dmark05-mini-replay"
    summary = {
        "manifest": str(args.manifest),
        "output_dir": str(args.output_dir),
        "source": str(source_path),
        "binary": str(binary_path),
        "shader_variant_count": len(shader_variants),
        "shader_variants": shader_variants,
        "draw_count": len(replay_draws),
        "draw_order": args.draw_order,
        "primitive_order": args.primitive_order,
        "force_fragment_color": bool(args.force_fragment_color),
        "force_fragment_primitive_id": bool(args.force_fragment_primitive_id),
        "index_cache_estimate": index_cache_estimate(replay_draws),
        "depth_clear": args.depth_clear,
        "depth_input": str(depth_input) if depth_input else None,
        "texture_input_dir": str(resolve_path(str(args.texture_input_dir))) if args.texture_input_dir else None,
        "texture_input_count": len(texture_sidecars),
        "texture_inputs": [
            {
                "handle": texture["handle"],
                "format": texture["formatName"],
                "pixel_format": texture["pixelFormat"],
                "texture_type": texture["textureType"],
                "levels": texture["levels"],
                "subresources": len(texture["subresources"]),
                "source": texture["source"],
            }
            for texture in texture_sidecars
        ],
        "color_output": str(args.color_output) if args.color_output else None,
        "index_bytes": sum(int(draw["geometry"]["index_bytes"]) for draw in replay_draws),
        "stream0_bytes": sum(int(draw["geometry"]["stream0_bytes"]) for draw in replay_draws),
        "uniform_draw_count": sum(
            1 for draw in replay_draws
            if any(int(draw.get("uniforms", {}).get(f"{key}_bytes", 0)) > 0
                   for key in ("vsconsts", "psconsts", "ffpvs", "ffpps"))
        ),
        "uniform_bytes": sum(
            int(draw.get("uniforms", {}).get(f"{key}_bytes", 0))
            for draw in replay_draws
            for key in ("vsconsts", "psconsts", "ffpvs", "ffpps")
        ),
        "vs_cbuf_slots": shader_variants[0]["vs_cbuf_slots"],
        "fs_cbuf_slots": shader_variants[0]["fs_cbuf_slots"],
        "actual_extra_vertex_buffer_slots": actual_extra_vertex_buffer_slots,
        "dummy_vertex_buffer_slots": dummy_vertex_buffer_slots,
        "scissor_draw_count": sum(
            1 for draw in replay_draws
            if int(draw.get("state", {}).get("scissor", 0))
        ),
        "vs_bindings": all_vs_bindings[0],
        "fs_bindings": all_fs_bindings[0],
        "all_vs_bindings": all_vs_bindings,
        "all_fs_bindings": all_fs_bindings,
    }
    (args.output_dir / "mini-replay-summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True), encoding="utf-8")
    return summary


def compile_source(summary: dict[str, Any]) -> None:
    cmd = [
        "xcrun", "-sdk", "macosx", "clang++",
        "-std=c++20",
        "-fobjc-arc",
        "-O2",
        "-Wall",
        "-Wextra",
        "-framework", "Foundation",
        "-framework", "Metal",
        "-o", summary["binary"],
        summary["source"],
    ]
    print("compile_cmd:", " ".join(shlex.quote(part) for part in cmd))
    subprocess.run(cmd, check=True)


def parse_min_capture_free_mb(value: str | None) -> int:
    if value is None or value == "":
        return DEFAULT_MIN_CAPTURE_FREE_MB
    try:
        parsed = int(value)
    except ValueError as exc:
        raise SystemExit(f"invalid min capture free MB: {value}") from exc
    if parsed < 0:
        raise SystemExit(f"invalid min capture free MB: {value}")
    return parsed


def check_capture_free_space(capture_path: Path, min_free_mb: int) -> None:
    if min_free_mb <= 0:
        return
    target_dir = capture_path.parent
    target_dir.mkdir(parents=True, exist_ok=True)
    free_mb = shutil.disk_usage(target_dir).free // (1024 * 1024)
    if free_mb < min_free_mb:
        raise SystemExit(
            f"insufficient free space for mini replay gputrace: "
            f"{free_mb}MiB available at {target_dir}, require {min_free_mb}MiB "
            f"(override with --min-capture-free-mb or DXMT9_MINI_REPLAY_MIN_CAPTURE_FREE_MB)"
        )


def run_binary(summary: dict[str, Any],
               repeat: int,
               capture_path: Path | None,
               color_output: Path | None) -> None:
    env = os.environ.copy()
    env["DXMT9_MINI_REPLAY_REPEAT"] = str(repeat)
    if capture_path is not None:
        env["MTL_CAPTURE_ENABLED"] = "1"
        env["DXMT9_MINI_REPLAY_CAPTURE_PATH"] = str(capture_path)
    if color_output is not None:
        color_output.parent.mkdir(parents=True, exist_ok=True)
        env["DXMT9_MINI_REPLAY_COLOR_OUTPUT_PATH"] = str(color_output)
    subprocess.run([summary["binary"]], check=True, env=env)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--width", type=int, default=1024)
    parser.add_argument("--height", type=int, default=768)
    parser.add_argument(
        "--primitive-order",
        choices=(
            "original",
            "reverse-triangles",
            "sort-min-index",
            "sort-max-index",
            "cache-opt-lru32",
            "cache-opt-lru64",
        ),
        default="original",
        help=(
            "rewrite each uint16 triangle-list index payload before replay; "
            "used for primitive/backend locality A/B probes"
        ),
    )
    parser.add_argument(
        "--draw-order",
        choices=("original", "reverse"),
        default="original",
        help="reorder manifest draws before replay",
    )
    parser.add_argument(
        "--trim-vsout-to-fs-reads",
        action="store_true",
        help=(
            "diagnostic: trim replay VSOut fields to the actual selected FS "
            "stage_in reads, preserving position and texcoord0 as fallbacks"
        ),
    )
    parser.add_argument(
        "--force-fragment-color",
        action="store_true",
        help=(
            "diagnostic: replace replay fragment shader body with a magenta "
            "constant to separate raster/depth coverage from real FS/texture output"
        ),
    )
    parser.add_argument(
        "--force-fragment-primitive-id",
        action="store_true",
        help=(
            "diagnostic: replace replay fragment output with an encoded Metal "
            "primitive_id color to identify which triangle owns changed pixels"
        ),
    )
    parser.add_argument("--compile", action="store_true")
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument(
        "--depth-clear",
        type=float,
        default=1.0,
        help=(
            "clear value for the standalone depth attachment; use this for "
            "depth-content sensitivity probes before a real depth attachment "
            "dump/load path exists"
        ),
    )
    parser.add_argument(
        "--depth-input",
        type=Path,
        help=(
            "raw depth attachment sidecar to upload before the replay render "
            "pass; pairs with DXMT9_DUMP_DEPTH_ATTACHMENT_PATH output"
        ),
    )
    parser.add_argument(
        "--texture-input-dir",
        type=Path,
        help=(
            "directory of draw-time raw texture sidecars emitted by "
            "DXMT9_DUMP_DRAW_TEXTURE_HANDLES / --dump-draw-texture-handles"
        ),
    )
    parser.add_argument("--capture-path", type=Path)
    parser.add_argument(
        "--color-output",
        type=Path,
        help=(
            "when --run is used, read back the replay color attachment and "
            "write a PPM image for same-input image comparison"
        ),
    )
    parser.add_argument(
        "--min-capture-free-mb",
        type=parse_min_capture_free_mb,
        default=parse_min_capture_free_mb(os.environ.get("DXMT9_MINI_REPLAY_MIN_CAPTURE_FREE_MB")),
        help=(
            "minimum free space required before --capture-path is used "
            f"(default: {DEFAULT_MIN_CAPTURE_FREE_MB}MiB, env DXMT9_MINI_REPLAY_MIN_CAPTURE_FREE_MB)"
        ),
    )
    args = parser.parse_args()

    summary = prepare(args)
    print(json.dumps(summary, indent=2, sort_keys=True))
    if args.run and args.capture_path is not None:
        check_capture_free_space(args.capture_path, args.min_capture_free_mb)
    if args.compile or args.run:
        compile_source(summary)
    if args.run:
        run_binary(summary, args.repeat, args.capture_path, args.color_output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
