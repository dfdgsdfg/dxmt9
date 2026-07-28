"""Regression tests for the mini-replay reporting neither its coverage nor the
validity of the image it produced.

Confirmed root cause (do not re-investigate): the harness replays *one
encoder's draw window from one frame* and its summary said nothing about what
that covers. A D3D9->MSL translator change (`959c848c`, reverted in
`65a2d769`, corrected in `d63f7a65`) was validated by replaying GT1 frame60
encoder 1 through this harness with the new emission applied by hand --
786,432 of 786,432 pixels identical to baseline -- and read as "the codegen is
correct". Under the real runtime the same emission made every skinned
character in 3DMark05 GT1 disappear. Instrumenting the eight affected vertex
shaders so that taking the DEF-select branch collapses the vertex position
produced a byte-identical image: across 229 draws and ~795,000 vertex
invocations the branch under test was never executed. The verification was
testing dead code. Forcing the same marker on unconditionally changed 15,134
pixels, so the marker was live and those eight shaders own only 1.9% of the
frame. The harness printed `mini replay draws=229 repeat=1` and exited 0.

Two independent claims are under test here, and they must stay independent:

  * `coverage` says *what the replay contained* -- rows, encoders, draw count,
    and per-vertex-shader-hash draw counts, so a reader can see whether the
    shader they changed is present at all and in how many draws.
  * `validity` says *the image is non-degenerate* -- more than one distinct RGB
    triple. It proves nothing whatsoever about coverage. A validity pass on a
    replay that never executed the changed path is exactly the false negative
    above, and no test here may assert that validity implies coverage.

The degenerate threshold is the shape of the earlier all-black defect
(`36a41ad5`): a 1024x768 PPM carrying exactly one distinct pixel value while
the run exited 0. Only that case is gated; no percentage-coverage gate is
claimed because degeneracy is the only failure shape with evidence behind it.
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

# Same minimal shader fixtures as tests/scripts/test_mini_replay_fs_volatile.py:
# prepare() requires shaders.vs_file/ps_file to exist and to carry the
# buffer(30) argument-buffer parameter that transform_msl() rewrites.
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


def write_manifest(root: Path,
                   draw_specs: list[tuple[str, str]] | None = None,
                   with_summary: bool = True) -> Path:
    """Write a manifest whose draws carry distinct (vs_hash, ps_hash) pairs.

    `draw_specs` is one (vs_hash, ps_hash) pair per draw. The default mirrors
    the shape the defect depends on: several draws sharing one vertex shader
    plus a second vertex shader used by a single draw -- the "is the shader I
    changed even in this replay, and how many draws use it" question.
    """
    if draw_specs is None:
        draw_specs = [
            ("0x61be862718e1d00c", "0x59d863e8b836e4f6"),
            ("0x61be862718e1d00c", "0x59d863e8b836e4f6"),
            ("0x61be862718e1d00c", "0x59d863e8b836e4f6"),
            ("0xdeadbeefdeadbeef", "0x59d863e8b836e4f6"),
        ]

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

    draws = []
    for ordinal, (vs_hash, ps_hash) in enumerate(draw_specs):
        draws.append({
            "row": "60/1",
            "seq": 60,
            "encoder": 1,
            "encoder_draw_index": ordinal,
            "draw_ordinal": 31685 + ordinal,
            "shaders": {
                "vs_file": str(vs_path),
                "ps_file": str(ps_path),
                "vs_hash": vs_hash,
                "ps_hash": ps_hash,
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
                "alpha_test": "0",
            },
            "uniforms": {},
        })

    manifest: dict = {
        "schema": "dxmt9.3dmark05.mini_replay_manifest.v1",
        "draws": draws,
    }
    if with_summary:
        # Same field names the real builder emits; see the GT1 frame60 enc1
        # manifest's own `summary` block.
        manifest["summary"] = {
            "rows": ["60/1"],
            "encoder_draw_min": 0,
            "encoder_draw_max": len(draws) - 1,
            "draw_count": len(draws),
        }
    manifest_path = root / "manifest.json"
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    return manifest_path


def run_cli(manifest_path: Path, output_dir: Path,
            extra: list[str] | None = None) -> subprocess.CompletedProcess:
    cmd = [sys.executable, str(SCRIPT), str(manifest_path),
           "--output-dir", str(output_dir)]
    if extra:
        cmd.extend(extra)
    return subprocess.run(cmd, capture_output=True, text=True)


def read_summary(output_dir: Path) -> dict:
    return json.loads(
        (output_dir / "mini-replay-summary.json").read_text(encoding="utf-8"))


def write_ppm(path: Path, width: int, height: int, rgb_for) -> None:
    body = bytearray()
    for y in range(height):
        for x in range(width):
            body.extend(bytes(rgb_for(x, y)))
    path.write_bytes(b"P6\n%d %d\n255\n" % (width, height) + bytes(body))


class CoverageBlockTest(unittest.TestCase):
    """`coverage` must name what the replay actually contained."""

    def test_coverage_names_manifest_rows_and_encoders(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest_path = write_manifest(root)
            output_dir = root / "out"
            result = run_cli(manifest_path, output_dir)
            self.assertEqual(result.returncode, 0, result.stderr)
            coverage = read_summary(output_dir)["coverage"]
            self.assertEqual(coverage["rows"], ["60/1"])
            self.assertEqual(coverage["encoders"], ["60/1"])
            self.assertEqual(coverage["manifest_rows"], ["60/1"])
            self.assertEqual(coverage["manifest_encoder_draw_min"], 0)
            self.assertEqual(coverage["manifest_encoder_draw_max"], 3)
            self.assertEqual(coverage["manifest_draw_count"], 4)

    def test_coverage_scope_says_single_encoder_of_one_frame(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest_path = write_manifest(root)
            output_dir = root / "out"
            result = run_cli(manifest_path, output_dir)
            self.assertEqual(result.returncode, 0, result.stderr)
            scope = read_summary(output_dir)["coverage"]["scope"]
            lowered = scope.lower()
            self.assertIn("encoder", lowered)
            self.assertIn("frame", lowered)
            # It must state the negative claim, not only the positive one.
            self.assertIn("not", lowered)

    def test_coverage_draw_count_matches_summary_draw_count(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest_path = write_manifest(root)
            output_dir = root / "out"
            result = run_cli(manifest_path, output_dir)
            self.assertEqual(result.returncode, 0, result.stderr)
            summary = read_summary(output_dir)
            self.assertEqual(summary["coverage"]["draw_count"],
                             summary["draw_count"])
            self.assertEqual(summary["coverage"]["shader_variant_count"],
                             summary["shader_variant_count"])

    def test_per_vs_hash_draw_counts_sum_to_draw_count(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest_path = write_manifest(root)
            output_dir = root / "out"
            result = run_cli(manifest_path, output_dir)
            self.assertEqual(result.returncode, 0, result.stderr)
            summary = read_summary(output_dir)
            by_vs = summary["coverage"]["draws_by_vs_hash"]
            self.assertEqual(by_vs["0x61be862718e1d00c"], 3)
            self.assertEqual(by_vs["0xdeadbeefdeadbeef"], 1)
            self.assertEqual(sum(by_vs.values()), summary["draw_count"])
            by_ps = summary["coverage"]["draws_by_ps_hash"]
            self.assertEqual(by_ps["0x59d863e8b836e4f6"], 4)
            self.assertEqual(sum(by_ps.values()), summary["draw_count"])

    def test_draws_without_shader_hashes_are_still_counted(self):
        """A manifest whose draws carry no vs_hash must not silently drop rows
        from the per-hash totals -- the sum is what makes the block readable as
        coverage, so an unknown hash gets an explicit bucket."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest_path = write_manifest(root, draw_specs=[("", "")])
            output_dir = root / "out"
            result = run_cli(manifest_path, output_dir)
            self.assertEqual(result.returncode, 0, result.stderr)
            summary = read_summary(output_dir)
            by_vs = summary["coverage"]["draws_by_vs_hash"]
            self.assertEqual(sum(by_vs.values()), summary["draw_count"])
            self.assertIn(mini.UNKNOWN_SHADER_HASH, by_vs)

    def test_coverage_present_when_manifest_has_no_summary_block(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest_path = write_manifest(root, with_summary=False)
            output_dir = root / "out"
            result = run_cli(manifest_path, output_dir)
            self.assertEqual(result.returncode, 0, result.stderr)
            coverage = read_summary(output_dir)["coverage"]
            self.assertIsNone(coverage["manifest_rows"])
            # Rows observed in the replayed draws are still reported.
            self.assertEqual(coverage["rows"], ["60/1"])
            self.assertEqual(sum(coverage["draws_by_vs_hash"].values()),
                             coverage["draw_count"])

    def test_coverage_does_not_claim_the_replay_proves_execution(self):
        """The coverage block must not be phrased as a positive correctness
        claim; it reports containment, not that any path executed."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest_path = write_manifest(root)
            output_dir = root / "out"
            result = run_cli(manifest_path, output_dir)
            self.assertEqual(result.returncode, 0, result.stderr)
            coverage = read_summary(output_dir)["coverage"]
            self.assertIn("execut", coverage["scope"].lower())


class ColorOutputValidityTest(unittest.TestCase):
    """`validity` must be populated from the written image, and gate only the
    degenerate case."""

    def test_multi_colour_image_is_valid(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "out.bin"
            write_ppm(path, 8, 4, lambda x, y: (x * 8, y * 8, 0))
            validity = mini.assess_color_output(path)
            self.assertTrue(validity["asserted"])
            self.assertFalse(validity["degenerate"])
            self.assertEqual(validity["width"], 8)
            self.assertEqual(validity["height"], 4)
            self.assertEqual(validity["pixel_count"], 32)
            self.assertGreater(validity["distinct_rgb_values"], 1)
            # (0,0,0) at x==0,y==0 is the clear colour and must not count as
            # rendered content.
            self.assertEqual(validity["non_background_pixels"], 31)
            self.assertEqual(validity["min_distinct_rgb_values"],
                             mini.MIN_DISTINCT_RGB_VALUES)

    def test_single_distinct_value_image_is_degenerate(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "out.bin"
            write_ppm(path, 8, 4, lambda x, y: (0, 0, 0))
            validity = mini.assess_color_output(path)
            self.assertTrue(validity["asserted"])
            self.assertTrue(validity["degenerate"])
            self.assertEqual(validity["distinct_rgb_values"], 1)
            self.assertEqual(validity["non_background_pixels"], 0)

    def test_uniform_non_black_image_is_also_degenerate(self):
        """The gate is one-distinct-value, not all-black: a uniformly magenta
        image carries just as little information and must fail the same way."""
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "out.bin"
            write_ppm(path, 8, 4, lambda x, y: (255, 0, 255))
            validity = mini.assess_color_output(path)
            self.assertTrue(validity["degenerate"])
            self.assertEqual(validity["distinct_rgb_values"], 1)
            self.assertEqual(validity["non_background_pixels"], 32)

    def test_validity_states_what_it_does_not_certify(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "out.bin"
            write_ppm(path, 4, 4, lambda x, y: (x * 8, y * 8, 0))
            validity = mini.assess_color_output(path)
            certifies = validity["certifies"].lower()
            self.assertIn("non-degenerate", certifies)
            self.assertIn("not", certifies)
            self.assertIn("coverage", certifies)

    def test_missing_color_output_fails_loudly(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "absent.bin"
            with self.assertRaises(SystemExit) as ctx:
                mini.assess_color_output(path)
            self.assertIn("absent.bin", str(ctx.exception))

    def test_truncated_ppm_fails_loudly(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "out.bin"
            path.write_bytes(b"P6\n8 4\n255\n\x00\x00\x00")
            with self.assertRaises(SystemExit) as ctx:
                mini.assess_color_output(path)
            self.assertIn("truncated", str(ctx.exception).lower())


class DegenerateExitCodeTest(unittest.TestCase):
    """The degenerate case must actually terminate the process non-zero, the
    way `36a41ad5`'s all-black run should have."""

    def _enforce_in_child(self, path: Path) -> subprocess.CompletedProcess:
        program = (
            "import sys; sys.path.insert(0, %r);\n"
            "import run_3dmark05_mini_replay as m\n"
            "m.enforce_color_output_validity(m.assess_color_output(__import__('pathlib').Path(%r)))\n"
            % (str(REPO_ROOT / "scripts" / "tools"), str(path))
        )
        return subprocess.run([sys.executable, "-c", program],
                              capture_output=True, text=True)

    def test_single_distinct_value_exits_non_zero_and_names_the_degeneracy(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "out.bin"
            write_ppm(path, 8, 4, lambda x, y: (0, 0, 0))
            result = self._enforce_in_child(path)
            self.assertNotEqual(result.returncode, 0)
            message = result.stderr.lower()
            self.assertIn("degenerate", message)
            self.assertIn("1 distinct", message)
            self.assertIn("out.bin", result.stderr)

    def test_multi_colour_image_exits_zero(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "out.bin"
            write_ppm(path, 8, 4, lambda x, y: (x * 8, y * 8, 0))
            result = self._enforce_in_child(path)
            self.assertEqual(result.returncode, 0, result.stderr)


class ValidityNotAssertedTest(unittest.TestCase):
    """`validity` is never absent, and its absence never reads as a pass."""

    def test_no_color_output_records_not_asserted(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest_path = write_manifest(root)
            output_dir = root / "out"
            result = run_cli(manifest_path, output_dir)
            self.assertEqual(result.returncode, 0, result.stderr)
            validity = read_summary(output_dir)["validity"]
            self.assertIn("validity", read_summary(output_dir))
            self.assertFalse(validity["asserted"])
            self.assertIn("color output", validity["reason"].lower())
            # Nothing in a not-asserted record may read as a result.
            self.assertNotIn("degenerate", validity)
            self.assertIsNone(validity.get("distinct_rgb_values"))

    def test_color_output_requested_but_not_run_records_not_asserted(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest_path = write_manifest(root)
            output_dir = root / "out"
            result = run_cli(manifest_path, output_dir,
                             extra=["--color-output", str(root / "out.bin")])
            self.assertEqual(result.returncode, 0, result.stderr)
            validity = read_summary(output_dir)["validity"]
            self.assertFalse(validity["asserted"])
            self.assertIn("not", validity["reason"].lower())
            self.assertNotIn("degenerate", validity)


if __name__ == "__main__":
    unittest.main()
