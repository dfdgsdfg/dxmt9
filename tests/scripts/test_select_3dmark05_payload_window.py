import csv
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tools" / "select_3dmark05_payload_window.py"


def write_csv(path: Path, rows: list[dict[str, object]]) -> None:
    keys: list[str] = []
    for row in rows:
        for key in row:
            if key not in keys:
                keys.append(key)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=keys)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def probe_row(
    draw: int,
    tris: int,
    cache64: int,
    vs: str,
    ps: str,
    alpha_blend: int = 1,
    depth_write: int = 0,
    scissor: int = 0,
    texture_mask: str = "0x7f",
    applied: int = 0,
    original_miss32: int | None = None,
    candidate_miss32: int | None = None,
    candidate_available: int = 1,
) -> dict[str, object]:
    if original_miss32 is None:
        original_miss32 = cache64
    if candidate_miss32 is None:
        candidate_miss32 = original_miss32
    return {
        "seq": 60,
        "encoder": 2,
        "encoder_draw_index": draw,
        "draw_ordinal": 42000 + draw,
        "primitive_count": tris,
        "applied": applied,
        "original_cache_miss32": original_miss32,
        "original_cache_miss64": cache64,
        "candidate_index_available": candidate_available,
        "candidate_cache_miss32": candidate_miss32,
        "vs": vs,
        "ps": ps,
        "alpha_blend": alpha_blend,
        "src_blend": 5,
        "dst_blend": 2,
        "blend_op": 1,
        "separate_alpha": 0,
        "src_blend_alpha": 0,
        "dst_blend_alpha": 0,
        "blend_op_alpha": 0,
        "alpha_test": 0,
        "depth_enabled": 1,
        "depth_write": depth_write,
        "depth_func": 4,
        "stencil": 0,
        "clip_plane": 0,
        "scissor": scissor,
        "cull": 2,
        "fill": 0,
        "texture_mask": texture_mask,
        "color_write": "0xf",
        "index_type": 0,
        "stream0_stride": 24,
    }


class SelectPayloadWindowTests(unittest.TestCase):
    def test_selects_top_same_run_shader_state_window(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            probe_csv = root / "probe.csv"
            output = root / "selection.json"
            rows = [
                probe_row(10, 100, 180, "0xaaa", "0x111"),
                probe_row(11, 120, 200, "0xaaa", "0x111"),
                probe_row(12, 90, 160, "0xaaa", "0x111"),
                probe_row(40, 1000, 1600, "0xbbb", "0x222"),
                probe_row(41, 1100, 1700, "0xbbb", "0x222"),
                probe_row(42, 1200, 1800, "0xbbb", "0x222"),
                probe_row(43, 1300, 1900, "0xbbb", "0x222"),
                probe_row(44, 1400, 2000, "0xbbb", "0x222"),
            ]
            write_csv(probe_csv, rows)

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--probe-draws",
                    str(probe_csv),
                    "--row",
                    "60/2",
                    "--max-draws",
                    "3",
                    "--output",
                    str(output),
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("encoder_draw_index 42..44", result.stdout)
            selection = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(selection["schema"], "dxmt9.3dmark05.payload_window.v1")
            selected = selection["selection"]
            self.assertEqual(selected["group"]["vs"], "0xbbb")
            self.assertEqual(selected["group"]["ps"], "0x222")
            self.assertEqual(selected["group"]["draws"], 5)
            self.assertEqual(selected["window"]["encoder_draw_min"], 42)
            self.assertEqual(selected["window"]["encoder_draw_max"], 44)
            self.assertEqual(selected["window"]["draw_ordinals"], [42042, 42043, 42044])
            self.assertEqual(
                selected["capture_flags"],
                [
                    "--dump-indexed-geometry",
                    "--dump-indexed-geometry-max-draws",
                    "3",
                    "--probe-reverse-indexed-triangles-row",
                    "60/2",
                    "--probe-indexed-triangle-encoder-draw-min",
                    "42",
                    "--probe-indexed-triangle-encoder-draw-max",
                    "44",
                ],
            )
            self.assertEqual(
                selected["shader_capture_flags"],
                [
                    "--dump-indexed-geometry",
                    "--dump-indexed-geometry-max-draws",
                    "3",
                    "--dump-indexed-geometry-vs",
                    "0xbbb",
                    "--dump-indexed-geometry-ps",
                    "0x222",
                ],
            )

    def test_rank_selects_second_group(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            probe_csv = root / "probe.csv"
            output = root / "selection.json"
            write_csv(probe_csv, [
                probe_row(1, 1000, 1000, "0xtop", "0xps"),
                probe_row(2, 1000, 1000, "0xtop", "0xps"),
                probe_row(20, 600, 800, "0xsecond", "0xps"),
                probe_row(21, 700, 900, "0xsecond", "0xps"),
            ])

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--probe-draws",
                    str(probe_csv),
                    "--row",
                    "60/2",
                    "--rank",
                    "2",
                    "--output",
                    str(output),
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            selection = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(selection["selection"]["group"]["vs"], "0xsecond")
            self.assertEqual(selection["selection"]["window"]["encoder_draw_min"], 20)
            self.assertEqual(selection["selection"]["window"]["encoder_draw_max"], 21)

    def test_rank_by_candidate_miss32_delta_selects_largest_reduction(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            probe_csv = root / "probe.csv"
            output = root / "selection.json"
            write_csv(probe_csv, [
                probe_row(
                    1,
                    5000,
                    5000,
                    "0xlarge",
                    "0xps",
                    original_miss32=5000,
                    candidate_miss32=4900,
                ),
                probe_row(
                    2,
                    5000,
                    5000,
                    "0xlarge",
                    "0xps",
                    original_miss32=5000,
                    candidate_miss32=4900,
                ),
                probe_row(
                    20,
                    1000,
                    1000,
                    "0xpayoff",
                    "0xps",
                    original_miss32=1000,
                    candidate_miss32=100,
                ),
                probe_row(
                    21,
                    1000,
                    1000,
                    "0xpayoff",
                    "0xps",
                    original_miss32=1000,
                    candidate_miss32=100,
                ),
            ])

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--probe-draws",
                    str(probe_csv),
                    "--row",
                    "60/2",
                    "--rank-by",
                    "candidate-miss32-delta",
                    "--output",
                    str(output),
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("candidate32_delta 1800", result.stdout)
            selection = json.loads(output.read_text(encoding="utf-8"))
            selected = selection["selection"]
            self.assertEqual(selected["group"]["vs"], "0xpayoff")
            self.assertEqual(selected["group"]["original_miss32"], 2000)
            self.assertEqual(selected["group"]["candidate_miss32"], 200)
            self.assertEqual(selected["group"]["candidate_miss32_delta"], 1800)
            self.assertEqual(selected["window"]["encoder_draw_min"], 20)
            self.assertEqual(selected["window"]["encoder_draw_max"], 21)

    def test_rank_scope_window_orders_by_best_window_score(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            probe_csv = root / "probe.csv"
            output = root / "selection-list.json"
            write_csv(probe_csv, [
                probe_row(
                    1,
                    1000,
                    1000,
                    "0xwide",
                    "0xps",
                    original_miss32=1000,
                    candidate_miss32=900,
                ),
                probe_row(
                    2,
                    1000,
                    1000,
                    "0xwide",
                    "0xps",
                    original_miss32=1000,
                    candidate_miss32=900,
                ),
                probe_row(
                    3,
                    1000,
                    1000,
                    "0xwide",
                    "0xps",
                    original_miss32=1000,
                    candidate_miss32=900,
                ),
                probe_row(
                    20,
                    700,
                    700,
                    "0xwindow",
                    "0xps",
                    original_miss32=700,
                    candidate_miss32=100,
                ),
                probe_row(
                    21,
                    700,
                    700,
                    "0xwindow",
                    "0xps",
                    original_miss32=700,
                    candidate_miss32=100,
                ),
            ])

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--probe-draws",
                    str(probe_csv),
                    "--row",
                    "60/2",
                    "--rank-by",
                    "candidate-miss32-delta",
                    "--rank-scope",
                    "window",
                    "--max-draws",
                    "2",
                    "--list-ranks",
                    "2",
                    "--output",
                    str(output),
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            selection = json.loads(output.read_text(encoding="utf-8"))
            first = selection["selections"][0]
            second = selection["selections"][1]
            self.assertEqual(first["rank_scope"], "window")
            self.assertEqual(first["group"]["vs"], "0xwindow")
            self.assertEqual(first["window"]["candidate_miss32_delta"], 1200)
            self.assertEqual(second["group"]["vs"], "0xwide")
            self.assertEqual(second["window"]["candidate_miss32_delta"], 200)

    def test_list_ranks_emits_multiple_payload_windows(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            probe_csv = root / "probe.csv"
            output = root / "selection-list.json"
            write_csv(probe_csv, [
                probe_row(1, 1000, 1000, "0xtop", "0xps"),
                probe_row(2, 1000, 1000, "0xtop", "0xps"),
                probe_row(20, 600, 800, "0xsecond", "0xps"),
                probe_row(21, 700, 900, "0xsecond", "0xps"),
            ])

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--probe-draws",
                    str(probe_csv),
                    "--row",
                    "60/2",
                    "--list-ranks",
                    "5",
                    "--output",
                    str(output),
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("rank 1", result.stdout)
            self.assertIn("rank 2", result.stdout)
            selection = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(selection["schema"], "dxmt9.3dmark05.payload_window_list.v1")
            self.assertEqual(selection["selection_count"], 2)
            self.assertEqual(selection["available_group_count"], 2)
            self.assertEqual(selection["selections"][0]["rank"], 1)
            self.assertEqual(selection["selections"][0]["group"]["vs"], "0xtop")
            self.assertEqual(selection["selections"][1]["rank"], 2)
            self.assertEqual(selection["selections"][1]["group"]["vs"], "0xsecond")

    def test_class_filter_selects_no_alpha_no_scissor_payload_window(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            probe_csv = root / "probe.csv"
            output = root / "selection.json"
            write_csv(probe_csv, [
                probe_row(1, 5000, 6000, "0xblend", "0xps", alpha_blend=1),
                probe_row(2, 5000, 6000, "0xblend", "0xps", alpha_blend=1),
                probe_row(10, 1000, 1500, "0xoff", "0xps", alpha_blend=0, applied=1),
                probe_row(11, 1200, 1800, "0xoff", "0xps", alpha_blend=0, applied=1),
                probe_row(12, 1400, 2100, "0xoff", "0xps", alpha_blend=0, applied=1),
                probe_row(20, 3000, 4500, "0xscissor", "0xps", alpha_blend=0, scissor=1),
                probe_row(30, 3000, 4500, "0xnotex", "0xps", alpha_blend=0, texture_mask="0x0"),
            ])

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--probe-draws",
                    str(probe_csv),
                    "--row",
                    "60/2",
                    "--class-filter",
                    "depth-read,no-alpha-blend,no-scissor,textured",
                    "--max-draws",
                    "2",
                    "--output",
                    str(output),
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("class_filters: depth-read,no-alpha-blend,no-scissor,textured", result.stdout)
            selection = json.loads(output.read_text(encoding="utf-8"))
            selected = selection["selection"]
            self.assertEqual(selected["class_filters"], [
                "depth-read",
                "no-alpha-blend",
                "no-scissor",
                "textured",
            ])
            self.assertEqual(selected["group"]["vs"], "0xoff")
            self.assertEqual(selected["window"]["encoder_draw_min"], 11)
            self.assertEqual(selected["window"]["encoder_draw_max"], 12)
            self.assertIn(
                "--probe-reverse-indexed-triangles-classes",
                selected["capture_flags"],
            )
            self.assertIn(
                "depth-read,no-alpha-blend,no-scissor,textured",
                selected["capture_flags"],
            )

    def test_applied_only_excludes_non_mutated_rows_before_window_selection(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            probe_csv = root / "probe.csv"
            output = root / "selection.json"
            write_csv(probe_csv, [
                probe_row(1, 5000, 6000, "0xoff", "0xps", alpha_blend=0, applied=0),
                probe_row(2, 5000, 6000, "0xoff", "0xps", alpha_blend=0, applied=0),
                probe_row(10, 1000, 1500, "0xoff", "0xps", alpha_blend=0, applied=1),
                probe_row(11, 1200, 1800, "0xoff", "0xps", alpha_blend=0, applied=1),
                probe_row(12, 1400, 2100, "0xoff", "0xps", alpha_blend=0, applied=1),
            ])

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--probe-draws",
                    str(probe_csv),
                    "--row",
                    "60/2",
                    "--class-filter",
                    "no-alpha-blend",
                    "--applied-only",
                    "--max-draws",
                    "2",
                    "--output",
                    str(output),
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("applied_only: true", result.stdout)
            selection = json.loads(output.read_text(encoding="utf-8"))
            selected = selection["selection"]
            self.assertTrue(selected["applied_only"])
            self.assertEqual(selected["window"]["encoder_draw_min"], 11)
            self.assertEqual(selected["window"]["encoder_draw_max"], 12)


if __name__ == "__main__":
    unittest.main()
