import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts" / "tools"))

import run_3dmark05_mini_replay as mini  # noqa: E402


STRIDE = 8
SLOT_COUNT = 5
MODES = ("first-reference", "scatter")
# Two triangles referencing slots 4, 2 and 0, leaving slots 1 and 3 unreferenced.
INDICES = [4, 2, 4, 0, 2, 4]


def make_payload(slot_count: int = SLOT_COUNT, offset: int = 0) -> bytes:
    """Each slot is filled with a distinct repeated byte so fetches are identifiable."""
    body = b"".join(bytes([0x10 + slot]) * STRIDE for slot in range(slot_count))
    return b"\xee" * offset + body


def fetch(payload: bytes, offset: int, stride: int, slot: int) -> bytes:
    start = offset + slot * stride
    return payload[start:start + stride]


def permute(indices, base_vertex, mode, offset=0, slot_count=SLOT_COUNT):
    payload = make_payload(slot_count=slot_count, offset=offset)
    capacity = mini.vertex_slot_capacity(len(payload), offset, STRIDE)
    order = mini.vertex_reference_order(indices, base_vertex, mode)
    new_for_old = mini.vertex_slot_assignment(order, base_vertex, capacity)
    new_payload = mini.apply_vertex_permutation(payload, offset, STRIDE, new_for_old)
    new_indices = mini.rewrite_indices_for_permutation(indices, base_vertex, order)
    return payload, new_payload, order, new_for_old, new_indices, capacity


class VertexSlotCapacityTest(unittest.TestCase):
    def test_capacity_accounts_for_offset(self):
        self.assertEqual(mini.vertex_slot_capacity(40, 0, 8), 5)
        self.assertEqual(mini.vertex_slot_capacity(40, 8, 8), 4)
        self.assertEqual(mini.vertex_slot_capacity(43, 3, 8), 5)

    def test_rejects_nonpositive_stride(self):
        with self.assertRaises(SystemExit):
            mini.vertex_slot_capacity(40, 0, 0)

    def test_rejects_payload_too_small_for_one_vertex(self):
        with self.assertRaises(SystemExit):
            mini.vertex_slot_capacity(8, 4, 8)


class VertexReferenceOrderTest(unittest.TestCase):
    def test_first_reference_order(self):
        self.assertEqual(
            mini.vertex_reference_order(INDICES, 0, "first-reference"),
            [4, 2, 0],
        )

    def test_base_vertex_shifts_slots(self):
        self.assertEqual(
            mini.vertex_reference_order([0, 1, 0], 2, "first-reference"),
            [2, 3],
        )

    def test_scatter_is_a_permutation_of_first_reference(self):
        first = mini.vertex_reference_order(INDICES, 0, "first-reference")
        scatter = mini.vertex_reference_order(INDICES, 0, "scatter")
        self.assertCountEqual(scatter, first)

    def test_scatter_differs_from_first_reference(self):
        first = mini.vertex_reference_order(INDICES, 0, "first-reference")
        scatter = mini.vertex_reference_order(INDICES, 0, "scatter")
        self.assertNotEqual(scatter, first)

    def test_scatter_is_deterministic(self):
        self.assertEqual(
            mini.vertex_reference_order(INDICES, 0, "scatter"),
            mini.vertex_reference_order(INDICES, 0, "scatter"),
        )

    def test_rejects_unknown_mode(self):
        with self.assertRaises(SystemExit):
            mini.vertex_reference_order(INDICES, 0, "sideways")


class VertexSlotAssignmentTest(unittest.TestCase):
    def test_assignment_is_a_bijection(self):
        for mode in MODES:
            with self.subTest(mode=mode):
                _, _, _, new_for_old, _, capacity = permute(INDICES, 0, mode)
                self.assertEqual(len(new_for_old), capacity)
                self.assertCountEqual(new_for_old, range(capacity))

    def test_referenced_slots_land_at_base_vertex_onwards(self):
        order = mini.vertex_reference_order(INDICES, 0, "first-reference")
        new_for_old = mini.vertex_slot_assignment(order, 0, SLOT_COUNT)
        for position, slot in enumerate(order):
            self.assertEqual(new_for_old[slot], position)

    def test_unreferenced_slots_fill_remaining_positions_ascending(self):
        order = mini.vertex_reference_order(INDICES, 0, "first-reference")
        new_for_old = mini.vertex_slot_assignment(order, 0, SLOT_COUNT)
        # order = [4, 2, 0] -> slots 4,2,0 take new slots 0,1,2.
        # Unreferenced slots 1 and 3 take the remaining new slots 3 and 4.
        self.assertEqual(new_for_old, [2, 3, 1, 4, 0])

    def test_rejects_more_referenced_vertices_than_slots(self):
        with self.assertRaises(SystemExit):
            mini.vertex_slot_assignment([0, 1, 2], 0, 2)

    def test_rejects_slot_beyond_capacity(self):
        with self.assertRaises(SystemExit):
            mini.vertex_slot_assignment([0, 7], 0, 5)


class FetchEquivalenceTest(unittest.TestCase):
    """The load-bearing invariant: every index position must fetch the same bytes."""

    def test_fetch_equivalence(self):
        for mode in MODES:
            for offset in (0, 3, 8):
                with self.subTest(mode=mode, offset=offset):
                    payload, new_payload, _, _, new_indices, _ = permute(
                        INDICES, 0, mode, offset=offset
                    )
                    for old_index, new_index in zip(INDICES, new_indices):
                        self.assertEqual(
                            fetch(new_payload, offset, STRIDE, new_index),
                            fetch(payload, offset, STRIDE, old_index),
                        )

    def test_fetch_equivalence_with_base_vertex(self):
        indices = [0, 1, 0, 2, 1, 0]
        base_vertex = 2
        for mode in MODES:
            with self.subTest(mode=mode):
                payload, new_payload, _, _, new_indices, _ = permute(
                    indices, base_vertex, mode, slot_count=5
                )
                for old_index, new_index in zip(indices, new_indices):
                    self.assertEqual(
                        fetch(new_payload, 0, STRIDE, new_index + base_vertex),
                        fetch(payload, 0, STRIDE, old_index + base_vertex),
                    )

    def test_multiple_strides_share_one_order(self):
        """Extra streams reuse the same reference order with their own stride."""
        order = mini.vertex_reference_order(INDICES, 0, "first-reference")
        new_indices = mini.rewrite_indices_for_permutation(INDICES, 0, order)
        for stride in (4, 8, 12):
            with self.subTest(stride=stride):
                payload = b"".join(bytes([0x10 + slot]) * stride for slot in range(SLOT_COUNT))
                capacity = mini.vertex_slot_capacity(len(payload), 0, stride)
                new_for_old = mini.vertex_slot_assignment(order, 0, capacity)
                new_payload = mini.apply_vertex_permutation(payload, 0, stride, new_for_old)
                for old_index, new_index in zip(INDICES, new_indices):
                    self.assertEqual(
                        fetch(new_payload, 0, stride, new_index),
                        fetch(payload, 0, stride, old_index),
                    )


class PayloadPreservationTest(unittest.TestCase):
    def test_size_and_byte_multiset_preserved(self):
        for mode in MODES:
            for offset in (0, 3):
                with self.subTest(mode=mode, offset=offset):
                    payload, new_payload, _, _, _, _ = permute(
                        INDICES, 0, mode, offset=offset
                    )
                    self.assertEqual(len(new_payload), len(payload))
                    self.assertEqual(sorted(new_payload), sorted(payload))

    def test_bytes_before_offset_are_untouched(self):
        offset = 3
        payload, new_payload, _, _, _, _ = permute(
            INDICES, 0, "first-reference", offset=offset
        )
        self.assertEqual(new_payload[:offset], payload[:offset])


class RewrittenIndexTest(unittest.TestCase):
    def test_first_reference_indices_are_monotonic_on_first_use(self):
        order = mini.vertex_reference_order(INDICES, 0, "first-reference")
        new_indices = mini.rewrite_indices_for_permutation(INDICES, 0, order)
        seen: set[int] = set()
        expected_next = 0
        for value in new_indices:
            if value in seen:
                continue
            self.assertEqual(value, expected_next)
            seen.add(value)
            expected_next += 1

    def test_index_reuse_pattern_is_unchanged(self):
        """A permutation must not change which positions share a vertex."""
        for mode in MODES:
            with self.subTest(mode=mode):
                order = mini.vertex_reference_order(INDICES, 0, mode)
                new_indices = mini.rewrite_indices_for_permutation(INDICES, 0, order)
                for left in range(len(INDICES)):
                    for right in range(len(INDICES)):
                        self.assertEqual(
                            INDICES[left] == INDICES[right],
                            new_indices[left] == new_indices[right],
                        )

    def test_new_max_index_does_not_exceed_original(self):
        for mode in MODES:
            with self.subTest(mode=mode):
                order = mini.vertex_reference_order(INDICES, 0, mode)
                new_indices = mini.rewrite_indices_for_permutation(INDICES, 0, order)
                self.assertLessEqual(max(new_indices), max(INDICES))

    def test_rewritten_payload_round_trips_through_uint16(self):
        order = mini.vertex_reference_order(INDICES, 0, "first-reference")
        new_indices = mini.rewrite_indices_for_permutation(INDICES, 0, order)
        packed = struct.pack(f"<{len(new_indices)}H", *new_indices)
        self.assertEqual(mini.uint16_indices(packed), new_indices)


SCRIPT = REPO_ROOT / "scripts" / "tools" / "run_3dmark05_mini_replay.py"

# Minimal valid MSL for the CLI round-trip test. `prepare()` requires
# `shaders.vs_file`/`shaders.ps_file` to exist and carry the
# `constant ArgbufLayout& abuf [[buffer(30)]]` parameter that
# `transform_msl()` rewrites; mirrors the fixture in
# tests/scripts/test_run_3dmark05_mini_replay.py.
VS_MSL = """
#include <metal_stdlib>
using namespace metal;
struct VsConsts { float4 vsFloatConst[256]; int4 vsIntConst[16]; uint vsBoolConst[16]; };
struct FfpVsConsts { float2 halfPixelFixup; };
struct DrawVolatile { int vertexBaseIndex; uint vertexStreamOffset; uint vertexStreamStride; uint _pad; };
struct VSOut {
  float4 position [[position]];
  float pointSize [[point_size]];
};
struct ArgbufLayout {
  constant VsConsts* vsConsts [[id(0)]];
  constant FfpVsConsts* ffpVs [[id(1)]];
};
vertex VSOut dxmt9_vs(uint vid [[vertex_id]],
                     constant ArgbufLayout& abuf [[buffer(30)]],
                     device const uchar* stream0 [[buffer(1)]],
                     constant DrawVolatile& drawVolatile [[buffer(5)]]) {
  constant VsConsts& vsConsts = *abuf.vsConsts;
  constant FfpVsConsts& ffpVs = *abuf.ffpVs;
  VSOut out;
  out.position = vsConsts.vsFloatConst[0] + float4(float(vid), 0.0, 0.0, 1.0);
  out.position.xy += ffpVs.halfPixelFixup;
  out.pointSize = 1.0;
  return out;
}
"""

FS_MSL = """
#include <metal_stdlib>
using namespace metal;
struct PsConsts { float4 psFloatConst[224]; int4 psIntConst[16]; uint psBoolConst[16]; };
struct FfpPsConsts { uint fogMode; };
struct VSOut {
  float4 position [[position]];
  float pointSize [[point_size]];
};
struct ArgbufLayout {
  constant PsConsts* psConsts [[id(2)]];
  constant FfpPsConsts* ffpPs [[id(3)]];
};
fragment float4 dxmt9_fs(VSOut in [[stage_in]],
                     constant ArgbufLayout& abuf [[buffer(30)]]) {
  constant PsConsts& psConsts = *abuf.psConsts;
  constant FfpPsConsts& ffpPs = *abuf.ffpPs;
  return psConsts.psFloatConst[0] + float4(float(ffpPs.fogMode), 0.0, 0.0, 1.0);
}
"""


FS_MSL_DIRECT_CBUF = """
#include <metal_stdlib>
using namespace metal;
struct PsConsts { float4 psFloatConst[224]; int4 psIntConst[16]; uint psBoolConst[16]; };
struct FfpPsConsts { uint fogMode; };
struct VSOut {
  float4 position [[position]];
};
fragment float4 dxmt9_fs(VSOut in [[stage_in]],
                     constant PsConsts& psConsts [[buffer(0)]],
                     constant FfpPsConsts& ffpPs [[buffer(3)]]) {
  return psConsts.psFloatConst[0] + float4(float(ffpPs.fogMode), 0.0, 0.0, 1.0);
}
"""

# Mirrors a real dumped programmable fragment shader under
# DXMT9_ARGBUF_DIRECT_CBUF=1: only psConsts is bound directly, no ffpPs and
# no buffer(30) argbuf.
FS_MSL_DIRECT_CBUF_PSCONSTS_ONLY = """
#include <metal_stdlib>
using namespace metal;
struct PsConsts { float4 psFloatConst[224]; int4 psIntConst[16]; uint psBoolConst[16]; };
struct FsVolatile { uint pad; };
struct VSOut {
  float4 position [[position]];
};
fragment float4 dxmt9_fs(VSOut in [[stage_in]],
                     constant PsConsts& psConsts [[buffer(0)]],
                     constant FsVolatile& fsVolatile [[buffer(5)]]) {
  return psConsts.psFloatConst[0];
}
"""

VS_MSL_DIRECT_CBUF_VSCONSTS_ONLY = """
#include <metal_stdlib>
using namespace metal;
struct VsConsts { float4 vsFloatConst[256]; int4 vsIntConst[16]; uint vsBoolConst[16]; };
struct VSOut {
  float4 position [[position]];
};
vertex VSOut dxmt9_vs(uint vid [[vertex_id]],
                     constant VsConsts& vsConsts [[buffer(2)]],
                     device const uchar* stream0 [[buffer(1)]]) {
  VSOut out;
  out.position = vsConsts.vsFloatConst[0] + float4(float(vid), 0.0, 0.0, 1.0);
  return out;
}
"""

NO_ARGBUF_NO_DIRECT_BINDING_MSL = """
#include <metal_stdlib>
using namespace metal;
struct VSOut {
  float4 position [[position]];
};
vertex VSOut dxmt9_vs(uint vid [[vertex_id]]) {
  VSOut out;
  out.position = float4(0.0, 0.0, 0.0, 1.0);
  return out;
}
"""


def write_manifest(root: Path) -> Path:
    """One draw, one stream, uint16 indices, base_vertex 0."""
    payload = make_payload()
    stream_path = root / "draw000.stream0.bin"
    stream_path.write_bytes(payload)
    index_path = root / "draw000.index.bin"
    index_path.write_bytes(struct.pack(f"<{len(INDICES)}H", *INDICES))
    vs_path = root / "draw000.vs.metal"
    vs_path.write_text(VS_MSL, encoding="utf-8")
    ps_path = root / "draw000.ps.metal"
    ps_path.write_text(FS_MSL, encoding="utf-8")
    manifest = {
        "schema": "dxmt9.3dmark05.mini_replay_manifest.v1",
        "draws": [{
            "row": "60/2",
            "seq": 60,
            "encoder": 2,
            "encoder_draw_index": 0,
            "draw_ordinal": 1,
            "shaders": {
                "vs_file": str(vs_path),
                "ps_file": str(ps_path),
            },
            "geometry": {
                "index_file": str(index_path),
                "index_bytes": index_path.stat().st_size,
                "stream0_file": str(stream_path),
                "stream0_bytes": stream_path.stat().st_size,
                "streams": [{
                    "stream": 0,
                    "metal_slot": 1,
                    "file": str(stream_path),
                    "bytes": stream_path.stat().st_size,
                    "offset": 0,
                    "stride": STRIDE,
                }],
            },
            "state": {
                "index_count": len(INDICES),
                "base_vertex": 0,
                "stream0_stride": STRIDE,
                "stream0_offset": 0,
                "index_type": "uint16",
            },
            "uniforms": {},
        }],
    }
    manifest_path = root / "manifest.json"
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    return manifest_path


class MaterializeVertexOrderTest(unittest.TestCase):
    def test_original_leaves_payloads_untouched(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = json.loads(write_manifest(root).read_text(encoding="utf-8"))
            replay = mini.materialize_replay_draws(
                manifest["draws"], root / "out", "original", "original", "original"
            )
            geometry = replay[0]["geometry"]
            self.assertNotIn("vertex_order", geometry)
            self.assertEqual(
                Path(geometry["stream0_file"]).read_bytes(), make_payload()
            )

    def test_first_reference_rewrites_stream_and_index(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = json.loads(write_manifest(root).read_text(encoding="utf-8"))
            original_stream = make_payload()
            replay = mini.materialize_replay_draws(
                manifest["draws"], root / "out", "original", "original", "first-reference"
            )
            geometry = replay[0]["geometry"]
            state = replay[0]["state"]
            self.assertEqual(geometry["vertex_order"], "first-reference")
            self.assertEqual(state["base_vertex"], 0)

            new_stream = Path(geometry["stream0_file"]).read_bytes()
            new_indices = mini.uint16_indices(
                Path(geometry["index_file"]).read_bytes()
            )
            self.assertEqual(len(new_stream), len(original_stream))
            self.assertEqual(geometry["stream0_bytes"], len(new_stream))
            self.assertEqual(geometry["index_bytes"], 2 * len(INDICES))
            self.assertEqual(geometry["streams"][0]["file"], geometry["stream0_file"])
            self.assertEqual(geometry["streams"][0]["bytes"], len(new_stream))

            for old_index, new_index in zip(INDICES, new_indices):
                self.assertEqual(
                    fetch(new_stream, 0, STRIDE, new_index),
                    fetch(original_stream, 0, STRIDE, old_index),
                )

    def test_composes_after_primitive_order(self):
        """Lane D: sorted primitive order plus a scattered layout."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = json.loads(write_manifest(root).read_text(encoding="utf-8"))
            original_stream = make_payload()
            replay = mini.materialize_replay_draws(
                manifest["draws"], root / "out", "sort-min-index", "original", "scatter"
            )
            geometry = replay[0]["geometry"]
            self.assertEqual(geometry["index_order"], "sort-min-index")
            self.assertEqual(geometry["vertex_order"], "scatter")

            sorted_indices = mini.uint16_indices(
                mini.transform_index_payload(
                    struct.pack(f"<{len(INDICES)}H", *INDICES), "sort-min-index"
                )
            )
            new_stream = Path(geometry["stream0_file"]).read_bytes()
            new_indices = mini.uint16_indices(
                Path(geometry["index_file"]).read_bytes()
            )
            for old_index, new_index in zip(sorted_indices, new_indices):
                self.assertEqual(
                    fetch(new_stream, 0, STRIDE, new_index),
                    fetch(original_stream, 0, STRIDE, old_index),
                )

    def test_index_reuse_estimate_is_unchanged_by_remap(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = json.loads(write_manifest(root).read_text(encoding="utf-8"))
            replay = mini.materialize_replay_draws(
                manifest["draws"], root / "out", "original", "original", "first-reference"
            )
            estimate = mini.index_cache_estimate(replay)
            self.assertEqual(
                estimate["original_lru32_misses"], estimate["replay_lru32_misses"]
            )


class TransformMslDirectCbufTest(unittest.TestCase):
    """Defect 1: DXMT9_ARGBUF_DIRECT_CBUF shaders have no buffer(30) argbuf."""

    def test_argbuf_source_is_still_rewritten(self):
        """Regression guard: the pre-existing buffer(30) rewrite path is untouched."""
        source, slots = mini.transform_msl(VS_MSL, "vs")
        self.assertNotIn("buffer(30)", source)
        self.assertEqual(set(slots), {"vsconsts", "ffpvs"})
        self.assertEqual(len(set(slots.values())), 2)

    def test_direct_cbuf_fs_source_is_returned_unchanged(self):
        source, slots = mini.transform_msl(FS_MSL_DIRECT_CBUF, "fs")
        self.assertEqual(source, FS_MSL_DIRECT_CBUF)
        self.assertEqual(slots, {"psconsts": 0, "ffpps": 3})

    def test_direct_cbuf_fs_source_with_only_psconsts_is_unchanged(self):
        source, slots = mini.transform_msl(FS_MSL_DIRECT_CBUF_PSCONSTS_ONLY, "fs")
        self.assertEqual(source, FS_MSL_DIRECT_CBUF_PSCONSTS_ONLY)
        self.assertEqual(slots["psconsts"], 0)
        self.assertNotIn(slots["ffpps"], {0, 5, 30})

    def test_direct_cbuf_vs_source_with_only_vsconsts_allocates_ffpvs_slot(self):
        source, slots = mini.transform_msl(VS_MSL_DIRECT_CBUF_VSCONSTS_ONLY, "vs")
        self.assertEqual(source, VS_MSL_DIRECT_CBUF_VSCONSTS_ONLY)
        self.assertEqual(slots["vsconsts"], 2)
        self.assertNotIn(slots["ffpvs"], {1, 2, 30})

    def test_neither_argbuf_nor_direct_binding_raises(self):
        with self.assertRaises(SystemExit):
            mini.transform_msl(NO_ARGBUF_NO_DIRECT_BINDING_MSL, "vs")


# Two triangles referencing exactly slots 0, 1, 2 with no gaps, so the
# unique-referenced count equals the tight payload capacity a "sliced" dump
# (start_byte == offset) would hold -- unlike the module-level INDICES, which
# has gaps (references 0, 2, 4 but not 1 or 3).
DENSE_INDICES = [0, 1, 2, 2, 1, 0]


def write_stream_offset_draws(root: Path) -> list[dict]:
    """Draws exercising the stream0_offset / stream0_start_byte relationship.

    - draws[0] ("sliced"): offset == start_byte == 840; the dumped payload is
      a slice holding exactly the referenced vertices, mirroring the real
      capture's defect-2 shape (byte 0 of the file is fetch slot 0).
    - draws[1] ("unsliced"): start_byte == 0, offset == 96; an un-sliced
      payload where the offset is a genuine in-payload byte offset.
    - draws[2] ("inconsistent"): start_byte > offset, which the manifest
      should never produce.
    """
    draws: list[dict] = []

    def make_draw(name: str, indices: list[int], payload: bytes,
                  offset: int, start_byte: int) -> dict:
        stream_path = root / f"{name}.stream0.bin"
        stream_path.write_bytes(payload)
        index_path = root / f"{name}.index.bin"
        index_path.write_bytes(struct.pack(f"<{len(indices)}H", *indices))
        return {
            "geometry": {
                "index_file": str(index_path),
                "index_bytes": index_path.stat().st_size,
                "stream0_file": str(stream_path),
                "stream0_bytes": stream_path.stat().st_size,
                "streams": [{
                    "stream": 0,
                    "metal_slot": 1,
                    "file": str(stream_path),
                    "bytes": stream_path.stat().st_size,
                    "offset": offset,
                    "stride": STRIDE,
                    "start_byte": start_byte,
                }],
            },
            "state": {
                "index_count": len(indices),
                "base_vertex": 0,
                "stream0_stride": STRIDE,
                "stream0_offset": offset,
            },
        }

    unique_vertices = len(set(DENSE_INDICES))
    draws.append(make_draw(
        "sliced", DENSE_INDICES,
        make_payload(slot_count=unique_vertices, offset=0), 840, 840,
    ))
    draws.append(make_draw(
        "unsliced", INDICES, make_payload(slot_count=SLOT_COUNT, offset=96), 96, 0
    ))
    draws.append(make_draw(
        "inconsistent", INDICES, make_payload(slot_count=SLOT_COUNT, offset=0), 10, 20
    ))
    return draws


class StreamPayloadOffsetTest(unittest.TestCase):
    """Defect 2: sliced stream payloads double-count the stream offset."""

    def test_sliced_payload_materializes_successfully(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            sliced = [write_stream_offset_draws(root)[0]]
            replay = mini.materialize_replay_draws(
                sliced, root / "out", "original", "original", "first-reference"
            )
            self.assertEqual(len(replay), 1)

    def test_sliced_payload_fetch_equivalence_uses_zero_offset(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            sliced = write_stream_offset_draws(root)[0]
            original_stream = Path(sliced["geometry"]["stream0_file"]).read_bytes()
            replay = mini.materialize_replay_draws(
                [sliced], root / "out", "original", "original", "first-reference"
            )
            geometry = replay[0]["geometry"]
            new_stream = Path(geometry["stream0_file"]).read_bytes()
            new_indices = mini.uint16_indices(
                Path(geometry["index_file"]).read_bytes()
            )
            for old_index, new_index in zip(DENSE_INDICES, new_indices):
                self.assertEqual(
                    fetch(new_stream, 0, STRIDE, new_index),
                    fetch(original_stream, 0, STRIDE, old_index),
                )

    def test_unsliced_payload_still_uses_nonzero_offset(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            unsliced = write_stream_offset_draws(root)[1]
            original_stream = Path(unsliced["geometry"]["stream0_file"]).read_bytes()
            replay = mini.materialize_replay_draws(
                [unsliced], root / "out", "original", "original", "first-reference"
            )
            geometry = replay[0]["geometry"]
            new_stream = Path(geometry["stream0_file"]).read_bytes()
            new_indices = mini.uint16_indices(
                Path(geometry["index_file"]).read_bytes()
            )
            self.assertEqual(new_stream[:96], original_stream[:96])
            for old_index, new_index in zip(INDICES, new_indices):
                self.assertEqual(
                    fetch(new_stream, 96, STRIDE, new_index),
                    fetch(original_stream, 96, STRIDE, old_index),
                )

    def test_inconsistent_start_byte_raises(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            inconsistent = [write_stream_offset_draws(root)[2]]
            with self.assertRaises(SystemExit):
                mini.materialize_replay_draws(
                    inconsistent, root / "out", "original", "original", "first-reference"
                )


class VertexOrderCliTest(unittest.TestCase):
    def test_cli_records_vertex_order_in_summary(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest_path = write_manifest(root)
            output_dir = root / "out"
            result = subprocess.run(
                [
                    sys.executable, str(SCRIPT), str(manifest_path),
                    "--output-dir", str(output_dir),
                    "--vertex-order", "first-reference",
                ],
                capture_output=True, text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            summary = json.loads(
                (output_dir / "mini-replay-summary.json").read_text(
                    encoding="utf-8"
                )
            )
            self.assertEqual(summary["vertex_order"], "first-reference")

    def test_cli_rejects_unknown_vertex_order(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest_path = write_manifest(root)
            result = subprocess.run(
                [
                    sys.executable, str(SCRIPT), str(manifest_path),
                    "--output-dir", str(root / "out"),
                    "--vertex-order", "sideways",
                ],
                capture_output=True, text=True,
            )
            self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
