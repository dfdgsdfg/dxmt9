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
# An image carrying exactly one distinct RGB triple is degenerate: it holds no
# information, so a pixel comparison against it is vacuous. This is the exact
# shape of the defect fixed in `36a41ad5` -- four replay lanes each wrote a
# 1024x768 PPM with one distinct pixel value, printed `mini replay draws=229
# repeat=1`, and exited 0. Two distinct values is therefore the whole gate
# (R-HARN-REPLAY-3.1). Deliberately no percentage-coverage threshold: the
# degenerate case is the only failure shape this harness has evidence for, and
# inventing a "at least N% non-background" rule would reject legitimate
# small-window replays on no evidence at all.
MIN_DISTINCT_RGB_VALUES = 2
# Exit status the *generated* program returns for that same degenerate case, so
# a maintainer running `dxmt9-3dmark05-mini-replay` directly is guarded too and
# not only through this wrapper's `--run` path. It is a distinct status rather
# than a generic failure so `run_binary()` can tell "the replay rendered, and
# the image is degenerate" apart from "the replay itself failed": the former is
# reported by the wrapper's own assertion, which is the only side that can also
# record the measured counts in `validity`.
REPLAY_DEGENERATE_EXIT_STATUS = 3
# The replay render pass clears to opaque black (MTLClearColorMake(0,0,0,1)),
# so "non-background" means "not the clear colour" -- pixels some draw wrote.
BACKGROUND_RGB = (0, 0, 0)
# Bucket for manifest draws whose `shaders.vs_hash`/`ps_hash` is absent or
# empty. Per-hash draw counts must sum to the replayed draw count, so an
# unattributable draw gets a named bucket rather than being dropped.
UNKNOWN_SHADER_HASH = "unknown"
# What `validity` proves, stated in the artifact itself so a reader cannot
# mistake it for a coverage claim. These are two independent facts: an image
# can be richly non-degenerate while the code path under test never executed
# once (defect 7).
VALIDITY_CERTIFIES = (
    "the written image is non-degenerate (more than one distinct RGB value); "
    "this does not certify that the replay executed any particular code path "
    "-- see the coverage block for what the replayed draw window contains"
)
VALIDITY_REASON_NOT_RUN = (
    "the replay binary was not executed, so no image was produced to assert on "
    "(pass --run together with --color-output)"
)
VALIDITY_REASON_NO_COLOR_OUTPUT = (
    "no color output was requested, so no image was produced to assert on "
    "(pass --color-output)"
)
# The replay renders one encoder's draw window from one frame. Stated in the
# artifact because the absence of this sentence is what let a byte-identical
# replay be read as a whole-frame correctness result (defect 7).
COVERAGE_SCOPE = (
    "single-encoder slice of one captured frame: only the draws this manifest "
    "lists were replayed. This is not a frame-level or scene-level result, and "
    "an image comparison over it certifies nothing about draws, shaders, or "
    "code paths outside this window. It does not establish that any particular "
    "branch or shader path executed -- consult draws_by_vs_hash to see whether "
    "a changed shader is present at all, and instrument the path itself to "
    "prove it executes."
)
# `--prove-executed 'REGEX=>REPLACEMENT'`. `coverage` answers containment --
# whether a changed shader is in the replay at all. This answers execution:
# mutate the construct under test in every generated .metal source, replay
# again, and compare. An identical image proves the construct was never
# executed, which is exactly what defect 7's hand instrumentation found.
PROVE_EXECUTED_SEPARATOR = "=>"
EXECUTION_PROOF_DIR_NAME = "execution-proof"
# The substitution matched nothing. This is *not* an execution verdict: the
# construct is absent (or the pattern is wrong), so the run says nothing about
# whether it would execute. Kept distinct from the verdict below because the
# two demand opposite responses -- fix the pattern, versus stop trusting the
# replay for this change.
EXECUTION_VERDICT_NOT_PRESENT = "not-present"
# The substitution matched sites and the image did not move: present, and never
# reached. Defect 7's exact shape.
EXECUTION_VERDICT_NOT_EXECUTED = "present-but-not-executed"
EXECUTION_VERDICT_EXECUTED = "executed"
EXECUTION_PROOF_CERTIFIES = (
    "whether the mutated construct is executed by this replay: mutating it "
    "changed the rendered image, so the replayed draw window does reach it. "
    "This certifies execution within this replay only -- it is not a "
    "correctness result, and it says nothing about draws or code paths outside "
    "the replayed window (see the coverage block)"
)
VSOUT_RE = re.compile(r"struct\s+VSOut\s*\{(?P<body>.*?)\};", re.S)
# dxmt9_fs is declared with one of three return shapes depending on which
# emitter produced it: `FSOut` (every D3D9-bytecode-translated pixel shader --
# dxmt9_shader_metal_ir.cpp hardcodes usesFragmentOutStruct = true, making
# this the dominant real shape for the per-draw shaders this harness dumps
# and replays), `FfpFsOut` (fixed-function draws, dxmt9_ffp_shaders.cpp), or
# a bare `float4` (only dxmt9's internal blit/gamma-apply/debug-fill utility
# shaders, dxmt9_shader_sources.cpp -- never the per-draw shader this harness
# replays). See specs/experiments/harness/replay/requirements.md
# R-HARN-REPLAY-6.2.
FRAGMENT_ENTRY_RE = re.compile(r"fragment\s+(\w+)\s+dxmt9_fs\s*\(")
STRUCT_OUTPUT_MEMBER_RE = re.compile(r"[\w:<>]+\s+(\w+)\s*\[\[([^\]]+)\]\]\s*;")


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


def vertex_slot_capacity(payload_bytes: int, offset: int, stride: int) -> int:
    """Number of addressable vertex slots in a dumped stream payload.

    The replay shader fetches a vertex at `offset + (index + base_vertex) *
    stride`, so slots start at `offset`, not at byte 0 of the payload.
    """
    if stride <= 0:
        raise SystemExit(f"vertex stride must be positive, got {stride}")
    if offset < 0:
        raise SystemExit(f"vertex offset must be non-negative, got {offset}")
    usable = payload_bytes - offset
    if usable < stride:
        raise SystemExit(
            f"stream payload of {payload_bytes} bytes at offset {offset} "
            f"cannot hold one {stride}-byte vertex"
        )
    return usable // stride


def resolve_stream_payload_offset(offset: int,
                                  start_byte: int,
                                  ordinal: int,
                                  stream_index: int) -> int:
    """In-payload byte offset for a (possibly sliced) dumped stream payload.

    The geometry dump writes a slice of the vertex buffer beginning at
    `start_byte`, so byte 0 of the dumped file is fetch slot 0 at that source
    byte, not at source byte 0. `offset - start_byte` is correct in all three
    cases: a slice taken at the stream offset yields 0, an unsliced payload
    (`start_byte == 0`) with a zero offset yields 0, and an unsliced payload
    with a non-zero offset yields the offset itself. A negative result means
    the manifest is inconsistent.
    """
    payload_offset = offset - start_byte
    if payload_offset < 0:
        raise SystemExit(
            f"draw {ordinal}: stream{stream_index} start_byte {start_byte} "
            f"exceeds offset {offset}"
        )
    return payload_offset


def vertex_reference_order(indices: list[int],
                          base_vertex: int,
                          vertex_order: str) -> list[int]:
    """Distinct fetch slots referenced by `indices`, ordered per `vertex_order`.

    `first-reference` returns them in order of first appearance, which makes the
    rewritten first-reference index sequence exactly 0, 1, 2, ... `scatter`
    returns them ordered by a multiplicative hash of the slot value, which
    decorrelates the layout from both slot value and reference order. The hash
    sort is used instead of an RNG so the permutation is reproducible across
    Python versions.
    """
    seen: set[int] = set()
    first_reference: list[int] = []
    for index in indices:
        slot = index + base_vertex
        if slot < 0:
            raise SystemExit(
                f"index {index} with base_vertex {base_vertex} yields negative slot"
            )
        if slot not in seen:
            seen.add(slot)
            first_reference.append(slot)
    if vertex_order == "first-reference":
        return first_reference
    if vertex_order == "scatter":
        return sorted(first_reference,
                      key=lambda slot: ((slot * 2654435761) % (2 ** 32), slot))
    raise SystemExit(f"unsupported vertex order: {vertex_order}")


def vertex_slot_assignment(order: list[int],
                           base_vertex: int,
                           slot_count: int) -> list[int]:
    """Map every old slot to a new slot. Returns `new_for_old`.

    Referenced slots land at `base_vertex + k` in `order` order, so the
    rewritten index for `order[k]` is simply `k`. Unreferenced slots fill the
    remaining positions in ascending order, which keeps the payload size and
    byte multiset identical to the source and leaves `base_vertex` valid.

    `base_vertex + len(order) <= slot_count` always holds for real input,
    because every index is non-negative, so `base_vertex <= min(order)` and
    therefore `base_vertex + len(order) <= max(order) + 1 <= slot_count`. The
    check below is a defensive assertion, not a reachable branch.
    """
    if len(order) > slot_count:
        raise SystemExit(
            f"{len(order)} referenced vertices exceed {slot_count} payload slots"
        )
    for slot in order:
        if slot >= slot_count:
            raise SystemExit(
                f"referenced slot {slot} escapes {slot_count} payload slots"
            )
    if base_vertex + len(order) > slot_count:
        raise SystemExit(
            f"base_vertex {base_vertex} plus {len(order)} referenced vertices "
            f"exceed {slot_count} payload slots"
        )
    new_for_old: list[int] = [-1] * slot_count
    taken: set[int] = set()
    for position, slot in enumerate(order):
        target = base_vertex + position
        new_for_old[slot] = target
        taken.add(target)
    spare = (slot for slot in range(slot_count) if slot not in taken)
    for slot in range(slot_count):
        if new_for_old[slot] < 0:
            new_for_old[slot] = next(spare)
    return new_for_old


def apply_vertex_permutation(payload: bytes,
                             offset: int,
                             stride: int,
                             new_for_old: list[int]) -> bytes:
    """Relocate each vertex slot. Reads from `payload`, writes into a copy."""
    out = bytearray(payload)
    for old_slot, new_slot in enumerate(new_for_old):
        src = offset + old_slot * stride
        dst = offset + new_slot * stride
        out[dst:dst + stride] = payload[src:src + stride]
    return bytes(out)


def rewrite_indices_for_permutation(indices: list[int],
                                    base_vertex: int,
                                    order: list[int]) -> list[int]:
    """Rewrite each index to its slot's position in `order`."""
    position = {slot: index for index, slot in enumerate(order)}
    rewritten = [position[index + base_vertex] for index in indices]
    if rewritten and max(rewritten) > 0xffff:
        raise SystemExit(
            f"rewritten index {max(rewritten)} exceeds uint16 range"
        )
    return rewritten


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


def remap_draw_vertex_layout(draw: dict[str, Any],
                            ordinal: int,
                            vertex_dir: Path,
                            vertex_order: str) -> None:
    """Permute this draw's vertex storage and rewrite its indices in place.

    Preserves triangle order, triangle composition, vertex bytes, payload size,
    and `base_vertex`. Every stream is permuted with one shared reference order
    because a D3D9 indexed draw uses the same vertex index for every stream.
    """
    geometry = draw["geometry"]
    state = draw["state"]
    index_source = resolve_path(str(geometry["index_file"]))
    indices = uint16_indices(index_source.read_bytes())
    if not indices:
        raise SystemExit(f"draw {ordinal}: index payload is empty")
    base_vertex = int(state.get("base_vertex", 0))
    order = vertex_reference_order(indices, base_vertex, vertex_order)

    stream_entries = geometry.get("streams") or []
    if not stream_entries:
        stream_entries = [{
            "stream": 0,
            "metal_slot": 1,
            "file": geometry["stream0_file"],
            "bytes": int(geometry.get("stream0_bytes", 0)),
            "offset": int(state.get("stream0_offset", 0)),
            "stride": int(state.get("stream0_stride", 0)),
            "start_byte": 0,
        }]
        geometry["streams"] = stream_entries

    for entry in stream_entries:
        stream_index = int(entry.get("stream", 0))
        stride = int(entry.get("stride", 0))
        offset = int(entry.get("offset", 0))
        start_byte = int(entry.get("start_byte", 0))
        if stream_index == 0:
            stride = int(state.get("stream0_stride", stride))
            offset = int(state.get("stream0_offset", offset))
        payload_offset = resolve_stream_payload_offset(offset, start_byte, ordinal, stream_index)
        source = resolve_path(str(entry.get("file", "")))
        if not source.exists():
            raise SystemExit(
                f"draw {ordinal}: missing stream{stream_index} payload: {source}"
            )
        payload = source.read_bytes()
        slot_count = vertex_slot_capacity(len(payload), payload_offset, stride)
        new_for_old = vertex_slot_assignment(order, base_vertex, slot_count)
        permuted = apply_vertex_permutation(payload, payload_offset, stride, new_for_old)
        target = vertex_dir / f"draw{ordinal:03d}-{vertex_order}.stream{stream_index}.bin"
        target.write_bytes(permuted)
        entry["file"] = str(target)
        entry["bytes"] = len(permuted)
        entry["vertex_order_source_file"] = str(source)
        if stream_index == 0:
            geometry["stream0_file"] = str(target)
            geometry["stream0_bytes"] = len(permuted)

    rewritten = rewrite_indices_for_permutation(indices, base_vertex, order)
    index_target = vertex_dir / f"draw{ordinal:03d}-{vertex_order}.index.bin"
    index_target.write_bytes(struct.pack(f"<{len(rewritten)}H", *rewritten))
    geometry.setdefault("index_order_source_file", str(index_source))
    geometry["index_file"] = str(index_target)
    geometry["index_bytes"] = index_target.stat().st_size
    geometry["vertex_order"] = vertex_order
    geometry["vertex_order_referenced_vertices"] = len(order)


def materialize_replay_draws(draws: list[dict[str, Any]],
                             output_dir: Path,
                             primitive_order: str,
                             draw_order: str,
                             vertex_order: str = "original") -> list[dict[str, Any]]:
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
    if vertex_order != "original":
        vertex_dir = output_dir / "vertex-order"
        vertex_dir.mkdir(parents=True, exist_ok=True)
        for ordinal, draw in enumerate(replay_draws):
            remap_draw_vertex_layout(draw, ordinal, vertex_dir, vertex_order)
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


def has_argbuf_parameter(source: str) -> bool:
    return re.search(
        r"constant\s+ArgbufLayout&\s+abuf\s+\[\[buffer\(30\)\]\]", source
    ) is not None


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


def find_direct_cbuf_slot(source: str, type_name: str, var_name: str) -> int | None:
    """Slot of an already-direct `constant <type_name>& <var_name> [[buffer(N)]]`.

    Under `DXMT9_ARGBUF_DIRECT_CBUF=1` dumped MSL already binds constants
    directly and carries no `buffer(30)` argbuf parameter at all; this finds
    the slot Metal already uses instead of rewriting anything.
    """
    match = re.search(
        rf"constant\s+{re.escape(type_name)}&\s+{re.escape(var_name)}\s+\[\[buffer\((\d+)\)\]\]",
        source,
    )
    return int(match.group(1)) if match else None


def transform_msl(source: str, stage: str) -> tuple[str, dict[str, int]]:
    if stage == "vs":
        if has_argbuf_parameter(source):
            vs_slot, ffp_slot = allocate_cbuf_slots(source, 2)
            source = replace_argbuf_parameter(
                source,
                f"constant VsConsts& vsConsts [[buffer({vs_slot})]],\n"
                f"                     constant FfpVsConsts& ffpVs [[buffer({ffp_slot})]]",
            )
            source = re.sub(r"\s*constant VsConsts& vsConsts = \*abuf\.vsConsts;\n", "\n", source)
            source = re.sub(r"\s*constant FfpVsConsts& ffpVs = \*abuf\.ffpVs;\n", "", source)
            return source, {"vsconsts": vs_slot, "ffpvs": ffp_slot}
        vs_slot = find_direct_cbuf_slot(source, "VsConsts", "vsConsts")
        ffp_slot = find_direct_cbuf_slot(source, "FfpVsConsts", "ffpVs")
        if vs_slot is None and ffp_slot is None:
            raise SystemExit(
                "mini replay cbuf rewrite found no buffer(30) argbuf and no "
                "direct constant binding for vs stage"
            )
        if vs_slot is None:
            vs_slot = allocate_cbuf_slots(source, 1)[0]
        if ffp_slot is None:
            ffp_slot = allocate_cbuf_slots(source, 1)[0]
        return source, {"vsconsts": vs_slot, "ffpvs": ffp_slot}
    elif stage == "fs":
        if has_argbuf_parameter(source):
            ps_slot, ffp_slot = allocate_cbuf_slots(source, 2)
            source = replace_argbuf_parameter(
                source,
                f"constant PsConsts& psConsts [[buffer({ps_slot})]],\n"
                f"                     constant FfpPsConsts& ffpPs [[buffer({ffp_slot})]]",
            )
            source = re.sub(r"\s*constant PsConsts& psConsts = \*abuf\.psConsts;\n", "\n", source)
            source = re.sub(r"\s*constant FfpPsConsts& ffpPs = \*abuf\.ffpPs;\n", "", source)
            return source, {"psconsts": ps_slot, "ffpps": ffp_slot}
        ps_slot = find_direct_cbuf_slot(source, "PsConsts", "psConsts")
        ffp_slot = find_direct_cbuf_slot(source, "FfpPsConsts", "ffpPs")
        if ps_slot is None and ffp_slot is None:
            raise SystemExit(
                "mini replay cbuf rewrite found no buffer(30) argbuf and no "
                "direct constant binding for fs stage"
            )
        if ps_slot is None:
            ps_slot = allocate_cbuf_slots(source, 1)[0]
        if ffp_slot is None:
            ffp_slot = allocate_cbuf_slots(source, 1)[0]
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


def fragment_entry_return_type(source: str) -> str:
    """The declared return type of `dxmt9_fs`: `FSOut`, `FfpFsOut`, or the
    bare `float4` used only by internal utility shaders. See the
    FRAGMENT_ENTRY_RE comment for the three real emission sites."""
    match = FRAGMENT_ENTRY_RE.search(source)
    if not match:
        raise SystemExit(
            "mini replay rewrite could not find dxmt9_fs's declared "
            "fragment return type (expected `fragment <Type> dxmt9_fs(`)"
        )
    return match.group(1)


def struct_definition_body(source: str, struct_name: str) -> str:
    """The brace-matched body text of `struct <struct_name> { ... };`."""
    match = re.search(rf"struct\s+{re.escape(struct_name)}\s*\{{", source)
    if not match:
        raise SystemExit(f"mini replay rewrite could not find struct {struct_name} declaration")
    open_index = match.end() - 1
    depth = 0
    for index in range(open_index, len(source)):
        char = source[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[open_index + 1:index]
    raise SystemExit(f"mini replay rewrite could not close struct {struct_name} declaration")


def fragment_output_struct_members(source: str, struct_name: str) -> list[tuple[str, str]]:
    """`[(member_name, attribute_text), ...]` for every attributed member of
    `struct_name` (e.g. `("color0", "color(0)")`, `("sampleMask",
    "sample_mask")`). The member name is parsed from the real source, not
    assumed -- `FfpFsOut`'s color member is `color`, not `color0`."""
    body = struct_definition_body(source, struct_name)
    members = STRUCT_OUTPUT_MEMBER_RE.findall(body)
    if not members:
        raise SystemExit(
            f"mini replay rewrite found no attributed output members in struct {struct_name}"
        )
    return members


def fragment_output_member_statement(struct_name: str, name: str, attribute: str, color_expr: str) -> str:
    if "color(" in attribute:
        return f"  o.{name} = {color_expr};\n"
    if "depth(" in attribute:
        return f"  o.{name} = 0.0f;\n"
    if attribute.strip() == "sample_mask":
        return f"  o.{name} = 0xffffffffu;\n"
    raise SystemExit(
        f"mini replay rewrite does not know how to force struct member "
        f"{struct_name}.{name} [[{attribute}]] in a forced fragment output "
        f"(recognized attributes: color(N), depth(...), sample_mask)"
    )


def fragment_output_body(source: str, color_expr: str, preamble: str = "") -> str:
    """The replacement `dxmt9_fs` body that assigns `color_expr` to every
    color-attachment member of the function's declared return type, leaving
    no member (sampleMask, depth) uninitialized. For the bare-`float4`
    utility-shader shape this degrades to the historical
    `return <color_expr>;`."""
    ret_type = fragment_entry_return_type(source)
    if ret_type == "float4":
        return "{\n" + preamble + f"  return {color_expr};\n}}"
    members = fragment_output_struct_members(source, ret_type)
    body = "{\n" + preamble + f"  {ret_type} o;\n"
    for name, attribute in members:
        body += fragment_output_member_statement(ret_type, name, attribute, color_expr)
    body += "  return o;\n}"
    return body


def force_fragment_color_source(source: str) -> str:
    body = fragment_output_body(source, "float4(1.0f, 0.0f, 1.0f, 1.0f)")
    return replace_function_body(source, "dxmt9_fs", body)


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
    preamble = "  uint value = primitiveId + 1u;\n"
    color_expr = (
        "float4(float(value & 255u) / 255.0f,\n"
        "                float((value >> 8u) & 255u) / 255.0f,\n"
        "                float((value >> 16u) & 255u) / 255.0f,\n"
        "                1.0f)"
    )
    body = fragment_output_body(source, color_expr, preamble)
    return replace_function_body(source, "dxmt9_fs", body)


def fragment_texture_signature(source: str, stage: int, diagnostic: str) -> None:
    signature_start = source.find("dxmt9_fs")
    signature_end = source.find("{", signature_start)
    signature = source[signature_start:signature_end]
    if not re.search(rf"texture2d<[^>]+>\s+tex{stage}\b", signature):
        raise SystemExit(
            f"mini replay {diagnostic} diagnostic requires texture2d tex{stage} "
            "in dxmt9_fs"
        )
    if not re.search(rf"sampler\s+samp{stage}\b", signature):
        raise SystemExit(
            f"mini replay {diagnostic} diagnostic requires sampler samp{stage} "
            "in dxmt9_fs"
        )


def force_fragment_texture_lod_source(
        source: str, stage: int, coordinate: str) -> str:
    fragment_texture_signature(source, stage, "LOD")
    preamble = (
        f"  float dxmt9Lod = tex{stage}.calculate_clamped_lod("
        f"samp{stage}, in.texcoord0.{coordinate});\n"
    )
    body = fragment_output_body(
        source,
        "float4(dxmt9Lod / 16.0f, fract(dxmt9Lod), 0.0f, 1.0f)",
        preamble,
    )
    return replace_function_body(source, "dxmt9_fs", body)


def force_fragment_texture_alpha_source(
        source: str, stage: int, coordinate: str) -> str:
    fragment_texture_signature(source, stage, "alpha")
    preamble = (
        f"  float dxmt9Alpha = tex{stage}.sample("
        f"samp{stage}, in.texcoord0.{coordinate}).a;\n"
    )
    body = fragment_output_body(
        source,
        "float4(dxmt9Alpha, dxmt9Alpha, dxmt9Alpha, 1.0f)",
        preamble,
    )
    return replace_function_body(source, "dxmt9_fs", body)


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


def color_pixel_format(format_value: int, width: int = 0, height: int = 0) -> str:
    """Maps a dumped `core::Format` render-target value to a Metal pixel
    format. Per specs/experiments/harness/replay/requirements.md
    R-HARN-REPLAY-2.1, a `format_value` outside this declared finite set is a
    hard failure naming the ordinal and RT dimensions -- not a fallback to a
    plausible-looking format -- because a real capture row (`60/0`) declares
    R32F (`core::Format` 16) and rendering its raw float bytes through
    RGBA8Unorm silently reinterprets the bit pattern with no diagnostic.
    `width`/`height` are only used to name the RT in that failure message;
    callers that have no attachment metadata at all (see `render_source`'s
    `attachments`-absent legacy path) must not route through this resolver.
    """
    if format_value in (1, 2):  # A8R8G8B8 / X8R8G8B8
        return "MTLPixelFormatBGRA8Unorm"
    if format_value in (3, 4):  # A8B8G8R8 / X8B8G8R8
        return "MTLPixelFormatRGBA8Unorm"
    if format_value == 11:  # A16B16G16R16F
        return "MTLPixelFormatRGBA16Float"
    raise SystemExit(
        f"mini replay: unsupported color render-target core::Format={format_value} "
        f"(RT {width}x{height}); no MTLPixelFormat mapping is declared for this "
        f"value -- add an explicit mapping instead of falling back to RGBA8Unorm "
        f"(specs/experiments/harness/replay/requirements.md R-HARN-REPLAY-2.1)"
    )


def depth_pixel_format(format_value: int, width: int = 0, height: int = 0) -> str:
    """See `color_pixel_format`'s R-HARN-REPLAY-2.1 note: the identical
    unnamed-fallback shape applies to depth formats (for example DF16,
    `core::Format` 52, is documented in include/dxmt9/core_constants.hpp as
    backed by MTLPixelFormatDepth16Unorm, not the old catch-all
    Depth32Float)."""
    if format_value in (40, 41, 49):  # D24S8 / D24X8 / D24FS8
        return "MTLPixelFormatDepth32Float_Stencil8"
    if format_value in (42, 46):  # D16 / D16_LOCKABLE
        return "MTLPixelFormatDepth16Unorm"
    raise SystemExit(
        f"mini replay: unsupported depth render-target core::Format={format_value} "
        f"(RT {width}x{height}); no MTLPixelFormat mapping is declared for this "
        f"value -- add an explicit mapping instead of falling back to Depth32Float "
        f"(specs/experiments/harness/replay/requirements.md R-HARN-REPLAY-2.1)"
    )


def depth_format_has_stencil(format_value: int) -> bool:
    return format_value in (40, 49)


def depth_bytes_per_pixel(format_value: int) -> int:
    if format_value in (42, 46):  # D16 / D16_LOCKABLE
        return 2
    if format_value == 49:  # D24FS8 -> Depth32Float_Stencil8 dump sidecar
        return 8
    return 4


LEGACY_COLOR_PIXEL_FORMAT = "MTLPixelFormatRGBA8Unorm"
LEGACY_DEPTH_PIXEL_FORMAT = "MTLPixelFormatDepth32Float"
ATTACHMENT_RESOLVED_FROM_MANIFEST = "manifest attachment core::Format"
ATTACHMENT_RESOLVED_FROM_LEGACY = (
    "legacy default: this manifest declares no attachments, so no core::Format "
    "was classified"
)


def resolve_attachment_formats(draws: list[dict[str, Any]]) -> dict[str, Any]:
    """Resolve this replay's Metal attachment formats and record the mapping.

    Returns both the resolved `MTLPixelFormat` names and the `core::Format`
    ordinals they were resolved from, so a reader of
    `mini-replay-summary.json` can audit the mapping rather than re-deriving it
    from the manifest's raw integer (R-HARN-REPLAY-2.3). `render_source()`
    consumes the same record it reports, so the summary can never name a format
    the generated program did not use.

    A manifest that never populates per-draw `attachments` has no format value
    to classify at all; that path keeps the historical defaults and says so in
    `resolved_from`, rather than routing an inferred `format_value=0` through
    the strict resolvers and looking like a declared-but-unrecognized value
    (see `color_pixel_format`'s R-HARN-REPLAY-2.1 note).
    """
    color_attachment = first_color_attachment(draws)
    depth_attachment = first_depth_attachment(draws)
    color_width = int(color_attachment.get("width", 0))
    color_height = int(color_attachment.get("height", 0))
    depth_width = int(depth_attachment.get("width", 0))
    depth_height = int(depth_attachment.get("height", 0))
    if color_attachment:
        color_format_value: int | None = int(color_attachment.get("format", 0))
        color_format = color_pixel_format(color_format_value, color_width, color_height)
        color_resolved_from = ATTACHMENT_RESOLVED_FROM_MANIFEST
    else:
        color_format_value = None
        color_format = LEGACY_COLOR_PIXEL_FORMAT
        color_resolved_from = ATTACHMENT_RESOLVED_FROM_LEGACY
    if depth_attachment:
        depth_format_value: int | None = int(depth_attachment.get("format", 0))
        depth_format = depth_pixel_format(depth_format_value, depth_width, depth_height)
        depth_resolved_from = ATTACHMENT_RESOLVED_FROM_MANIFEST
        has_stencil = depth_format_has_stencil(depth_format_value)
        depth_bpp = depth_bytes_per_pixel(depth_format_value)
    else:
        depth_format_value = None
        depth_format = LEGACY_DEPTH_PIXEL_FORMAT
        depth_resolved_from = ATTACHMENT_RESOLVED_FROM_LEGACY
        has_stencil = depth_format_has_stencil(0)
        depth_bpp = depth_bytes_per_pixel(0)
    return {
        "color": {
            "core_format": color_format_value,
            "metal_pixel_format": color_format,
            "width": color_width,
            "height": color_height,
            "resolved_from": color_resolved_from,
        },
        "depth": {
            "core_format": depth_format_value,
            "metal_pixel_format": depth_format,
            "width": depth_width,
            "height": depth_height,
            "has_stencil": has_stencil,
            "bytes_per_pixel": depth_bpp,
            "resolved_from": depth_resolved_from,
        },
        "stencil_metal_pixel_format": (
            depth_format if has_stencil else "MTLPixelFormatInvalid"
        ),
    }


def draw_blend_state(state: dict[str, Any]) -> tuple[int, int, int, int, int, int, int, int]:
    """The subset of per-draw state that feeds a Metal color-attachment blend
    descriptor. Separate-alpha resolution is baked in here so the returned
    tuple is exactly what the PSO needs -- two draws that differ only in
    `separate_alpha` but resolve to the same effective factors share a PSO."""
    alpha_blend = int(state.get("alpha_blend", 0))
    src_blend = int(state.get("src_blend", 2))
    dst_blend = int(state.get("dst_blend", 1))
    blend_op = int(state.get("blend_op", 1))
    separate_alpha = int(state.get("separate_alpha", 0))
    src_blend_alpha = int(state.get("src_blend_alpha", src_blend))
    dst_blend_alpha = int(state.get("dst_blend_alpha", dst_blend))
    blend_op_alpha = int(state.get("blend_op_alpha", blend_op))
    color_write = int(str(state.get("color_write", "0xf")), 0)
    effective_src_alpha = src_blend_alpha if separate_alpha else src_blend
    effective_dst_alpha = dst_blend_alpha if separate_alpha else dst_blend
    effective_blend_op_alpha = blend_op_alpha if separate_alpha else blend_op
    return (
        color_write,
        alpha_blend,
        src_blend,
        dst_blend,
        blend_op,
        effective_src_alpha,
        effective_dst_alpha,
        effective_blend_op_alpha,
    )


def draw_depth_state(state: dict[str, Any]) -> tuple[int, int, int]:
    """The subset of per-draw state that feeds a Metal depth-stencil
    descriptor: (depth_enabled, depth_write, depth_func)."""
    depth_enabled = int(state.get("depth_enabled", 0))
    depth_write = int(state.get("depth_write", 0))
    depth_func = int(state.get("depth_func", 8 if not depth_enabled else 4))
    return (depth_enabled, depth_write, depth_func)


def draw_cull_mode(state: dict[str, Any]) -> int:
    return int(state.get("cull", 1))


def draw_fs_volatile(state: dict[str, Any], ordinal: int) -> tuple[int, float]:
    """The subset of per-draw state that feeds the fragment-stage FsVolatile
    binding: (alphaTest, alphaRef). Mirrors makeFsVolatile/buildFsVolatile
    (src/dxmt9/dxmt9_draw_state.cpp:514-543): alphaTest folds enable+func into
    one field (0 = alpha test disabled, otherwise the raw D3DCMPFUNC), and
    alphaRef is the raw RS_ALPHA_REF byte scaled by 1.0f/255.0f. sampleMask
    is not part of this tuple -- mini-replay is always single-sample, so the
    emitted FsVolatile initializer hardcodes 0xFFFFFFFFu the same way
    DrawVolatile hardcodes its trailing pad to 0 (see render_source's
    DrawVolatile dv initializer).

    The manifest has no alpha_ref field, so a draw with alpha_test != 0 but
    no alpha_ref must fail loudly here instead of silently substituting a
    reference of 0 -- that would render a wrong image and report success,
    exactly the class of defect this function's caller fixes."""
    alpha_test = int(str(state.get("alpha_test", "0")), 0)
    if alpha_test == 0:
        return (0, 0.0)
    if "alpha_ref" not in state:
        raise SystemExit(
            f"draw {ordinal}: alpha_test={alpha_test} but state has no "
            f"alpha_ref; the fragment alpha-test switch "
            f"(dxmt9_fs.replay.metal:308-320) requires a real reference "
            f"value -- refusing to substitute 0 and render a silently wrong "
            f"image"
        )
    alpha_ref = int(str(state.get("alpha_ref")), 0)
    return (alpha_test, alpha_ref / 255.0)


def pso_build_statement(index: int, shader_index: int,
                         blend_key: tuple[int, int, int, int, int, int, int, int]) -> str:
    """Emit the literal-valued PSO build block for one (shader, blend state)
    combo. Values are baked as C++ literals per combo -- not read from a
    runtime table -- so distinct combos are visible as distinct
    `colorWriteMask(N)` / `blendFactor(N, ...)` call sites in the generated
    source, the same way the pre-fix single-PSO path baked draws[0]'s state."""
    (color_write, alpha_blend, src_blend, dst_blend, blend_op,
     effective_src_alpha, effective_dst_alpha, effective_blend_op_alpha) = blend_key
    return f"""    psoDesc.colorAttachments[0].writeMask = colorWriteMask({color_write});
    psoDesc.colorAttachments[0].blendingEnabled = {alpha_blend};
    psoDesc.colorAttachments[0].sourceRGBBlendFactor = blendFactor({src_blend}, false);
    psoDesc.colorAttachments[0].destinationRGBBlendFactor = blendFactor({dst_blend}, false);
    psoDesc.colorAttachments[0].rgbBlendOperation = blendOperation({blend_op});
    psoDesc.colorAttachments[0].sourceAlphaBlendFactor = blendFactor({effective_src_alpha}, true);
    psoDesc.colorAttachments[0].destinationAlphaBlendFactor = blendFactor({effective_dst_alpha}, true);
    psoDesc.colorAttachments[0].alphaBlendOperation = blendOperation({effective_blend_op_alpha});
    psoDesc.vertexFunction = [vsLibs[{shader_index}] newFunctionWithName:@"dxmt9_vs"];
    psoDesc.fragmentFunction = [fsLibs[{shader_index}] newFunctionWithName:@"dxmt9_fs"];
    psos[{index}] = [device newRenderPipelineStateWithDescriptor:psoDesc error:&error];
    if (!psos[{index}]) {{
      std::cerr << "failed to create PSO {index}: "
                << [[error localizedDescription] UTF8String] << "\\n";
      return 2;
    }}
"""


def depth_build_statement(index: int, depth_key: tuple[int, int, int]) -> str:
    """Emit the literal-valued depth-stencil-state build block for one
    distinct (depth_enabled, depth_write, depth_func) tuple."""
    depth_enabled, depth_write, depth_func = depth_key
    write_enabled = 1 if (depth_enabled and depth_write) else 0
    return f"""    {{
      MTLDepthStencilDescriptor* depthStateDesc = [MTLDepthStencilDescriptor new];
      depthStateDesc.depthCompareFunction =
          {depth_enabled} ? compareFunction({depth_func}) : MTLCompareFunctionAlways;
      depthStateDesc.depthWriteEnabled = {write_enabled};
      depthStates[{index}] = [device newDepthStencilStateWithDescriptor:depthStateDesc];
    }}
"""


def metal_pixel_format_name(value: int) -> str:
    names = {
        10: "MTLPixelFormatR8Unorm",
        11: "MTLPixelFormatR8Unorm_sRGB",
        20: "MTLPixelFormatR16Unorm",
        55: "MTLPixelFormatR32Float",
        72: "MTLPixelFormatRGBA8Snorm",
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
                  texture_sidecar_by_key: dict[tuple[str, bool], int],
                  sampler_mip_filter: str) -> str:
    # The same record `prepare()` writes to `mini-replay-summary.json`
    # (R-HARN-REPLAY-2.3), so the reported format is by construction the one
    # this generated program renders with. `resolve_attachment_formats()` also
    # owns the legacy no-`attachments` default and the strict-resolver split.
    attachment_formats = resolve_attachment_formats(draws)
    color_attachment = first_color_attachment(draws)
    depth_attachment = first_depth_attachment(draws)
    color_format = attachment_formats["color"]["metal_pixel_format"]
    depth_format = attachment_formats["depth"]["metal_pixel_format"]
    stencil_format = attachment_formats["stencil_metal_pixel_format"]
    pass_width = int(color_attachment.get("width", 0)) or int(depth_attachment.get("width", 0)) or width
    pass_height = int(color_attachment.get("height", 0)) or int(depth_attachment.get("height", 0)) or height
    color_bytes_per_pixel = 8 if color_format == "MTLPixelFormatRGBA16Float" else 4
    color_row_bytes = ((pass_width * color_bytes_per_pixel + 255) // 256) * 256
    color_byte_count = color_row_bytes * pass_height
    color_is_bgra = "true" if color_format == "MTLPixelFormatBGRA8Unorm" else "false"
    color_is_float16 = "true" if color_format == "MTLPixelFormatRGBA16Float" else "false"
    sampler_mip_filter_name = {
        "none": "MTLSamplerMipFilterNotMipmapped",
        "nearest": "MTLSamplerMipFilterNearest",
        "linear": "MTLSamplerMipFilterLinear",
    }[sampler_mip_filter]
    depth_bpp = attachment_formats["depth"]["bytes_per_pixel"]
    depth_row_bytes = pass_width * depth_bpp
    depth_byte_count = depth_row_bytes * pass_height
    depth_input_path = cxx_string(str(depth_input)) if depth_input else '""'
    depth_load_action = "MTLLoadActionLoad" if depth_input else "MTLLoadActionClear"
    # Per-draw render state (blend/color-write, depth, cull) is deduplicated
    # into small tables instead of being baked once from draws[0]: real
    # captures mix depth-prepass draws (color_write=0x0) with color-writing
    # draws (color_write=0xf) in the same encoder, and a single collapsed PSO
    # silently masked every color-writing draw with the prepass mask.
    blend_states: list[tuple[int, int, int, int, int, int, int, int]] = []
    blend_state_index_by_key: dict[tuple[int, int, int, int, int, int, int, int], int] = {}
    depth_states: list[tuple[int, int, int]] = []
    depth_state_index_by_key: dict[tuple[int, int, int], int] = {}
    pso_combos: list[tuple[int, int]] = []
    pso_combo_index_by_key: dict[tuple[int, int], int] = {}

    def blend_state_index(draw_state: dict[str, Any]) -> int:
        key = draw_blend_state(draw_state)
        index = blend_state_index_by_key.get(key)
        if index is None:
            index = len(blend_states)
            blend_state_index_by_key[key] = index
            blend_states.append(key)
        return index

    def depth_state_index(draw_state: dict[str, Any]) -> int:
        key = draw_depth_state(draw_state)
        index = depth_state_index_by_key.get(key)
        if index is None:
            index = len(depth_states)
            depth_state_index_by_key[key] = index
            depth_states.append(key)
        return index

    def pso_combo_index(shader_index: int, blend_index: int) -> int:
        key = (shader_index, blend_index)
        index = pso_combo_index_by_key.get(key)
        if index is None:
            index = len(pso_combos)
            pso_combo_index_by_key[key] = index
            pso_combos.append(key)
        return index

    draw_entries = []
    for ordinal, draw in enumerate(draws):
        geometry = draw["geometry"]
        uniforms = draw.get("uniforms", {})
        state = draw["state"]
        shader_index = int(draw.get("_shader_variant", 0))
        blend_index = blend_state_index(state)
        depth_index = depth_state_index(state)
        cull_value = draw_cull_mode(state)
        alpha_test_value, alpha_ref_value = draw_fs_volatile(state, ordinal)
        pipeline_index = pso_combo_index(shader_index, blend_index)
        extra_stream_paths = ['""'] * 16
        extra_stream_slots = ["0"] * 16
        fragment_texture_indices = [
            str(index)
            for index in draw_fragment_texture_indices(draw, texture_sidecar_by_key)
        ]
        stream0_entry = next(
            (s for s in geometry.get("streams", []) if int(s.get("stream", 0)) == 0),
            None,
        )
        stream0_start_byte = int(stream0_entry.get("start_byte", 0)) if stream0_entry else 0
        stream0_payload_offset = resolve_stream_payload_offset(
            int(state.get("stream0_offset", 0)), stream0_start_byte, ordinal, 0
        )
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
                str(shader_index),
                str(pipeline_index),
                str(depth_index),
                str(cull_value),
                str(int(state.get("index_count", 0))),
                str(int(state.get("base_vertex", 0))),
                str(int(state.get("stream0_stride", 0))),
                str(stream0_payload_offset),
                str(int(state.get("scissor", 0))),
                str(int(state.get("scissor_l", 0))),
                str(int(state.get("scissor_t", 0))),
                str(int(state.get("scissor_r", pass_width))),
                str(int(state.get("scissor_b", pass_height))),
                str(alpha_test_value),
                f"{alpha_ref_value!r}f",
            ])
            + "}"
        )
    draw_array = ",\n".join(draw_entries)
    pso_combo_count = len(pso_combos)
    pso_build_statements = "\n".join(
        pso_build_statement(i, shader_index, blend_states[blend_index])
        for i, (shader_index, blend_index) in enumerate(pso_combos)
    )
    depth_state_count = len(depth_states)
    depth_build_statements = "\n".join(
        depth_build_statement(i, depth_key)
        for i, depth_key in enumerate(depth_states)
    )
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
#include <cmath>
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
  unsigned pipelineIndex;
  unsigned depthStateIndex;
  unsigned cullMode;
  unsigned indexCount;
  int baseVertex;
  unsigned streamStride;
  unsigned streamOffset;
  unsigned scissorEnabled;
  unsigned scissorL;
  unsigned scissorT;
  unsigned scissorR;
  unsigned scissorB;
  unsigned alphaTest;
  float alphaRef;
}};

struct DrawVolatile {{
  int vertexBaseIndex;
  unsigned vertexStreamOffset;
  unsigned vertexStreamStride;
  unsigned pad;
  unsigned streamInstanceDivisors[16];
}};

struct FsVolatile {{
  unsigned alphaTest;
  float alphaRef;
  unsigned sampleMask;
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

static float halfToFloat(unsigned short bits) {{
  const unsigned sign = bits >> 15u;
  const unsigned exponent = (bits >> 10u) & 0x1fu;
  const unsigned mantissa = bits & 0x3ffu;
  float value = 0.0f;
  if (exponent == 0u) {{
    value = std::ldexp(static_cast<float>(mantissa), -24);
  }} else if (exponent == 0x1fu) {{
    value = mantissa == 0u ? INFINITY : NAN;
  }} else {{
    value = std::ldexp(static_cast<float>(1024u + mantissa),
                       static_cast<int>(exponent) - 25);
  }}
  return sign != 0u ? -value : value;
}}

static unsigned char floatToUnorm8(float value) {{
  if (!std::isfinite(value)) value = 0.0f;
  value = std::clamp(value, 0.0f, 1.0f);
  return static_cast<unsigned char>(value * 255.0f + 0.5f);
}}

static void readRgb8(const unsigned char* pixel,
                     bool bgra,
                     bool rgba16Float,
                     unsigned char rgb[3]) {{
  if (rgba16Float) {{
    for (unsigned channel = 0; channel < 3; ++channel) {{
      const unsigned offset = channel * 2u;
      const unsigned short bits = static_cast<unsigned short>(
          static_cast<unsigned>(pixel[offset]) |
          (static_cast<unsigned>(pixel[offset + 1u]) << 8u));
      rgb[channel] = floatToUnorm8(halfToFloat(bits));
    }}
    return;
  }}
  rgb[0] = bgra ? pixel[2] : pixel[0];
  rgb[1] = pixel[1];
  rgb[2] = bgra ? pixel[0] : pixel[2];
}}

static bool writePpm(const char* path,
                     const unsigned char* pixels,
                     unsigned width,
                     unsigned height,
                     unsigned rowBytes,
                     unsigned pixelBytes,
                     bool bgra,
                     bool rgba16Float) {{
  std::ofstream out(path, std::ios::binary);
  if (!out) {{
    std::cerr << "failed to open color output " << path << "\\n";
    return false;
  }}
  out << "P6\\n" << width << " " << height << "\\n255\\n";
  for (unsigned y = 0; y < height; ++y) {{
    const unsigned char* row = pixels + static_cast<size_t>(y) * rowBytes;
    for (unsigned x = 0; x < width; ++x) {{
      const unsigned char* p = row + static_cast<size_t>(x) * pixelBytes;
      unsigned char rgb[3];
      readRgb8(p, bgra, rgba16Float, rgb);
      out.write(reinterpret_cast<const char*>(rgb), sizeof(rgb));
    }}
  }}
  return true;
}}

// Counts distinct RGB triples in the same readback buffer writePpm just wrote
// from, plus how many are not the clear colour. This is the binary-side half of
// the degenerate-image contract the harness wrapper enforces after the run
// (run_3dmark05_mini_replay.py::enforce_color_output_validity): one contract,
// one threshold, whichever way the replay is invoked.
static unsigned countDistinctRgb(const unsigned char* pixels,
                                 unsigned width,
                                 unsigned height,
                                 unsigned rowBytes,
                                 unsigned pixelBytes,
                                 bool bgra,
                                 bool rgba16Float,
                                 unsigned long long* nonBackground) {{
  std::vector<unsigned> values;
  values.reserve(static_cast<size_t>(width) * static_cast<size_t>(height));
  unsigned long long nonBackgroundCount = 0;
  for (unsigned y = 0; y < height; ++y) {{
    const unsigned char* row = pixels + static_cast<size_t>(y) * rowBytes;
    for (unsigned x = 0; x < width; ++x) {{
      const unsigned char* p = row + static_cast<size_t>(x) * pixelBytes;
      unsigned char rgb[3];
      readRgb8(p, bgra, rgba16Float, rgb);
      const unsigned r = rgb[0];
      const unsigned g = rgb[1];
      const unsigned b = rgb[2];
      values.push_back((r << 16) | (g << 8) | b);
      if (r != 0u || g != 0u || b != 0u) {{
        ++nonBackgroundCount;
      }}
    }}
  }}
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  if (nonBackground) {{
    *nonBackground = nonBackgroundCount;
  }}
  return static_cast<unsigned>(values.size());
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
  const NSUInteger length = ([data length] + 15u) & ~static_cast<NSUInteger>(15u);
  id<MTLBuffer> buffer =
      [device newBufferWithLength:length options:MTLResourceStorageModeShared];
  std::memset([buffer contents], 0, length);
  std::memcpy([buffer contents], [data bytes], [data length]);
  return buffer;
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
    case 0: return MTLCullModeNone;
    case 1: return MTLCullModeFront;
    case 2: return MTLCullModeBack;
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
    std::vector<id<MTLLibrary>> vsLibs(shaderCount);
    std::vector<id<MTLLibrary>> fsLibs(shaderCount);
    NSError* error = nil;
    for (unsigned i = 0; i < shaderCount; ++i) {{
      vsLibs[i] = makeLibrary(device, shaders[i].vsPath);
      fsLibs[i] = makeLibrary(device, shaders[i].fsPath);
      if (!vsLibs[i] || !fsLibs[i]) return 2;
    }}

    // One PSO per distinct (shader, blend/color-write state) combination that
    // actually occurs among the replayed draws (see draw_blend_state /
    // pso_combo_index in run_3dmark05_mini_replay.py). Each combo's values
    // are baked as literals below instead of coming from one descriptor
    // mutated from draws[0]'s state, so a depth-prepass draw's
    // color_write=0x0 no longer masks every other draw's color_write=0xf.
    const unsigned psoComboCount = {pso_combo_count};
    std::vector<id<MTLRenderPipelineState>> psos(psoComboCount);
    MTLRenderPipelineDescriptor* psoDesc = [MTLRenderPipelineDescriptor new];
    psoDesc.colorAttachments[0].pixelFormat = {color_format};
    psoDesc.depthAttachmentPixelFormat = {depth_format};
    psoDesc.stencilAttachmentPixelFormat = {stencil_format};
{pso_build_statements}

    // One MTLDepthStencilState per distinct (depth_enabled, depth_write,
    // depth_func) tuple that actually occurs among the replayed draws.
    const unsigned depthStateTableCount = {depth_state_count};
    std::vector<id<MTLDepthStencilState>> depthStates(depthStateTableCount);
{depth_build_statements}

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
    samplerDesc.mipFilter = {sampler_mip_filter_name};
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
    [encoder setFrontFacingWinding:MTLWindingClockwise];
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
        if (draw.pipelineIndex >= psoComboCount) return 2;
        if (draw.depthStateIndex >= depthStateTableCount) return 2;
        const ShaderEntry& shader = shaders[draw.shaderIndex];
        DrawVolatile dv = {{}};
        dv.vertexBaseIndex = draw.baseVertex;
        dv.vertexStreamOffset = draw.streamOffset;
        dv.vertexStreamStride = draw.streamStride;
        FsVolatile fsv = {{draw.alphaTest, draw.alphaRef, 0xFFFFFFFFu, 0}};
        [encoder setScissorRect:scissorRect(draw, {pass_width}, {pass_height})];
        [encoder setRenderPipelineState:psos[draw.pipelineIndex]];
        [encoder setDepthStencilState:depthStates[draw.depthStateIndex]];
        [encoder setCullMode:cullMode(draw.cullMode)];
{per_draw_fragment_texture_binds}
        [encoder setVertexBuffer:drawVsConsts offset:0 atIndex:shader.vsConstsSlot];
        [encoder setVertexBuffer:drawFfpVs offset:0 atIndex:shader.ffpVsSlot];
        [encoder setFragmentBuffer:drawPsConsts offset:0 atIndex:shader.psConstsSlot];
        [encoder setFragmentBuffer:drawFfpPs offset:0 atIndex:shader.ffpPsSlot];
        [encoder setFragmentBytes:&fsv length:sizeof(fsv) atIndex:5];
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
    if ([commandBuffer status] == MTLCommandBufferStatusError) {{
      std::cerr << "mini replay command buffer failed: "
                << [[[commandBuffer error] localizedDescription] UTF8String]
                << "\\n";
      return 2;
    }}
    long long distinctRgb = -1;
    unsigned long long nonBackgroundPixels = 0;
    if (colorOutputPath && colorOutputPath[0] != '\\0' && colorReadback) {{
      const unsigned char* pixels =
          static_cast<const unsigned char*>([colorReadback contents]);
      if (!writePpm(colorOutputPath, pixels, {pass_width}, {pass_height},
                    {color_row_bytes}, {color_bytes_per_pixel},
                    {color_is_bgra}, {color_is_float16})) {{
        return 2;
      }}
      distinctRgb = static_cast<long long>(
          countDistinctRgb(pixels, {pass_width}, {pass_height},
                           {color_row_bytes}, {color_bytes_per_pixel},
                           {color_is_bgra}, {color_is_float16},
                           &nonBackgroundPixels));
    }}
    if ([[MTLCaptureManager sharedCaptureManager] isCapturing]) {{
      [[MTLCaptureManager sharedCaptureManager] stopCapture];
    }}
    std::cout << "mini replay draws=" << (sizeof(draws) / sizeof(draws[0]))
              << " repeat=" << repeat << " distinct_rgb=";
    if (distinctRgb < 0) {{
      std::cout << "n/a";
    }} else {{
      std::cout << distinctRgb;
    }}
    std::cout << "\\n";
    // An image carrying one distinct value holds no information, so any pixel
    // comparison against it is vacuous -- the all-black failure shape fixed in
    // 36a41ad5, which printed `mini replay draws=229 repeat=1` and exited 0.
    // Wording is kept in step with degenerate_message() in the harness wrapper.
    if (distinctRgb >= 0 &&
        static_cast<unsigned>(distinctRgb) < {MIN_DISTINCT_RGB_VALUES}u) {{
      std::cerr << "mini replay color output is degenerate: " << colorOutputPath
                << " carries " << distinctRgb << " distinct RGB value across "
                << (static_cast<unsigned long long>({pass_width}) * {pass_height}ull)
                << " pixels (" << nonBackgroundPixels
                << " non-background), require at least "
                << {MIN_DISTINCT_RGB_VALUES}u
                << ". An image with one value holds no information, so any "
                   "pixel comparison against it is vacuous; this is the "
                   "all-black failure shape fixed in 36a41ad5\\n";
      return {REPLAY_DEGENERATE_EXIT_STATUS};
    }}
  }}
  return 0;
}}
"""


def shader_hash_counts(replay_draws: list[dict[str, Any]], key: str) -> dict[str, int]:
    """Count replayed draws per manifest shader hash.

    Keyed by the manifest's own `shaders.vs_hash` / `shaders.ps_hash` strings so
    a reader can match a changed shader against the replay by the same hash the
    dump and the joined Xcode reports use. The counts must sum to the replayed
    draw count, so a draw with no hash lands in UNKNOWN_SHADER_HASH rather than
    disappearing.
    """
    counts: dict[str, int] = {}
    for draw in replay_draws:
        value = str(draw.get("shaders", {}).get(key, "") or "").strip()
        counts[value or UNKNOWN_SHADER_HASH] = counts.get(value or UNKNOWN_SHADER_HASH, 0) + 1
    return dict(sorted(counts.items()))


def build_coverage(manifest: dict[str, Any],
                   replay_draws: list[dict[str, Any]],
                   draw_count: int,
                   shader_variant_count: int) -> dict[str, Any]:
    """Describe what this replay contains, so a pixel result can be read in scope.

    This block makes no correctness claim. It answers only "what was in the
    replay": which manifest rows and encoders, how many draws, and how many
    draws use each vertex/pixel shader hash. A reader who changed a shader can
    tell from `draws_by_vs_hash` whether that shader is present at all and in
    how many draws -- the question nobody could answer when a byte-identical
    replay was read as proof of a translator change that had, in fact, never
    executed the branch under test (R-HARN-REPLAY-3.4).
    """
    manifest_summary = manifest.get("summary") or {}
    rows = sorted({str(draw.get("row", "")) for draw in replay_draws if draw.get("row")})
    encoders = sorted({
        f"{draw.get('seq')}/{draw.get('encoder')}"
        for draw in replay_draws
        if draw.get("seq") is not None and draw.get("encoder") is not None
    })
    return {
        "scope": COVERAGE_SCOPE,
        # Reported by the manifest builder; None when the manifest carries no
        # summary block, which must not be confused with "covers nothing".
        "manifest_rows": manifest_summary.get("rows"),
        "manifest_encoder_draw_min": manifest_summary.get("encoder_draw_min"),
        "manifest_encoder_draw_max": manifest_summary.get("encoder_draw_max"),
        "manifest_draw_count": manifest_summary.get("draw_count"),
        # Observed in the draws actually materialized for this replay.
        "rows": rows,
        "encoders": encoders,
        "draw_count": draw_count,
        "shader_variant_count": shader_variant_count,
        "draws_by_vs_hash": shader_hash_counts(replay_draws, "vs_hash"),
        "draws_by_ps_hash": shader_hash_counts(replay_draws, "ps_hash"),
    }


def not_asserted_validity(reason: str) -> dict[str, Any]:
    """A `validity` record for a run that produced no image to assert on.

    `validity` is never omitted from the summary: an absent field would read as
    a pass to any consumer that only checks for a failure marker.
    """
    return {"asserted": False, "reason": reason}


def read_ppm_rgb(path: Path) -> tuple[int, int, bytes]:
    """Read back the P6 PPM the replay binary wrote (see writePpm in the
    generated source). Malformed or truncated output is a hard failure, not a
    fallback -- same no-silent-degradation contract as color_pixel_format()."""
    if not path.exists():
        raise SystemExit(f"mini replay color output was not written: {path}")
    data = path.read_bytes()
    if not data.startswith(b"P6"):
        raise SystemExit(
            f"mini replay color output is not a P6 PPM: {path} "
            f"(starts with {data[:2]!r})"
        )
    fields: list[bytes] = []
    offset = 2
    while len(fields) < 3:
        while offset < len(data) and data[offset:offset + 1].isspace():
            offset += 1
        if offset < len(data) and data[offset:offset + 1] == b"#":
            while offset < len(data) and data[offset:offset + 1] != b"\n":
                offset += 1
            continue
        start = offset
        while offset < len(data) and not data[offset:offset + 1].isspace():
            offset += 1
        if offset == start:
            raise SystemExit(f"mini replay color output has a truncated PPM header: {path}")
        fields.append(data[start:offset])
    offset += 1  # single whitespace byte terminating the maxval field
    try:
        width, height, maxval = (int(field) for field in fields)
    except ValueError as exc:
        raise SystemExit(
            f"mini replay color output has a malformed PPM header: {path} ({fields!r})"
        ) from exc
    if maxval != 255:
        raise SystemExit(
            f"mini replay color output has unsupported PPM maxval {maxval}: {path}"
        )
    body = data[offset:]
    expected = width * height * 3
    if len(body) != expected:
        raise SystemExit(
            f"mini replay color output is truncated: {path} carries {len(body)} "
            f"pixel bytes, expected {expected} for {width}x{height}"
        )
    return width, height, body


def assess_color_output(path: Path) -> dict[str, Any]:
    """Assert the written image is non-degenerate and report what was measured.

    Counts distinct RGB triples and non-background pixels. It proves only that
    the image carries more than one value; it says nothing about whether the
    replay covered the code under test. Those are separate claims and the
    returned record keeps them separate (see VALIDITY_CERTIFIES).
    """
    width, height, body = read_ppm_rgb(path)
    background = bytes(BACKGROUND_RGB)
    distinct: set[bytes] = set()
    non_background = 0
    for index in range(0, len(body), 3):
        pixel = body[index:index + 3]
        distinct.add(pixel)
        if pixel != background:
            non_background += 1
    return {
        "asserted": True,
        "color_output": str(path),
        "width": width,
        "height": height,
        "pixel_count": width * height,
        "distinct_rgb_values": len(distinct),
        "non_background_pixels": non_background,
        "background_rgb": list(BACKGROUND_RGB),
        "min_distinct_rgb_values": MIN_DISTINCT_RGB_VALUES,
        "degenerate": len(distinct) < MIN_DISTINCT_RGB_VALUES,
        "certifies": VALIDITY_CERTIFIES,
    }


def degenerate_message(validity: dict[str, Any]) -> str:
    return (
        f"mini replay color output is degenerate: {validity['color_output']} "
        f"carries {validity['distinct_rgb_values']} distinct RGB value across "
        f"{validity['pixel_count']} pixels "
        f"({validity['non_background_pixels']} non-background), require at "
        f"least {validity['min_distinct_rgb_values']}. An image with one value "
        f"holds no information, so any pixel comparison against it is vacuous; "
        f"this is the all-black failure shape fixed in 36a41ad5"
    )


def enforce_color_output_validity(validity: dict[str, Any]) -> None:
    """Exit non-zero on a degenerate image (R-HARN-REPLAY-3.1).

    A pass here is not evidence of coverage; it only removes the degenerate
    case from the set of things a comparison could be silently reading.
    """
    if validity.get("degenerate"):
        raise SystemExit(degenerate_message(validity))


def parse_prove_executed(value: str) -> tuple[str, str]:
    """Split a `REGEX=>REPLACEMENT` argument on its first separator.

    The replacement half may itself contain the separator; the pattern half may
    not, which is the deliberate limit of a raw two-field interface.
    """
    if PROVE_EXECUTED_SEPARATOR not in value:
        raise SystemExit(
            f"invalid --prove-executed argument: {value!r} has no "
            f"{PROVE_EXECUTED_SEPARATOR!r} separator; expected "
            f"'REGEX{PROVE_EXECUTED_SEPARATOR}REPLACEMENT'"
        )
    pattern, replacement = value.split(PROVE_EXECUTED_SEPARATOR, 1)
    if not pattern:
        raise SystemExit(
            f"invalid --prove-executed argument: {value!r} has an empty regex"
        )
    return pattern, replacement


class ShaderMutation:
    """A `re.sub` applied to every generated `.metal` source, with the counts.

    The counts are the evidence that separates the two failure modes: zero
    sites means the construct is not present, and a non-zero site count with an
    unchanged image means it is present and unexecuted. Collapsing those into
    one "the check failed" message is precisely how defect 7 could be
    misdiagnosed a second time.
    """

    def __init__(self, pattern: str, replacement: str) -> None:
        try:
            self.regex = re.compile(pattern)
        except re.error as exc:
            raise SystemExit(
                f"invalid --prove-executed regex {pattern!r}: {exc}"
            ) from exc
        self.pattern = pattern
        self.replacement = replacement
        self.files_scanned = 0
        self.files_mutated = 0
        self.sites = 0
        self.mutated_files: list[str] = []

    def apply(self, source: str, label: str) -> str:
        self.files_scanned += 1
        try:
            mutated, count = self.regex.subn(self.replacement, source)
        except re.error as exc:
            raise SystemExit(
                f"invalid --prove-executed replacement {self.replacement!r}: {exc}"
            ) from exc
        if count:
            self.files_mutated += 1
            self.sites += count
            self.mutated_files.append(label)
        return mutated

    def record(self) -> dict[str, Any]:
        return {
            "pattern": self.pattern,
            "replacement": self.replacement,
            "files_scanned": self.files_scanned,
            "files_mutated": self.files_mutated,
            "sites": self.sites,
            "mutated_files": list(self.mutated_files),
        }


def count_differing_pixels(baseline: Path, mutated: Path) -> tuple[int, int]:
    """Count pixels that differ between two replay images.

    Differing dimensions are a hard failure: the two runs would no longer be
    the same replay differing only by the substitution, so no comparison
    between them means anything.
    """
    baseline_width, baseline_height, baseline_body = read_ppm_rgb(baseline)
    mutated_width, mutated_height, mutated_body = read_ppm_rgb(mutated)
    if (baseline_width, baseline_height) != (mutated_width, mutated_height):
        raise SystemExit(
            f"mini replay --prove-executed images have different dimensions: "
            f"{baseline} is {baseline_width}x{baseline_height} but {mutated} is "
            f"{mutated_width}x{mutated_height}; the mutated replay must differ "
            f"from the baseline only by the substitution"
        )
    differing = sum(
        1 for index in range(0, len(baseline_body), 3)
        if baseline_body[index:index + 3] != mutated_body[index:index + 3]
    )
    return differing, baseline_width * baseline_height


def build_execution_proof(mutation: ShaderMutation,
                          baseline_output: Path | None,
                          mutated_output: Path | None,
                          differing_pixels: int | None,
                          pixel_count: int | None) -> dict[str, Any]:
    """Record what the mutation touched and what the comparison showed.

    Records the verdict as one of three named values rather than a boolean, so
    "not present" can never be read back out of the artifact as "not executed".
    """
    if mutation.sites == 0:
        verdict = EXECUTION_VERDICT_NOT_PRESENT
    elif differing_pixels:
        verdict = EXECUTION_VERDICT_EXECUTED
    else:
        verdict = EXECUTION_VERDICT_NOT_EXECUTED
    proof = dict(mutation.record())
    proof.update({
        "asserted": True,
        "verdict": verdict,
        "baseline_color_output": str(baseline_output) if baseline_output else None,
        "mutated_color_output": str(mutated_output) if mutated_output else None,
        "differing_pixels": differing_pixels,
        "pixel_count": pixel_count,
        "certifies": EXECUTION_PROOF_CERTIFIES,
    })
    return proof


def execution_proof_message(proof: dict[str, Any]) -> str:
    """One message per verdict. The two failing ones must not read alike."""
    verdict = proof["verdict"]
    if verdict == EXECUTION_VERDICT_NOT_PRESENT:
        return (
            f"mini replay --prove-executed: the construct is NOT PRESENT. "
            f"Pattern {proof['pattern']!r} matched 0 sites across "
            f"{proof['files_scanned']} generated .metal shader sources. This is "
            f"not an execution verdict: nothing was mutated, so this run says "
            f"nothing about whether the construct would execute. Either the "
            f"pattern is wrong or these dumped shaders do not carry the "
            f"emission under test -- read a generated *.replay.metal in the "
            f"output directory and correct the pattern before reading any "
            f"execution result"
        )
    if verdict == EXECUTION_VERDICT_NOT_EXECUTED:
        return (
            f"mini replay --prove-executed: the construct is present but was "
            f"NEVER EXECUTED by this replay. Pattern {proof['pattern']!r} "
            f"matched {proof['sites']} sites in {proof['files_mutated']} of "
            f"{proof['files_scanned']} generated .metal shader sources, yet the "
            f"mutated replay produced a byte-identical image (0 of "
            f"{proof['pixel_count']} pixels differ between "
            f"{proof['baseline_color_output']} and "
            f"{proof['mutated_color_output']}). A pixel comparison over this "
            f"replay therefore certifies nothing about that construct -- this "
            f"is defect 7's exact shape, where a byte-identical replay was read "
            f"as proof of a codegen change whose branch was never reached"
        )
    return (
        f"mini replay --prove-executed: EXECUTED. Pattern {proof['pattern']!r} "
        f"matched {proof['sites']} sites in {proof['files_mutated']} of "
        f"{proof['files_scanned']} generated .metal shader sources and changed "
        f"{proof['differing_pixels']} of {proof['pixel_count']} pixels, so this "
        f"replay does reach the construct. This is an execution result for the "
        f"replayed draw window only; it is not a correctness result"
    )


def enforce_execution_proof(proof: dict[str, Any]) -> None:
    """Exit non-zero on either failing verdict (R-HARN-REPLAY-3.4/3.5).

    A pass here is not evidence of correctness, only that the replay reaches
    the mutated construct at all.
    """
    if proof["verdict"] != EXECUTION_VERDICT_EXECUTED:
        raise SystemExit(execution_proof_message(proof))


def write_summary(summary: dict[str, Any], output_dir: Path) -> None:
    (output_dir / "mini-replay-summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True), encoding="utf-8")


def prepare(args: argparse.Namespace,
            shader_mutation: ShaderMutation | None = None) -> dict[str, Any]:
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
        args.vertex_order,
    )
    for draw in replay_draws:
        state = draw.setdefault("state", {})
        if args.disable_cull:
            state["cull"] = 0
        if args.disable_depth:
            state["depth_enabled"] = 0
            state["depth_write"] = 0
            state["depth_func"] = 8
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
            if args.force_fragment_texture_lod is not None:
                fs_source = force_fragment_texture_lod_source(
                    fs_source,
                    args.force_fragment_texture_lod,
                    args.texture_coordinate,
                )
            if args.force_fragment_texture_alpha is not None:
                fs_source = force_fragment_texture_alpha_source(
                    fs_source,
                    args.force_fragment_texture_alpha,
                    args.texture_coordinate,
                )
            if shader_mutation is not None:
                # Applied last, to the exact text that would otherwise have been
                # compiled, so the mutated replay differs from the baseline by
                # the substitution and nothing else.
                vs_source = shader_mutation.apply(vs_source, str(vs_replay))
                fs_source = shader_mutation.apply(fs_source, str(fs_replay))
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
            args.sampler_mip_filter,
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
        "disable_cull": args.disable_cull,
        "disable_depth": args.disable_depth,
        "vertex_order": args.vertex_order,
        "force_fragment_color": bool(args.force_fragment_color),
        "force_fragment_primitive_id": bool(args.force_fragment_primitive_id),
        "force_fragment_texture_lod": args.force_fragment_texture_lod,
        "force_fragment_texture_alpha": args.force_fragment_texture_alpha,
        "texture_coordinate": args.texture_coordinate,
        "sampler_mip_filter": args.sampler_mip_filter,
        # Present only on the mutated copy an execution proof generates; None
        # on the baseline, so the two trees are distinguishable from their own
        # artifacts.
        "shader_mutation": shader_mutation.record() if shader_mutation else None,
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
        # The Metal formats this run actually resolved, beside the
        # `core::Format` ordinals they came from, so the mapping is auditable
        # from the artifact (R-HARN-REPLAY-2.3).
        "attachment_formats": resolve_attachment_formats(replay_draws),
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
    # Coverage answers "what did this replay contain"; validity answers "is the
    # image it wrote non-degenerate". They are recorded as two separate fields
    # because neither implies the other.
    summary["coverage"] = build_coverage(
        manifest,
        replay_draws,
        summary["draw_count"],
        summary["shader_variant_count"],
    )
    summary["validity"] = not_asserted_validity(
        VALIDITY_REASON_NO_COLOR_OUTPUT if args.color_output is None
        else VALIDITY_REASON_NOT_RUN
    )
    write_summary(summary, args.output_dir)
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
               color_output: Path | None) -> dict[str, Any]:
    """Run the replay binary and return the `validity` record for its output.

    A zero exit from the binary is not by itself evidence that the render
    produced anything (R-HARN-REPLAY-3.2): it printed `mini replay draws=229
    repeat=1` for four fully black images. When a color output was requested,
    the artifact itself is read back and asserted.

    The generated program now runs the same degenerate check itself and returns
    REPLAY_DEGENERATE_EXIT_STATUS, so that status is carried through rather than
    raised: the wrapper still owns the diagnostic, because it is the only side
    that can also record the measured counts in `validity` and handle the
    missing/truncated/unreadable-image cases. Any other non-zero status is a
    failure of the replay itself and stops the run.
    """
    env = os.environ.copy()
    env["DXMT9_MINI_REPLAY_REPEAT"] = str(repeat)
    if capture_path is not None:
        env["MTL_CAPTURE_ENABLED"] = "1"
        env["DXMT9_MINI_REPLAY_CAPTURE_PATH"] = str(capture_path)
    if color_output is not None:
        color_output.parent.mkdir(parents=True, exist_ok=True)
        env["DXMT9_MINI_REPLAY_COLOR_OUTPUT_PATH"] = str(color_output)
    completed = subprocess.run([summary["binary"]], env=env)
    if completed.returncode not in (0, REPLAY_DEGENERATE_EXIT_STATUS):
        raise SystemExit(
            f"mini replay binary failed with exit status {completed.returncode}: "
            f"{summary['binary']}"
        )
    if color_output is None:
        return not_asserted_validity(VALIDITY_REASON_NO_COLOR_OUTPUT)
    return assess_color_output(color_output)


def build_parser() -> argparse.ArgumentParser:
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
        "--vertex-order",
        choices=("original", "first-reference", "scatter"),
        default="original",
        help=(
            "permute vertex storage and rewrite indices before replay while "
            "preserving triangle order, vertex bytes, payload size, and "
            "base_vertex; first-reference monotonizes index references, "
            "scatter decorrelates them for the locality discriminator"
        ),
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
    parser.add_argument(
        "--force-fragment-texture-lod",
        type=int,
        metavar="STAGE",
        help=(
            "diagnostic: replace the fragment body with texSTAGE implicit LOD "
            "encoded as R=lod/16 and G=fract(lod)"
        ),
    )
    parser.add_argument(
        "--force-fragment-texture-alpha",
        type=int,
        metavar="STAGE",
        help=(
            "diagnostic: replace the fragment body with texSTAGE sampled alpha "
            "as grayscale; useful when authored mip alpha is a level marker"
        ),
    )
    parser.add_argument(
        "--texture-coordinate",
        choices=("xy", "zw"),
        default="xy",
        help="texcoord0 component pair used by texture diagnostics (default: xy)",
    )
    parser.add_argument(
        "--sampler-mip-filter",
        choices=("none", "nearest", "linear"),
        default="none",
        help="mip filter for the replay-owned sampler (default: none)",
    )
    parser.add_argument(
        "--disable-cull",
        action="store_true",
        help="diagnostic: replay every draw with culling disabled",
    )
    parser.add_argument(
        "--disable-depth",
        action="store_true",
        help="diagnostic: replay every draw with depth testing and writes disabled",
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
    parser.add_argument(
        "--prove-executed",
        metavar=f"REGEX{PROVE_EXECUTED_SEPARATOR}REPLACEMENT",
        type=parse_prove_executed,
        help=(
            "prove the replay actually executes a construct, instead of only "
            "containing it: replay once normally, then re-replay a copy of the "
            "generated shader sources with re.sub(REGEX, REPLACEMENT) applied "
            "to every .metal file, and exit non-zero when the two images are "
            "identical -- an identical image proves the mutated construct was "
            "never executed. Requires --run and --color-output. Two distinct "
            "failures are reported: 'not present' when the pattern matched no "
            "site (wrong pattern, or the construct is absent from these dumped "
            "shaders -- this is not an execution verdict), and 'present but "
            "never executed' when it matched sites and the image did not move. "
            "This is a raw regex over emitted MSL text with no knowledge of "
            "shader syntax: it will happily match inside comments, strings, or "
            "an unrelated identifier that shares a substring, and a "
            "replacement that does not compile fails the run at compile time. "
            "Read a generated *.replay.metal and check the match count in the "
            "summary's execution_proof block before trusting a verdict"
        ),
    )
    return parser


def run_execution_proof(args: argparse.Namespace,
                        mutation: ShaderMutation,
                        mutated_summary: dict[str, Any],
                        mutated_color_output: Path) -> dict[str, Any]:
    """Compile and run the mutated replay, then compare it against the baseline.

    The mutated run's own `validity` is recorded but not enforced: a mutation
    that blanks the image is a difference, not a degenerate baseline, and the
    comparison is what this proof is about.
    """
    compile_source(mutated_summary)
    mutated_summary["validity"] = run_binary(
        mutated_summary, args.repeat, None, mutated_color_output)
    write_summary(mutated_summary, Path(mutated_summary["output_dir"]))
    differing, pixel_count = count_differing_pixels(
        args.color_output, mutated_color_output)
    return build_execution_proof(
        mutation, args.color_output, mutated_color_output, differing, pixel_count)


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    fragment_overrides = sum((
        bool(args.force_fragment_color),
        bool(args.force_fragment_primitive_id),
        args.force_fragment_texture_lod is not None,
        args.force_fragment_texture_alpha is not None,
    ))
    if fragment_overrides > 1:
        raise SystemExit("fragment diagnostic overrides are mutually exclusive")
    if (args.force_fragment_texture_lod is not None and
            not 0 <= args.force_fragment_texture_lod < 16):
        raise SystemExit("--force-fragment-texture-lod must be in [0, 15]")
    if (args.force_fragment_texture_alpha is not None and
            not 0 <= args.force_fragment_texture_alpha < 16):
        raise SystemExit("--force-fragment-texture-alpha must be in [0, 15]")

    # Validated before any work: a malformed proof request should cost nothing.
    mutation: ShaderMutation | None = None
    if args.prove_executed is not None:
        if not args.run or args.color_output is None:
            raise SystemExit(
                "--prove-executed requires --run and --color-output: the proof "
                "is a comparison between the baseline image and the mutated "
                "replay's image, so both replays have to actually run"
            )
        mutation = ShaderMutation(*args.prove_executed)

    summary = prepare(args)
    print(json.dumps(summary, indent=2, sort_keys=True))

    mutated_summary: dict[str, Any] | None = None
    mutated_color_output: Path | None = None
    if mutation is not None:
        mutated_args = copy.copy(args)
        mutated_args.output_dir = args.output_dir / EXECUTION_PROOF_DIR_NAME
        mutated_color_output = mutated_args.output_dir / "mutated-color.ppm"
        mutated_args.color_output = mutated_color_output
        mutated_args.capture_path = None
        mutated_args.prove_executed = None
        mutated_summary = prepare(mutated_args, shader_mutation=mutation)
        if mutation.sites == 0:
            # Decided from the generated sources alone, before anything is
            # compiled or run: a wrong pattern must not cost a GPU replay, and
            # must never be reported as an execution verdict.
            summary["execution_proof"] = build_execution_proof(
                mutation, None, None, None, None)
            write_summary(summary, args.output_dir)
            enforce_execution_proof(summary["execution_proof"])

    if args.run and args.capture_path is not None:
        check_capture_free_space(args.capture_path, args.min_capture_free_mb)
    if args.compile or args.run:
        compile_source(summary)
    if args.run:
        summary["validity"] = run_binary(
            summary, args.repeat, args.capture_path, args.color_output)
        # Record the result before enforcing it, so a degenerate run leaves the
        # measured numbers behind rather than only an error message.
        write_summary(summary, args.output_dir)
        enforce_color_output_validity(summary["validity"])
    if mutation is not None:
        assert mutated_summary is not None and mutated_color_output is not None
        summary["execution_proof"] = run_execution_proof(
            args, mutation, mutated_summary, mutated_color_output)
        write_summary(summary, args.output_dir)
        enforce_execution_proof(summary["execution_proof"])
        print(execution_proof_message(summary["execution_proof"]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
