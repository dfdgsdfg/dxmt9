#!/usr/bin/env python3
"""Tests for indexed probe class aggregation."""

from __future__ import annotations

import csv
import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tools" / "analyze_indexed_probe_classes.py"


def load_module():
    spec = importlib.util.spec_from_file_location("analyze_indexed_probe_classes", SCRIPT)
    assert spec is not None
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def write_probe_csv(path: Path) -> None:
    fieldnames = [
        "seq",
        "encoder",
        "eligible",
        "applied",
        "optimized_eligible",
        "optimized_applied",
        "reorder_bytes",
        "original_index_available",
        "original_index_unique",
        "original_cache_miss16",
        "original_cache_miss32",
        "original_cache_miss64",
        "effective_cache_miss16",
        "effective_cache_miss32",
        "effective_cache_miss64",
        "primitive_count",
        "vertex_count",
        "texture_mask",
        "color_write",
        "alpha_blend",
        "src_blend",
        "dst_blend",
        "blend_op",
        "separate_alpha",
        "alpha_test",
        "depth_enabled",
        "depth_write",
        "depth_func",
        "stencil",
        "clip_plane",
        "scissor",
        "cull",
        "fill",
        "index_buffer",
        "effective_index_source",
        "stream0_handle",
        "pso",
        "vs",
        "ps",
    ]
    rows = [
        {
            "seq": "50",
            "encoder": "2",
            "eligible": "1",
            "applied": "1",
            "optimized_eligible": "0",
            "optimized_applied": "0",
            "reorder_bytes": "600",
            "original_index_available": "1",
            "original_index_unique": "90",
            "original_cache_miss16": "120",
            "original_cache_miss32": "100",
            "original_cache_miss64": "95",
            "effective_cache_miss16": "90",
            "effective_cache_miss32": "75",
            "effective_cache_miss64": "80",
            "primitive_count": "5000",
            "vertex_count": "15000",
            "texture_mask": "0x1",
            "color_write": "0xf",
            "alpha_blend": "1",
            "src_blend": "10",
            "dst_blend": "2",
            "blend_op": "1",
            "separate_alpha": "0",
            "alpha_test": "0",
            "depth_enabled": "1",
            "depth_write": "0",
            "depth_func": "4",
            "stencil": "0",
            "clip_plane": "0",
            "scissor": "1",
            "cull": "2",
            "fill": "0",
            "index_buffer": "ib-a",
            "effective_index_source": "cached-reordered-created",
            "stream0_handle": "vb-a",
            "pso": "pso-a",
            "vs": "vs-a",
            "ps": "ps-a",
        },
        {
            "seq": "50",
            "encoder": "2",
            "eligible": "1",
            "applied": "1",
            "optimized_eligible": "0",
            "optimized_applied": "0",
            "reorder_bytes": "300",
            "original_index_available": "1",
            "original_index_unique": "60",
            "original_cache_miss16": "80",
            "original_cache_miss32": "70",
            "original_cache_miss64": "65",
            "effective_cache_miss16": "65",
            "effective_cache_miss32": "60",
            "effective_cache_miss64": "62",
            "primitive_count": "1000",
            "vertex_count": "3000",
            "texture_mask": "0x1",
            "color_write": "0xf",
            "alpha_blend": "1",
            "src_blend": "10",
            "dst_blend": "2",
            "blend_op": "1",
            "separate_alpha": "0",
            "alpha_test": "0",
            "depth_enabled": "1",
            "depth_write": "0",
            "depth_func": "4",
            "stencil": "0",
            "clip_plane": "0",
            "scissor": "0",
            "cull": "2",
            "fill": "0",
            "index_buffer": "ib-b",
            "effective_index_source": "cached-reordered-hit",
            "stream0_handle": "vb-b",
            "pso": "pso-b",
            "vs": "vs-a",
            "ps": "ps-a",
        },
        {
            "seq": "50",
            "encoder": "1",
            "eligible": "0",
            "applied": "0",
            "optimized_eligible": "0",
            "optimized_applied": "0",
            "reorder_bytes": "0",
            "original_index_available": "1",
            "original_index_unique": "40",
            "original_cache_miss16": "50",
            "original_cache_miss32": "40",
            "original_cache_miss64": "35",
            "effective_cache_miss16": "50",
            "effective_cache_miss32": "40",
            "effective_cache_miss64": "35",
            "primitive_count": "256",
            "vertex_count": "768",
            "texture_mask": "0x0",
            "color_write": "0xf",
            "alpha_blend": "0",
            "src_blend": "2",
            "dst_blend": "1",
            "blend_op": "1",
            "separate_alpha": "0",
            "alpha_test": "0",
            "depth_enabled": "1",
            "depth_write": "1",
            "depth_func": "4",
            "stencil": "0",
            "clip_plane": "0",
            "scissor": "0",
            "cull": "2",
            "fill": "0",
            "index_buffer": "ib-c",
            "effective_index_source": "original",
            "stream0_handle": "vb-c",
            "pso": "pso-c",
            "vs": "vs-c",
            "ps": "ps-c",
        },
    ]
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def write_joined_csv(path: Path) -> None:
    fieldnames = [
        "seq",
        "enc",
        "gpu_ms",
        "vs_invocations",
        "vs_buffer_write_mib",
        "dxmt_hidden_backend_write_mib",
    ]
    rows = [
        {
            "seq": "50",
            "enc": "2",
            "gpu_ms": "13.5",
            "vs_invocations": "1350",
            "vs_buffer_write_mib": "270",
            "dxmt_hidden_backend_write_mib": "216",
        },
        {
            "seq": "50",
            "enc": "1",
            "gpu_ms": "4.0",
            "vs_invocations": "400",
            "vs_buffer_write_mib": "80",
            "dxmt_hidden_backend_write_mib": "64",
        },
    ]
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


class AnalyzeIndexedProbeClassesTests(unittest.TestCase):
    def test_aggregates_row_class_and_labels_screen_blend(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory() as tmp:
            probe_csv = Path(tmp) / "probe.csv"
            write_probe_csv(probe_csv)
            rows = module.filter_rows(module.load_rows(probe_csv), {"50/2"})
            aggregates = module.aggregate_rows(rows, "row-class")

        self.assertEqual(len(aggregates), 2)
        keys = {agg.key for agg in aggregates}
        self.assertIn(
            "50/2|depth=read|blend=screen|scissor=on|textured=yes|large4096=yes|color_write=0xf",
            keys,
        )
        self.assertIn(
            "50/2|depth=read|blend=screen|scissor=off|textured=yes|large4096=no|color_write=0xf",
            keys,
        )

        large = next(agg for agg in aggregates if "large4096=yes" in agg.key)
        self.assertEqual(large.applied, 1)
        self.assertEqual(large.original_miss32, 100)
        self.assertEqual(large.effective_miss32, 75)
        self.assertEqual(large.miss32_delta, -25)
        self.assertAlmostEqual(large.miss32_delta_pct, -25.0)
        self.assertEqual(large.effective_index_sources["cached-reordered-created"], 1)

    def test_row_state_class_preserves_visibility_state(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory() as tmp:
            probe_csv = Path(tmp) / "probe.csv"
            write_probe_csv(probe_csv)
            rows = module.filter_rows(module.load_rows(probe_csv), {"50/2"})
            aggregates = module.aggregate_rows(rows, "row-state-class")

        keys = {agg.key for agg in aggregates}
        self.assertIn(
            "50/2|depth=read|blend=screen|scissor=on|textured=yes|"
            "large4096=yes|color_write=0xf|alpha_test=off|"
            "depth_func=less-equal|stencil=off|clip=off|cull=back|fill=fill",
            keys,
        )

    def test_writes_csv_and_markdown_report(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            probe_csv = tmp_path / "probe.csv"
            output = tmp_path / "report.md"
            csv_output = tmp_path / "summary.csv"
            write_probe_csv(probe_csv)

            rc = module.main([
                str(probe_csv),
                "--row",
                "50/2",
                "--group",
                "row-class",
                "--output",
                str(output),
                "--csv-output",
                str(csv_output),
            ])

            self.assertEqual(rc, 0)
            report = output.read_text(encoding="utf-8")
            self.assertIn("# Indexed Probe Class Breakdown", report)
            self.assertIn("50/2|depth=read|blend=screen|scissor=on", report)
            self.assertIn("cached-reordered-created:1", report)

            with csv_output.open(newline="", encoding="utf-8") as f:
                rows = list(csv.DictReader(f))
            self.assertEqual(len(rows), 2)
            self.assertEqual(rows[0]["miss32_delta"], "-25")
            self.assertEqual(
                rows[0]["applied_effective_index_sources"],
                "cached-reordered-created:1",
            )

    def test_xcode_proxy_allocates_row_metrics_by_effective_miss32(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            probe_csv = tmp_path / "probe.csv"
            joined_csv = tmp_path / "joined.csv"
            output = tmp_path / "report.md"
            csv_output = tmp_path / "summary.csv"
            write_probe_csv(probe_csv)
            write_joined_csv(joined_csv)

            rc = module.main([
                str(probe_csv),
                "--row",
                "50/2",
                "--group",
                "row-class",
                "--joined-summary",
                str(joined_csv),
                "--output",
                str(output),
                "--csv-output",
                str(csv_output),
            ])

            self.assertEqual(rc, 0)
            with csv_output.open(newline="", encoding="utf-8") as f:
                rows = {row["group"]: row for row in csv.DictReader(f)}
            report = output.read_text(encoding="utf-8")

        large_key = (
            "50/2|depth=read|blend=screen|scissor=on|textured=yes|"
            "large4096=yes|color_write=0xf"
        )
        small_key = (
            "50/2|depth=read|blend=screen|scissor=off|textured=yes|"
            "large4096=no|color_write=0xf"
        )
        self.assertAlmostEqual(float(rows[large_key]["xcode_proxy_vs_write_mib"]), 150.0)
        self.assertAlmostEqual(float(rows[small_key]["xcode_proxy_vs_write_mib"]), 120.0)
        self.assertAlmostEqual(float(rows[large_key]["xcode_proxy_hidden_backend_mib"]), 120.0)
        self.assertAlmostEqual(float(rows[small_key]["xcode_proxy_hidden_backend_mib"]), 96.0)
        self.assertEqual(rows[large_key]["semantic_risk"], "screen-blend-tolerance")
        self.assertIn("explicit lsb1", rows[large_key]["candidate_action"])
        self.assertIn("## Xcode Proxy", report)
        self.assertIn("proxy hidden backend MiB", report)
        self.assertIn("## Candidate Advice", report)
        self.assertIn("screen-blend-tolerance", report)


if __name__ == "__main__":
    unittest.main()
