#!/usr/bin/env python3
"""Regression tests for mini-replay semantic bisection analysis."""

from __future__ import annotations

import csv
import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np
from PIL import Image


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tools" / "analyze_mini_replay_semantics.py"


def load_module():
    spec = importlib.util.spec_from_file_location("analyze_mini_replay_semantics", SCRIPT)
    assert spec is not None
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def save_rgb(path: Path, data: np.ndarray) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(data.astype(np.uint8), mode="RGB").save(path)


def write_payload(path: Path, value: bytes) -> str:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(value)
    return str(path)


def manifest_draw(root: Path, slot: int, group: str = "a") -> dict:
    payload = b"\x00\x01" if group == "a" else b"\x02\x03"
    stream = b"stream-a" if group == "a" else b"stream-b"
    return {
        "row": "50/2",
        "seq": 50,
        "encoder": 2,
        "encoder_draw_index": 14 + slot,
        "draw_ordinal": 25578 + slot,
        "state": {
            "primitive_type": 3,
            "index_count": 6,
            "primitive_count": 2,
            "index_type": "uint16",
            "base_vertex": 0,
            "stream0_offset": 0,
            "stream0_stride": 24,
            "texture_mask": "0x3f",
            "color_write": "0xf",
            "alpha_blend": 0,
            "src_blend": 10,
            "dst_blend": 2,
            "blend_op": 1,
            "separate_alpha": 0,
            "src_blend_alpha": 2,
            "dst_blend_alpha": 1,
            "blend_op_alpha": 1,
            "alpha_test": 0,
            "depth_enabled": 1,
            "depth_write": 0,
            "depth_func": 4,
            "scissor": 0,
            "scissor_l": 0,
            "scissor_t": 0,
            "scissor_r": 4,
            "scissor_b": 4,
            "cull": 2,
            "fill": 0,
        },
        "geometry": {
            "index_file": write_payload(root / f"draw{slot}.index.bin", payload),
            "stream0_file": write_payload(root / f"draw{slot}.stream0.bin", stream),
            "streams": [
                {"stream": 0, "file": str(root / f"draw{slot}.stream0.bin")},
                {"stream": 1, "file": write_payload(root / f"draw{slot}.stream1.bin", b"extra")},
            ],
        },
        "uniforms": {
            "vsconsts_file": write_payload(root / f"draw{slot}.vsconsts.bin", f"vs-{slot}".encode()),
            "psconsts_file": write_payload(root / f"draw{slot}.psconsts.bin", b"ps"),
            "ffpvs_file": write_payload(root / f"draw{slot}.ffpvs.bin", b"ffpvs"),
            "ffpps_file": write_payload(root / f"draw{slot}.ffpps.bin", b"ffpps"),
        },
        "shaders": {
            "vs_hash": "0xvs",
            "ps_hash": "0xps",
            "vsout": "0xfff",
            "ps_vsout_read_fields": "position,texcoord0",
        },
    }


def write_fixture(root: Path) -> tuple[Path, Path, Path]:
    manifest = {
        "schema": "dxmt9.3dmark05.mini_replay_manifest.v1",
        "draws": [
            manifest_draw(root, 0, "a"),
            manifest_draw(root, 1, "a"),
            manifest_draw(root, 2, "b"),
        ],
    }
    manifest_path = root / "manifest.json"
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

    summary_path = root / "single-draw-summary.csv"
    with summary_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=[
            "draw_index",
            "compare_returncode",
            "changed_pixels",
            "changed_pct",
            "before_active_pct",
            "after_active_pct",
            "max_delta",
            "ssim",
            "original_lru32",
            "cacheopt_lru32",
            "lru32_delta",
            "lru32_delta_pct",
        ])
        writer.writeheader()
        writer.writerows([
            {
                "draw_index": "0",
                "compare_returncode": "0",
                "changed_pixels": "0",
                "changed_pct": "0",
                "before_active_pct": "6.25",
                "after_active_pct": "6.25",
                "max_delta": "0",
                "ssim": "1",
                "original_lru32": "10",
                "cacheopt_lru32": "8",
                "lru32_delta": "-2",
                "lru32_delta_pct": "-20",
            },
            {
                "draw_index": "1",
                "compare_returncode": "1",
                "changed_pixels": "1",
                "changed_pct": "6.25",
                "before_active_pct": "6.25",
                "after_active_pct": "6.25",
                "max_delta": "9",
                "ssim": "0.9",
                "original_lru32": "10",
                "cacheopt_lru32": "8",
                "lru32_delta": "-2",
                "lru32_delta_pct": "-20",
            },
            {
                "draw_index": "2",
                "compare_returncode": "0",
                "changed_pixels": "0",
                "changed_pct": "0",
                "before_active_pct": "0",
                "after_active_pct": "0",
                "max_delta": "0",
                "ssim": "1",
                "original_lru32": "6",
                "cacheopt_lru32": "5",
                "lru32_delta": "-1",
                "lru32_delta_pct": "-16.7",
            },
        ])

    for draw_index in range(3):
        before = np.zeros((4, 4, 3), dtype=np.uint8)
        after = before.copy()
        if draw_index in (0, 1):
            before[1, 1, :] = 20
            after[1, 1, :] = 20
        if draw_index == 1:
            after[1, 1, 0] = 29
        save_rgb(root / f"draw{draw_index:03d}-original" / "original.ppm", before)
        save_rgb(root / f"draw{draw_index:03d}-cacheopt" / "cacheopt.ppm", after)

    fields = [
        "primitive_identity_changed_pixels",
        "primitive_identity_changed_bbox",
        "color_changed_pixels",
        "color_and_primitive_changed_pixels",
    ]
    for draw_index, values in {
        0: {
            "primitive_identity_changed_pixels": "2",
            "primitive_identity_changed_bbox": "1,1-2,1",
            "color_changed_pixels": "0",
            "color_and_primitive_changed_pixels": "0",
        },
        1: {
            "primitive_identity_changed_pixels": "1",
            "primitive_identity_changed_bbox": "1,1-1,1",
            "color_changed_pixels": "1",
            "color_and_primitive_changed_pixels": "1",
        },
    }.items():
        with (root / f"draw{draw_index:03d}-primitive-id-summary.csv").open(
            "w", newline="", encoding="utf-8"
        ) as handle:
            writer = csv.DictWriter(handle, fieldnames=fields)
            writer.writeheader()
            writer.writerow(values)

    return manifest_path, summary_path, root


class AnalyzeMiniReplaySemanticsTests(unittest.TestCase):
    def test_analyze_marks_mixed_broad_group(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory() as tmp:
            manifest, summary, single_dir = write_fixture(Path(tmp))
            rows = module.analyze(manifest, summary, single_dir)

        by_draw = {row.draw_index: row for row in rows}
        self.assertEqual(by_draw[0].semantic_status, "pass")
        self.assertEqual(by_draw[1].semantic_status, "fail")
        self.assertEqual(by_draw[0].broad_group_id, by_draw[1].broad_group_id)
        self.assertEqual(by_draw[0].broad_group_status, "mixed")
        self.assertEqual(by_draw[1].after_stats.changed_bbox, "1,1-1,1")
        self.assertEqual(by_draw[0].primitive_owner_risk, "primitive-owner-changed-color-stable")
        self.assertEqual(by_draw[1].primitive_owner_risk, "color-change-follows-primitive-owner")
        self.assertNotEqual(by_draw[1].broad_group_id, by_draw[2].broad_group_id)
        self.assertEqual(by_draw[0].visibility_class, "sparse-exact-pass")
        self.assertEqual(by_draw[1].visibility_class, "sparse-fail")
        self.assertEqual(by_draw[2].visibility_class, "no-final-color-exact-pass")
        self.assertEqual(by_draw[0].before_stats.active_pct, 6.25)

    def test_cli_writes_report_and_joined_csv(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest, summary, single_dir = write_fixture(root)
            report = root / "report.md"
            joined = root / "joined.csv"
            subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--manifest",
                    str(manifest),
                    "--single-draw-summary",
                    str(summary),
                    "--single-draw-dir",
                    str(single_dir),
                    "--output",
                    str(report),
                    "--csv-output",
                    str(joined),
                ],
                check=True,
            )

            text = report.read_text(encoding="utf-8")
            self.assertIn("# Mini Replay Semantic Bisection Analysis", text)
            self.assertIn("Mixed broad groups", text)
            self.assertIn("Proof Verdict", text)
            self.assertIn("fail-visible-primitive-owner-conflict", text)
            self.assertIn("do-not-spend-production-gputrace", text)
            self.assertIn("Final-Color Visibility", text)
            self.assertIn("Primitive-Owner Predicate Check", text)
            self.assertIn("Selector Scout", text)
            self.assertIn("No-final-color / sparse visibility", text)
            self.assertIn("Primitive-owner change", text)
            self.assertIn("production reorder gate", text)
            with joined.open(newline="", encoding="utf-8") as handle:
                rows = list(csv.DictReader(handle))
            self.assertEqual(rows[1]["semantic_status"], "fail")
            self.assertEqual(rows[1]["broad_group_status"], "mixed")
            self.assertEqual(rows[1]["changed_bbox"], "1,1-1,1")
            self.assertEqual(rows[0]["visibility_class"], "sparse-exact-pass")
            self.assertEqual(rows[1]["visibility_class"], "sparse-fail")
            self.assertEqual(rows[2]["visibility_class"], "no-final-color-exact-pass")
            self.assertEqual(rows[0]["active_pct_before"], "6.250000")
            self.assertEqual(rows[0]["primitive_owner_risk"], "primitive-owner-changed-color-stable")
            self.assertEqual(rows[1]["color_change_primitive_overlap_pct"], "100.00")


if __name__ == "__main__":
    unittest.main()
