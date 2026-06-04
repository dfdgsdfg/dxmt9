#!/usr/bin/env python3
"""Regression tests for joined-summary row drift analysis."""

from __future__ import annotations

import csv
import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tools" / "analyze_xcode_row_drift.py"


def load_module():
    spec = importlib.util.spec_from_file_location("analyze_xcode_row_drift", SCRIPT)
    assert spec is not None
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def write_joined(path: Path, rows: list[dict[str, object]]) -> None:
    fields = [
        "seq",
        "enc",
        "gpu_ms",
        "vs_buffer_write_mib",
        "vs_invocations",
        "vs_buffer_bytes_per_vs_invocation",
        "vs_buffer_bytes_per_primitive",
        "vs_buffer_write_limiter_pct",
        "buffer_write_limiter_pct",
        "vertex_stage_time_pct",
        "tiled_vertex_buffer_mib",
        "tiled_primitive_block_mib",
        "dxmt_named_tiled_buffer_mib",
        "dxmt_hidden_backend_write_mib",
        "dxmt_hidden_backend_write_ratio",
        "dxmt_vs_buffer_write_to_tvb_proxy_ratio",
        "dxmt_cpu_writer_mib",
        "dxmt_draw_calls",
        "dxmt_vertex_count",
        "dxmt_triangle_estimate",
        "dxmt_expanded_indexed_draws",
        "dxmt_indexed_vertex_cache_miss_estimate_32",
        "dxmt_reordered_index_cache_hits",
        "dxmt_reordered_index_cache_rejected_hits",
        "dxmt_draw_geometry_signature_samples",
        "dxmt_draw_geometry_signature_unique",
        "dxmt_draw_geometry_signature_duplicates",
        "dxmt_draw_geometry_signature_consecutive_duplicates",
        "dxmt_draw_geometry_signature_duplicate_ratio",
        "dxmt_draw_geometry_signature_consecutive_duplicate_ratio",
        "dxmt_indexed_cache_opt_candidate_draws",
        "dxmt_indexed_cache_opt_candidate_bytes",
        "dxmt_indexed_cache_opt_candidate_original_miss32",
        "dxmt_indexed_cache_opt_candidate_miss32",
        "dxmt_shader_variant_changes",
        "dxmt_ib_handle_changes",
        "dxmt_stream_handle_changes",
        "dxmt_transient_vertex_expanded_main_bytes",
        "dxmt_transient_vertex_expanded_extra_bytes",
    ]
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


class AnalyzeXcodeRowDriftTests(unittest.TestCase):
    def test_parse_labeled_path_requires_label(self) -> None:
        module = load_module()
        self.assertEqual(
            module.parse_labeled_path("base=/tmp/a.csv"),
            ("base", Path("/tmp/a.csv")),
        )
        with self.assertRaises(Exception):
            module.parse_labeled_path("/tmp/a.csv")

    def test_compare_rows_classifies_shape_drift(self) -> None:
        module = load_module()
        baseline_rows = {
            ("50", "11"): {
                "gpu_ms": "9.0",
                "vs_buffer_write_mib": "500",
                "vs_invocations": "2000",
                "vs_buffer_bytes_per_vs_invocation": "262.144",
                "vs_buffer_bytes_per_primitive": "1574.0",
                "vs_buffer_write_limiter_pct": "20.0",
                "buffer_write_limiter_pct": "21.0",
                "vertex_stage_time_pct": "90.0",
                "tiled_vertex_buffer_mib": "2.0",
                "tiled_primitive_block_mib": "1.0",
                "dxmt_named_tiled_buffer_mib": "3.0",
                "dxmt_hidden_backend_write_mib": "497.0",
                "dxmt_hidden_backend_write_ratio": "0.994",
                "dxmt_vs_buffer_write_to_tvb_proxy_ratio": "8.0",
                "dxmt_cpu_writer_mib": "0.1",
                "dxmt_draw_calls": "100",
                "dxmt_vertex_count": "1000",
                "dxmt_triangle_estimate": "333",
                "dxmt_expanded_indexed_draws": "0",
                "dxmt_indexed_vertex_cache_miss_estimate_32": "1200",
                "dxmt_reordered_index_cache_hits": "0",
                "dxmt_reordered_index_cache_rejected_hits": "0",
                "dxmt_draw_geometry_signature_samples": "100",
                "dxmt_draw_geometry_signature_unique": "95",
                "dxmt_draw_geometry_signature_duplicates": "5",
                "dxmt_draw_geometry_signature_consecutive_duplicates": "4",
                "dxmt_draw_geometry_signature_duplicate_ratio": "0.05",
                "dxmt_draw_geometry_signature_consecutive_duplicate_ratio": "0.04",
                "dxmt_indexed_cache_opt_candidate_draws": "90",
                "dxmt_indexed_cache_opt_candidate_bytes": "2048",
                "dxmt_indexed_cache_opt_candidate_original_miss32": "1100",
                "dxmt_indexed_cache_opt_candidate_miss32": "800",
                "dxmt_shader_variant_changes": "20",
                "dxmt_ib_handle_changes": "25",
                "dxmt_stream_handle_changes": "30",
                "dxmt_transient_vertex_expanded_main_bytes": "0",
                "dxmt_transient_vertex_expanded_extra_bytes": "0",
            },
        }
        run_rows = {
            ("50", "11"): {
                "gpu_ms": "12.0",
                "vs_buffer_write_mib": "620",
                "vs_invocations": "2400",
                "vs_buffer_bytes_per_vs_invocation": "270.0",
                "vs_buffer_bytes_per_primitive": "1600.0",
                "vs_buffer_write_limiter_pct": "24.0",
                "buffer_write_limiter_pct": "25.0",
                "vertex_stage_time_pct": "92.0",
                "tiled_vertex_buffer_mib": "2.5",
                "tiled_primitive_block_mib": "1.2",
                "dxmt_named_tiled_buffer_mib": "3.7",
                "dxmt_hidden_backend_write_mib": "616.3",
                "dxmt_hidden_backend_write_ratio": "0.994",
                "dxmt_vs_buffer_write_to_tvb_proxy_ratio": "8.2",
                "dxmt_cpu_writer_mib": "0.1",
                "dxmt_draw_calls": "130",
                "dxmt_vertex_count": "1300",
                "dxmt_triangle_estimate": "433",
                "dxmt_expanded_indexed_draws": "12",
                "dxmt_indexed_vertex_cache_miss_estimate_32": "1500",
                "dxmt_reordered_index_cache_hits": "4",
                "dxmt_reordered_index_cache_rejected_hits": "2",
                "dxmt_draw_geometry_signature_samples": "130",
                "dxmt_draw_geometry_signature_unique": "120",
                "dxmt_draw_geometry_signature_duplicates": "10",
                "dxmt_draw_geometry_signature_consecutive_duplicates": "8",
                "dxmt_draw_geometry_signature_duplicate_ratio": "0.0769",
                "dxmt_draw_geometry_signature_consecutive_duplicate_ratio": "0.0615",
                "dxmt_indexed_cache_opt_candidate_draws": "118",
                "dxmt_indexed_cache_opt_candidate_bytes": "3072",
                "dxmt_indexed_cache_opt_candidate_original_miss32": "1400",
                "dxmt_indexed_cache_opt_candidate_miss32": "1050",
                "dxmt_shader_variant_changes": "25",
                "dxmt_ib_handle_changes": "32",
                "dxmt_stream_handle_changes": "38",
                "dxmt_transient_vertex_expanded_main_bytes": "4096",
                "dxmt_transient_vertex_expanded_extra_bytes": "8192",
            },
        }

        comparisons = module.compare_rows(
            ("base", baseline_rows),
            [("run", run_rows)],
            [("50", "11")],
            list(module.DEFAULT_METRICS),
            0.05,
        )

        self.assertEqual(comparisons[0]["status"], "shape-drift")
        self.assertAlmostEqual(comparisons[0]["shape_max_ratio"], 0.3003, places=3)
        decision, next_action = module.decision_for_comparison(comparisons[0])
        self.assertEqual(decision, "reject-shape-drift")
        self.assertIn("stable row", next_action)

    def test_decision_summary_classifies_probe_outcomes(self) -> None:
        module = load_module()

        def item(after_overrides: dict[str, object]) -> dict[str, object]:
            before = {
                "gpu_ms": "20",
                "vs_buffer_write_mib": "1000",
                "vs_invocations": "100000",
                "dxmt_hidden_backend_write_mib": "950",
                "dxmt_expanded_indexed_draws": "0",
                "dxmt_reordered_index_cache_hits": "0",
            }
            after = dict(before)
            after.update({key: str(value) for key, value in after_overrides.items()})
            return {
                "status": "ok",
                "before": before,
                "after": after,
            }

        self.assertEqual(
            module.decision_for_comparison(item({
                "gpu_ms": 18,
                "vs_buffer_write_mib": 890,
                "vs_invocations": 89000,
                "dxmt_hidden_backend_write_mib": 845,
                "dxmt_reordered_index_cache_hits": 12,
            }))[0],
            "reorder-performance-mechanism",
        )
        self.assertEqual(
            module.decision_for_comparison(item({
                "gpu_ms": 19,
                "vs_buffer_write_mib": 960,
                "vs_invocations": 97000,
                "dxmt_hidden_backend_write_mib": 910,
            }))[0],
            "non-reorder-xcode-candidate",
        )
        self.assertEqual(
            module.decision_for_comparison(item({
                "gpu_ms": 20.2,
                "vs_buffer_write_mib": 985,
                "vs_invocations": 99000,
                "dxmt_hidden_backend_write_mib": 940,
            }))[0],
            "secondary-counter-only",
        )

    def test_cli_emits_report_summary_and_shape_failure(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            baseline = root / "baseline.csv"
            run = root / "run.csv"
            report = root / "report.md"
            summary = root / "summary.csv"
            write_joined(baseline, [
                {
                    "seq": 50,
                    "enc": 11,
                    "gpu_ms": 9.0,
                    "vs_buffer_write_mib": 500.0,
                    "vs_invocations": 2000,
                    "vs_buffer_bytes_per_vs_invocation": 262.144,
                    "vs_buffer_bytes_per_primitive": 1574.0,
                    "vs_buffer_write_limiter_pct": 20.0,
                    "buffer_write_limiter_pct": 21.0,
                    "vertex_stage_time_pct": 90.0,
                    "tiled_vertex_buffer_mib": 2.0,
                    "tiled_primitive_block_mib": 1.0,
                    "dxmt_named_tiled_buffer_mib": 3.0,
                    "dxmt_hidden_backend_write_mib": 497.0,
                    "dxmt_hidden_backend_write_ratio": 0.994,
                    "dxmt_vs_buffer_write_to_tvb_proxy_ratio": 8.0,
                    "dxmt_cpu_writer_mib": 0.1,
                    "dxmt_draw_calls": 100,
                    "dxmt_vertex_count": 1000,
                    "dxmt_triangle_estimate": 333,
                    "dxmt_expanded_indexed_draws": 0,
                    "dxmt_indexed_vertex_cache_miss_estimate_32": 1200,
                    "dxmt_reordered_index_cache_hits": 0,
                    "dxmt_reordered_index_cache_rejected_hits": 0,
                    "dxmt_draw_geometry_signature_samples": 100,
                    "dxmt_draw_geometry_signature_unique": 95,
                    "dxmt_draw_geometry_signature_duplicates": 5,
                    "dxmt_draw_geometry_signature_consecutive_duplicates": 4,
                    "dxmt_draw_geometry_signature_duplicate_ratio": 0.05,
                    "dxmt_draw_geometry_signature_consecutive_duplicate_ratio": 0.04,
                    "dxmt_indexed_cache_opt_candidate_draws": 90,
                    "dxmt_indexed_cache_opt_candidate_bytes": 2048,
                    "dxmt_indexed_cache_opt_candidate_original_miss32": 1100,
                    "dxmt_indexed_cache_opt_candidate_miss32": 800,
                    "dxmt_shader_variant_changes": 20,
                    "dxmt_ib_handle_changes": 25,
                    "dxmt_stream_handle_changes": 30,
                    "dxmt_transient_vertex_expanded_main_bytes": 0,
                    "dxmt_transient_vertex_expanded_extra_bytes": 0,
                },
            ])
            write_joined(run, [
                {
                    "seq": 50,
                    "enc": 11,
                    "gpu_ms": 12.0,
                    "vs_buffer_write_mib": 620.0,
                    "vs_invocations": 2400,
                    "vs_buffer_bytes_per_vs_invocation": 270.0,
                    "vs_buffer_bytes_per_primitive": 1600.0,
                    "vs_buffer_write_limiter_pct": 24.0,
                    "buffer_write_limiter_pct": 25.0,
                    "vertex_stage_time_pct": 92.0,
                    "tiled_vertex_buffer_mib": 2.5,
                    "tiled_primitive_block_mib": 1.2,
                    "dxmt_named_tiled_buffer_mib": 3.7,
                    "dxmt_hidden_backend_write_mib": 616.3,
                    "dxmt_hidden_backend_write_ratio": 0.994,
                    "dxmt_vs_buffer_write_to_tvb_proxy_ratio": 8.2,
                    "dxmt_cpu_writer_mib": 0.1,
                    "dxmt_draw_calls": 130,
                    "dxmt_vertex_count": 1300,
                    "dxmt_triangle_estimate": 433,
                    "dxmt_expanded_indexed_draws": 12,
                    "dxmt_indexed_vertex_cache_miss_estimate_32": 1500,
                    "dxmt_reordered_index_cache_hits": 4,
                    "dxmt_reordered_index_cache_rejected_hits": 2,
                    "dxmt_draw_geometry_signature_samples": 130,
                    "dxmt_draw_geometry_signature_unique": 120,
                    "dxmt_draw_geometry_signature_duplicates": 10,
                    "dxmt_draw_geometry_signature_consecutive_duplicates": 8,
                    "dxmt_draw_geometry_signature_duplicate_ratio": 0.0769,
                    "dxmt_draw_geometry_signature_consecutive_duplicate_ratio": 0.0615,
                    "dxmt_indexed_cache_opt_candidate_draws": 118,
                    "dxmt_indexed_cache_opt_candidate_bytes": 3072,
                    "dxmt_indexed_cache_opt_candidate_original_miss32": 1400,
                    "dxmt_indexed_cache_opt_candidate_miss32": 1050,
                    "dxmt_shader_variant_changes": 25,
                    "dxmt_ib_handle_changes": 32,
                    "dxmt_stream_handle_changes": 38,
                    "dxmt_transient_vertex_expanded_main_bytes": 4096,
                    "dxmt_transient_vertex_expanded_extra_bytes": 8192,
                },
            ])

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--baseline",
                    f"baseline={baseline}",
                    "--run",
                    f"run={run}",
                    "--row-key",
                    "50/11",
                    "--require-shape-stable",
                    "--output",
                    str(report),
                    "--summary-output",
                    str(summary),
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 1)
            self.assertIn("shape drift", result.stderr)
            text = report.read_text(encoding="utf-8")
            self.assertIn("# Xcode/dxmt Row Drift Report", text)
            self.assertIn("## Decision Summary", text)
            self.assertIn("`shape-drift`", text)
            self.assertIn("`reject-shape-drift`", text)
            self.assertIn("| `dxmt_draw_calls` |", text)
            self.assertIn("| `dxmt_hidden_backend_write_mib` |", text)
            self.assertIn("| `vs_buffer_bytes_per_vs_invocation` |", text)
            self.assertIn("| `dxmt_draw_geometry_signature_unique` |", text)
            self.assertIn("| `dxmt_indexed_cache_opt_candidate_original_miss32` |", text)
            self.assertIn("| `dxmt_ib_handle_changes` |", text)
            self.assertTrue(summary.exists())
            with summary.open(newline="", encoding="utf-8") as handle:
                rows = list(csv.DictReader(handle))
            self.assertEqual(rows[0]["decision"], "reject-shape-drift")
            self.assertIn("stable row", rows[0]["next_action"])


if __name__ == "__main__":
    unittest.main()
