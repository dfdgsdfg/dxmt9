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
) -> dict[str, object]:
    return {
        "seq": 60,
        "encoder": 2,
        "encoder_draw_index": draw,
        "draw_ordinal": 42000 + draw,
        "primitive_count": tris,
        "original_cache_miss64": cache64,
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
        "scissor": 0,
        "cull": 2,
        "fill": 0,
        "texture_mask": "0x7f",
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


if __name__ == "__main__":
    unittest.main()
