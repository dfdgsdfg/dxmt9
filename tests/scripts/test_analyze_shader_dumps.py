#!/usr/bin/env python3
"""Regression tests for shader dump attribution reports."""

from __future__ import annotations

import csv
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tools" / "analyze_shader_dumps.py"


class AnalyzeShaderDumpsTests(unittest.TestCase):
    def write_joined_csv(self, path: Path, *, vs_source: int = 0,
                         ps_source: int = 0,
                         vs_buffer_bytes_per_invocation: str = "64") -> None:
        fields = [
            "seq",
            "enc",
            "gpu_ms",
            "vs_buffer_bytes_per_vs_invocation",
            "dxmt_vertex_shader_last",
            "dxmt_vertex_shader_source_last",
            "dxmt_pixel_shader_last",
            "dxmt_pixel_shader_source_last",
        ]
        with path.open("w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=fields)
            writer.writeheader()
            writer.writerow({
                "seq": "42",
                "enc": "0",
                "gpu_ms": "18.5",
                "vs_buffer_bytes_per_vs_invocation": vs_buffer_bytes_per_invocation,
                "dxmt_vertex_shader_last": "111",
                "dxmt_vertex_shader_source_last": str(vs_source),
                "dxmt_pixel_shader_last": "222",
                "dxmt_pixel_shader_source_last": str(ps_source),
            })

    def write_shader(self, shader_dir: Path, name: str) -> None:
        shader_dir.joinpath(name).write_text(
            "struct VSOut {\n"
            "  float4 position [[position]];\n"
            "  float2 texcoord0;\n"
            "  half4 color0;\n"
            "};\n"
            "float4 r[3];\n"
            "vertex VSOut main0() {\n"
            "  VSOut out;\n"
            "  float4 outTexcoord[8];\n"
            "  for (uint i = 0; i < 8u; ++i) { outTexcoord[i] = float4(0.0f, 0.0f, 0.0f, 1.0f); }\n"
            "  outTexcoord[0] = r[1];\n"
            "  out.position = r[0];\n"
            "  out.texcoord0 = outTexcoord[0].xy;\n"
            "  return out;\n"
            "}\n"
            "inline float4 dxmt9_select_texcoord(VSOut in, uint index) {\n"
            "  switch (index) {\n"
            "    case 0u: return float4(in.texcoord0, 0.0f, 1.0f);\n"
            "    case 1u: return float4(in.texcoord1, 0.0f, 1.0f);\n"
            "    default: return float4(in.texcoord0, 0.0f, 1.0f);\n"
            "  }\n"
            "}\n"
            "fragment float4 dxmt9_fs(VSOut in [[stage_in]]) {\n"
            "  return dxmt9_select_texcoord(in, 0u) + float4(in.color0);\n"
            "}\n",
            encoding="utf-8",
        )

    def run_analyzer(self, root: Path, *, require: bool) -> subprocess.CompletedProcess[str]:
        args = [
            sys.executable,
            str(SCRIPT),
            str(root / "joined.csv"),
            "--shader-dir",
            str(root / "shaders"),
            "--output",
            str(root / "report.md"),
            "--csv-output",
            str(root / "summary.csv"),
        ]
        if require:
            args.append("--require-matches")
        return subprocess.run(args, text=True, capture_output=True, check=False)

    def test_require_matches_fails_ambiguous_shader_hash_without_source_hash(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            shader_dir = root / "shaders"
            shader_dir.mkdir()
            self.write_joined_csv(root / "joined.csv")
            self.write_shader(shader_dir, "translated-vs-shader-111-source-10.metal")
            self.write_shader(shader_dir, "translated-vs-shader-111-source-11.metal")
            self.write_shader(shader_dir, "translated-fs-shader-222-source-20.metal")

            result = self.run_analyzer(root, require=True)

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("ambiguous", result.stderr)

    def test_require_matches_accepts_exact_source_hash(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            shader_dir = root / "shaders"
            shader_dir.mkdir()
            self.write_joined_csv(root / "joined.csv", vs_source=11, ps_source=20)
            self.write_shader(shader_dir, "translated-vs-shader-111-source-10.metal")
            self.write_shader(shader_dir, "translated-vs-shader-111-source-11.metal")
            self.write_shader(shader_dir, "translated-fs-shader-222-source-20.metal")

            result = self.run_analyzer(root, require=True)

            self.assertEqual(result.returncode, 0, result.stderr)
            with (root / "summary.csv").open(newline="", encoding="utf-8") as handle:
                row = next(csv.DictReader(handle))
            self.assertEqual(row["vs_file"], "translated-vs-shader-111-source-11.metal")
            self.assertEqual(row["vsout_field_count"], "3")
            self.assertEqual(row["vsout_estimated_bytes"], "32")
            self.assertEqual(float(row["vs_buffer_to_msl_vsout_ratio"]), 2.0)
            self.assertEqual(row["vs_temp_r_count"], "3")
            self.assertEqual(row["vs_temp_literal_index_count"], "2")
            self.assertEqual(row["vs_temp_literal_max_index"], "1")
            self.assertEqual(row["vs_temp_literal_span"], "2")
            self.assertEqual(row["vs_temp_dynamic_access_count"], "0")
            self.assertEqual(row["vs_temp_relative_access_count"], "0")
            self.assertEqual(row["vs_temp_zero_init_bytes"], "48")
            self.assertEqual(row["vs_temp_overdeclared_literal_bytes"], "16")
            self.assertEqual(row["vs_out_texcoord_count"], "8")
            self.assertEqual(row["vs_out_texcoord_literal_index_count"], "1")
            self.assertEqual(row["vs_out_texcoord_literal_max_index"], "0")
            self.assertEqual(row["vs_out_texcoord_literal_span"], "1")
            self.assertEqual(row["vs_out_texcoord_dynamic_access_count"], "0")
            self.assertEqual(row["vs_out_texcoord_relative_access_count"], "0")
            self.assertEqual(row["vs_out_texcoord_zero_init_bytes"], "128")
            self.assertEqual(row["vs_out_texcoord_overdeclared_literal_bytes"], "112")
            self.assertEqual(row["vsout_field_types"], "float4,float2,half4")
            self.assertEqual(row["vsout_write_count"], "2")
            self.assertEqual(row["ps_vsout_read_field_count"], "2")
            self.assertEqual(row["ps_vsout_read_fields"], "color0,texcoord0")
            self.assertEqual(row["ps_texcoord_read_mask"], "0x1")
            self.assertEqual(row["vsout_unread_field_count"], "0")
            self.assertEqual(row["vsout_unread_fields"], "")
            self.assertEqual(row["vsout_unread_estimated_bytes"], "0")
            self.assertEqual(float(row["vsout_unread_estimated_share"]), 0.0)
            self.assertNotIn("ambiguous_vs_dump", row["missing_reason"])

    def test_summary_reports_vsout_fields_not_read_by_fragment_shader(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            shader_dir = root / "shaders"
            shader_dir.mkdir()
            self.write_joined_csv(root / "joined.csv", vs_source=11, ps_source=20)
            shader_dir.joinpath("translated-vs-shader-111-source-11.metal").write_text(
                "struct VSOut {\n"
                "  float4 position [[position]];\n"
                "  float2 texcoord0;\n"
                "  float4 texcoord7;\n"
                "  float fogFactor;\n"
                "};\n"
                "vertex VSOut dxmt9_vs() {\n"
                "  VSOut out;\n"
                "  out.position = float4(0);\n"
                "  out.texcoord0 = float2(0);\n"
                "  out.texcoord7 = float4(0);\n"
                "  out.fogFactor = 1.0f;\n"
                "  return out;\n"
                "}\n",
                encoding="utf-8",
            )
            shader_dir.joinpath("translated-fs-shader-222-source-20.metal").write_text(
                "fragment float4 dxmt9_fs(VSOut in [[stage_in]]) {\n"
                "  return float4(in.texcoord0, 0.0f, 1.0f);\n"
                "}\n",
                encoding="utf-8",
            )

            result = self.run_analyzer(root, require=True)

            self.assertEqual(result.returncode, 0, result.stderr)
            with (root / "summary.csv").open(newline="", encoding="utf-8") as handle:
                row = next(csv.DictReader(handle))
            self.assertEqual(row["ps_vsout_read_fields"], "texcoord0")
            self.assertEqual(row["vsout_unread_field_count"], "2")
            self.assertEqual(row["vsout_unread_fields"], "fogFactor,texcoord7")
            self.assertEqual(row["vsout_unread_estimated_bytes"], "20")
            self.assertAlmostEqual(float(row["vsout_unread_estimated_share"]), 20 / 44)

    def test_summary_reports_dynamic_relative_temp_accesses(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            shader_dir = root / "shaders"
            shader_dir.mkdir()
            self.write_joined_csv(root / "joined.csv", vs_source=11, ps_source=20)
            shader_dir.joinpath("translated-vs-shader-111-source-11.metal").write_text(
                "struct VSOut { float4 position [[position]]; };\n"
                "vertex VSOut dxmt9_vs() {\n"
                "  float4 r[32];\n"
                "  for (uint i = 0; i < 32u; ++i) { r[i] = float4(0.0f); }\n"
                "  VSOut out;\n"
                "  r[clamp(a0.x + 2, 0, 31)] = r[5];\n"
                "  out.position = r[clamp(a0.y, 0, 31)];\n"
                "  return out;\n"
                "}\n",
                encoding="utf-8",
            )
            shader_dir.joinpath("translated-fs-shader-222-source-20.metal").write_text(
                "fragment float4 dxmt9_fs(VSOut in [[stage_in]]) {\n"
                "  return in.position;\n"
                "}\n",
                encoding="utf-8",
            )

            result = self.run_analyzer(root, require=True)

            self.assertEqual(result.returncode, 0, result.stderr)
            with (root / "summary.csv").open(newline="", encoding="utf-8") as handle:
                row = next(csv.DictReader(handle))
            self.assertEqual(row["vs_temp_r_count"], "32")
            self.assertEqual(row["vs_temp_literal_index_count"], "1")
            self.assertEqual(row["vs_temp_literal_max_index"], "5")
            self.assertEqual(row["vs_temp_literal_span"], "6")
            self.assertEqual(row["vs_temp_dynamic_access_count"], "2")
            self.assertEqual(row["vs_temp_relative_access_count"], "2")
            self.assertEqual(row["vs_temp_zero_init_bytes"], "512")
            self.assertEqual(row["vs_temp_overdeclared_literal_bytes"], "416")


if __name__ == "__main__":
    unittest.main()
