"""Regression tests for `color_pixel_format()` / `depth_pixel_format()`
silently degrading to a plausible-looking Metal pixel format for an
unrecognized `core::Format` value.

`color_pixel_format()` recognized `core::Format` values 1-4 and returned
`MTLPixelFormatRGBA8Unorm` for every other value, including R32F
(`core::Format` 16 -- confirmed against `include/dxmt9/core_constants.hpp`'s
`enum class Format` by counting ordinals from `Unknown = 0`), which a real
capture row (`60/0`) actually uses. The replay then rendered into a
wrong-format attachment (byte-reinterpreting a 4-byte-per-pixel float
channel as 4 interleaved 8-bit RGBA channels) with no diagnostic that the
format was unrecognized and no non-zero exit.

Per `specs/experiments/harness/replay/requirements.md` R-HARN-REPLAY-2.1,
`depth_pixel_format()` has "the identical unnamed-fallback shape ... and is
not exempt from this requirement merely because no wild run has yet
exercised an unrecognized depth format" -- so this file covers both
resolvers on the same terms.

The fix makes both resolvers hard-fail (`SystemExit` naming the format
ordinal and RT dimensions) for anything outside their declared finite set.
A manifest draw that omits per-draw `attachments` metadata entirely (the
shape every pre-existing mini-replay test fixture uses, and the shape
`tests/scripts/test_mini_replay_draw_state.py`/`test_mini_replay_vertex_order.py`
still exercise) is a different, legacy input shape -- not a declared-but-
unrecognized format value -- and keeps resolving to the historical
RGBA8Unorm/Depth32Float default; that path is intentionally kept separate
from these two strict resolvers so this fix does not regress the existing
suite that never populates `attachments`.
"""

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

SCRIPT = REPO_ROOT / "scripts" / "tools" / "run_3dmark05_mini_replay.py"

STRIDE = 8
INDICES = [0, 1, 2]

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


def write_manifest(root: Path, attachments: dict | None) -> Path:
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
        "state": {
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
        },
        "uniforms": {},
    }
    if attachments is not None:
        draw["attachments"] = attachments

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


class ColorPixelFormatResolverTest(unittest.TestCase):
    """Unit-level checks directly against the resolver, the way
    tests/scripts/test_mini_replay_draw_state.py checks draw_blend_state()
    etc. directly."""

    def test_recognized_bgra_formats(self):
        self.assertEqual(mini.color_pixel_format(1), "MTLPixelFormatBGRA8Unorm")  # A8R8G8B8
        self.assertEqual(mini.color_pixel_format(2), "MTLPixelFormatBGRA8Unorm")  # X8R8G8B8

    def test_recognized_rgba_formats(self):
        self.assertEqual(mini.color_pixel_format(3), "MTLPixelFormatRGBA8Unorm")  # A8B8G8R8
        self.assertEqual(mini.color_pixel_format(4), "MTLPixelFormatRGBA8Unorm")  # X8B8G8R8

    def test_r32f_is_a_hard_failure_not_a_silent_rgba8_fallback(self):
        """core::Format 16 is R32F (counting ordinals in
        include/dxmt9/core_constants.hpp's enum class Format from
        Unknown=0: 1 A8R8G8B8, 2 X8R8G8B8, 3 A8B8G8R8, 4 X8B8G8R8, 5 R5G6B5,
        6 A1R5G5B5, 7 X1R5G5B5, 8 A4R4G4B4, 9 A8, 10 R8G8B8, 11
        A16B16G16R16F, 12 A32B32G32R32F, 13 G16R16F, 14 R16F, 15 G32R32F,
        16 R32F). Rendering an R32F target's raw float bytes through an
        RGBA8Unorm attachment silently reinterprets the bit pattern; this
        must fail loudly instead, naming the ordinal and RT dimensions."""
        with self.assertRaises(SystemExit) as ctx:
            mini.color_pixel_format(16, width=1024, height=768)
        message = str(ctx.exception)
        self.assertIn("16", message)
        self.assertIn("1024", message)
        self.assertIn("768", message)

    def test_unrecognized_format_zero_is_also_a_hard_failure(self):
        """Format 0 (`core::Format::Unknown`) reached through a *declared*
        attachment (as opposed to a manifest that omits `attachments`
        entirely) must fail the same way as any other unrecognized value."""
        with self.assertRaises(SystemExit):
            mini.color_pixel_format(0, width=640, height=480)


class DepthPixelFormatResolverTest(unittest.TestCase):
    def test_recognized_stencil_formats(self):
        self.assertEqual(mini.depth_pixel_format(40), "MTLPixelFormatDepth32Float_Stencil8")  # D24S8
        self.assertEqual(mini.depth_pixel_format(41), "MTLPixelFormatDepth32Float_Stencil8")  # D24X8
        self.assertEqual(mini.depth_pixel_format(49), "MTLPixelFormatDepth32Float_Stencil8")  # D24FS8

    def test_recognized_d16_formats(self):
        self.assertEqual(mini.depth_pixel_format(42), "MTLPixelFormatDepth16Unorm")  # D16
        self.assertEqual(mini.depth_pixel_format(46), "MTLPixelFormatDepth16Unorm")  # D16_LOCKABLE

    def test_df16_is_a_hard_failure_not_a_silent_depth32float_fallback(self):
        """DF16 (core::Format 52, counting onward from D16_LOCKABLE=46: 47
        D15S1, 48 D24X4S4, 49 D24FS8, 50 S8_LOCKABLE, 51 INTZ, 52 DF16) is
        documented in core_constants.hpp as backed by
        MTLPixelFormatDepth16Unorm, not Depth32Float -- the old catch-all
        fallback would have silently mismapped it. It must fail loudly
        instead of guessing, exactly like the color resolver."""
        with self.assertRaises(SystemExit) as ctx:
            mini.depth_pixel_format(52, width=1024, height=768)
        message = str(ctx.exception)
        self.assertIn("52", message)
        self.assertIn("1024", message)
        self.assertIn("768", message)


class ManifestWithAttachmentsCliTest(unittest.TestCase):
    """Drives the real CLI end-to-end, the way
    GeneratedSourcePerDrawStateTest in test_mini_replay_draw_state.py does."""

    def test_supported_color_and_depth_format_succeeds(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest_path = write_manifest(root, {
                "colors": [{"format": 2, "width": 1024, "height": 768}],  # X8R8G8B8
                "depth": {"format": 41, "width": 1024, "height": 768},    # D24X8
            })
            result = run_cli(manifest_path, root / "out")
            self.assertEqual(result.returncode, 0, result.stderr)
            source = (root / "out" / "dxmt9_3dmark05_mini_replay.mm").read_text(encoding="utf-8")
            self.assertIn("MTLPixelFormatBGRA8Unorm", source)
            self.assertIn("MTLPixelFormatDepth32Float_Stencil8", source)

    def test_r32f_color_format_fails_the_whole_run_naming_ordinal_and_dims(self):
        """The exact defect-3 repro: row 60/0's declared render target is
        R32F (core::Format 16). The harness must exit non-zero and name the
        format and dimensions rather than reporting `mini replay draws=...`
        success into a wrong-format attachment."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest_path = write_manifest(root, {
                "colors": [{"format": 16, "width": 1024, "height": 768}],  # R32F
                "depth": {"format": 41, "width": 1024, "height": 768},
            })
            result = run_cli(manifest_path, root / "out")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("16", result.stderr)
            self.assertIn("1024", result.stderr)
            self.assertIn("768", result.stderr)
            self.assertNotIn("mini replay draws=", result.stdout)

    def test_unsupported_depth_format_fails_the_whole_run(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest_path = write_manifest(root, {
                "colors": [{"format": 2, "width": 1024, "height": 768}],
                "depth": {"format": 52, "width": 1024, "height": 768},  # DF16
            })
            result = run_cli(manifest_path, root / "out")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("52", result.stderr)

    def test_manifest_without_attachments_still_succeeds_with_legacy_default(self):
        """Regression guard for the design decision above: a manifest that
        never populates `attachments` (every pre-existing mini-replay test
        fixture) must keep working via the historical default, not start
        hard-failing on an inferred format_value=0."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest_path = write_manifest(root, attachments=None)
            result = run_cli(manifest_path, root / "out")
            self.assertEqual(result.returncode, 0, result.stderr)
            source = (root / "out" / "dxmt9_3dmark05_mini_replay.mm").read_text(encoding="utf-8")
            self.assertIn("MTLPixelFormatRGBA8Unorm", source)
            self.assertIn("MTLPixelFormatDepth32Float", source)


if __name__ == "__main__":
    unittest.main()
