"""Regression tests for a mini-replay pixel result carrying no proof that the
code path under test ever executed.

Confirmed root cause (do not re-investigate): defect 7. A D3D9->MSL translator
change was validated by replaying GT1 frame60 encoder 1 through this harness
with the new emission applied by hand -- 786,432 of 786,432 pixels identical to
baseline -- and read as "the codegen is correct". Under the real runtime the
same emission made every skinned character in 3DMark05 GT1 disappear.
Instrumenting the eight affected vertex shaders so that taking the branch under
test collapses the vertex position produced a byte-identical image: across 229
draws and ~795,000 vertex invocations the branch was never executed. Forcing
the same marker on unconditionally changed 15,134 pixels, proving the
instrumentation was live and the shaders own 1.9% of the frame.

`coverage` (tests/scripts/test_mini_replay_coverage_validity.py) answers
*containment* -- is the shader I changed in this replay, and in how many draws.
It cannot answer *execution*: defect 7's replay contained all eight affected
vertex shaders. `--prove-executed 'REGEX=>REPLACEMENT'` is what answers
execution, by generalizing the hand technique that found the defect: mutate the
construct in every generated `.metal` file, replay again, and compare. An
identical image proves the construct was never executed by this replay.

Two failure modes are under test here and they must never collapse into one
message, because they demand opposite responses from the maintainer:

  * **not present** -- the substitution matched no site at all. The pattern is
    wrong, or the construct is absent from these dumped shaders. This is not an
    execution verdict; the run says nothing about whether the construct would
    execute, and the fix is to correct the pattern.
  * **present but not executed** -- the substitution matched sites and the
    images are still identical. The construct is there and this replay never
    reaches it. This is defect 7's exact shape, and the fix is to stop using
    this replay as the oracle for that change.

A third outcome, **executed**, is the control: the mutation changed pixels, so
the replay does reach the construct and a pixel comparison over it means
something.
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

# `dxmt9_marker_token` stands in for the construct under test: it is present in
# the dumped vertex shader, so a pattern naming it matches sites, and a pattern
# naming anything else matches none.
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
  const float dxmt9_marker_token = 1.0f;
  out.position = vsConsts.vsFloatConst[0] * dxmt9_marker_token + float4(float(vid), 0.0, 0.0, 1.0);
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


def write_manifest(root: Path) -> Path:
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

    manifest = {
        "schema": "dxmt9.3dmark05.mini_replay_manifest.v1",
        "draws": [{
            "row": "60/1",
            "seq": 60,
            "encoder": 1,
            "encoder_draw_index": 0,
            "draw_ordinal": 31685,
            "shaders": {
                "vs_file": str(vs_path),
                "ps_file": str(ps_path),
                "vs_hash": "0x61be862718e1d00c",
                "ps_hash": "0x59d863e8b836e4f6",
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
        }],
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


def write_ppm(path: Path, width: int, height: int, rgb_for) -> None:
    body = bytearray()
    for y in range(height):
        for x in range(width):
            body.extend(bytes(rgb_for(x, y)))
    path.write_bytes(b"P6\n%d %d\n255\n" % (width, height) + bytes(body))


class ParseProveExecutedTest(unittest.TestCase):
    """The flag is a raw `REGEX=>REPLACEMENT` pair. It is deliberately not
    shader-aware, so its parse failures must be explicit."""

    def test_splits_on_the_first_separator(self):
        pattern, replacement = mini.parse_prove_executed("foo=>bar")
        self.assertEqual(pattern, "foo")
        self.assertEqual(replacement, "bar")

    def test_replacement_may_contain_the_separator(self):
        pattern, replacement = mini.parse_prove_executed("a=>b=>c")
        self.assertEqual(pattern, "a")
        self.assertEqual(replacement, "b=>c")

    def test_missing_separator_is_a_hard_failure(self):
        with self.assertRaises(SystemExit) as ctx:
            mini.parse_prove_executed("no-separator-here")
        self.assertIn(mini.PROVE_EXECUTED_SEPARATOR, str(ctx.exception))

    def test_empty_pattern_is_a_hard_failure(self):
        with self.assertRaises(SystemExit):
            mini.parse_prove_executed("=>replacement")

    def test_invalid_regex_is_a_hard_failure_naming_the_pattern(self):
        with self.assertRaises(SystemExit) as ctx:
            mini.ShaderMutation("(unclosed", "x")
        self.assertIn("(unclosed", str(ctx.exception))


class ShaderMutationTest(unittest.TestCase):
    """The substitution counts are the evidence separating "not present" from
    "present but not executed", so they are asserted directly."""

    def test_counts_files_scanned_changed_and_sites(self):
        mutation = mini.ShaderMutation(r"token", "TOKEN")
        self.assertEqual(mutation.apply("a token b token", "vs.metal"),
                         "a TOKEN b TOKEN")
        self.assertEqual(mutation.apply("nothing here", "fs.metal"),
                         "nothing here")
        self.assertEqual(mutation.files_scanned, 2)
        self.assertEqual(mutation.files_mutated, 1)
        self.assertEqual(mutation.sites, 2)
        self.assertEqual(mutation.mutated_files, ["vs.metal"])

    def test_backreferences_are_honoured(self):
        """The defect-7 reconstruction needs a group backreference to rebuild
        the original relative read inside the injected select."""
        mutation = mini.ShaderMutation(r"cFloat\[(a0\.x)\]", r"(\1 == 95 ? float4(9.0f) : cFloat[\1])")
        self.assertEqual(
            mutation.apply("y = cFloat[a0.x];", "vs.metal"),
            "y = (a0.x == 95 ? float4(9.0f) : cFloat[a0.x]);")
        self.assertEqual(mutation.sites, 1)


class DifferingPixelsTest(unittest.TestCase):
    def test_identical_images_report_zero(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_ppm(root / "a.ppm", 4, 4, lambda x, y: (x, y, 0))
            write_ppm(root / "b.ppm", 4, 4, lambda x, y: (x, y, 0))
            differing, total = mini.count_differing_pixels(root / "a.ppm", root / "b.ppm")
            self.assertEqual(differing, 0)
            self.assertEqual(total, 16)

    def test_differing_images_report_the_count(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_ppm(root / "a.ppm", 4, 4, lambda x, y: (x, y, 0))
            write_ppm(root / "b.ppm", 4, 4, lambda x, y: (x, y, 1 if x == 0 else 0))
            differing, total = mini.count_differing_pixels(root / "a.ppm", root / "b.ppm")
            self.assertEqual(differing, 4)
            self.assertEqual(total, 16)

    def test_dimension_mismatch_fails_loudly(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_ppm(root / "a.ppm", 4, 4, lambda x, y: (0, 0, 0))
            write_ppm(root / "b.ppm", 8, 4, lambda x, y: (0, 0, 0))
            with self.assertRaises(SystemExit) as ctx:
                mini.count_differing_pixels(root / "a.ppm", root / "b.ppm")
            self.assertIn("4x4", str(ctx.exception))
            self.assertIn("8x4", str(ctx.exception))


class ExecutionProofVerdictTest(unittest.TestCase):
    """The three verdicts and, above all, that the two failing ones do not read
    alike."""

    def _mutation(self, sites: int) -> mini.ShaderMutation:
        mutation = mini.ShaderMutation(r"token", "TOKEN")
        mutation.files_scanned = 4
        if sites:
            mutation.files_mutated = 2
            mutation.sites = sites
            mutation.mutated_files = ["vs.metal", "vs_01.metal"]
        return mutation

    def test_no_match_is_not_present_not_an_execution_verdict(self):
        proof = mini.build_execution_proof(self._mutation(0), None, None, None, None)
        self.assertEqual(proof["verdict"], mini.EXECUTION_VERDICT_NOT_PRESENT)
        self.assertEqual(proof["sites"], 0)
        self.assertIsNone(proof["differing_pixels"])
        message = mini.execution_proof_message(proof).lower()
        self.assertIn("not present", message)
        # It must not be readable as "the construct did not execute".
        self.assertNotIn("never executed", message)

    def test_matched_sites_with_identical_images_is_present_but_not_executed(self):
        proof = mini.build_execution_proof(
            self._mutation(48), Path("a.ppm"), Path("b.ppm"), 0, 786432)
        self.assertEqual(proof["verdict"], mini.EXECUTION_VERDICT_NOT_EXECUTED)
        self.assertEqual(proof["sites"], 48)
        self.assertEqual(proof["differing_pixels"], 0)
        message = mini.execution_proof_message(proof).lower()
        self.assertIn("never executed", message)
        self.assertIn("48", message)
        # The distinguishing claim: the construct *is* there.
        self.assertNotIn("not present", message)

    def test_matched_sites_with_differing_images_is_executed(self):
        proof = mini.build_execution_proof(
            self._mutation(48), Path("a.ppm"), Path("b.ppm"), 15134, 786432)
        self.assertEqual(proof["verdict"], mini.EXECUTION_VERDICT_EXECUTED)
        self.assertEqual(proof["differing_pixels"], 15134)
        self.assertIn("15134", mini.execution_proof_message(proof))

    def test_the_two_failure_messages_are_not_the_same_text(self):
        absent = mini.execution_proof_message(
            mini.build_execution_proof(self._mutation(0), None, None, None, None))
        unexecuted = mini.execution_proof_message(mini.build_execution_proof(
            self._mutation(48), Path("a.ppm"), Path("b.ppm"), 0, 786432))
        self.assertNotEqual(absent, unexecuted)

    def test_proof_record_states_what_it_certifies(self):
        proof = mini.build_execution_proof(
            self._mutation(48), Path("a.ppm"), Path("b.ppm"), 15134, 786432)
        certifies = proof["certifies"].lower()
        self.assertIn("execut", certifies)
        self.assertIn("this replay", certifies)


class EnforceExecutionProofTest(unittest.TestCase):
    """Both failing verdicts must terminate the process non-zero; the passing
    one must not."""

    def _mutation(self, sites: int) -> mini.ShaderMutation:
        mutation = mini.ShaderMutation(r"token", "TOKEN")
        mutation.files_scanned = 4
        if sites:
            mutation.files_mutated = 2
            mutation.sites = sites
        return mutation

    def test_not_present_exits(self):
        proof = mini.build_execution_proof(self._mutation(0), None, None, None, None)
        with self.assertRaises(SystemExit) as ctx:
            mini.enforce_execution_proof(proof)
        self.assertIn("not present", str(ctx.exception).lower())

    def test_not_executed_exits(self):
        proof = mini.build_execution_proof(
            self._mutation(48), Path("a.ppm"), Path("b.ppm"), 0, 786432)
        with self.assertRaises(SystemExit) as ctx:
            mini.enforce_execution_proof(proof)
        self.assertIn("never executed", str(ctx.exception).lower())

    def test_executed_does_not_exit(self):
        proof = mini.build_execution_proof(
            self._mutation(48), Path("a.ppm"), Path("b.ppm"), 15134, 786432)
        self.assertIsNone(mini.enforce_execution_proof(proof))


class MutatedShaderTreeTest(unittest.TestCase):
    """The mutated sources are a second, separate copy under the output dir.
    The baseline copies must be left untouched so the two replays differ only
    by the substitution -- otherwise a pixel difference proves nothing about
    the construct."""

    def test_prepare_writes_a_mutated_copy_and_leaves_the_baseline_intact(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest_path = write_manifest(root)
            parser = mini.build_parser()
            baseline_args = parser.parse_args(
                [str(manifest_path), "--output-dir", str(root / "out")])
            mini.prepare(baseline_args)
            mutated_args = parser.parse_args(
                [str(manifest_path), "--output-dir",
                 str(root / "out" / mini.EXECUTION_PROOF_DIR_NAME)])
            mutation = mini.ShaderMutation("dxmt9_marker_token", "dxmt9_mutated_token")
            mini.prepare(mutated_args, shader_mutation=mutation)

            # Two occurrences in the vertex shader (declaration and use).
            self.assertEqual(mutation.sites, 2)
            self.assertEqual(mutation.files_mutated, 1)
            self.assertEqual(mutation.files_scanned, 2)  # one vs + one fs
            mutated_vs = (root / "out" / mini.EXECUTION_PROOF_DIR_NAME
                          / "dxmt9_vs.replay.metal").read_text(encoding="utf-8")
            self.assertIn("dxmt9_mutated_token", mutated_vs)
            baseline_vs = (root / "out" / "dxmt9_vs.replay.metal").read_text(
                encoding="utf-8")
            self.assertIn("dxmt9_marker_token", baseline_vs)
            self.assertNotIn("dxmt9_mutated_token", baseline_vs)

    def test_mutated_summary_records_the_substitution(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest_path = write_manifest(root)
            args = mini.build_parser().parse_args(
                [str(manifest_path), "--output-dir", str(root / "out")])
            mutation = mini.ShaderMutation("dxmt9_marker_token", "dxmt9_mutated_token")
            summary = mini.prepare(args, shader_mutation=mutation)
            recorded = summary["shader_mutation"]
            self.assertEqual(recorded["pattern"], "dxmt9_marker_token")
            self.assertEqual(recorded["replacement"], "dxmt9_mutated_token")
            self.assertEqual(recorded["sites"], 2)

    def test_unmutated_prepare_records_no_shader_mutation(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest_path = write_manifest(root)
            args = mini.build_parser().parse_args(
                [str(manifest_path), "--output-dir", str(root / "out")])
            summary = mini.prepare(args)
            self.assertIsNone(summary["shader_mutation"])


class ProveExecutedCliTest(unittest.TestCase):
    """End-to-end behaviour that does not need a GPU: the argument contract and
    the not-present verdict, which is decided from the generated shader sources
    before anything is compiled."""

    def test_requires_run_and_color_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest_path = write_manifest(root)
            result = run_cli(manifest_path, root / "out",
                             extra=["--prove-executed", "token=>TOKEN"])
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("--run", result.stderr)
            self.assertIn("--color-output", result.stderr)

    def test_pattern_matching_nothing_exits_before_compiling(self):
        """A wrong pattern must be reported as "not present" from the generated
        sources alone -- no compile, no GPU, no image."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest_path = write_manifest(root)
            output_dir = root / "out"
            result = run_cli(manifest_path, output_dir, extra=[
                "--run", "--color-output", str(root / "color.ppm"),
                "--prove-executed", "no_such_construct_anywhere=>x",
            ])
            self.assertNotEqual(result.returncode, 0)
            lowered = result.stderr.lower()
            self.assertIn("not present", lowered)
            self.assertNotIn("never executed", lowered)
            # Nothing was built or run.
            self.assertNotIn("compile_cmd:", result.stdout)
            self.assertFalse((root / "color.ppm").exists())

    def test_not_present_verdict_is_recorded_in_the_summary(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest_path = write_manifest(root)
            output_dir = root / "out"
            run_cli(manifest_path, output_dir, extra=[
                "--run", "--color-output", str(root / "color.ppm"),
                "--prove-executed", "no_such_construct_anywhere=>x",
            ])
            summary = json.loads(
                (output_dir / "mini-replay-summary.json").read_text(encoding="utf-8"))
            proof = summary["execution_proof"]
            self.assertEqual(proof["verdict"], mini.EXECUTION_VERDICT_NOT_PRESENT)
            self.assertEqual(proof["pattern"], "no_such_construct_anywhere")
            self.assertEqual(proof["replacement"], "x")
            self.assertEqual(proof["sites"], 0)
            self.assertGreater(proof["files_scanned"], 0)

    def test_help_documents_the_sharpness_of_a_raw_regex(self):
        result = subprocess.run([sys.executable, str(SCRIPT), "--help"],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--prove-executed", result.stdout)
        lowered = result.stdout.lower()
        self.assertIn("regex", lowered)
        # The help must say what a non-match means, since that is the mode a
        # maintainer will hit first with a wrong pattern.
        self.assertIn("not present", lowered)


if __name__ == "__main__":
    unittest.main()
