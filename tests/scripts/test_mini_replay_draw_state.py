"""Regression test for the mini-replay per-draw render-state collapse.

The generator used to bake alpha_blend/color_write/depth_enabled/depth_write/
depth_func/cull from draws[0]["state"] into a single MTLRenderPipelineDescriptor
and MTLDepthStencilDescriptor shared by every draw. A real capture
(traces/app-d3d9-3dmark05-vertexremap-enc1-r1/analysis/frame60-mini-replay-
manifest-enc1.json) has 229 draws where 42 depth-prepass draws carry
color_write=0x0 and the other 187 carry color_write=0xf; because draw 0 is a
prepass draw, every one of those 187 color-writing draws replayed with every
color channel masked off.

This test drives the real CLI (`run_3dmark05_mini_replay.py`) against a small
two-draw manifest built to reproduce that shape -- one prepass-like draw
(color_write=0x0) and one color-writing draw (color_write=0xf), plus a
depth_func disagreement -- and inspects the emitted Objective-C++ source.
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
INDICES = [0, 1, 2, 2, 1, 0]

# Minimal valid MSL mirroring tests/scripts/test_mini_replay_vertex_order.py's
# fixture: `prepare()` requires shaders.vs_file/ps_file to exist and carry the
# `constant ArgbufLayout& abuf [[buffer(30)]]` parameter transform_msl()
# rewrites.
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


def make_payload(slot_count: int = 3) -> bytes:
    return b"".join(bytes([0x10 + slot]) * STRIDE for slot in range(slot_count))


def write_two_draw_manifest(root: Path) -> Path:
    """A depth-prepass draw (color_write=0x0) followed by a color-writing draw
    (color_write=0xf), sharing one shader variant, disagreeing on depth_func --
    the minimal shape of the real capture's collapse defect."""
    shader_dir = root / "shaders"
    shader_dir.mkdir()
    vs_path = shader_dir / "shared.vs.metal"
    vs_path.write_text(VS_MSL, encoding="utf-8")
    ps_path = shader_dir / "shared.ps.metal"
    ps_path.write_text(FS_MSL, encoding="utf-8")

    draws = []
    draw_specs = [
        # (name, color_write, depth_func, cull)
        ("prepass", "0x0", 2, 1),   # Less, cull none
        ("color", "0xf", 4, 3),     # LessEqual, cull back
    ]
    for name, color_write, depth_func, cull in draw_specs:
        payload = make_payload()
        stream_path = root / f"{name}.stream0.bin"
        stream_path.write_bytes(payload)
        index_path = root / f"{name}.index.bin"
        index_path.write_bytes(struct.pack(f"<{len(INDICES)}H", *INDICES))
        draws.append({
            "row": "60/2",
            "seq": 60,
            "encoder": 2,
            "encoder_draw_index": len(draws),
            "draw_ordinal": len(draws) + 1,
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
                "color_write": color_write,
                "depth_enabled": 1,
                "depth_write": 1 if color_write == "0x0" else 0,
                "depth_func": depth_func,
                "cull": cull,
            },
            "uniforms": {},
        })

    manifest = {
        "schema": "dxmt9.3dmark05.mini_replay_manifest.v1",
        "draws": draws,
    }
    manifest_path = root / "manifest.json"
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    return manifest_path


class DrawStateHelperTest(unittest.TestCase):
    """Unit-level checks on the extraction helpers themselves."""

    def test_blend_state_differs_on_color_write(self):
        prepass = mini.draw_blend_state({"color_write": "0x0"})
        color = mini.draw_blend_state({"color_write": "0xf"})
        self.assertNotEqual(prepass, color)
        self.assertEqual(prepass[0], 0)
        self.assertEqual(color[0], 15)

    def test_depth_state_differs_on_depth_func(self):
        a = mini.draw_depth_state({"depth_enabled": 1, "depth_write": 1, "depth_func": 2})
        b = mini.draw_depth_state({"depth_enabled": 1, "depth_write": 0, "depth_func": 4})
        self.assertNotEqual(a, b)
        self.assertEqual(a[2], 2)
        self.assertEqual(b[2], 4)

    def test_cull_mode_reads_per_draw_value(self):
        self.assertEqual(mini.draw_cull_mode({"cull": 3}), 3)
        self.assertEqual(mini.draw_cull_mode({}), 1)


class GeneratedSourcePerDrawStateTest(unittest.TestCase):
    """Drives the real generator and inspects the emitted .mm source, the way
    the mini-replay CLI would actually be used."""

    def _generate(self, tmp: str) -> tuple[Path, dict]:
        root = Path(tmp)
        manifest_path = write_two_draw_manifest(root)
        output_dir = root / "out"
        result = subprocess.run(
            [
                sys.executable, str(SCRIPT), str(manifest_path),
                "--output-dir", str(output_dir),
            ],
            capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        source = (output_dir / "dxmt9_3dmark05_mini_replay.mm").read_text(encoding="utf-8")
        summary = json.loads((output_dir / "mini-replay-summary.json").read_text(encoding="utf-8"))
        return source, summary

    def test_distinct_color_write_masks_are_emitted(self):
        """The core regression: draw 0 (prepass, color_write=0x0) must not
        collapse draw 1's color_write=0xf into a single shared PSO."""
        with tempfile.TemporaryDirectory() as tmp:
            source, _ = self._generate(tmp)
            masks = set(re.findall(r"colorWriteMask\((\d+)\)", source))
            self.assertGreater(
                len(masks), 1,
                f"expected more than one distinct colorWriteMask(...) argument, got {masks}",
            )
            self.assertIn(
                "15", masks,
                f"colorWriteMask(15) (0xf, full write) must appear among {masks}",
            )
            self.assertIn(
                "0", masks,
                f"colorWriteMask(0) (the prepass mask) must still appear among {masks}",
            )

    def test_color_writing_draw_does_not_share_masked_off_pipeline(self):
        """Index into the pipeline table via each DrawEntry's pipelineIndex
        and confirm the color-writing draw's PSO literal is colorWriteMask(15),
        not the prepass draw's colorWriteMask(0). This is exactly the
        assertion that would have caught the collapse: pre-fix, both draws
        shared psos[draw.shaderIndex] built once from draws[0]'s state, so
        both pipelineIndex values below would have resolved to the same
        colorWriteMask(0)-masked PSO."""
        with tempfile.TemporaryDirectory() as tmp:
            source, _ = self._generate(tmp)

            # Find each DrawEntry's pipelineIndex: the second field after the
            # three array-valued members ({...}, {...}, {...}) is shaderIndex,
            # and the field right after it is pipelineIndex. Rather than
            # parse the initializer positionally (brittle across unrelated
            # field-order changes), recover it via the PSO-build-statement
            # ordering: each `psos[i] = ...` block is emitted in the same
            # order as pso_combo_index() first assigned that combo, and the
            # literal colorWriteMask argument immediately precedes it in that
            # same block.
            pso_blocks = re.findall(
                r"colorWriteMask\((\d+)\).*?psos\[(\d+)\] = ",
                source, re.DOTALL,
            )
            self.assertGreaterEqual(
                len(pso_blocks), 2,
                f"expected at least 2 PSO build blocks, found {pso_blocks}",
            )
            mask_by_pso_index = {int(idx): int(mask) for mask, idx in pso_blocks}

            # Recover each DrawEntry's pipelineIndex from the draw array.
            # DrawEntry fields up to shaderIndex are, in order: indexPath,
            # streamPath, vsConstsPath, psConstsPath, ffpVsPath, ffpPsPath,
            # extraStreamPaths[16], extraStreamSlots[16], fragmentTextures[16],
            # shaderIndex, pipelineIndex, depthStateIndex, cullMode, ...
            # so pipelineIndex is the field immediately after shaderIndex,
            # which is itself immediately after the third `{...}` array.
            draw_entry_pattern = re.compile(
                r"\{[^{}]*\},\s*\{[^{}]*\},\s*\{[^{}]*\},\s*"  # extraStreamPaths/Slots/fragmentTextures
                r"(\d+),\s*(\d+),\s*(\d+),\s*(\d+),"           # shaderIndex, pipelineIndex, depthStateIndex, cullMode
            )
            draw_rows = draw_entry_pattern.findall(source)
            self.assertEqual(
                len(draw_rows), 2,
                f"expected to recover 2 DrawEntry rows, got {draw_rows}\nsource:\n{source}",
            )
            pipeline_indices = [int(row[1]) for row in draw_rows]
            self.assertNotEqual(
                pipeline_indices[0], pipeline_indices[1],
                "prepass and color-writing draws must not share a pipelineIndex",
            )
            self.assertEqual(mask_by_pso_index[pipeline_indices[0]], 0)
            self.assertEqual(mask_by_pso_index[pipeline_indices[1]], 15)

    def test_distinct_depth_compare_functions_are_emitted(self):
        with tempfile.TemporaryDirectory() as tmp:
            source, _ = self._generate(tmp)
            funcs = set(re.findall(r"compareFunction\((\d+)\)", source))
            self.assertGreater(
                len(funcs), 1,
                f"expected more than one distinct compareFunction(...) argument, got {funcs}",
            )
            self.assertIn("2", funcs)
            self.assertIn("4", funcs)


if __name__ == "__main__":
    unittest.main()
