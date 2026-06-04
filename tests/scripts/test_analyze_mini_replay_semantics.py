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
    payloads = {"a": b"\x00\x01", "b": b"\x02\x03", "c": b"\x04\x05"}
    streams = {"a": b"stream-a", "b": b"stream-b", "c": b"stream-c"}
    index_counts = {"a": 6, "b": 12, "c": 24}
    primitive_counts = {"a": 2, "b": 4, "c": 8}
    unique_indices = {"a": 4, "b": 6, "c": 8}
    cache_miss_64 = {"a": 3, "b": 5, "c": 7}
    max_indices = {"a": 5, "b": 7, "c": 9}
    payload = payloads[group]
    stream = streams[group]
    return {
        "row": "50/2",
        "seq": 50,
        "encoder": 2,
        "encoder_draw_index": 14 + slot,
        "draw_ordinal": 25578 + slot,
        "state": {
            "primitive_type": 3,
            "index_count": index_counts[group],
            "primitive_count": primitive_counts[group],
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
            "unique_indices": unique_indices[group],
            "cache_miss_64": cache_miss_64[group],
            "index_bytes": len(payload),
            "stream0_bytes": len(stream),
            "min_index": 0,
            "max_index": max_indices[group],
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
            manifest_draw(root, 3, "c"),
            manifest_draw(root, 4, "a"),
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
            {
                "draw_index": "3",
                "compare_returncode": "0",
                "changed_pixels": "0",
                "changed_pct": "0",
                "before_active_pct": "62.5",
                "after_active_pct": "62.5",
                "max_delta": "0",
                "ssim": "1",
                "original_lru32": "20",
                "cacheopt_lru32": "12",
                "lru32_delta": "-8",
                "lru32_delta_pct": "-40",
            },
            {
                "draw_index": "4",
                "compare_returncode": "0",
                "changed_pixels": "0",
                "changed_pct": "0",
                "before_active_pct": "62.5",
                "after_active_pct": "62.5",
                "max_delta": "0",
                "ssim": "1",
                "original_lru32": "10",
                "cacheopt_lru32": "7",
                "lru32_delta": "-3",
                "lru32_delta_pct": "-30",
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

    before = np.zeros((8, 8, 3), dtype=np.uint8)
    after = before.copy()
    before[:5, :, :] = 20
    after[:5, :, :] = 20
    save_rgb(root / "draw003-original" / "original.ppm", before)
    save_rgb(root / "draw003-cacheopt" / "cacheopt.ppm", after)
    save_rgb(root / "draw004-original" / "original.ppm", before)
    save_rgb(root / "draw004-cacheopt" / "cacheopt.ppm", after)

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


def write_runtime_probe_csv(root: Path) -> Path:
    path = root / "runtime-probe.csv"
    fields = [
        "seq",
        "encoder",
        "encoder_draw_index",
        "draw_ordinal",
        "vs_constants_hash",
        "ps_constants_hash",
        "uniform_payload_hash",
    ]
    rows = []
    for slot, group in enumerate(["a", "a", "b", "c", "a"]):
        rows.append({
            "seq": "50",
            "encoder": "2",
            "encoder_draw_index": str(14 + slot),
            "draw_ordinal": str(99000 + slot),
            "vs_constants_hash": {
                "a": "0xvs-runtime-a",
                "b": "0xvs-runtime-b",
                "c": "0xvs-runtime-c",
            }[group],
            "ps_constants_hash": "0xps-runtime",
            "uniform_payload_hash": f"0xuniform-{slot}",
        })
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)
    return path


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
        self.assertEqual(by_draw[3].visibility_class, "visible-exact-pass")
        self.assertEqual(by_draw[4].visibility_class, "visible-exact-pass")
        self.assertEqual(by_draw[0].before_stats.active_pct, 6.25)

    def test_cli_writes_report_and_joined_csv(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest, summary, single_dir = write_fixture(root)
            runtime_probe = write_runtime_probe_csv(root)
            report = root / "report.md"
            joined = root / "joined.csv"
            selectors = root / "selectors.csv"
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
                    "--runtime-probe-csv",
                    str(runtime_probe),
                    "--output",
                    str(report),
                    "--csv-output",
                    str(joined),
                    "--selector-csv-output",
                    str(selectors),
                    "--selector-max-fields",
                    "3",
                    "--selector-limit",
                    "20",
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
            self.assertIn("Final-Writer Oracle", text)
            self.assertIn("final-writer-color-hazard", text)
            self.assertIn("owner-change-color-stable", text)
            self.assertIn("Selector Scout", text)
            self.assertIn("No-final-color / sparse visibility", text)
            self.assertIn("Primitive-owner change", text)
            self.assertIn("Selector Candidate Sweep", text)
            self.assertIn("all exact-pass draws", text)
            self.assertIn("trace-local-upper-bound", text)
            self.assertIn("encoder draw index excluding failures", text)
            self.assertIn("Runtime Field Combination Sweep", text)
            self.assertIn("Final-Color Runtime Selector Sweep", text)
            self.assertIn("Final-Writer Runtime Selector Sweep", text)
            self.assertIn("Final-Color Runtime Blockers", text)
            self.assertIn("trace-local", text)
            self.assertIn("runtime.uniform_payload_hash", text)
            self.assertIn("state.index_count", text)
            self.assertIn("production reorder gate", text)
            with joined.open(newline="", encoding="utf-8") as handle:
                rows = list(csv.DictReader(handle))
            self.assertEqual(rows[1]["semantic_status"], "fail")
            self.assertEqual(rows[1]["broad_group_status"], "mixed")
            self.assertEqual(rows[1]["changed_bbox"], "1,1-1,1")
            self.assertEqual(rows[0]["visibility_class"], "sparse-exact-pass")
            self.assertEqual(rows[1]["visibility_class"], "sparse-fail")
            self.assertEqual(rows[2]["visibility_class"], "no-final-color-exact-pass")
            self.assertEqual(rows[3]["visibility_class"], "visible-exact-pass")
            self.assertEqual(rows[4]["visibility_class"], "visible-exact-pass")
            self.assertEqual(rows[0]["active_pct_before"], "6.250000")
            self.assertEqual(rows[0]["primitive_owner_risk"], "primitive-owner-changed-color-stable")
            self.assertEqual(rows[0]["final_writer_oracle_status"], "owner-change-color-stable")
            self.assertEqual(rows[1]["color_change_primitive_overlap_pct"], "100.00")
            self.assertEqual(rows[1]["final_writer_oracle_status"], "final-writer-color-hazard")
            self.assertIn("reject reorder", rows[1]["final_writer_oracle_action"])
            self.assertEqual(rows[0]["state_index_count"], "6")
            self.assertEqual(rows[0]["state_texture_mask"], "0x3f")
            self.assertEqual(rows[0]["geometry_unique_indices"], "4")
            self.assertEqual(rows[0]["geometry_index_bytes"], "2")
            self.assertEqual(rows[0]["shader_vs_hash"], "0xvs")
            self.assertEqual(rows[0]["shader_ps_vsout_read_fields"], "position,texcoord0")
            self.assertEqual(rows[0]["runtime_vs_constants_hash"], "0xvs-runtime-a")
            self.assertEqual(rows[0]["runtime_ps_constants_hash"], "0xps-runtime")
            self.assertEqual(rows[0]["runtime_uniform_payload_hash"], "0xuniform-0")
            with selectors.open(newline="", encoding="utf-8") as handle:
                selector_rows = list(csv.DictReader(handle))
            queues = {row["queue"] for row in selector_rows}
            self.assertIn("selector-candidate", queues)
            self.assertIn("runtime-field-combination", queues)
            self.assertIn("final-color-runtime-selector", queues)
            self.assertIn("final-writer-runtime-selector", queues)
            self.assertIn("final-color-runtime-blocker", queues)
            self.assertTrue(any(
                row["selector"] == "all exact-pass draws"
                and row["verdict"] == "trace-local-upper-bound"
                for row in selector_rows
            ))
            self.assertTrue(any(
                row["selector"] == "state.index_count"
                and row["verdict"] == "runtime-scout"
                for row in selector_rows
            ))
            self.assertTrue(any(
                row["queue"] == "final-color-runtime-selector"
                and row["selector"] == "state.index_count"
                and row["verdict"] == "runtime-scout"
                and row["gain_share"] == "72.73%"
                for row in selector_rows
            ))
            self.assertTrue(any(
                row["queue"] == "final-color-runtime-blocker"
                and row["selector"] == "all-runtime-visible-fields"
                and row["verdict"] == "runtime-indistinguishable-target-fail"
                and row["kept_draws"] == "4"
                and row["kept_fail"] == "1"
                and "runtime.uniform_payload_hash" in row["meaning"]
                for row in selector_rows
            ))
            self.assertTrue(any(
                row["queue"] == "final-writer-runtime-selector"
                and row["selector"] == "runtime.uniform_payload_hash"
                and row["verdict"] == "runtime-payload-overfit"
                for row in selector_rows
            ))


if __name__ == "__main__":
    unittest.main()
