#!/usr/bin/env python3

from __future__ import annotations

import csv
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tools" / "summarize_xctrace_metal_intervals.py"


def write_encoder_rows(path: Path) -> None:
    fields = [
        "seq",
        "encoder",
        "draw_calls",
        "primitive_count",
        "vertex_count",
        "indexed_triangle_opaque_depth_write_primitives",
        "indexed_triangle_alpha_blend_primitives",
        "end_reason",
    ]
    rows = [
        {
            "seq": "60",
            "encoder": "2",
            "draw_calls": "100",
            "primitive_count": "333333",
            "vertex_count": "1000000",
            "indexed_triangle_opaque_depth_write_primitives": "333333",
            "indexed_triangle_alpha_blend_primitives": "0",
            "end_reason": "rt_change",
        },
        {
            "seq": "60",
            "encoder": "3",
            "draw_calls": "80",
            "primitive_count": "250000",
            "vertex_count": "750000",
            "indexed_triangle_opaque_depth_write_primitives": "0",
            "indexed_triangle_alpha_blend_primitives": "250000",
            "end_reason": "rt_change",
        },
    ]
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def write_indexed_probe_rows(path: Path) -> None:
    fields = [
        "seq",
        "encoder",
        "primitive_count",
        "vertex_count",
        "texture_mask",
        "color_write",
        "alpha_blend",
        "alpha_test",
        "depth_enabled",
        "depth_write",
    ]
    rows = [
        {
            "seq": "60",
            "encoder": "2",
            "primitive_count": "333333",
            "vertex_count": "1000000",
            "texture_mask": "0x0",
            "color_write": "0x0",
            "alpha_blend": "0",
            "alpha_test": "0",
            "depth_enabled": "1",
            "depth_write": "1",
        },
        {
            "seq": "60",
            "encoder": "3",
            "primitive_count": "250000",
            "vertex_count": "750000",
            "texture_mask": "0x7",
            "color_write": "0xf",
            "alpha_blend": "0",
            "alpha_test": "0",
            "depth_enabled": "1",
            "depth_write": "0",
        },
    ]
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


class SummarizeXctraceMetalIntervalsTests(unittest.TestCase):
    def test_adds_normalized_vertex_cost_and_primitive_class(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            intervals = root / "metal-gpu-intervals.xml"
            encoders = root / "3dmark05-perf-encoders.csv"
            indexed_probe = root / "3dmark05-perf-indexed-probe-draws.csv"
            output_csv = root / "summary.csv"
            output_md = root / "summary.md"
            intervals.write_text(
                textwrap.dedent(
                    """\
                    <trace-toc>
                      <run>
                        <data>
                          <table schema="metal-gpu-intervals">
                            <row>
                              <formatted-label>RenderPass[seq=60,enc=2,rt=0x1,depth=0x2]</formatted-label>
                              <duration>10 ms</duration>
                              <gpu-channel-name>Vertex</gpu-channel-name>
                              <gpu-frame-number>Frame 1</gpu-frame-number>
                              <process>3DMark05.exe</process>
                            </row>
                            <row>
                              <formatted-label>RenderPass[seq=60,enc=2,rt=0x1,depth=0x2]</formatted-label>
                              <duration>500 us</duration>
                              <gpu-channel-name>Fragment</gpu-channel-name>
                              <gpu-frame-number>Frame 1</gpu-frame-number>
                              <process>3DMark05.exe</process>
                            </row>
                            <row>
                              <formatted-label>RenderPass[seq=60,enc=3,rt=0x3,depth=0x2]</formatted-label>
                              <duration>15 ms</duration>
                              <gpu-channel-name>Vertex</gpu-channel-name>
                              <gpu-frame-number>Frame 2</gpu-frame-number>
                              <process>3DMark05.exe</process>
                            </row>
                          </table>
                        </data>
                      </run>
                    </trace-toc>
                    """
                ),
                encoding="utf-8",
            )
            write_encoder_rows(encoders)
            write_indexed_probe_rows(indexed_probe)

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--gpu-intervals",
                    str(intervals),
                    "--dxmt-encoders",
                    str(encoders),
                    "--output-csv",
                    str(output_csv),
                    "--output-md",
                    str(output_md),
                    "--indexed-probe-draws",
                    str(indexed_probe),
                    "--run-label",
                    "unit",
                    "--trace",
                    "unit.trace",
                    "--top",
                    "2",
                    "--require-xctrace-render-rows",
                    "--min-dxmt-join-coverage",
                    "0.99",
                    "--require-indexed-probe-routes",
                ],
                cwd=REPO_ROOT,
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            with output_csv.open(newline="", encoding="utf-8") as handle:
                rows = {(row["seq"], row["encoder"]): row for row in csv.DictReader(handle)}

            opaque = rows[("60", "2")]
            self.assertEqual(opaque["primitive_class"], "opaque-depth-indexed")
            self.assertAlmostEqual(float(opaque["xctrace_vertex_ms_per_mvertex"]), 10.0)
            self.assertAlmostEqual(float(opaque["xctrace_stage_ms_per_mvertex"]), 10.5)
            self.assertEqual(opaque["route_verdict"], "candidate-depth-only-route")
            self.assertEqual(opaque["route_depth_only_primitives"], "333333")

            alpha = rows[("60", "3")]
            self.assertEqual(alpha["primitive_class"], "alpha-blend-indexed")
            self.assertAlmostEqual(float(alpha["xctrace_vertex_ms_per_mvertex"]), 20.0)
            self.assertEqual(alpha["route_verdict"], "needs-programmable-textured-route")
            self.assertEqual(alpha["route_programmable_textured_primitives"], "250000")

            markdown = output_md.read_text(encoding="utf-8")
            self.assertIn("Top-2 vertex ms/Mvertex", markdown)
            self.assertIn("`alpha-blend-indexed=1`", markdown)
            self.assertIn("`opaque-depth-indexed=1`", markdown)
            self.assertIn("Vertex ms/Mvert", markdown)
            self.assertIn("## Aggregate By Primitive Class", markdown)
            self.assertIn("| `opaque-depth-indexed` | 1 | 10.500 | 41.18% | 10.000 | 0.500 | 95.24% | 10.000 |", markdown)
            self.assertIn("## Aggregate By End Reason", markdown)
            self.assertIn("| `rt_change` | 2 | 25.500 | 100.00% | 25.000 | 0.500 | 98.04% | 14.286 |", markdown)
            self.assertIn("## Aggregate By Route Verdict", markdown)
            self.assertIn("`candidate-depth-only-route`", markdown)
            self.assertIn("`needs-programmable-textured-route`", markdown)
            self.assertIn("Route", markdown)

    def test_require_xctrace_render_rows_rejects_unlabelled_xml(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            intervals = root / "metal-gpu-intervals.xml"
            encoders = root / "3dmark05-perf-encoders.csv"
            output_csv = root / "summary.csv"
            output_md = root / "summary.md"
            intervals.write_text(
                textwrap.dedent(
                    """\
                    <trace-toc>
                      <run>
                        <data>
                          <table schema="metal-gpu-intervals">
                            <row>
                              <formatted-label>UnrelatedEncoder</formatted-label>
                              <duration>10 ms</duration>
                              <gpu-channel-name>Vertex</gpu-channel-name>
                            </row>
                          </table>
                        </data>
                      </run>
                    </trace-toc>
                    """
                ),
                encoding="utf-8",
            )
            write_encoder_rows(encoders)

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--gpu-intervals",
                    str(intervals),
                    "--dxmt-encoders",
                    str(encoders),
                    "--output-csv",
                    str(output_csv),
                    "--output-md",
                    str(output_md),
                    "--require-xctrace-render-rows",
                ],
                cwd=REPO_ROOT,
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("xctrace render rows required", result.stderr)
            self.assertFalse(output_csv.exists())
            self.assertFalse(output_md.exists())

    def test_min_dxmt_join_coverage_rejects_wrong_encoder_csv(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            intervals = root / "metal-gpu-intervals.xml"
            encoders = root / "3dmark05-perf-encoders.csv"
            output_csv = root / "summary.csv"
            output_md = root / "summary.md"
            intervals.write_text(
                textwrap.dedent(
                    """\
                    <trace-toc>
                      <run>
                        <data>
                          <table schema="metal-gpu-intervals">
                            <row>
                              <formatted-label>RenderPass[seq=61,enc=2,rt=0x1,depth=0x2]</formatted-label>
                              <duration>10 ms</duration>
                              <gpu-channel-name>Vertex</gpu-channel-name>
                            </row>
                          </table>
                        </data>
                      </run>
                    </trace-toc>
                    """
                ),
                encoding="utf-8",
            )
            write_encoder_rows(encoders)

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--gpu-intervals",
                    str(intervals),
                    "--dxmt-encoders",
                    str(encoders),
                    "--output-csv",
                    str(output_csv),
                    "--output-md",
                    str(output_md),
                    "--require-xctrace-render-rows",
                    "--min-dxmt-join-coverage",
                    "0.99",
                ],
                cwd=REPO_ROOT,
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("dxmt encoder join coverage below required threshold", result.stderr)
            self.assertFalse(output_csv.exists())
            self.assertFalse(output_md.exists())

    def test_require_indexed_probe_routes_rejects_header_only_probe_csv(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            intervals = root / "metal-gpu-intervals.xml"
            encoders = root / "3dmark05-perf-encoders.csv"
            indexed_probe = root / "3dmark05-perf-indexed-probe-draws.csv"
            output_csv = root / "summary.csv"
            output_md = root / "summary.md"
            intervals.write_text(
                textwrap.dedent(
                    """\
                    <trace-toc>
                      <run>
                        <data>
                          <table schema="metal-gpu-intervals">
                            <row>
                              <formatted-label>RenderPass[seq=60,enc=2,rt=0x1,depth=0x2]</formatted-label>
                              <duration>10 ms</duration>
                              <gpu-channel-name>Vertex</gpu-channel-name>
                              <gpu-frame-number>Frame 1</gpu-frame-number>
                              <process>3DMark05.exe</process>
                            </row>
                          </table>
                        </data>
                      </run>
                    </trace-toc>
                    """
                ),
                encoding="utf-8",
            )
            write_encoder_rows(encoders)
            with indexed_probe.open("w", newline="", encoding="utf-8") as handle:
                writer = csv.DictWriter(handle, fieldnames=["seq", "encoder", "primitive_count"])
                writer.writeheader()

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--gpu-intervals",
                    str(intervals),
                    "--dxmt-encoders",
                    str(encoders),
                    "--output-csv",
                    str(output_csv),
                    "--output-md",
                    str(output_md),
                    "--indexed-probe-draws",
                    str(indexed_probe),
                    "--require-indexed-probe-routes",
                ],
                cwd=REPO_ROOT,
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("indexed probe route join required", result.stderr)
            self.assertFalse(output_csv.exists())
            self.assertFalse(output_md.exists())


if __name__ == "__main__":
    unittest.main()
