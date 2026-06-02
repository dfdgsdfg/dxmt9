import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tools" / "run_3dmark05_mini_replay.py"


VS_MSL = """
#include <metal_stdlib>
using namespace metal;
struct VsConsts { float4 vsFloatConst[256]; int4 vsIntConst[16]; uint vsBoolConst[16]; };
struct FfpVsConsts { float2 halfPixelFixup; };
struct DrawVolatile { int vertexBaseIndex; uint vertexStreamOffset; uint vertexStreamStride; uint _pad; };
struct VSOut { float4 position [[position]]; float4 texcoord0; float fogFactor; float pointSize [[point_size]]; };
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
  out.texcoord0 = float4(stream0[drawVolatile.vertexStreamOffset], 0.0, 0.0, 1.0);
  out.fogFactor = 1.0;
  out.pointSize = 1.0;
  return out;
}
"""


FS_MSL = """
#include <metal_stdlib>
using namespace metal;
struct PsConsts { float4 psFloatConst[224]; int4 psIntConst[16]; uint psBoolConst[16]; };
struct FfpPsConsts { uint fogMode; };
struct VSOut { float4 position [[position]]; float4 texcoord0; float fogFactor; float pointSize [[point_size]]; };
struct ArgbufLayout {
  constant PsConsts* psConsts [[id(2)]];
  constant FfpPsConsts* ffpPs [[id(3)]];
};
fragment float4 dxmt9_fs(VSOut in [[stage_in]],
                     constant ArgbufLayout& abuf [[buffer(30)]],
                     texture2d<float> tex0 [[texture(0)]],
                     sampler samp0 [[sampler(0)]]) {
  constant PsConsts& psConsts = *abuf.psConsts;
  constant FfpPsConsts& ffpPs = *abuf.ffpPs;
  return psConsts.psFloatConst[0] + float4(float(ffpPs.fogMode), 0.0, 0.0, 1.0);
}
"""


class MiniReplayScriptTests(unittest.TestCase):
    def write_manifest_fixture(self, root: Path) -> tuple[Path, Path]:
        shader_dir = root / "shaders"
        geometry_dir = root / "geometry"
        output_dir = root / "out"
        shader_dir.mkdir()
        geometry_dir.mkdir()
        vs = shader_dir / "translated-vs-shader-2748-source-1.metal"
        fs = shader_dir / "translated-fs-shader-3567-source-2.metal"
        vs.write_text(VS_MSL, encoding="utf-8")
        fs.write_text(FS_MSL, encoding="utf-8")

        index_file = geometry_dir / "draw.index.bin"
        stream_file = geometry_dir / "draw.stream0.bin"
        vsconsts_file = geometry_dir / "draw.vsconsts.bin"
        psconsts_file = geometry_dir / "draw.psconsts.bin"
        ffpvs_file = geometry_dir / "draw.ffpvs.bin"
        ffpps_file = geometry_dir / "draw.ffpps.bin"
        index_file.write_bytes(b"\x00\x00\x01\x00\x02\x00")
        stream_file.write_bytes(bytes(range(24)))
        vsconsts_file.write_bytes(b"vs")
        psconsts_file.write_bytes(b"ps")
        ffpvs_file.write_bytes(b"ffpvs")
        ffpps_file.write_bytes(b"ffpps")
        manifest = root / "manifest.json"
        manifest.write_text(json.dumps({
            "schema": "dxmt9.3dmark05.mini_replay_manifest.v1",
            "draws": [{
                "state": {
                    "index_count": 3,
                    "base_vertex": 0,
                    "stream0_stride": 24,
                    "stream0_offset": 0,
                    "alpha_blend": 1,
                    "src_blend": 5,
                    "dst_blend": 6,
                    "blend_op": 1,
                    "separate_alpha": 1,
                    "src_blend_alpha": 2,
                    "dst_blend_alpha": 1,
                    "blend_op_alpha": 3,
                    "color_write": "0xf",
                    "depth_enabled": 1,
                    "depth_write": 0,
                    "depth_func": 4,
                    "cull": 2,
                    "scissor": 1,
                    "scissor_l": 10,
                    "scissor_t": 20,
                    "scissor_r": 110,
                    "scissor_b": 220,
                },
                "shaders": {
                    "vs_file": str(vs),
                    "ps_file": str(fs),
                },
                "geometry": {
                    "index_file": str(index_file),
                    "stream0_file": str(stream_file),
                    "index_bytes": 6,
                    "stream0_bytes": 24,
                },
                "uniforms": {
                    "vsconsts_file": str(vsconsts_file),
                    "psconsts_file": str(psconsts_file),
                    "ffpvs_file": str(ffpvs_file),
                    "ffpps_file": str(ffpps_file),
                    "vsconsts_bytes": 2,
                    "psconsts_bytes": 2,
                    "ffpvs_bytes": 5,
                    "ffpps_bytes": 5,
                },
            }],
        }), encoding="utf-8")
        return manifest, output_dir

    def test_prepare_rewrites_argbuf_slots_and_summarizes_payloads(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest, output_dir = self.write_manifest_fixture(root)

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(manifest),
                    "--output-dir",
                    str(output_dir),
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            summary = json.loads((output_dir / "mini-replay-summary.json").read_text(encoding="utf-8"))
            self.assertEqual(summary["draw_count"], 1)
            self.assertEqual(summary["index_bytes"], 6)
            self.assertEqual(summary["stream0_bytes"], 24)
            self.assertEqual(summary["uniform_draw_count"], 1)
            self.assertEqual(summary["uniform_bytes"], 14)
            self.assertEqual(summary["vs_bindings"]["buffer"], [1, 5, 6, 7])
            self.assertEqual(summary["fs_bindings"]["buffer"], [6, 7])
            self.assertIn("texture2d<float> tex0 [[texture(0)]]", (output_dir / "dxmt9_fs.replay.metal").read_text(encoding="utf-8"))
            self.assertNotIn("ArgbufLayout& abuf [[buffer(30)]]", (output_dir / "dxmt9_vs.replay.metal").read_text(encoding="utf-8"))
            self.assertIn("constant VsConsts& vsConsts [[buffer(6)]]", (output_dir / "dxmt9_vs.replay.metal").read_text(encoding="utf-8"))
            objc = (output_dir / "dxmt9_3dmark05_mini_replay.mm").read_text(encoding="utf-8")
            self.assertIn("psoDesc.colorAttachments[0].blendingEnabled = 1;", objc)
            self.assertIn("const char* vsConstsPath;", objc)
            self.assertIn("bufferFromFileOrDefault(device, draw.vsConstsPath, vsConsts)", objc)
            self.assertIn("draw.vsconsts.bin", objc)
            self.assertIn("sourceRGBBlendFactor = blendFactor(5, false);", objc)
            self.assertIn("destinationRGBBlendFactor = blendFactor(6, false);", objc)
            self.assertIn("sourceAlphaBlendFactor =\n        blendFactor(2, true);", objc)
            self.assertIn("alphaBlendOperation =\n        blendOperation(3);", objc)
            self.assertIn("depthStateDesc.depthCompareFunction =\n        1 ? compareFunction(4) : MTLCompareFunctionAlways;", objc)
            self.assertIn("depthStateDesc.depthWriteEnabled = 0;", objc)
            self.assertIn("[encoder setCullMode:cullMode(2)];", objc)
            self.assertIn("if (1) {", objc)
            self.assertIn("static_cast<NSUInteger>(10)", objc)
            self.assertIn("static_cast<NSUInteger>(20)", objc)
            self.assertIn("static_cast<NSUInteger>(std::max(0, 110 - 10))", objc)
            self.assertIn("static_cast<NSUInteger>(std::max(0, 220 - 20))", objc)

    def test_capture_path_requires_enough_free_space_before_compile(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest, output_dir = self.write_manifest_fixture(root)

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(manifest),
                    "--output-dir",
                    str(output_dir),
                    "--run",
                    "--capture-path",
                    str(output_dir / "mini-replay.gputrace"),
                    "--min-capture-free-mb",
                    "999999999",
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("insufficient free space for mini replay gputrace", result.stderr)
            self.assertNotIn("compile_cmd:", result.stdout)


if __name__ == "__main__":
    unittest.main()
