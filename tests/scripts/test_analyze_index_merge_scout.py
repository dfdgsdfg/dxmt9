#!/usr/bin/env python3
"""Regression tests for indexed draw merge scout analysis."""

from __future__ import annotations

import csv
import importlib.util
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tools" / "analyze_index_merge_scout.py"


def load_module():
    spec = importlib.util.spec_from_file_location("analyze_index_merge_scout", SCRIPT)
    assert spec is not None
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


FIELDS = [
    "seq",
    "encoder",
    "encoder_draw_index",
    "draw_ordinal",
    "primitive_type",
    "texture_mask",
    "color_write",
    "alpha_blend",
    "src_blend",
    "dst_blend",
    "blend_op",
    "separate_alpha",
    "src_blend_alpha",
    "dst_blend_alpha",
    "blend_op_alpha",
    "alpha_test",
    "depth_enabled",
    "depth_write",
    "depth_func",
    "stencil",
    "clip_plane",
    "scissor",
    "scissor_l",
    "scissor_t",
    "scissor_r",
    "scissor_b",
    "cull",
    "fill",
    "index_type",
    "index_buffer",
    "stream0_handle",
    "stream0_offset",
    "stream0_stride",
    "base_vertex",
    "pso",
    "shader_variant",
    "vs",
    "ps",
    "vsout",
]


def base_row(slot: int, **overrides: str) -> dict[str, str]:
    row = {
        "seq": "10",
        "encoder": "1",
        "encoder_draw_index": str(slot),
        "draw_ordinal": str(100 + slot),
        "primitive_type": "3",
        "texture_mask": "0x7f",
        "color_write": "0xf",
        "alpha_blend": "1",
        "src_blend": "10",
        "dst_blend": "2",
        "blend_op": "1",
        "separate_alpha": "0",
        "src_blend_alpha": "2",
        "dst_blend_alpha": "1",
        "blend_op_alpha": "1",
        "alpha_test": "0",
        "depth_enabled": "1",
        "depth_write": "0",
        "depth_func": "4",
        "stencil": "0",
        "clip_plane": "0",
        "scissor": "0",
        "scissor_l": "0",
        "scissor_t": "0",
        "scissor_r": "1024",
        "scissor_b": "768",
        "cull": "2",
        "fill": "0",
        "index_type": "0",
        "index_buffer": "ib-a",
        "stream0_handle": "vb-a",
        "stream0_offset": "0",
        "stream0_stride": "24",
        "base_vertex": "0",
        "pso": "pso-a",
        "shader_variant": "sv-a",
        "vs": "vs-a",
        "ps": "ps-a",
        "vsout": "0xfff",
    }
    row.update(overrides)
    return row


def write_fixture(root: Path) -> tuple[Path, Path]:
    probe_csv = root / "probe.csv"
    geometry = root / "geometry"
    geometry.mkdir()
    rows = [
        base_row(0),
        base_row(1),
        base_row(2, index_buffer="ib-b", stream0_handle="vb-b"),
        base_row(3, pso="pso-b", shader_variant="sv-b"),
    ]
    with probe_csv.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerows(rows)

    index_payloads = {
        0: [0, 1, 2, 0, 2, 3],
        1: [3, 2, 4],
        2: [4, 5, 6],
        3: [6, 7, 8],
    }
    for slot, indices in index_payloads.items():
        payload = struct.pack("<" + "H" * len(indices), *indices)
        (geometry / f"seq10-enc1-draw{100 + slot}-slot{slot}.index.bin").write_bytes(payload)
    return probe_csv, geometry


class AnalyzeIndexMergeScoutTests(unittest.TestCase):
    def test_adjacent_modes_compute_candidate_lru_savings(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory() as tmp:
            probe_csv, geometry = write_fixture(Path(tmp))
            rows = module.load_probe_rows(probe_csv, ("10", "1"))
            draws = module.load_draws(rows, geometry)

        full = module.analyze_mode(draws, "full_row", 32)
        same_binding = module.analyze_mode(draws, "same_binding", 32)
        same_pipeline = module.analyze_mode(draws, "same_pipeline", 32)

        self.assertEqual((full.miss_before, full.miss_after, full.delta), (13, 9, -4))
        self.assertEqual(same_binding.group_count, 3)
        self.assertEqual(same_binding.multi_group_count, 1)
        self.assertEqual(same_binding.draws_in_multi_groups, 2)
        self.assertEqual((same_binding.miss_before, same_binding.miss_after), (7, 5))
        self.assertEqual(same_pipeline.group_count, 2)
        self.assertEqual(same_pipeline.draws_in_multi_groups, 3)
        self.assertEqual((same_pipeline.miss_before, same_pipeline.miss_after), (10, 7))

    def test_cli_writes_markdown_and_summary_csv(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            probe_csv, geometry = write_fixture(root)
            report = root / "report.md"
            summary = root / "summary.csv"
            subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--probe-draws",
                    str(probe_csv),
                    "--geometry-dir",
                    str(geometry),
                    "--row",
                    "10/1",
                    "--output",
                    str(report),
                    "--summary-output",
                    str(summary),
                ],
                check=True,
            )

            text = report.read_text(encoding="utf-8")
            self.assertIn("# Indexed Draw Merge Scout", text)
            self.assertIn("`same_binding`", text)
            self.assertIn("reject draw-boundary merge", text)

            with summary.open(newline="", encoding="utf-8") as handle:
                rows = list(csv.DictReader(handle))
            by_mode = {row["mode"]: row for row in rows}
            self.assertEqual(by_mode["same_binding"]["miss_before"], "7")
            self.assertEqual(by_mode["same_binding"]["miss_after"], "5")
            self.assertEqual(by_mode["same_pipeline"]["draw_saved"], "2")


if __name__ == "__main__":
    unittest.main()
