#!/usr/bin/env python3

from __future__ import annotations

import csv
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tools" / "analyze_pso_backend_churn.py"


FIELDS = [
    "seq",
    "encoder",
    "draw_calls",
    "triangle_estimate",
    "vertex_count",
    "pso_handle_changes",
    "pso_unique_handles",
    "shader_variant_changes",
    "stream_metal_bind_handle_changes",
    "ib_handle_changes",
    "draw_geometry_signature_unique",
    "indexed_cache_opt_candidate_miss_delta_32",
]

PROBE_FIELDS = [
    "seq",
    "encoder",
    "encoder_draw_index",
    "pso",
    "shader_variant",
    "stream0_handle",
    "index_buffer",
    "stream_extra_bindings",
    "primitive_type",
    "primitive_count",
    "vertex_count",
    "base_vertex",
    "start_index",
    "index_type",
    "effective_index_offset",
    "effective_index_bytes",
]


class AnalyzePsoBackendChurnTests(unittest.TestCase):
    def test_classifies_stream_ib_dominant_and_candidate_rows(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            encoders = root / "encoders.csv"
            report = root / "pso.md"
            csv_output = root / "pso.csv"
            with encoders.open("w", newline="", encoding="utf-8") as handle:
                writer = csv.DictWriter(handle, fieldnames=FIELDS)
                writer.writeheader()
                writer.writerow({
                    "seq": "60",
                    "encoder": "2",
                    "draw_calls": "187",
                    "triangle_estimate": "389376",
                    "vertex_count": "1168128",
                    "pso_handle_changes": "47",
                    "pso_unique_handles": "20",
                    "shader_variant_changes": "78",
                    "stream_metal_bind_handle_changes": "271",
                    "ib_handle_changes": "160",
                    "draw_geometry_signature_unique": "159",
                    "indexed_cache_opt_candidate_miss_delta_32": "-175168",
                })
                writer.writerow({
                    "seq": "61",
                    "encoder": "0",
                    "draw_calls": "100",
                    "triangle_estimate": "200000",
                    "vertex_count": "600000",
                    "pso_handle_changes": "80",
                    "pso_unique_handles": "12",
                    "shader_variant_changes": "10",
                    "stream_metal_bind_handle_changes": "20",
                    "ib_handle_changes": "20",
                    "draw_geometry_signature_unique": "50",
                    "indexed_cache_opt_candidate_miss_delta_32": "0",
                })

            subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(encoders),
                    "--output",
                    str(report),
                    "--csv-output",
                    str(csv_output),
                ],
                cwd=REPO_ROOT,
                check=True,
            )

            text = report.read_text(encoding="utf-8")
            self.assertIn("candidate-pso-coupling", text)
            self.assertIn("stream-ib-dominant", text)

            with csv_output.open(newline="", encoding="utf-8") as handle:
                rows = {row["row"]: row for row in csv.DictReader(handle)}
            self.assertEqual(rows["60/2"]["verdict"], "stream-ib-dominant")
            self.assertEqual(rows["61/0"]["verdict"], "candidate-pso-coupling")

    def test_probe_draws_reject_pso_coupled_to_binding_tuples(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            encoders = root / "encoders.csv"
            probe_draws = root / "3dmark05-perf-indexed-probe-draws.csv"
            csv_output = root / "pso.csv"
            with encoders.open("w", newline="", encoding="utf-8") as handle:
                writer = csv.DictWriter(handle, fieldnames=FIELDS)
                writer.writeheader()
                writer.writerow({
                    "seq": "62",
                    "encoder": "0",
                    "draw_calls": "4",
                    "triangle_estimate": "120000",
                    "vertex_count": "360000",
                    "pso_handle_changes": "3",
                    "pso_unique_handles": "4",
                    "shader_variant_changes": "3",
                    "stream_metal_bind_handle_changes": "3",
                    "ib_handle_changes": "3",
                    "draw_geometry_signature_unique": "4",
                    "indexed_cache_opt_candidate_miss_delta_32": "0",
                })
                writer.writerow({
                    "seq": "63",
                    "encoder": "0",
                    "draw_calls": "4",
                    "triangle_estimate": "120000",
                    "vertex_count": "360000",
                    "pso_handle_changes": "3",
                    "pso_unique_handles": "4",
                    "shader_variant_changes": "3",
                    "stream_metal_bind_handle_changes": "0",
                    "ib_handle_changes": "0",
                    "draw_geometry_signature_unique": "1",
                    "indexed_cache_opt_candidate_miss_delta_32": "0",
                })

            with probe_draws.open("w", newline="", encoding="utf-8") as handle:
                writer = csv.DictWriter(handle, fieldnames=PROBE_FIELDS)
                writer.writeheader()
                for index in range(4):
                    writer.writerow({
                        "seq": "62",
                        "encoder": "0",
                        "encoder_draw_index": str(index),
                        "pso": f"0x10{index}",
                        "shader_variant": f"0x20{index}",
                        "stream0_handle": f"0x30{index}",
                        "index_buffer": f"0x40{index}",
                        "stream_extra_bindings": "",
                        "primitive_type": "trianglelist",
                        "primitive_count": "100",
                        "vertex_count": "300",
                        "base_vertex": "0",
                        "start_index": str(index * 300),
                        "index_type": "u16",
                        "effective_index_offset": str(index * 600),
                        "effective_index_bytes": "600",
                    })
                    writer.writerow({
                        "seq": "63",
                        "encoder": "0",
                        "encoder_draw_index": str(index),
                        "pso": f"0x50{index}",
                        "shader_variant": f"0x60{index}",
                        "stream0_handle": "0x700",
                        "index_buffer": "0x800",
                        "stream_extra_bindings": "s1:0x900@0/32",
                        "primitive_type": "trianglelist",
                        "primitive_count": "100",
                        "vertex_count": "300",
                        "base_vertex": "0",
                        "start_index": "0",
                        "index_type": "u16",
                        "effective_index_offset": "0",
                        "effective_index_bytes": "600",
                    })

            subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(encoders),
                    "--probe-draws",
                    str(probe_draws),
                    "--min-probe-run-draws",
                    "4",
                    "--csv-output",
                    str(csv_output),
                ],
                cwd=REPO_ROOT,
                check=True,
            )

            with csv_output.open(newline="", encoding="utf-8") as handle:
                rows = {row["row"]: row for row in csv.DictReader(handle)}
            self.assertEqual(rows["62/0"]["verdict"], "pso-coupled-to-bindings")
            self.assertEqual(rows["62/0"]["probe_pso_isolated_run_count"], "0")
            self.assertEqual(rows["63/0"]["verdict"], "candidate-pso-coupling")
            self.assertEqual(rows["63/0"]["probe_pso_isolated_run_count"], "1")


if __name__ == "__main__":
    unittest.main()
