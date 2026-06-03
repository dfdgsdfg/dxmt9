#!/usr/bin/env python3
"""Regression tests for Xcode replay variance analysis."""

from __future__ import annotations

import csv
import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tools" / "analyze_xcode_replay_variance.py"


def load_module():
    spec = importlib.util.spec_from_file_location(
        "analyze_xcode_replay_variance", SCRIPT)
    assert spec is not None
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def write_xcode_csv(path: Path, rows: list[dict[str, object]]) -> None:
    """Write a raw-Xcode-style counter CSV with the columns the tool reads.

    Each row maps Encoder Label to metric values keyed by raw column name.
    """
    fields = [
        "Encoder Label",
        "GPU Time",
        "Buffer Device Memory Bytes Written",
        "VS Buffer Device Memory Bytes Written",
        "VS Invocations",
        "Tiled Vertex Buffer Bytes",
        "Tiled Vertex Buffer Primitive Blocks Bytes",
    ]
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


class AnalyzeXcodeReplayVarianceTests(unittest.TestCase):
    def test_read_xcode_counters_extracts_encoder_label_and_metrics(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "run1.csv"
            write_xcode_csv(path, [
                {
                    "Encoder Label": "RenderPass[seq=50,enc=1,rt=0x10,depth=0x20]",
                    "GPU Time": 10_000_000,
                    "Buffer Device Memory Bytes Written": 100 * 1048576,
                    "VS Buffer Device Memory Bytes Written": 80 * 1048576,
                    "VS Invocations": 1_000_000,
                    "Tiled Vertex Buffer Bytes": 20 * 1048576,
                    "Tiled Vertex Buffer Primitive Blocks Bytes": 10 * 1048576,
                },
            ])
            rows = module.read_xcode_counters(path)
        self.assertEqual(len(rows), 1)
        row = rows[0]
        self.assertEqual(row["encoder_label"],
                          "RenderPass[seq=50,enc=1,rt=0x10,depth=0x20]")
        self.assertAlmostEqual(row["metrics"]["gpu_ms"], 10.0)
        self.assertAlmostEqual(row["metrics"]["buffer_write_mib"], 100.0)
        self.assertAlmostEqual(row["metrics"]["vs_buffer_write_mib"], 80.0)
        self.assertEqual(row["metrics"]["vs_invocations"], 1_000_000.0)
        self.assertAlmostEqual(row["metrics"]["tiled_vertex_buffer_mib"], 20.0)
        self.assertAlmostEqual(row["metrics"]["tiled_primitive_block_mib"], 10.0)

    def test_collect_samples_unions_rows_across_csvs(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            paths = []
            for i, gpu_ns in enumerate([10_000_000, 11_000_000, 10_500_000]):
                p = root / f"run{i + 1}.csv"
                write_xcode_csv(p, [
                    {
                        "Encoder Label": "RenderPass[seq=50,enc=1]",
                        "GPU Time": gpu_ns,
                        "Buffer Device Memory Bytes Written": 100 * 1048576,
                        "VS Buffer Device Memory Bytes Written": 80 * 1048576,
                        "VS Invocations": 1_000_000,
                        "Tiled Vertex Buffer Bytes": 20 * 1048576,
                        "Tiled Vertex Buffer Primitive Blocks Bytes": 10 * 1048576,
                    },
                ])
                paths.append(p)
            samples, missing = module.collect_samples(paths,
                                                       module.DEFAULT_METRICS)
        self.assertEqual(set(samples.keys()), {"RenderPass[seq=50,enc=1]"})
        gpu_samples = samples["RenderPass[seq=50,enc=1]"]["gpu_ms"]
        self.assertEqual(len(gpu_samples), 3)
        self.assertAlmostEqual(gpu_samples[0], 10.0)
        self.assertAlmostEqual(gpu_samples[1], 11.0)
        self.assertAlmostEqual(gpu_samples[2], 10.5)
        self.assertEqual(missing, {})

    def test_collect_samples_tracks_rows_missing_from_some_csvs(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            full_row = {
                "Encoder Label": "A",
                "GPU Time": 10_000_000,
                "Buffer Device Memory Bytes Written": 100 * 1048576,
                "VS Buffer Device Memory Bytes Written": 80 * 1048576,
                "VS Invocations": 1_000_000,
                "Tiled Vertex Buffer Bytes": 20 * 1048576,
                "Tiled Vertex Buffer Primitive Blocks Bytes": 10 * 1048576,
            }
            extra_row = {
                "Encoder Label": "B",
                "GPU Time": 5_000_000,
                "Buffer Device Memory Bytes Written": 50 * 1048576,
                "VS Buffer Device Memory Bytes Written": 40 * 1048576,
                "VS Invocations": 500_000,
                "Tiled Vertex Buffer Bytes": 10 * 1048576,
                "Tiled Vertex Buffer Primitive Blocks Bytes": 5 * 1048576,
            }
            paths = []
            for i in range(3):
                p = root / f"run{i + 1}.csv"
                rows = [full_row] if i == 0 else [full_row, extra_row]
                write_xcode_csv(p, rows)
                paths.append(p)
            samples, missing = module.collect_samples(paths,
                                                       module.DEFAULT_METRICS)
        self.assertIn("A", samples)
        self.assertIn("B", samples)
        self.assertEqual(len(samples["A"]["gpu_ms"]), 3)
        self.assertEqual(len(samples["B"]["gpu_ms"]), 2)
        self.assertEqual(missing.get("B"), {paths[0].name})


    def test_summarize_variance_computes_mean_stddev_cv(self) -> None:
        module = load_module()
        samples = {
            "A": {"gpu_ms": [10.0, 11.0, 12.0]},  # mean=11.0, stdev=1.0, cv=9.09%
            "B": {"gpu_ms": [5.0, 5.0, 5.0]},     # mean=5.0,  stdev=0.0, cv=0%
        }
        summary = module.summarize_variance(samples)
        self.assertEqual(summary["A"]["gpu_ms"]["n"], 3)
        self.assertAlmostEqual(summary["A"]["gpu_ms"]["mean"], 11.0)
        self.assertAlmostEqual(summary["A"]["gpu_ms"]["stddev"], 1.0)
        self.assertAlmostEqual(summary["A"]["gpu_ms"]["cv_pct"],
                                100.0 * 1.0 / 11.0, places=4)
        self.assertEqual(summary["B"]["gpu_ms"]["n"], 3)
        self.assertEqual(summary["B"]["gpu_ms"]["mean"], 5.0)
        self.assertEqual(summary["B"]["gpu_ms"]["stddev"], 0.0)
        self.assertEqual(summary["B"]["gpu_ms"]["cv_pct"], 0.0)

    def test_summarize_variance_blank_cv_when_mean_is_zero(self) -> None:
        module = load_module()
        samples = {"X": {"gpu_ms": [0.0, 0.0, 0.0]}}
        summary = module.summarize_variance(samples)
        self.assertEqual(summary["X"]["gpu_ms"]["n"], 3)
        self.assertEqual(summary["X"]["gpu_ms"]["mean"], 0.0)
        self.assertEqual(summary["X"]["gpu_ms"]["stddev"], 0.0)
        self.assertIsNone(summary["X"]["gpu_ms"]["cv_pct"])

    def test_summarize_variance_blank_when_fewer_than_two_samples(self) -> None:
        module = load_module()
        samples = {
            "X": {"gpu_ms": [10.0]},
            "Y": {"gpu_ms": []},
        }
        summary = module.summarize_variance(samples)
        self.assertEqual(summary["X"]["gpu_ms"]["n"], 1)
        self.assertEqual(summary["X"]["gpu_ms"]["mean"], 10.0)
        self.assertIsNone(summary["X"]["gpu_ms"]["stddev"])
        self.assertIsNone(summary["X"]["gpu_ms"]["cv_pct"])
        self.assertEqual(summary["Y"]["gpu_ms"]["n"], 0)
        self.assertIsNone(summary["Y"]["gpu_ms"]["mean"])
        self.assertIsNone(summary["Y"]["gpu_ms"]["stddev"])
        self.assertIsNone(summary["Y"]["gpu_ms"]["cv_pct"])


if __name__ == "__main__":
    unittest.main()
