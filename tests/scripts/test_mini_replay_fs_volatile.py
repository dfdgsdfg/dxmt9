"""Regression tests for the mini-replay generator omitting the fragment-stage
FsVolatile binding at buffer(5).

Confirmed root cause (do not re-investigate): the generated Objective-C++
program bound nothing to fragment `buffer(5)`, but every real dxmt9 fragment
shader declares `constant FsVolatile& fsVolatile [[buffer(5)]]` and drives an
alpha-test switch from it that ends in `discard_fragment()`
(`dxmt9_fs.replay.metal:308-320`). Reading an unbound Metal buffer is
undefined; the garbage selected a failing alpha-test case and discarded every
fragment, so the replay rendered fully black and exited 0.

The vertex stage already binds its own volatile (`DrawVolatile dv` at
`atIndex:5` on the *vertex* stage -- an independent Metal binding-index
namespace from the fragment stage). The fix mirrors that pattern for the
fragment stage: emit a per-draw `FsVolatile` matching
`src/dxmt9/dxmt9_draw_state.hpp:131-139`
(`static_assert(sizeof(FsVolatile) == 16)`) and bind it with
`setFragmentBytes:...atIndex:5`.

`alphaTest`/`alphaRef` semantics mirror `makeFsVolatile`/`buildFsVolatile`
(`src/dxmt9/dxmt9_draw_state.cpp:514-543`): `alphaTest` folds enable+func into
one field (`0` disabled, else the raw `D3DCMPFUNC`), and `alphaRef` is the
raw `RS_ALPHA_REF` byte scaled by `1.0f/255.0f`. `sampleMask` is always
`0xffffffffu` here because the mini-replay render target is always
single-sample -- a zero sample mask would mask out every fragment, which is
exactly the class of defect this fix closes for buffer(5) itself, so it must
never appear as zero in the emitted source.
"""

import json
import re
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts" / "tools"))

import run_3dmark05_mini_replay as mini  # noqa: E402

SCRIPT = REPO_ROOT / "scripts" / "tools" / "run_3dmark05_mini_replay.py"

STRIDE = 8
INDICES = [0, 1, 2]

# Minimal valid MSL mirroring tests/scripts/test_mini_replay_attachment_format.py's
# fixture: `prepare()` requires shaders.vs_file/ps_file to exist and carry the
# `constant ArgbufLayout& abuf [[buffer(30)]]` parameter transform_msl()
# rewrites. Neither shape needs to declare an `FsVolatile` parameter itself --
# the generator always binds fragment buffer(5) unconditionally; it does not
# inspect the fragment source to decide whether to bind it.
VS_MSL = """
#include <metal_stdlib>
using namespace metal;
struct VsConsts { float4 vsFloatConst[256]; int4 vsIntConst[16]; uint vsBoolConst[16]; };
struct FfpVsConsts { float2 halfPixelFixup; };
struct DrawVolatile { int vertexBaseIndex; uint vertexStreamOffset; uint vertexStreamStride; uint _pad; };
struct VSOut {
  float4 position [[position]];
};
vertex VSOut dxmt9_vs(uint vid [[vertex_id]],
                     constant VsConsts& vsConsts [[buffer(0)]],
                     constant FfpVsConsts& ffpVs [[buffer(3)]],
                     device const uchar* stream0 [[buffer(1)]],
                     constant DrawVolatile& drawVolatile [[buffer(5)]]) {
  VSOut out;
  out.position = vsConsts.vsFloatConst[0] + float4(float(vid), 0.0, 0.0, 1.0);
  out.position.xy += ffpVs.halfPixelFixup;
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
};
fragment float4 dxmt9_fs(VSOut in [[stage_in]],
                     constant PsConsts& psConsts [[buffer(0)]],
                     constant FfpPsConsts& ffpPs [[buffer(3)]]) {
  return psConsts.psFloatConst[0] + float4(float(ffpPs.fogMode), 0.0, 0.0, 1.0);
}
"""


def write_manifest(root: Path, alpha_test: str = "0", alpha_ref=None) -> Path:
    shader_dir = root / "shaders"
    shader_dir.mkdir()
    vs_path = shader_dir / "shared.vs.metal"
    vs_path.write_text(VS_MSL, encoding="utf-8")
    ps_path = shader_dir / "shared.ps.metal"
    ps_path.write_text(FS_MSL, encoding="utf-8")

    stream_path = root / "draw0.stream0.bin"
    stream_path.write_bytes(bytes(range(STRIDE * 3)))
    index_path = root / "draw0.index.bin"
    index_path.write_bytes(struct.pack(f"<{len(INDICES)}H", *INDICES))

    state = {
        "index_count": len(INDICES),
        "base_vertex": 0,
        "stream0_stride": STRIDE,
        "stream0_offset": 0,
        "index_type": "uint16",
        "color_write": "0xf",
        "depth_enabled": 1,
        "depth_write": 1,
        "depth_func": 4,
        "cull": 1,
        "alpha_test": alpha_test,
    }
    if alpha_ref is not None:
        state["alpha_ref"] = alpha_ref

    draw = {
        "row": "60/0",
        "seq": 60,
        "encoder": 0,
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
        "state": state,
        "uniforms": {},
    }

    manifest = {
        "schema": "dxmt9.3dmark05.mini_replay_manifest.v1",
        "draws": [draw],
    }
    manifest_path = root / "manifest.json"
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    return manifest_path


def run_cli(manifest_path: Path, output_dir: Path) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(SCRIPT), str(manifest_path), "--output-dir", str(output_dir)],
        capture_output=True, text=True,
    )


# The DrawEntry field order after the three array members
# (extraStreamPaths[16], extraStreamSlots[16], fragmentTextures[16]) is:
# shaderIndex, pipelineIndex, depthStateIndex, cullMode, indexCount,
# baseVertex, streamStride, streamOffset, scissorEnabled, scissorL, scissorT,
# scissorR, scissorB, alphaTest, alphaRef -- see
# tests/scripts/test_mini_replay_draw_state.py's identical positional-recovery
# technique for the fields before cullMode.
DRAW_ENTRY_TAIL_PATTERN = re.compile(
    r"\{[^{}]*\},\s*\{[^{}]*\},\s*\{[^{}]*\},\s*"      # extraStreamPaths/Slots/fragmentTextures
    r"\d+,\s*\d+,\s*\d+,\s*\d+,\s*"                     # shaderIndex, pipelineIndex, depthStateIndex, cullMode
    r"\d+,\s*-?\d+,\s*\d+,\s*\d+,\s*"                   # indexCount, baseVertex, streamStride, streamOffset
    r"\d+,\s*\d+,\s*\d+,\s*\d+,\s*\d+,\s*"               # scissorEnabled, scissorL, scissorT, scissorR, scissorB
    r"(\d+),\s*([0-9.eE+-]+)f"                          # alphaTest, alphaRef
)


class DrawFsVolatileHelperTest(unittest.TestCase):
    """Unit-level checks directly against the resolver, the way
    tests/scripts/test_mini_replay_draw_state.py checks draw_cull_mode() etc.
    directly."""

    def test_alpha_test_zero_yields_zero_ref_regardless_of_alpha_ref(self):
        self.assertEqual(mini.draw_fs_volatile({"alpha_test": "0"}, 0), (0, 0.0))
        # Even a present alpha_ref must not matter when alpha_test is off.
        self.assertEqual(
            mini.draw_fs_volatile({"alpha_test": "0", "alpha_ref": 200}, 0), (0, 0.0)
        )

    def test_alpha_test_nonzero_without_alpha_ref_fails_loudly(self):
        """This is the failure contract in the brief: alpha_test != 0 with no
        alpha_ref in the draw's state must raise/exit rather than silently
        substituting 0.0 and rendering a wrong-but-successful image."""
        with self.assertRaises(SystemExit) as ctx:
            mini.draw_fs_volatile({"alpha_test": "3"}, 7)
        message = str(ctx.exception)
        self.assertIn("7", message, "message must name the draw ordinal")
        self.assertIn("3", message, "message must name the alpha_test value")
        self.assertIn("alpha_ref", message, "message must name the missing field")

    def test_alpha_test_nonzero_with_alpha_ref_scales_by_255(self):
        alpha_test, alpha_ref = mini.draw_fs_volatile(
            {"alpha_test": "3", "alpha_ref": 128}, 0
        )
        self.assertEqual(alpha_test, 3)
        self.assertAlmostEqual(alpha_ref, 128 / 255.0)

    def test_alpha_test_parses_hex_string_like_color_write(self):
        # Matches the manifest's actual shape: state["alpha_test"] is a
        # string, parsed the way color_write already is (int(str(...), 0)).
        self.assertEqual(mini.draw_fs_volatile({"alpha_test": "0x0"}, 0), (0, 0.0))


class GeneratedSourceFsVolatileTest(unittest.TestCase):
    """Drives the real CLI end-to-end, the way
    GeneratedSourcePerDrawStateTest in test_mini_replay_draw_state.py and
    ManifestWithAttachmentsCliTest in test_mini_replay_attachment_format.py
    do."""

    def _generate(self, tmp: str, alpha_test: str = "0", alpha_ref=None):
        root = Path(tmp)
        manifest_path = write_manifest(root, alpha_test=alpha_test, alpha_ref=alpha_ref)
        output_dir = root / "out"
        result = run_cli(manifest_path, output_dir)
        return result, output_dir

    def test_fsvolatile_struct_declared_with_correct_field_order_and_types(self):
        with tempfile.TemporaryDirectory() as tmp:
            result, output_dir = self._generate(tmp)
            self.assertEqual(result.returncode, 0, result.stderr)
            source = (output_dir / "dxmt9_3dmark05_mini_replay.mm").read_text(encoding="utf-8")
            match = re.search(r"struct FsVolatile \{(.*?)\};", source, re.DOTALL)
            self.assertIsNotNone(match, "FsVolatile struct not declared in generated source")
            fields = [
                line.strip().rstrip(";")
                for line in match.group(1).strip().splitlines()
                if line.strip()
            ]
            # Field order/types must match src/dxmt9/dxmt9_draw_state.hpp:131-139
            # (static_assert(sizeof(FsVolatile) == 16)): u32 alphaTest, f32
            # alphaRef, u32 sampleMask, u32 _pad.
            self.assertEqual(len(fields), 4, fields)
            self.assertRegex(fields[0], r"^(unsigned|unsigned int|uint32_t)\s+alphaTest$")
            self.assertRegex(fields[1], r"^float\s+alphaRef$")
            self.assertRegex(fields[2], r"^(unsigned|unsigned int|uint32_t)\s+sampleMask$")
            self.assertRegex(fields[3], r"^(unsigned|unsigned int|uint32_t)\s+(pad|_pad)$")

    def test_fragment_buffer5_bind_emitted(self):
        with tempfile.TemporaryDirectory() as tmp:
            result, output_dir = self._generate(tmp)
            self.assertEqual(result.returncode, 0, result.stderr)
            source = (output_dir / "dxmt9_3dmark05_mini_replay.mm").read_text(encoding="utf-8")
            self.assertRegex(
                source,
                r"\[encoder setFragmentBytes:&\w+ length:sizeof\(\w+\) atIndex:5\];",
                "expected a setFragmentBytes bind at atIndex:5 mirroring the "
                "vertex setVertexBytes:&dv ... atIndex:5 bind",
            )

    def test_alpha_test_zero_emits_zero_alpha_test_and_all_ones_sample_mask(self):
        with tempfile.TemporaryDirectory() as tmp:
            result, output_dir = self._generate(tmp, alpha_test="0")
            self.assertEqual(result.returncode, 0, result.stderr)
            source = (output_dir / "dxmt9_3dmark05_mini_replay.mm").read_text(encoding="utf-8")
            # sampleMask must be all-ones -- never zero, which would mask out
            # every fragment.
            self.assertIn("0xFFFFFFFFu", source)
            self.assertNotRegex(source, r"0x0+u,\s*0\)", "sampleMask must not be zero")
            rows = DRAW_ENTRY_TAIL_PATTERN.findall(source)
            self.assertEqual(len(rows), 1, f"expected exactly one DrawEntry row, got {rows}")
            alpha_test_field, alpha_ref_field = rows[0]
            self.assertEqual(int(alpha_test_field), 0)
            self.assertAlmostEqual(float(alpha_ref_field), 0.0)

    def test_alpha_test_nonzero_without_alpha_ref_fails_the_whole_run(self):
        with tempfile.TemporaryDirectory() as tmp:
            result, output_dir = self._generate(tmp, alpha_test="3", alpha_ref=None)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("alpha_ref", result.stderr)
            self.assertIn("3", result.stderr)
            self.assertFalse((output_dir / "dxmt9_3dmark05_mini_replay.mm").exists())

    def test_alpha_test_nonzero_with_alpha_ref_scales_in_generated_source(self):
        with tempfile.TemporaryDirectory() as tmp:
            result, output_dir = self._generate(tmp, alpha_test="3", alpha_ref=128)
            self.assertEqual(result.returncode, 0, result.stderr)
            source = (output_dir / "dxmt9_3dmark05_mini_replay.mm").read_text(encoding="utf-8")
            rows = DRAW_ENTRY_TAIL_PATTERN.findall(source)
            self.assertEqual(len(rows), 1, f"expected exactly one DrawEntry row, got {rows}")
            alpha_test_field, alpha_ref_field = rows[0]
            self.assertEqual(int(alpha_test_field), 3)
            self.assertAlmostEqual(float(alpha_ref_field), 128 / 255.0, places=5)


if __name__ == "__main__":
    unittest.main()
