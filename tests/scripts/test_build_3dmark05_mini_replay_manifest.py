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
                "src_blend": 5,
                "dst_blend": 6,
                "blend_op": 1,
                "separate_alpha": 1,
                "src_blend_alpha": 2,
                "dst_blend_alpha": 1,
                "blend_op_alpha": 3,
                "alpha_test": 1,
                "depth_enabled": 1,
                "depth_write": 0,
                "depth_func": 4,
                "scissor": 0,
                "scissor_l": 10,
                "scissor_t": 20,
                "scissor_r": 640,
                "scissor_b": 480,
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
            Path(str(stem) + ".stream1.bin").write_bytes(b"stream-one")
            Path(str(stem) + ".vsconsts.bin").write_bytes(b"vs")
            Path(str(stem) + ".psconsts.bin").write_bytes(b"ps")
            Path(str(stem) + ".ffpvs.bin").write_bytes(b"ffpvs")
            Path(str(stem) + ".ffpps.bin").write_bytes(b"ffpps")
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
                    "stream_payload_count=2",
                    "stream1_handle=0x101",
                    "stream1_metal_slot=6",
                    "stream1_offset=0",
                    "stream1_stride=64",
                    "stream1_start_byte=0",
                    "stream1_byte_count=10",
                    "stream1_range_valid=1",
                    "wrote_stream1=1",
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
                    "vsconsts_byte_count=2",
                    "psconsts_byte_count=2",
                    "ffpvs_byte_count=5",
                    "ffpps_byte_count=5",
                    "wrote_vsconsts=1",
                    "wrote_psconsts=1",
                    "wrote_ffpvs=1",
                    "wrote_ffpps=1",
                    "texture_mask=0x7f",
                    "texture0_handle=0x1000",
                    "texture0_lod=2",
                    "texture0_format=21",
                    "texture0_type=0",
                    "texture0_pool=1",
                    "texture0_usage=0x4",
                    "texture0_width=256",
                    "texture0_height=128",
                    "texture0_depth=1",
                    "texture0_levels=5",
                    "texture0_has_metal_texture=1",
                    "texture0_has_shader_read_texture=1",
                    "texture0_has_srgb_shader_read_texture=0",
                    "texture3_handle=0x3000",
                    "texture3_lod=0",
                    "texture3_missing_record=1",
                    "attachment_color0_handle=0x4000",
                    "attachment_color0_level=0",
                    "attachment_color0_sample_count=1",
                    "attachment_color0_format=22",
                    "attachment_color0_pool=0",
                    "attachment_color0_usage=0x1",
                    "attachment_color0_width=1024",
                    "attachment_color0_height=768",
                    "attachment_color0_bytes_per_pixel=4",
                    "attachment_color0_render_target=1",
                    "attachment_color0_depth_stencil=0",
                    "attachment_color0_has_metal_texture=1",
                    "attachment_color0_has_srgb_texture=1",
                    "attachment_color0_has_resolve_texture=0",
                    "attachment_color0_alias_texture=0x5000",
                    "attachment_color0_alias_level=2",
                    "attachment_color0_alias_slice=0",
                    "attachment_color0_alias_texture_format=22",
                    "attachment_color0_alias_texture_type=0",
                    "attachment_color0_alias_texture_usage=0x5",
                    "attachment_color0_alias_texture_width=1024",
                    "attachment_color0_alias_texture_height=768",
                    "attachment_color0_alias_texture_levels=4",
                    "attachment_depth_handle=0x6000",
                    "attachment_depth_level=0",
                    "attachment_depth_sample_count=1",
                    "attachment_depth_format=75",
                    "attachment_depth_pool=0",
                    "attachment_depth_usage=0x2",
                    "attachment_depth_width=1024",
                    "attachment_depth_height=768",
                    "attachment_depth_bytes_per_pixel=4",
                    "attachment_depth_render_target=0",
                    "attachment_depth_depth_stencil=1",
                    "attachment_depth_has_metal_texture=1",
                    "attachment_depth_has_srgb_texture=0",
                    "attachment_depth_has_resolve_texture=0",
                    "attachment_depth_alias_texture=0x0",
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
            self.assertEqual(manifest["summary"]["texture_handles"], ["0x1000", "0x3000"])
            self.assertEqual(manifest["summary"]["texture_handle_count"], 2)
            self.assertEqual(manifest["summary"]["texture_capture_handles"], ["0x1000"])
            self.assertEqual(manifest["summary"]["texture_capture_handle_count"], 1)
            self.assertEqual(manifest["summary"]["texture_capture_handles_arg"], "0x1000")
            self.assertEqual(
                manifest["summary"]["texture_capture_flags"],
                [
                    "--dump-draw-texture-handles",
                    "0x1000",
                    "--dump-draw-texture-seq",
                    "60",
                    "--dump-draw-texture-enc",
                    "2",
                ],
            )
            draw = manifest["draws"][0]
            self.assertEqual(draw["encoder_draw_index"], 189)
            self.assertEqual(draw["draw_ordinal"], 42428)
            self.assertEqual(draw["state"]["primitive_count"], 7097)
            self.assertEqual(draw["state"]["index_type"], "uint16")
            self.assertEqual(draw["state"]["alpha_blend"], 1)
            self.assertEqual(draw["state"]["src_blend"], 5)
            self.assertEqual(draw["state"]["dst_blend"], 6)
            self.assertEqual(draw["state"]["blend_op"], 1)
            self.assertEqual(draw["state"]["separate_alpha"], 1)
            self.assertEqual(draw["state"]["src_blend_alpha"], 2)
            self.assertEqual(draw["state"]["dst_blend_alpha"], 1)
            self.assertEqual(draw["state"]["blend_op_alpha"], 3)
            self.assertEqual(draw["state"]["alpha_test"], 1)
            self.assertEqual(draw["state"]["depth_enabled"], 1)
            self.assertEqual(draw["state"]["depth_write"], 0)
            self.assertEqual(draw["state"]["depth_func"], 4)
            self.assertEqual(draw["state"]["scissor_l"], 10)
            self.assertEqual(draw["state"]["scissor_t"], 20)
            self.assertEqual(draw["state"]["scissor_r"], 640)
            self.assertEqual(draw["state"]["scissor_b"], 480)
            self.assertEqual(draw["state"]["cull"], 2)
            self.assertEqual(draw["shaders"]["vs_hash"], "0xabc")
            self.assertTrue(draw["shaders"]["vs_file"].endswith(direct_vs.name))
            self.assertTrue(draw["shaders"]["ps_file"].endswith(direct_ps.name))
            self.assertEqual(draw["shaders"]["vs_file_source"], "draw-hash")
            self.assertEqual(draw["shaders"]["ps_file_source"], "draw-hash")
            self.assertEqual(draw["geometry"]["index_bytes"], 5)
            self.assertEqual(draw["geometry"]["stream0_bytes"], 6)
            self.assertEqual(len(draw["geometry"]["streams"]), 2)
            self.assertEqual(draw["geometry"]["streams"][0]["stream"], 0)
            self.assertEqual(draw["geometry"]["streams"][0]["metal_slot"], 1)
            self.assertEqual(draw["geometry"]["streams"][1]["stream"], 1)
            self.assertEqual(draw["geometry"]["streams"][1]["metal_slot"], 6)
            self.assertEqual(draw["geometry"]["streams"][1]["bytes"], 10)
            self.assertTrue(draw["geometry"]["streams"][1]["file"].endswith(".stream1.bin"))
            self.assertEqual(draw["uniforms"]["vsconsts_bytes"], 2)
            self.assertEqual(draw["uniforms"]["psconsts_bytes"], 2)
            self.assertEqual(draw["uniforms"]["ffpvs_bytes"], 5)
            self.assertEqual(draw["uniforms"]["ffpps_bytes"], 5)
            self.assertEqual(draw["uniforms"]["wrote_vsconsts"], 1)
            self.assertTrue(draw["uniforms"]["vsconsts_file"].endswith(".vsconsts.bin"))
            self.assertTrue(draw["uniforms"]["psconsts_file"].endswith(".psconsts.bin"))
            self.assertTrue(draw["uniforms"]["ffpvs_file"].endswith(".ffpvs.bin"))
            self.assertTrue(draw["uniforms"]["ffpps_file"].endswith(".ffpps.bin"))
            self.assertEqual(len(draw["textures"]), 2)
            self.assertEqual(draw["textures"][0]["stage"], 0)
            self.assertEqual(draw["textures"][0]["handle"], "0x1000")
            self.assertEqual(draw["textures"][0]["lod"], 2)
            self.assertEqual(draw["textures"][0]["format"], 21)
            self.assertEqual(draw["textures"][0]["type"], 0)
            self.assertEqual(draw["textures"][0]["pool"], 1)
            self.assertEqual(draw["textures"][0]["usage"], "0x4")
            self.assertEqual(draw["textures"][0]["width"], 256)
            self.assertEqual(draw["textures"][0]["height"], 128)
            self.assertEqual(draw["textures"][0]["levels"], 5)
            self.assertEqual(draw["textures"][0]["has_metal_texture"], 1)
            self.assertEqual(draw["textures"][0]["has_shader_read_texture"], 1)
            self.assertEqual(draw["textures"][0]["has_srgb_shader_read_texture"], 0)
            self.assertEqual(draw["textures"][1]["stage"], 3)
            self.assertEqual(draw["textures"][1]["handle"], "0x3000")
            self.assertEqual(draw["textures"][1]["missing_record"], 1)
            self.assertEqual(len(draw["attachments"]["colors"]), 1)
            color = draw["attachments"]["colors"][0]
            self.assertEqual(color["index"], 0)
            self.assertEqual(color["handle"], "0x4000")
            self.assertEqual(color["format"], 22)
            self.assertEqual(color["width"], 1024)
            self.assertEqual(color["height"], 768)
            self.assertEqual(color["bytes_per_pixel"], 4)
            self.assertEqual(color["render_target"], 1)
            self.assertEqual(color["has_srgb_texture"], 1)
            self.assertEqual(color["alias_texture"], "0x5000")
            self.assertEqual(color["alias_level"], 2)
            self.assertEqual(color["alias_texture_levels"], 4)
            depth = draw["attachments"]["depth"]
            self.assertEqual(depth["handle"], "0x6000")
            self.assertEqual(depth["format"], 75)
            self.assertEqual(depth["depth_stencil"], 1)
            self.assertEqual(depth["alias_texture"], "0x0")

    def test_manifest_uses_draw_shader_files_for_vsout_read_fields(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            shaders = root / "shaders.csv"
            probes = root / "probe.csv"
            geometry = root / "geometry"
            shader_dir = root / "shaders" / "msl"
            output = root / "manifest.json"
            geometry.mkdir()
            shader_dir.mkdir(parents=True)
            direct_vs = shader_dir / "translated-vs-shader-2748-source-1.metal"
            direct_ps = shader_dir / "translated-fs-shader-3567-source-2.metal"
            direct_vs.write_text(
                "struct VSOut {\n"
                "  float4 position [[position]];\n"
                "  float4 texcoord0;\n"
                "  float4 texcoord7;\n"
                "  float fogFactor;\n"
                "};\n",
                encoding="utf-8",
            )
            direct_ps.write_text(
                "fragment float4 dxmt9_fs(VSOut in [[stage_in]]) {\n"
                "  return in.position + in.texcoord7 + float4(in.fogFactor);\n"
                "}\n",
                encoding="utf-8",
            )
            write_csv(shaders, [{
                "seq": 60,
                "enc": 2,
                "vsout_fields": "position,color",
                "ps_vsout_read_fields": "position",
            }])
            write_csv(probes, [{
                "seq": 60,
                "encoder": 2,
                "encoder_draw_index": 14,
                "draw_ordinal": 42604,
                "primitive_type": 3,
                "primitive_count": 1,
                "vs": "0xabc",
                "ps": "0xdef",
                "vsout": "0xfff",
            }])

            stem = geometry / "seq60-enc2-draw42604-slot0"
            stem.with_suffix(".index.bin").write_bytes(b"iii")
            stem.with_suffix(".stream0.bin").write_bytes(b"ssss")
            stem.with_suffix(".meta").write_text(
                "\n".join([
                    "seq=60",
                    "encoder=2",
                    "encoder_draw_index=14",
                    "draw_ordinal=42604",
                    "primitive_count=1",
                    "index_count=3",
                    "index_byte_count=3",
                    "stream0_byte_count=4",
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
            draw = json.loads(output.read_text(encoding="utf-8"))["draws"][0]
            self.assertEqual(
                draw["shaders"]["vsout_fields"],
                "position,texcoord0,texcoord7,fogFactor",
            )
            self.assertEqual(
                draw["shaders"]["ps_vsout_read_fields"],
                "fogFactor,position,texcoord7",
            )

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

    def test_manifest_sorts_encoder_draw_index_zero_before_one(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            shaders = root / "shaders.csv"
            probes = root / "probe.csv"
            geometry = root / "geometry"
            output = root / "manifest.json"
            geometry.mkdir()
            write_csv(shaders, [{"seq": 60, "enc": 2}])
            write_csv(probes, [
                {
                    "seq": 60,
                    "encoder": 2,
                    "encoder_draw_index": draw,
                    "draw_ordinal": 42000 + draw,
                    "primitive_count": 1,
                }
                for draw in [0, 1]
            ])
            for draw in [1, 0]:
                stem = geometry / f"seq60-enc2-draw{42000 + draw}-slot0"
                stem.with_suffix(".index.bin").write_bytes(b"i")
                stem.with_suffix(".stream0.bin").write_bytes(b"s")
                stem.with_suffix(".meta").write_text(
                    "\n".join([
                        "seq=60",
                        "encoder=2",
                        f"encoder_draw_index={draw}",
                        f"draw_ordinal={42000 + draw}",
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
            self.assertEqual(
                [draw["encoder_draw_index"] for draw in manifest["draws"]],
                [0, 1],
            )
            self.assertEqual(manifest["summary"]["draw_ordinals"], [42000, 42001])

    def test_manifest_filters_payloads_by_shader_hash_without_row(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            shaders = root / "shaders.csv"
            probes = root / "probe.csv"
            geometry = root / "geometry"
            shader_dir = root / "shaders" / "msl"
            output = root / "manifest.json"
            geometry.mkdir()
            shader_dir.mkdir(parents=True)
            (shader_dir / "translated-vs-shader-2748-source-1.metal").write_text(
                "// target vs\n", encoding="utf-8")
            (shader_dir / "translated-fs-shader-3567-source-2.metal").write_text(
                "// target ps\n", encoding="utf-8")
            (shader_dir / "translated-vs-shader-291-source-3.metal").write_text(
                "// other vs\n", encoding="utf-8")
            (shader_dir / "translated-fs-shader-1110-source-4.metal").write_text(
                "// other ps\n", encoding="utf-8")
            write_csv(shaders, [
                {"seq": 60, "enc": 4},
                {"seq": 60, "enc": 1},
            ])
            write_csv(probes, [
                {
                    "seq": 60,
                    "encoder": 4,
                    "encoder_draw_index": 80,
                    "draw_ordinal": 42346,
                    "primitive_count": 18179,
                    "vs": "0xabc",
                    "ps": "0xdef",
                },
                {
                    "seq": 60,
                    "encoder": 1,
                    "encoder_draw_index": 10,
                    "draw_ordinal": 42010,
                    "primitive_count": 1000,
                    "vs": "0x123",
                    "ps": "0x456",
                },
            ])

            for seq, enc, ordinal, vs, ps in [
                (60, 4, 42346, "0xabc", "0xdef"),
                (60, 1, 42010, "0x123", "0x456"),
            ]:
                stem = geometry / f"seq{seq}-enc{enc}-draw{ordinal}-slot0"
                stem.with_suffix(".index.bin").write_bytes(b"ii")
                stem.with_suffix(".stream0.bin").write_bytes(b"ssss")
                stem.with_suffix(".meta").write_text(
                    "\n".join([
                        f"seq={seq}",
                        f"encoder={enc}",
                        "encoder_draw_index=80",
                        f"draw_ordinal={ordinal}",
                        "primitive_count=1",
                        "index_count=3",
                        f"vs={vs}",
                        f"ps={ps}",
                        "index_byte_count=2",
                        "stream0_byte_count=4",
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
                    "--vs",
                    "0xabc",
                    "--ps",
                    "0xdef",
                    "--output",
                    str(output),
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            manifest = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(manifest["summary"]["rows"], ["60/4"])
            self.assertEqual(manifest["summary"]["draw_count"], 1)
            self.assertEqual(manifest["sources"]["vs_filter"], "0xabc")
            self.assertEqual(manifest["sources"]["ps_filter"], "0xdef")
            self.assertEqual(manifest["draws"][0]["row"], "60/4")
            self.assertEqual(manifest["draws"][0]["shaders"]["vs_hash"], "0xabc")
            self.assertEqual(manifest["draws"][0]["shaders"]["ps_hash"], "0xdef")
            self.assertEqual(manifest["draws"][0]["shaders"]["vs_file_source"], "draw-hash")
            self.assertEqual(manifest["draws"][0]["shaders"]["ps_file_source"], "draw-hash")

    def test_manifest_accepts_payload_window_selection(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            shaders = root / "shaders.csv"
            probes = root / "probe.csv"
            geometry = root / "geometry"
            selection = root / "selection.json"
            output = root / "manifest.json"
            geometry.mkdir()
            write_csv(shaders, [{"seq": 60, "enc": 2}])
            write_csv(probes, [
                {
                    "seq": 60,
                    "encoder": 2,
                    "encoder_draw_index": draw,
                    "draw_ordinal": 42000 + draw,
                    "primitive_count": 100 + draw,
                    "vs": "0xaaa",
                    "ps": "0xbbb",
                }
                for draw in [10, 11, 12, 13]
            ])
            for draw in [10, 11, 12, 13]:
                stem = geometry / f"seq60-enc2-draw{42000 + draw}-slot0"
                stem.with_suffix(".index.bin").write_bytes(bytes([draw, draw + 1]))
                stem.with_suffix(".stream0.bin").write_bytes(bytes([draw]) * 4)
                stem.with_suffix(".meta").write_text(
                    "\n".join([
                        "seq=60",
                        "encoder=2",
                        f"encoder_draw_index={draw}",
                        f"draw_ordinal={42000 + draw}",
                        "slot=0",
                        f"primitive_count={100 + draw}",
                        "index_count=3",
                        "index_byte_count=2",
                        "stream0_byte_count=4",
                        "index_range_valid=1",
                        "stream0_range_valid=1",
                        "wrote_index=1",
                        "wrote_stream0=1",
                    ]),
                    encoding="utf-8",
                )
            selection.write_text(
                json.dumps({
                    "schema": "dxmt9.3dmark05.payload_window.v1",
                    "selection": {
                        "row": "60/2",
                        "window": {
                            "encoder_draw_min": 11,
                            "encoder_draw_max": 12,
                            "draw_ordinals": [42011, 42012],
                        },
                    },
                }),
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
                    "--payload-selection",
                    str(selection),
                    "--output",
                    str(output),
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            manifest = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(manifest["sources"]["payload_selection"], str(selection))
            self.assertEqual(manifest["sources"]["encoder_draw_min"], "11")
            self.assertEqual(manifest["sources"]["encoder_draw_max"], "12")
            self.assertEqual(manifest["sources"]["draw_ordinals_filter"], "42011,42012")
            self.assertEqual(manifest["summary"]["draw_count"], 2)
            self.assertEqual(manifest["summary"]["encoder_draw_min"], 11)
            self.assertEqual(manifest["summary"]["encoder_draw_max"], 12)
            self.assertEqual(manifest["summary"]["draw_ordinals"], [42011, 42012])
            self.assertEqual(
                [draw["encoder_draw_index"] for draw in manifest["draws"]],
                [11, 12],
            )

    def test_manifest_filters_noncontiguous_encoder_draw_indices(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            shaders = root / "shaders.csv"
            probes = root / "probe.csv"
            geometry = root / "geometry"
            output = root / "manifest.json"
            geometry.mkdir()
            write_csv(shaders, [{"seq": 60, "enc": 2}])
            write_csv(probes, [
                {
                    "seq": 60,
                    "encoder": 2,
                    "encoder_draw_index": draw,
                    "draw_ordinal": 42000 + draw,
                    "primitive_count": 100 + draw,
                    "vs": "0xaaa",
                    "ps": "0xbbb",
                }
                for draw in [10, 11, 12, 13]
            ])
            for draw in [10, 11, 12, 13]:
                stem = geometry / f"seq60-enc2-draw{42000 + draw}-slot0"
                stem.with_suffix(".index.bin").write_bytes(bytes([draw, draw + 1]))
                stem.with_suffix(".stream0.bin").write_bytes(bytes([draw]) * 4)
                stem.with_suffix(".meta").write_text(
                    "\n".join([
                        "seq=60",
                        "encoder=2",
                        f"encoder_draw_index={draw}",
                        f"draw_ordinal={42000 + draw}",
                        "slot=0",
                        f"primitive_count={100 + draw}",
                        "index_count=3",
                        "index_byte_count=2",
                        "stream0_byte_count=4",
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
                    "--encoder-draw-indices",
                    "10,12",
                    "--output",
                    str(output),
                ],
                text=True,
                capture_output=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            manifest = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(manifest["sources"]["encoder_draw_indices_filter"], "10,12")
            self.assertEqual(manifest["summary"]["draw_count"], 2)
            self.assertEqual(manifest["summary"]["encoder_draw_min"], 10)
            self.assertEqual(manifest["summary"]["encoder_draw_max"], 12)
            self.assertEqual(
                [draw["encoder_draw_index"] for draw in manifest["draws"]],
                [10, 12],
            )


if __name__ == "__main__":
    unittest.main()
