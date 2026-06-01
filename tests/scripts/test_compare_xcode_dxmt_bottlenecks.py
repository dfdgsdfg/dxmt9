#!/usr/bin/env python3
"""Regression tests for Xcode/dxmt joined-summary comparisons."""

from __future__ import annotations

import csv
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tools" / "compare_xcode_dxmt_bottlenecks.py"


def write_joined(path: Path, gpu_ms: float, buffer_write_mib: float,
                 stream_changes: int = 10, ib_changes: int = 5,
                 unexplained_ratio: float = 0.9,
                 draw_calls: int = 10,
                 pso_samples: int = 10,
                 seq: int = 60,
                 enc: int = 2,
                 vs_invocations: int = 1000,
                 vs_bytes_per_invocation: float = 819.2,
                 tiled_vertex_mib: float = 1.0,
                 tiled_primitive_mib: float = 0.5,
                 clip_limiter_pct: float = 1.0,
                 vsout_layout: str = "0xfff") -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fields = [
        "seq",
        "enc",
        "gpu_ms",
        "gpu_share_pct",
        "buffer_write_mib",
        "device_write_mib",
        "vs_buffer_write_mib",
        "vs_invocations",
        "vs_buffer_bytes_per_vs_invocation",
        "tiled_vertex_buffer_mib",
        "tiled_primitive_block_mib",
        "clip_unit_limiter_pct",
        "dxmt_vertex_count",
        "dxmt_draw_calls",
        "dxmt_pso_state_samples",
        "dxmt_stream_handle_changes",
        "dxmt_ib_handle_changes",
        "dxmt_argbuf_cbuf_bytes",
        "dxmt_transient_vertex_bytes",
        "dxmt_transient_index_bytes",
        "dxmt_unexplained_buffer_write_mib",
        "dxmt_unexplained_buffer_write_ratio",
        "dxmt_vsout_layout_last",
    ]
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerow({
            "seq": seq,
            "enc": enc,
            "gpu_ms": gpu_ms,
            "gpu_share_pct": 90.0,
            "buffer_write_mib": buffer_write_mib,
            "device_write_mib": buffer_write_mib,
            "vs_buffer_write_mib": buffer_write_mib * 0.8,
            "vs_invocations": vs_invocations,
            "vs_buffer_bytes_per_vs_invocation": vs_bytes_per_invocation,
            "tiled_vertex_buffer_mib": tiled_vertex_mib,
            "tiled_primitive_block_mib": tiled_primitive_mib,
            "clip_unit_limiter_pct": clip_limiter_pct,
            "dxmt_vertex_count": 1000,
            "dxmt_draw_calls": draw_calls,
            "dxmt_pso_state_samples": pso_samples,
            "dxmt_stream_handle_changes": stream_changes,
            "dxmt_ib_handle_changes": ib_changes,
            "dxmt_argbuf_cbuf_bytes": 1024,
            "dxmt_transient_vertex_bytes": 512,
            "dxmt_transient_index_bytes": 0,
            "dxmt_unexplained_buffer_write_mib": buffer_write_mib * unexplained_ratio,
            "dxmt_unexplained_buffer_write_ratio": unexplained_ratio,
            "dxmt_vsout_layout_last": vsout_layout,
        })


class CompareXcodeDxmtBottlenecksTests(unittest.TestCase):
    def test_output_parent_directory_is_created(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            before = root / "before.csv"
            after = root / "after.csv"
            output = root / "nested" / "reports" / "comparison.md"
            write_joined(before, gpu_ms=10.0, buffer_write_mib=100.0)
            write_joined(after, gpu_ms=8.0, buffer_write_mib=80.0)

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(before),
                    str(after),
                    "--output",
                    str(output),
                    "--require-top-gpu-decrease",
                    "--require-top-buffer-write-decrease",
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(output.exists())
            self.assertIn("Xcode/dxmt Bottleneck Comparison",
                          output.read_text(encoding="utf-8"))

    def test_requirement_failure_is_nonzero(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            before = root / "before.csv"
            after = root / "after.csv"
            write_joined(before, gpu_ms=10.0, buffer_write_mib=100.0)
            write_joined(after, gpu_ms=11.0, buffer_write_mib=110.0)

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(before),
                    str(after),
                    "--output",
                    str(root / "comparison.md"),
                    "--require-top-gpu-decrease",
                    "--require-top-buffer-write-decrease",
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("top_gpu_ms did not decrease", result.stderr)
            self.assertIn("top_buffer_write_mib did not decrease", result.stderr)

    def test_unexplained_buffer_write_gates(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            before = root / "before.csv"
            after = root / "after.csv"
            write_joined(before, gpu_ms=10.0, buffer_write_mib=100.0,
                         unexplained_ratio=0.95)
            write_joined(after, gpu_ms=9.0, buffer_write_mib=100.0,
                         unexplained_ratio=0.95)

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(before),
                    str(after),
                    "--output",
                    str(root / "comparison.md"),
                    "--require-top-unexplained-buffer-write-decrease",
                    "--max-top-unexplained-buffer-write-ratio",
                    "0.50",
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("top_unexplained_buffer_write_mib did not decrease",
                          result.stderr)
            self.assertIn("top_unexplained_buffer_write_ratio exceeds limit",
                          result.stderr)

    def test_report_warns_when_unexplained_writes_lack_pso_attribution(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            before = root / "before.csv"
            after = root / "after.csv"
            output = root / "comparison.md"
            write_joined(before, gpu_ms=10.0, buffer_write_mib=100.0,
                         unexplained_ratio=0.95, draw_calls=10, pso_samples=0)
            write_joined(after, gpu_ms=9.9, buffer_write_mib=100.0,
                         unexplained_ratio=0.95, draw_calls=10, pso_samples=0)

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(before),
                    str(after),
                    "--output",
                    str(output),
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            report = output.read_text(encoding="utf-8")
            self.assertIn("Unexplained top buffer write traffic remains dominant", report)
            self.assertIn("PSO/VSOut attribution is incomplete", report)
            self.assertIn("top_pso_state_samples_per_draw", report)

    def test_report_includes_top_encoder_delta_table(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            before = root / "before.csv"
            after = root / "after.csv"
            output = root / "comparison.md"
            write_joined(before, gpu_ms=20.0, buffer_write_mib=1000.0,
                         seq=60, enc=2, vs_invocations=640000,
                         vs_bytes_per_invocation=1600.0,
                         tiled_vertex_mib=12.0, tiled_primitive_mib=10.0,
                         clip_limiter_pct=3.25, vsout_layout="0xfff")
            write_joined(after, gpu_ms=19.0, buffer_write_mib=920.0,
                         seq=60, enc=2, vs_invocations=610000,
                         vs_bytes_per_invocation=1540.0,
                         tiled_vertex_mib=3.0, tiled_primitive_mib=2.0,
                         clip_limiter_pct=0.33, vsout_layout="0x0")

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(before),
                    str(after),
                    "--output",
                    str(output),
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            report = output.read_text(encoding="utf-8")
            self.assertIn("## Top Encoder Deltas", report)
            self.assertIn("`60/2`", report)
            self.assertIn("640,000 -> 610,000", report)
            self.assertIn("`0xfff -> 0x0`", report)
            self.assertIn("## VS Write Delta Attribution", report)
            self.assertIn("Invocation-count effect MiB", report)
            self.assertIn("`invocations`", report)
            self.assertIn(
                "`top_vsout_expected_stage_out_bytes_per_vertex` | `184.000` | `16.000`",
                report,
            )


if __name__ == "__main__":
    unittest.main()
