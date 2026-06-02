import csv
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tools" / "build_3dmark05_mini_replay_manifest.py"


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


class BuildMiniReplayManifestTests(unittest.TestCase):
    def test_manifest_joins_payload_probe_and_shader_rows(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            shaders = root / "frame60-shader-dump-summary.csv"
            probes = root / "probe.csv"
            geometry = root / "geometry"
            shader_dir = root / "shaders" / "msl"
            output = root / "manifest.json"
            geometry.mkdir()
            shader_dir.mkdir(parents=True)
            (shader_dir / "vs.metal").write_text("// vs\n", encoding="utf-8")
            (shader_dir / "ps.metal").write_text("// ps\n", encoding="utf-8")
            direct_vs = shader_dir / "translated-vs-shader-2748-source-1.metal"
            direct_ps = shader_dir / "translated-fs-shader-3567-source-2.metal"
            direct_vs.write_text("// direct vs\n", encoding="utf-8")
            direct_ps.write_text("// direct ps\n", encoding="utf-8")

            write_csv(shaders, [{
                "seq": 60,
                "enc": 2,
                "dxmt_vertex_shader_last": "0xrowvs",
                "dxmt_pixel_shader_last": "0xrowps",
                "vs_file": "vs.metal",
                "ps_file": "ps.metal",
                "vsout_fields": "position,color",
                "ps_vsout_read_fields": "position",
            }])
            write_csv(probes, [{
                "seq": 60,
                "encoder": 2,
                "encoder_draw_index": 189,
                "draw_ordinal": 42428,
                "primitive_type": 3,
                "primitive_count": 7097,
                "index_type": 0,
                "alpha_blend": 1,
                "depth_enabled": 1,
                "depth_write": 0,
                "depth_func": 4,
                "scissor": 0,
                "cull": 2,
                "fill": 0,
                "texture_mask": "0x7f",
                "color_write": "0xf",
                "vs": "0xabc",
                "ps": "0xdef",
                "vsout": "0xfff",
            }])

            stem = geometry / "seq60-enc2-draw42428-slot0"
            stem.with_suffix(".index.bin").write_bytes(b"index")
            stem.with_suffix(".stream0.bin").write_bytes(b"stream")
            stem.with_suffix(".meta").write_text(
                "\n".join([
                    "seq=60",
                    "encoder=2",
                    "encoder_draw_index=189",
                    "draw_ordinal=42428",
                    "slot=0",
                    "primitive_count=7097",
                    "index_count=21291",
                    "index_type=uint16",
                    "start_index=144",
                    "base_vertex=0",
                    "stream0_offset=1440",
                    "stream0_stride=24",
                    "min_index=0",
                    "max_index=7902",
                    "unique_indices=7903",
                    "cache_miss_64=13080",
                    "index_byte_count=5",
                    "stream0_byte_count=6",
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
                    "--shader-summary",
                    str(shaders),
                    "--probe-draws",
                    str(probes),
                    "--geometry-dir",
                    str(geometry),
                    "--shader-msl-dir",
                    str(shader_dir),
                    "--row",
                    "60/2",
                    "--output",
                    str(output),
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            manifest = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(manifest["schema"], "dxmt9.3dmark05.mini_replay_manifest.v1")
            self.assertEqual(manifest["summary"]["rows"], ["60/2"])
            self.assertEqual(manifest["summary"]["draw_count"], 1)
            self.assertEqual(manifest["summary"]["missing_probe_rows"], 0)
            self.assertEqual(manifest["summary"]["missing_shader_rows"], 0)
            draw = manifest["draws"][0]
            self.assertEqual(draw["encoder_draw_index"], 189)
            self.assertEqual(draw["draw_ordinal"], 42428)
            self.assertEqual(draw["state"]["primitive_count"], 7097)
            self.assertEqual(draw["state"]["index_type"], "uint16")
            self.assertEqual(draw["shaders"]["vs_hash"], "0xabc")
            self.assertTrue(draw["shaders"]["vs_file"].endswith(direct_vs.name))
            self.assertTrue(draw["shaders"]["ps_file"].endswith(direct_ps.name))
            self.assertEqual(draw["shaders"]["vs_file_source"], "draw-hash")
            self.assertEqual(draw["shaders"]["ps_file_source"], "draw-hash")
            self.assertEqual(draw["geometry"]["index_bytes"], 5)
            self.assertEqual(draw["geometry"]["stream0_bytes"], 6)

    def test_manifest_accepts_legacy_encoder_draw_payload_field(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            shaders = root / "shaders.csv"
            probes = root / "probe.csv"
            geometry = root / "geometry"
            output = root / "manifest.json"
            geometry.mkdir()
            write_csv(shaders, [{"seq": 60, "enc": 2}])
            write_csv(probes, [{
                "seq": 60,
                "encoder": 2,
                "encoder_draw_index": 189,
                "draw_ordinal": 42428,
                "primitive_count": 1,
            }])
            stem = geometry / "seq60-enc2-draw42428-slot0"
            stem.with_suffix(".index.bin").write_bytes(b"i")
            stem.with_suffix(".stream0.bin").write_bytes(b"s")
            stem.with_suffix(".meta").write_text(
                "\n".join([
                    "seq=60",
                    "encoder=2",
                    "encoder_draw=42428",
                    "primitive_count=1",
                    "index_count=3",
                    "index_byte_count=1",
                    "stream0_byte_count=1",
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
                    "--shader-summary",
                    str(shaders),
                    "--probe-draws",
                    str(probes),
                    "--geometry-dir",
                    str(geometry),
                    "--row",
                    "60/2",
                    "--output",
                    str(output),
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            manifest = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(manifest["draws"][0]["encoder_draw_index"], 189)
            self.assertEqual(manifest["draws"][0]["draw_ordinal"], 42428)


if __name__ == "__main__":
    unittest.main()
