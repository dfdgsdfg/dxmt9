"""Regression tests for the mini-replay --force-fragment-color /
--force-fragment-primitive-id rewrites.

`force_fragment_color_source` used to replace the whole body of `dxmt9_fs`
with a bare `return float4(1.0f, 0.0f, 1.0f, 1.0f);` regardless of the
function's declared return type. dxmt9 emits `dxmt9_fs` with one of three
declared return shapes:

  - `FSOut`     -- every D3D9-bytecode-translated pixel shader
                   (`dxmt9_shader_metal_ir.cpp` hardcodes
                   `usesFragmentOutStruct = true`). This is the dominant
                   real shape for the per-draw shaders this harness dumps
                   and replays.
  - `FfpFsOut`  -- fixed-function draws (`dxmt9_ffp_shaders.cpp`); its
                   color member is named `color`, not `color0`.
  - bare `float4` -- only dxmt9's internal blit/gamma-apply/debug-fill
                   utility shaders (`dxmt9_shader_sources.cpp`), never the
                   per-draw shader this harness replays.

A struct return type also always carries a `uint ... [[sample_mask]]`
member (and sometimes a `float ... [[depth(any)]]` member for shaders that
write depth); leaving those uninitialized after a bare
`return float4(...);` either fails to compile (wrong return type) or, if
it happened to compile, would leave `sampleMask`/`depth` as garbage.

These tests drive `force_fragment_color_source` /
`force_fragment_primitive_id_source` directly against small synthetic MSL
fragments mirroring the three declared shapes -- see
tests/scripts/test_mini_replay_draw_state.py for the "drive the real
generator" convention this file borrows string-fixture style from.
"""

import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts" / "tools"))

import run_3dmark05_mini_replay as mini  # noqa: E402


def fsout_source(members: str, body: str = "  return dxmt9_make_fs_out(float4(0.0), 0xffffffffu);\n") -> str:
    """A minimal FSOut-shaped fragment function, the dominant real shape
    (every D3D9-bytecode-translated pixel shader)."""
    return f"""
#include <metal_stdlib>
using namespace metal;
struct VSOut {{
  float4 position [[position]];
}};
struct FSOut {{
{members}
}};
fragment FSOut dxmt9_fs(VSOut in [[stage_in]],
                     constant int& dummy [[buffer(0)]]) {{
{body}}}
"""


def ffp_fsout_source() -> str:
    """A minimal FfpFsOut-shaped fragment function (fixed-function draws).
    Its color member is named `color`, not `color0` -- getting this wrong
    is the exact mistake the fix must avoid."""
    return """
#include <metal_stdlib>
using namespace metal;
struct VSOut {
  float4 position [[position]];
};
struct FfpFsOut {
  float4 color [[color(0)]];
  uint sampleMask [[sample_mask]];
};
fragment FfpFsOut dxmt9_fs(VSOut in [[stage_in]]) {
  FfpFsOut result;
  result.color = float4(1.0);
  result.sampleMask = 0xffffffffu;
  return result;
}
"""


def bare_float4_source() -> str:
    """A minimal bare-float4 fragment function, matching only dxmt9's
    internal blit/gamma-apply/debug-fill utility shaders -- never the
    per-draw shader this harness replays, but still a shape the rewrite
    must not break."""
    return """
#include <metal_stdlib>
using namespace metal;
struct VSOut {
  float4 position [[position]];
};
fragment float4 dxmt9_fs(VSOut in [[stage_in]]) {
  return float4(1.0);
}
"""


class ForceFragmentColorTest(unittest.TestCase):
    def test_fsout_single_color_and_sample_mask(self):
        source = fsout_source(
            "  float4 color0 [[color(0)]];\n"
            "  uint sampleMask [[sample_mask]];"
        )
        out = mini.force_fragment_color_source(source)
        self.assertIn("FSOut o;", out)
        self.assertIn("o.color0 = float4(1.0f, 0.0f, 1.0f, 1.0f);", out)
        self.assertIn("o.sampleMask = 0xffffffffu;", out)
        self.assertIn("return o;", out)
        # The old bug: a bare `return float4(...)` against a struct return
        # type. Must not reappear.
        self.assertNotRegex(out, r"\{\s*return float4\(1\.0f, 0\.0f, 1\.0f, 1\.0f\);\s*\}")

    def test_fsout_with_depth_output(self):
        source = fsout_source(
            "  float4 color0 [[color(0)]];\n"
            "  float depth [[depth(any)]];\n"
            "  uint sampleMask [[sample_mask]];"
        )
        out = mini.force_fragment_color_source(source)
        self.assertIn("o.color0 = float4(1.0f, 0.0f, 1.0f, 1.0f);", out)
        self.assertIn("o.depth = 0.0f;", out)
        self.assertIn("o.sampleMask = 0xffffffffu;", out)

    def test_fsout_multiple_color_attachments(self):
        source = fsout_source(
            "  float4 color0 [[color(0)]];\n"
            "  float4 color1 [[color(1)]];\n"
            "  uint sampleMask [[sample_mask]];"
        )
        out = mini.force_fragment_color_source(source)
        self.assertIn("o.color0 = float4(1.0f, 0.0f, 1.0f, 1.0f);", out)
        self.assertIn("o.color1 = float4(1.0f, 0.0f, 1.0f, 1.0f);", out)

    def test_ffpfsout_color_member_is_not_named_color0(self):
        """FfpFsOut's color member is named `color`. Assuming `color0` (the
        FSOut convention) is exactly the kind of mistake the fix must not
        make -- it must parse the real member name."""
        out = mini.force_fragment_color_source(ffp_fsout_source())
        self.assertIn("FfpFsOut o;", out)
        self.assertIn("o.color = float4(1.0f, 0.0f, 1.0f, 1.0f);", out)
        self.assertNotIn("o.color0", out)
        self.assertIn("o.sampleMask = 0xffffffffu;", out)

    def test_bare_float4_return_unchanged_shape(self):
        """Only dxmt9's internal utility shaders declare a bare `float4`
        return; the rewrite must keep the simple `return float4(...);`
        shape for them instead of wrapping in a struct that does not
        exist."""
        out = mini.force_fragment_color_source(bare_float4_source())
        self.assertIn("return float4(1.0f, 0.0f, 1.0f, 1.0f);", out)
        self.assertNotIn(" o;", out)
        self.assertNotIn("o.color", out)

    def test_unrecognized_struct_member_attribute_fails_loudly(self):
        """A struct output member this rewrite does not know how to force
        (neither color/depth/sample_mask) must fail loudly rather than be
        silently left uninitialized."""
        source = fsout_source(
            "  float4 color0 [[color(0)]];\n"
            "  uint sampleMask [[sample_mask]];\n"
            "  float2 reserved [[user(locn7)]];"
        )
        with self.assertRaises(SystemExit):
            mini.force_fragment_color_source(source)


class ForceFragmentPrimitiveIdTest(unittest.TestCase):
    def test_fsout_gets_primitive_id_parameter_and_struct_assignment(self):
        source = fsout_source(
            "  float4 color0 [[color(0)]];\n"
            "  uint sampleMask [[sample_mask]];"
        )
        out = mini.force_fragment_primitive_id_source(source)
        self.assertEqual(out.count("[[primitive_id]]"), 1)
        self.assertIn("uint primitiveId [[primitive_id]]", out)
        self.assertIn("uint value = primitiveId + 1u;", out)
        self.assertIn("FSOut o;", out)
        self.assertIn("o.color0 = float4(float(value & 255u)", out)
        self.assertIn("o.sampleMask = 0xffffffffu;", out)
        self.assertIn("return o;", out)

    def test_ffpfsout_color_member_name_respected(self):
        out = mini.force_fragment_primitive_id_source(ffp_fsout_source())
        self.assertIn("o.color = float4(float(value & 255u)", out)
        self.assertNotIn("o.color0", out)

    def test_bare_float4_unchanged_shape(self):
        out = mini.force_fragment_primitive_id_source(bare_float4_source())
        self.assertIn("uint value = primitiveId + 1u;", out)
        self.assertIn("return float4(float(value & 255u)", out)
        self.assertNotIn("o.color", out)


if __name__ == "__main__":
    unittest.main()
