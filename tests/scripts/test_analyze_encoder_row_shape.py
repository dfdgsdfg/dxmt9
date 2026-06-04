#!/usr/bin/env python3
"""Tests for encoder-row shape drift preflight."""

from __future__ import annotations

import csv
import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tools" / "analyze_encoder_row_shape.py"


def load_module():
    spec = importlib.util.spec_from_file_location("analyze_encoder_row_shape", SCRIPT)
    assert spec is not None
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def write_encoders(path: Path, rows: list[dict[str, object]]) -> None:
    fields = [
        "seq",
        "encoder",
        "draw_calls",
        "indexed_draws",
        "expanded_indexed_draws",
        "primitive_count",
        "vertex_count",
        "triangle_estimate",
        "alpha_blend_enabled_draws",
        "scissor_enabled_draws",
        "depth_enabled_draws",
        "depth_write_draws",
        "indexed_triangle_depth_read_draws",
        "indexed_triangle_alpha_blend_draws",
        "indexed_triangle_textured_draws",
        "indexed_cache_opt_candidate_draws",
        "indexed_cache_opt_candidate_miss_delta_32",
        "reordered_index_cache_hits",
        "reordered_index_cache_created",
    ]
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


class AnalyzeEncoderRowShapeTests(unittest.TestCase):
    def test_compare_row_accepts_stable_shape(self) -> None:
        module = load_module()
        before = {"draw_calls": "100", "vertex_count": "3000", "triangle_estimate": "1000"}
        after = {"draw_calls": "101", "vertex_count": "3003", "triangle_estimate": "1001"}
        result = module.compare_row(before, after, max_shape_delta_ratio=0.05)
        self.assertEqual(result["status"], "ok")

    def test_cli_require_stable_shape_fails_on_drift(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            baseline = root / "baseline.csv"
            run = root / "run.csv"
            report = root / "report.md"
            summary = root / "summary.csv"
            write_encoders(baseline, [{
                "seq": "50",
                "encoder": "2",
                "draw_calls": 187,
                "indexed_draws": 187,
                "expanded_indexed_draws": 0,
                "primitive_count": 389376,
                "vertex_count": 1168128,
                "triangle_estimate": 389376,
                "alpha_blend_enabled_draws": 145,
                "scissor_enabled_draws": 42,
                "depth_enabled_draws": 187,
                "depth_write_draws": 0,
                "indexed_triangle_depth_read_draws": 187,
                "indexed_triangle_alpha_blend_draws": 145,
                "indexed_triangle_textured_draws": 187,
                "indexed_cache_opt_candidate_draws": 103,
                "indexed_cache_opt_candidate_miss_delta_32": -175168,
                "reordered_index_cache_hits": 0,
                "reordered_index_cache_created": 0,
            }])
            write_encoders(run, [{
                "seq": "50",
                "encoder": "2",
                "draw_calls": 16,
                "indexed_draws": 16,
                "expanded_indexed_draws": 0,
                "primitive_count": 19441,
                "vertex_count": 58323,
                "triangle_estimate": 19441,
                "alpha_blend_enabled_draws": 0,
                "scissor_enabled_draws": 0,
                "depth_enabled_draws": 16,
                "depth_write_draws": 0,
                "indexed_triangle_depth_read_draws": 16,
                "indexed_triangle_alpha_blend_draws": 0,
                "indexed_triangle_textured_draws": 16,
                "indexed_cache_opt_candidate_draws": 8,
                "indexed_cache_opt_candidate_miss_delta_32": -7299,
                "reordered_index_cache_hits": 4,
                "reordered_index_cache_created": 4,
            }])

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--baseline-encoders",
                    str(baseline),
                    "--run-encoders",
                    str(run),
                    "--row-key",
                    "50/2",
                    "--require-stable-shape",
                    "--output",
                    str(report),
                    "--csv-output",
                    str(summary),
                ],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

            self.assertEqual(result.returncode, 1)
            self.assertIn("Status: `shape-drift`", report.read_text(encoding="utf-8"))
            with summary.open(newline="", encoding="utf-8") as handle:
                rows = list(csv.DictReader(handle))
            self.assertEqual(rows[0]["status"], "shape-drift")


if __name__ == "__main__":
    unittest.main()
