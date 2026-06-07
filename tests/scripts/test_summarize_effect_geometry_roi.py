#!/usr/bin/env python3
"""Regression tests for effect-geometry ROI summaries."""

from __future__ import annotations

import csv
import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tools" / "summarize_effect_geometry_roi.py"


def load_module():
    spec = importlib.util.spec_from_file_location("summarize_effect_geometry_roi", SCRIPT)
    assert spec is not None
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class SummarizeEffectGeometryRoiTests(unittest.TestCase):
    def test_projected_screen_bbox_overlaps_roi(self) -> None:
        module = load_module()
        fields = module.parse_geometry_fields(
            "[dxmt9-effect-geometry seq=1 encoder=2 encoder_draw_index=3 "
            "primitive_count=4 texture0=0x7f projected_refs=6 "
            "screen_min=(10,20) screen_max=(30,60)]"
        )

        bbox = module.bbox_from_fields(fields)

        self.assertIsNotNone(bbox)
        self.assertEqual(bbox.source, "projected-screen")
        self.assertEqual(bbox.left, 10)
        self.assertEqual(module.intersect_area(bbox, module.parse_roi("20,30,40,50:r")), 200)

    def test_screen_space_pos_bbox_fallback(self) -> None:
        module = load_module()
        fields = module.parse_geometry_fields(
            "[dxmt9-effect-geometry seq=1 encoder=2 encoder_draw_index=3 "
            "primitive_count=2 texture0=0x80 projected_refs=0 pretransformed=0 "
            "pos_min=(-0.5,-0.5,0,1) pos_max=(1023.5,767.5,0,1) "
            "viewport_size=(1024,768)]"
        )

        bbox = module.bbox_from_fields(fields)

        self.assertIsNotNone(bbox)
        self.assertEqual(bbox.source, "screen-space-pos")
        self.assertAlmostEqual(
            module.intersect_area(bbox, module.parse_roi("700,230,800,330:muzzle")),
            10000,
        )

    def test_min_bbox_coverage_filters_broad_bbox_overlap(self) -> None:
        module = load_module()
        broad_fields = module.parse_geometry_fields(
            "[dxmt9-effect-geometry seq=1 encoder=2 encoder_draw_index=3 "
            "primitive_count=8 texture0=0x7f projected_refs=6 "
            "screen_min=(0,0) screen_max=(1000,1000)]"
        )
        tight_fields = module.parse_geometry_fields(
            "[dxmt9-effect-geometry seq=1 encoder=2 encoder_draw_index=4 "
            "primitive_count=8 texture0=0x80 projected_refs=6 "
            "screen_min=(10,20) screen_max=(30,60)]"
        )
        rows = [
            module.GeometryRow(Path("effect.log"), 1, broad_fields, module.bbox_from_fields(broad_fields)),
            module.GeometryRow(Path("effect.log"), 2, tight_fields, module.bbox_from_fields(tight_fields)),
        ]

        overlaps = module.build_overlaps(
            rows,
            [module.parse_roi("10,20,30,60:component")],
            include_zero_overlap=False,
            min_bbox_coverage_pct=1.0,
        )

        self.assertEqual(len(overlaps), 1)
        self.assertEqual(overlaps[0].geometry.fields["texture0"], "0x80")

    def test_cli_writes_overlap_csv_and_markdown(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            log = root / "effect.log"
            report = root / "report.md"
            csv_output = root / "overlap.csv"
            log.write_text(
                "\n".join(
                    [
                        "[dxmt9-effect-geometry seq=10 encoder=4 encoder_draw_index=1 "
                        "ordinal=100 command_index=7 command_draw_index=0 command_draw_count=2 "
                        "primitive_count=4 texture0=0x7f complete=1 "
                        "src_blend=10 dst_blend=2 depth_enabled=1 depth_write=0 "
                        "vs_hash=0xaaa ps_hash=0xbbb projected_refs=6 "
                        "screen_min=(700,230) screen_max=(760,290)]",
                        "[dxmt9-effect-geometry seq=11 encoder=4 encoder_draw_index=1 "
                        "ordinal=101 command_index=8 command_draw_index=1 command_draw_count=2 "
                        "primitive_count=2 texture0=0x80 complete=1 "
                        "src_blend=5 dst_blend=2 depth_enabled=1 depth_write=0 "
                        "vs_hash=0xccc ps_hash=0xddd projected_refs=0 "
                        "pos_min=(-0.5,-0.5,0,1) pos_max=(1023.5,767.5,0,1) "
                        "viewport_size=(1024,768)]",
                    ]
                ),
                encoding="utf-8",
            )

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(log),
                    "--roi",
                    "700,230,800,330:muzzle",
                    "--output",
                    str(report),
                    "--csv-output",
                    str(csv_output),
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("Effect Geometry ROI Summary", report.read_text(encoding="utf-8"))
            with csv_output.open(newline="", encoding="utf-8") as handle:
                rows = list(csv.DictReader(handle))
            self.assertEqual(len(rows), 2)
            self.assertEqual(rows[0]["texture0"], "0x80")
            self.assertEqual(rows[0]["command_index"], "8")
            self.assertEqual(rows[0]["command_draw_index"], "1")
            self.assertEqual(rows[0]["command_draw_count"], "2")
            self.assertEqual(rows[0]["bbox_source"], "screen-space-pos")
            self.assertEqual(rows[0]["roi_coverage_pct"], "100.000")
            self.assertEqual(rows[1]["texture0"], "0x7f")
            self.assertEqual(rows[1]["command_index"], "7")
            self.assertEqual(rows[1]["command_draw_index"], "0")
            self.assertEqual(rows[1]["command_draw_count"], "2")
            self.assertEqual(rows[1]["intersection_area"], "3600.000")

    def test_component_roi_csv_can_match_geometry_seq(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            log = root / "effect.log"
            components = root / "components.csv"
            csv_output = root / "overlap.csv"
            log.write_text(
                "\n".join(
                    [
                        "[dxmt9-effect-geometry seq=10 encoder=4 encoder_draw_index=1 "
                        "primitive_count=4 texture0=0x7f projected_refs=6 "
                        "screen_min=(10,20) screen_max=(30,60)]",
                        "[dxmt9-effect-geometry seq=11 encoder=4 encoder_draw_index=1 "
                        "primitive_count=4 texture0=0x80 projected_refs=6 "
                        "screen_min=(10,20) screen_max=(30,60)]",
                    ]
                ),
                encoding="utf-8",
            )
            components.write_text(
                "\n".join(
                    [
                        "file,image,frame,component_id,search_roi,bbox,bbox_width,bbox_height,area,"
                        "warm_pixels,white_pixels,bright_pixels,max_r,max_g,max_b,hot_x,hot_y,"
                        "hot_r,hot_g,hot_b,warm_hot_x,warm_hot_y,warm_hot_r,warm_hot_g,warm_hot_b",
                        "capture.png,capture.png,10,2,scene,\"10,20,30,60\",20,40,800,"
                        "40,20,50,255,220,180,12,22,255,220,180,12,22,255,220,180",
                    ]
                ),
                encoding="utf-8",
            )

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(log),
                    "--component-roi-csv",
                    str(components),
                    "--component-roi-match-seq",
                    "--csv-output",
                    str(csv_output),
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            with csv_output.open(newline="", encoding="utf-8") as handle:
                rows = list(csv.DictReader(handle))
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0]["roi"], "frame10-component2")
            self.assertEqual(rows[0]["roi_seq"], "10")
            self.assertEqual(rows[0]["seq"], "10")
            self.assertEqual(rows[0]["texture0"], "0x7f")


if __name__ == "__main__":
    unittest.main()
