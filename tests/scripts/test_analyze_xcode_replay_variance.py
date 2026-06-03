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


if __name__ == "__main__":
    unittest.main()
