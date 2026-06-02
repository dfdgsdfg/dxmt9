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
    def test_prepare_rewrites_argbuf_slots_and_summarizes_payloads(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
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
            index_file.write_bytes(b"\x00\x00\x01\x00\x02\x00")
            stream_file.write_bytes(bytes(range(24)))
            manifest = root / "manifest.json"
            manifest.write_text(json.dumps({
                "schema": "dxmt9.3dmark05.mini_replay_manifest.v1",
                "draws": [{
                    "state": {
                        "index_count": 3,
                        "base_vertex": 0,
                        "stream0_stride": 24,
                        "stream0_offset": 0,
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
                }],
            }), encoding="utf-8")

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
            self.assertEqual(summary["vs_bindings"]["buffer"], [1, 5, 6, 7])
            self.assertEqual(summary["fs_bindings"]["buffer"], [6, 7])
            self.assertIn("texture2d<float> tex0 [[texture(0)]]", (output_dir / "dxmt9_fs.replay.metal").read_text(encoding="utf-8"))
            self.assertNotIn("ArgbufLayout& abuf [[buffer(30)]]", (output_dir / "dxmt9_vs.replay.metal").read_text(encoding="utf-8"))
            self.assertIn("constant VsConsts& vsConsts [[buffer(6)]]", (output_dir / "dxmt9_vs.replay.metal").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
