import csv
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tools" / "plan_3dmark05_mini_replay.py"


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


class MiniReplayPlanTests(unittest.TestCase):
    def test_report_marks_geometry_payload_missing(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            joined = root / "joined.csv"
            shaders = root / "shaders.csv"
            probes = root / "probe.csv"
            output = root / "mini-replay.md"

            write_csv(joined, [
                {
                    "seq": 60,
                    "enc": 2,
                    "gpu_ms": 20.0,
                    "vs_buffer_write_mib": 981.0,
                    "vs_buffer_bytes_per_vs_invocation": 1602.0,
                },
                {
                    "seq": 60,
                    "enc": 1,
                    "gpu_ms": 9.0,
                    "vs_buffer_write_mib": 421.0,
                    "vs_buffer_bytes_per_vs_invocation": 1151.0,
                },
            ])
            write_csv(shaders, [
                {"seq": 60, "enc": 2, "vs_file": "vs.metal", "ps_file": "ps.metal"},
            ])
            write_csv(probes, [
                {
                    "seq": 60,
                    "encoder": 2,
                    "primitive_count": 22622,
                    "original_cache_miss64": 36514,
                    "vs": "0xaaaabbbbccccdddd",
                    "ps": "0x1111222233334444",
                    "alpha_blend": 1,
                    "depth_enabled": 1,
                    "depth_write": 0,
                    "scissor": 1,
                    "texture_mask": "0x7f",
                    "cull": 3,
                },
                {
                    "seq": 60,
                    "encoder": 2,
                    "primitive_count": 10,
                    "original_cache_miss64": 11,
                    "vs": "0xdead",
                    "ps": "0xbeef",
                    "alpha_blend": 0,
                    "depth_enabled": 1,
                    "depth_write": 1,
                    "scissor": 0,
                    "texture_mask": "0x0",
                    "cull": 2,
                },
            ])

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--joined",
                    str(joined),
                    "--shader-summary",
                    str(shaders),
                    "--probe-draws",
                    str(probes),
                    "--output",
                    str(output),
                    "--top",
                    "2",
                    "--top-groups",
                    "1",
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            report = output.read_text(encoding="utf-8")
            self.assertIn("# 3DMark05 Mini Replay Readiness", report)
            self.assertIn("Raw vertex/index payload", report)
            self.assertIn("missing", report)
            self.assertIn("`60/2`", report)
            self.assertIn("22,622 tris", report)
            self.assertIn("geometry payload", report)
            self.assertIn("traces/<run-id>/analysis/geometry/", report)

    def test_report_marks_geometry_payload_partial_when_triplets_exist(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            joined = root / "joined.csv"
            shaders = root / "shaders.csv"
            probes = root / "probe.csv"
            geometry = root / "geometry"
            output = root / "mini-replay.md"
            geometry.mkdir()

            write_csv(joined, [
                {
                    "seq": 60,
                    "enc": 0,
                    "gpu_ms": 20.0,
                    "vs_buffer_write_mib": 981.0,
                    "vs_buffer_bytes_per_vs_invocation": 856.0,
                },
            ])
            write_csv(shaders, [
                {"seq": 60, "enc": 0, "vs_file": "vs.metal", "ps_file": "ps.metal"},
            ])
            write_csv(probes, [
                {
                    "seq": 60,
                    "encoder": 0,
                    "encoder_draw_index": 0,
                    "draw_ordinal": 100,
                    "primitive_count": 2020,
                    "original_cache_miss64": 3185,
                    "vs": "0xaaaabbbbccccdddd",
                    "ps": "0x1111222233334444",
                    "alpha_blend": 0,
                    "depth_enabled": 1,
                    "depth_write": 1,
                    "scissor": 0,
                    "texture_mask": "0x7f",
                    "cull": 2,
                },
                {
                    "seq": 60,
                    "encoder": 0,
                    "encoder_draw_index": 1,
                    "draw_ordinal": 101,
                    "primitive_count": 3216,
                    "original_cache_miss64": 6011,
                    "vs": "0xaaaabbbbccccdddd",
                    "ps": "0x1111222233334444",
                    "alpha_blend": 0,
                    "depth_enabled": 1,
                    "depth_write": 1,
                    "scissor": 0,
                    "texture_mask": "0x7f",
                    "cull": 2,
                },
            ])
            stem = geometry / "seq60-enc0-draw100-slot0"
            stem.with_suffix(".index.bin").write_bytes(b"index-bytes")
            stem.with_suffix(".stream0.bin").write_bytes(b"stream0-bytes")
            stem.with_suffix(".meta").write_text(
                "\n".join([
                    "seq=60",
                    "encoder=0",
                    "encoder_draw_index=0",
                    "draw_ordinal=100",
                    "slot=0",
                    "index_byte_count=11",
                    "stream0_byte_count=13",
                    "index_range_valid=1",
                    "stream0_range_valid=1",
                    "wrote_index=1",
                    "wrote_stream0=1",
                ]),
                encoding="utf-8",
            )

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--joined",
                    str(joined),
                    "--shader-summary",
                    str(shaders),
                    "--probe-draws",
                    str(probes),
                    "--geometry-dir",
                    str(geometry),
                    "--output",
                    str(output),
                    "--top",
                    "1",
                    "--top-groups",
                    "1",
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            report = output.read_text(encoding="utf-8")
            self.assertIn("| Raw vertex/index payload | partial |", report)
            self.assertIn("`1` valid payload triplets across `1/1` hot rows", report)
            self.assertIn("| `60/0` | `1` | `11` | `13` | `1` |", report)
            self.assertIn("remaining geometry payloads", report)
            self.assertIn("mini replay harness", report)


if __name__ == "__main__":
    unittest.main()
