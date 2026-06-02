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
struct VSOut {
  float4 position [[position]];
  float4 texcoord0;
  float fogFactor;
  float pointSize [[point_size]];
};
struct ArgbufLayout {
  constant VsConsts* vsConsts [[id(0)]];
  constant FfpVsConsts* ffpVs [[id(1)]];
};
vertex VSOut dxmt9_vs(uint vid [[vertex_id]],
                     constant ArgbufLayout& abuf [[buffer(30)]],
                     device const uchar* stream0 [[buffer(1)]],
                     device const uchar* stream1 [[buffer(6)]],
                     constant DrawVolatile& drawVolatile [[buffer(5)]]) {
  constant VsConsts& vsConsts = *abuf.vsConsts;
  constant FfpVsConsts& ffpVs = *abuf.ffpVs;
  VSOut out;
  out.position = vsConsts.vsFloatConst[0] + float4(float(vid), 0.0, 0.0, 1.0);
  out.position.xy += ffpVs.halfPixelFixup;
  out.texcoord0 = float4(stream0[drawVolatile.vertexStreamOffset] + stream1[0], 0.0, 0.0, 1.0);
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
struct VSOut {
  float4 position [[position]];
  float4 texcoord0;
  float fogFactor;
  float pointSize [[point_size]];
};
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
        stream1_file = geometry_dir / "draw.stream1.bin"
        vsconsts_file = geometry_dir / "draw.vsconsts.bin"
        psconsts_file = geometry_dir / "draw.psconsts.bin"
        ffpvs_file = geometry_dir / "draw.ffpvs.bin"
        ffpps_file = geometry_dir / "draw.ffpps.bin"
        index_file.write_bytes(b"\x00\x00\x01\x00\x02\x00")
        stream_file.write_bytes(bytes(range(24)))
        stream1_file.write_bytes(bytes(range(64)))
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
                    "streams": [
                        {
                            "stream": 0,
                            "metal_slot": 1,
                            "file": str(stream_file),
                            "bytes": 24,
                        },
                        {
                            "stream": 1,
                            "metal_slot": 6,
                            "file": str(stream1_file),
                            "bytes": 64,
                        },
                    ],
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
                "attachments": {
                    "colors": [{
                        "index": 0,
                        "format": 2,
                        "width": 1024,
                        "height": 768,
                    }],
                    "depth": {
                        "format": 41,
                        "width": 1024,
                        "height": 768,
                    },
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
            self.assertEqual(summary["draw_order"], "original")
            self.assertEqual(summary["primitive_order"], "original")
            self.assertEqual(summary["depth_clear"], 1.0)
            self.assertIsNone(summary["depth_input"])
            self.assertEqual(summary["index_bytes"], 6)
            self.assertEqual(summary["stream0_bytes"], 24)
            self.assertEqual(summary["uniform_draw_count"], 1)
            self.assertEqual(summary["uniform_bytes"], 14)
            self.assertEqual(summary["vs_cbuf_slots"], {"ffpvs": 28, "vsconsts": 29})
            self.assertEqual(summary["fs_cbuf_slots"], {"ffpps": 28, "psconsts": 29})
            self.assertEqual(summary["actual_extra_vertex_buffer_slots"], [6])
            self.assertEqual(summary["dummy_vertex_buffer_slots"], [6])
            self.assertEqual(summary["scissor_draw_count"], 1)
            self.assertEqual(summary["vs_bindings"]["buffer"], [1, 5, 6, 28, 29])
            self.assertEqual(summary["fs_bindings"]["buffer"], [28, 29])
            self.assertIn("texture2d<float> tex0 [[texture(0)]]", (output_dir / "dxmt9_fs.replay.metal").read_text(encoding="utf-8"))
            self.assertNotIn("ArgbufLayout& abuf [[buffer(30)]]", (output_dir / "dxmt9_vs.replay.metal").read_text(encoding="utf-8"))
            self.assertIn("constant VsConsts& vsConsts [[buffer(29)]]", (output_dir / "dxmt9_vs.replay.metal").read_text(encoding="utf-8"))
            self.assertIn("device const uchar* stream1 [[buffer(6)]]", (output_dir / "dxmt9_vs.replay.metal").read_text(encoding="utf-8"))
            objc = (output_dir / "dxmt9_3dmark05_mini_replay.mm").read_text(encoding="utf-8")
            self.assertIn("psoDesc.colorAttachments[0].blendingEnabled = 1;", objc)
            self.assertIn("psoDesc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;", objc)
            self.assertIn("psoDesc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;", objc)
            self.assertIn("psoDesc.stencilAttachmentPixelFormat = MTLPixelFormatInvalid;", objc)
            self.assertIn("width:1024", objc)
            self.assertIn("height:768", objc)
            self.assertNotIn("pass.stencilAttachment.texture = depth;", objc)
            self.assertIn("pass.depthAttachment.loadAction = MTLLoadActionClear;", objc)
            self.assertIn("pass.depthAttachment.clearDepth = 1;", objc)
            self.assertIn("const char* vsConstsPath;", objc)
            self.assertIn("bufferFromFileOrDefault(device, draw.vsConstsPath, vsConsts)", objc)
            self.assertIn("const char* extraStreamPaths[16];", objc)
            self.assertIn("draw.stream1.bin", objc)
            self.assertIn("[encoder setVertexBuffer:dummyVertexStream offset:0 atIndex:6];", objc)
            self.assertIn("[encoder setVertexBuffer:extraStreams[s] offset:0 atIndex:draw.extraStreamSlots[s]];", objc)
            self.assertIn("[encoder setRenderPipelineState:psos[draw.shaderIndex]];", objc)
            self.assertIn("[encoder setVertexBuffer:drawVsConsts offset:0 atIndex:shader.vsConstsSlot];", objc)
            self.assertIn("[encoder setVertexBuffer:drawFfpVs offset:0 atIndex:shader.ffpVsSlot];", objc)
            self.assertIn("[encoder setFragmentBuffer:drawPsConsts offset:0 atIndex:shader.psConstsSlot];", objc)
            self.assertIn("[encoder setFragmentBuffer:drawFfpPs offset:0 atIndex:shader.ffpPsSlot];", objc)
            self.assertIn("draw.vsconsts.bin", objc)
            self.assertIn("sourceRGBBlendFactor = blendFactor(5, false);", objc)
            self.assertIn("destinationRGBBlendFactor = blendFactor(6, false);", objc)
            self.assertIn("sourceAlphaBlendFactor =\n        blendFactor(2, true);", objc)
            self.assertIn("alphaBlendOperation =\n        blendOperation(3);", objc)
            self.assertIn("depthStateDesc.depthCompareFunction =\n        1 ? compareFunction(4) : MTLCompareFunctionAlways;", objc)
            self.assertIn("depthStateDesc.depthWriteEnabled = 0;", objc)
            self.assertIn("[encoder setCullMode:cullMode(2)];", objc)
            self.assertIn("unsigned scissorEnabled;", objc)
            self.assertIn("static MTLScissorRect scissorRect(const DrawEntry& draw", objc)
            self.assertIn("[encoder setScissorRect:scissorRect(draw, 1024, 768)];", objc)
            self.assertIn(", 1, 10, 20, 110, 220}", objc)

    def test_trim_vsout_to_fs_reads_removes_unread_replay_fields(self) -> None:
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
                    "--trim-vsout-to-fs-reads",
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            summary = json.loads((output_dir / "mini-replay-summary.json").read_text(encoding="utf-8"))
            trim = summary["shader_variants"][0]["vsout_trim"]
            self.assertTrue(trim["enabled"])
            self.assertEqual(trim["keep_fields"], ["position", "texcoord0"])
            self.assertEqual(trim["removed_fields"], ["fogFactor", "pointSize"])
            vs_text = (output_dir / "dxmt9_vs.replay.metal").read_text(encoding="utf-8")
            fs_text = (output_dir / "dxmt9_fs.replay.metal").read_text(encoding="utf-8")
            self.assertIn("float4 position [[position]];", vs_text)
            self.assertIn("float4 texcoord0;", vs_text)
            self.assertNotIn("float fogFactor;", vs_text)
            self.assertNotIn("float pointSize [[point_size]];", vs_text)
            self.assertNotIn("out.fogFactor =", vs_text)
            self.assertNotIn("out.pointSize =", vs_text)
            self.assertNotIn("float fogFactor;", fs_text)
            self.assertNotIn("float pointSize [[point_size]];", fs_text)

    def test_primitive_order_rewrites_index_payload_in_output_dir(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest, output_dir = self.write_manifest_fixture(root)
            index_file = root / "geometry" / "draw.index.bin"
            index_file.write_bytes(
                b"\x00\x00\x01\x00\x02\x00"
                b"\x08\x00\x04\x00\x06\x00"
                b"\x03\x00\x02\x00\x01\x00"
            )

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(manifest),
                    "--output-dir",
                    str(output_dir),
                    "--primitive-order",
                    "sort-min-index",
                    "--draw-order",
                    "reverse",
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            summary = json.loads((output_dir / "mini-replay-summary.json").read_text(encoding="utf-8"))
            self.assertEqual(summary["draw_order"], "reverse")
            self.assertEqual(summary["primitive_order"], "sort-min-index")
            self.assertEqual(summary["index_cache_estimate"]["original_index_count"], 9)
            self.assertEqual(summary["index_cache_estimate"]["replay_index_count"], 9)
            self.assertEqual(summary["index_cache_estimate"]["original_lru16_misses"], 7)
            self.assertEqual(summary["index_cache_estimate"]["replay_lru16_misses"], 7)
            rewritten = output_dir / "index-order" / "draw000-sort-min-index.index.bin"
            self.assertEqual(
                rewritten.read_bytes(),
                b"\x00\x00\x01\x00\x02\x00"
                b"\x03\x00\x02\x00\x01\x00"
                b"\x08\x00\x04\x00\x06\x00",
            )
            objc = (output_dir / "dxmt9_3dmark05_mini_replay.mm").read_text(encoding="utf-8")
            self.assertIn(str(rewritten), objc)
            self.assertNotIn(str(index_file) + '",', objc)

    def test_depth_clear_overrides_generated_pass_clear_value(self) -> None:
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
                    "--depth-clear",
                    "0.25",
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            summary = json.loads((output_dir / "mini-replay-summary.json").read_text(encoding="utf-8"))
            objc = (output_dir / "dxmt9_3dmark05_mini_replay.mm").read_text(encoding="utf-8")
            self.assertEqual(summary["depth_clear"], 0.25)
            self.assertIn("pass.depthAttachment.clearDepth = 0.25;", objc)

    def test_depth_input_uploads_raw_sidecar_and_loads_depth_attachment(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest, output_dir = self.write_manifest_fixture(root)
            depth_input = root / "frame60-2-depth.bin"
            depth_input.write_bytes(b"\x00" * (1024 * 768 * 4))

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(manifest),
                    "--output-dir",
                    str(output_dir),
                    "--depth-input",
                    str(depth_input),
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            summary = json.loads((output_dir / "mini-replay-summary.json").read_text(encoding="utf-8"))
            objc = (output_dir / "dxmt9_3dmark05_mini_replay.mm").read_text(encoding="utf-8")
            self.assertEqual(summary["depth_input"], str(depth_input))
            self.assertIn(str(depth_input), objc)
            self.assertIn("id<MTLBuffer> depthUpload =", objc)
            self.assertIn("copyFromBuffer:depthUpload", objc)
            self.assertIn("sourceBytesPerRow:4096", objc)
            self.assertIn("length:3145728", objc)
            self.assertIn("pass.depthAttachment.loadAction = MTLLoadActionLoad;", objc)

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
