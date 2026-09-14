#!/usr/bin/env python3
"""Real source publication -> encoder selection -> completed pixel hash join.

This is a bounded native draw fixture, not a replacement for captured GT2
counterexamples or a positive published-Wine-Tape coverage claim.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile


def main() -> int:
    fixture = Path(sys.argv[1]).resolve()
    root = Path(tempfile.mkdtemp(prefix="dxmt9-capture-color-metal-"))
    try:
        for mode in ("event", "segment"):
            output = root / mode
            output.mkdir()
            environment = {key: value for key, value in os.environ.items()
                           if not key.startswith(("DXMT9_DUMP_COLOR_ATTACHMENT_",
                                                  "DXMT9_EFFECT_DRAW_TRACE"))}
            environment.update({
                "DXMT9_DUMP_COLOR_ATTACHMENT_DIR": str(output / "hashes"),
                "DXMT9_DUMP_COLOR_ATTACHMENT_INDEX": "0",
                "DXMT9_DUMP_COLOR_ATTACHMENT_AFTER_DRAW": "0",
                "DXMT9_DUMP_COLOR_ATTACHMENT_HASH_ONLY": "1",
                "DXMT9_DUMP_COLOR_ATTACHMENT_CAPTURE_SOURCES_ONLY": "1",
                "DXMT9_DUMP_COLOR_ATTACHMENT_MAX_DUMPS": "16",
                "MTL_DEBUG_LAYER": "1",
                "DXMT9_EFFECT_DRAW_TRACE": "1",
                "DXMT9_EFFECT_DRAW_TRACE_BINDINGS": "1",
                "DXMT9_EFFECT_DRAW_TRACE_CAPTURE_SOURCES_ONLY": "1",
                "DXMT9_EFFECT_DRAW_TRACE_INCLUDE_NON_ALPHA": "1",
                "DXMT9_EFFECT_DRAW_TRACE_INCLUDE_UNTEXTURED": "1",
            })
            child = subprocess.run([str(fixture), "--capture-observer", mode],
                                   env=environment, capture_output=True, text=True,
                                   timeout=30, check=False)
            (output / "stdout.txt").write_text(child.stdout)
            (output / "stderr.txt").write_text(child.stderr)
            if child.returncode == 77:
                shutil.rmtree(root)
                return 77
            assert child.returncode == 0, (mode, child.returncode, child.stderr)
            receipt = json.loads(child.stdout)
            assert receipt["draws"] == 4 and receipt["strict_pixel_cuts"] == 4
            sources = {row["seq"]: row for row in receipt["sources"]}
            assert len(sources) == 2
            assert {row["event_ordinal"] for row in sources.values()} == {2, 4}
            def check_trace(text: str) -> None:
                rows = [dict(re.findall(r"(\w+)=([^\s\]]*)", line))
                        for line in text.splitlines()
                        if line.startswith("[dxmt9-effect-draw ")]
                assert len(rows) == 2
                assert {int(row["seq"]) for row in rows} == set(sources)
                for row in rows:
                    source = sources[int(row["seq"])]
                    assert int(row["capture_source_ordinal"]) == source["source_ordinal"]
                    assert int(row["capture_raw_ordinal"]) == source["event_ordinal"]
                    assert int(row["capture_source_owner"]) != 0
                    assert int(row["capture_source_generation"]) != 0
                    assert int(row["capture_storage_generation"]) != 0
            check_trace(child.stderr)
            hashes = list((output / "hashes").glob("*.bin.json"))
            assert len(hashes) == 2, (mode, "missing/extra GPU source hashes", hashes)
            assert not list((output / "hashes").glob("*.bin"))
            seen = set()
            for path in hashes:
                row = json.loads(path.read_text())
                assert row["seq"] in sources and row["seq"] not in seen
                seen.add(row["seq"])
                source = sources[row["seq"]]
                assert row["captureSourceOrdinal"] == source["source_ordinal"]
                assert row["captureRawOrdinal"] == source["event_ordinal"]
                assert row["captureSourceOwner"] != 0
                assert row["captureSourceGeneration"] != 0
                assert row["captureStorageGeneration"] != 0
                assert row["rawBytesRetained"] is False and row["afterDraw"] == 0
                assert row["storeActionKnown"] and row["effectiveStoreAction"] == 1
                assert row["sampleCount"] == 1 and row["storedAttachmentContents"]
                assert (row["width"], row["height"], row["rowBytes"], row["byteCount"]) == (
                    16, 16, 64, 1024)
                pixel = (bytes((0xef, 0xcd, 0xab, 0xff))
                         if source["event_ordinal"] == 2
                         else bytes((0x56, 0x34, 0x12, 0xff)))
                assert row["sha256"] == hashlib.sha256(pixel * 256).hexdigest(), row
                assert row["rawChannelOrder"] == "BGRA"
                assert row["channelSha256"] == [
                    hashlib.sha256(bytes((value,)) * 256).hexdigest() for value in pixel]
            assert seen == set(sources)
            # Paired depth is pulled only from an already selected physical
            # color pass, with the same production source provenance/budget.
            for name, fixture_mode, overrides, color_count, depth_count in (
                ("depth-pair", "--capture-depth-observer", {}, 2, 2),
                ("depth-d24x8", "--capture-d24x8-observer", {}, 2, 2),
                ("depth-cap-one", "--capture-depth-observer",
                 {"DXMT9_DUMP_COLOR_ATTACHMENT_MAX_DUMPS": "1"}, 1, 1),
                ("depth-disabled", "--capture-depth-observer",
                 {"DXMT9_DUMP_COLOR_ATTACHMENT_DEPTH_PAIR": "0"}, 2, 0),
                ("depth-missing", "--capture-observer", {}, 2, 0),
                ("depth-stencil", "--capture-stencil-observer", {}, 2, 0),
                ("depth-disjoint", "--capture-depth-observer",
                 {"DXMT9_DUMP_COLOR_ATTACHMENT_WIDTH": "17"}, 0, 0),
                ("depth-malformed", "--capture-depth-observer",
                 {"DXMT9_DUMP_COLOR_ATTACHMENT_DEPTH_PAIR": "yes"}, 0, 0),
                ("depth-over-budget", "--capture-depth-observer",
                 {"DXMT9_DUMP_COLOR_ATTACHMENT_MAX_DUMPS": "257"}, 0, 0),
                ("depth-after-draw", "--capture-depth-observer",
                 {"DXMT9_DUMP_COLOR_ATTACHMENT_AFTER_DRAW": "1"}, 0, 0),
            ):
                depth_dir = output / name
                depth_environment = dict(environment,
                    DXMT9_DUMP_COLOR_ATTACHMENT_DIR=str(depth_dir),
                    DXMT9_DUMP_COLOR_ATTACHMENT_DEPTH_PAIR="1")
                depth_environment.update(overrides)
                child = subprocess.run([str(fixture), fixture_mode, mode],
                    env=depth_environment, capture_output=True, text=True,
                    timeout=30, check=False)
                (output / f"{name}-stdout.txt").write_text(child.stdout)
                (output / f"{name}-stderr.txt").write_text(child.stderr)
                assert child.returncode == 0, (mode, name, child.stderr)
                assert "[dxmt9-depth-pair]" not in child.stderr, (mode, name, child.stderr)
                receipt = json.loads(child.stdout)
                assert receipt["draws"] == 4 and receipt["strict_pixel_cuts"] == 4
                paired_sources = {row["seq"]: row for row in receipt["sources"]}
                color_paths = list(depth_dir.glob("*.bin.json"))
                depth_paths = list(depth_dir.glob("*.depth.json"))
                assert len(color_paths) == color_count, (mode, name, color_paths)
                assert len(depth_paths) == depth_count, (mode, name, depth_paths)
                assert len(list(depth_dir.glob("*.bin"))) == depth_count
                for path in color_paths:
                    row = json.loads(path.read_text())
                    pixel = (bytes((0xef, 0xcd, 0xab, 0xff))
                             if paired_sources[row["seq"]]["event_ordinal"] == 2
                             else bytes((0x56, 0x34, 0x12, 0xff)))
                    assert row["sha256"] == hashlib.sha256(pixel * 256).hexdigest()
                for path in depth_paths:
                    row = json.loads(path.read_text())
                    color = json.loads((depth_dir / row["pairedColorMetadata"]).read_text())
                    assert row["schema"] == "dxmt9.paired_pass_depth.v1"
                    assert row["diagnosticOnly"] and row["rawBytesRetained"]
                    for key in ("seq", "enc", "encodedFrame", "captureSourceOwner",
                                "captureSourceOrdinal", "captureRawOrdinal",
                                "captureSourceGeneration", "captureStorageGeneration"):
                        assert row[key] == color[key], (mode, name, key)
                    assert row["handle"] != color["handle"]
                    assert row["captureSourceGeneration"] > 0
                    assert row["captureStorageGeneration"] > 0
                    assert (row["width"], row["height"], row["rowBytes"], row["byteCount"]
                            ) == (16, 16, 64, 1024)
                    assert row["depthDataFormat"] == "float32-le"
                    if name == "depth-d24x8":
                        assert row["metalPixelFormat"] in (252, 260)
                        assert row["depthPlaneExtraction"] == (row["metalPixelFormat"] == 260)
                    else:
                        assert row["metalPixelFormat"] == 252 and not row["depthPlaneExtraction"]
                    assert row["storeActionKnown"] and row["effectiveStoreAction"] == 1
                    assert row["sampleCount"] == 1 and row["storedAttachmentContents"]
                    data = path.with_suffix(".bin").read_bytes()
                    expected_depth = paired_sources[row["seq"]]["event_ordinal"] * 0.125
                    assert data == struct.pack("<f", expected_depth) * 256, (mode, name, row)
                    assert row["sha256"] == hashlib.sha256(data).hexdigest()
            for name, overrides, count in (
                ("extent-equal", {"DXMT9_DUMP_COLOR_ATTACHMENT_WIDTH": "16",
                                  "DXMT9_DUMP_COLOR_ATTACHMENT_HEIGHT": "16"}, 2),
                ("extent-width-disjoint", {"DXMT9_DUMP_COLOR_ATTACHMENT_WIDTH": "17"}, 0),
                ("extent-height-disjoint", {"DXMT9_DUMP_COLOR_ATTACHMENT_HEIGHT": "17"}, 0),
                ("extent-zero", {"DXMT9_DUMP_COLOR_ATTACHMENT_WIDTH": "0"}, 0),
                ("extent-malformed", {"DXMT9_DUMP_COLOR_ATTACHMENT_HEIGHT": "16x"}, 0),
                ("extent-overflow", {"DXMT9_DUMP_COLOR_ATTACHMENT_WIDTH": "4294967296"}, 0),
            ):
                extent_dir = output / name
                child = subprocess.run([str(fixture), "--capture-observer", mode],
                    env=dict(environment, DXMT9_DUMP_COLOR_ATTACHMENT_DIR=str(extent_dir), **overrides),
                    capture_output=True, text=True, timeout=30, check=False)
                (output / f"{name}-stdout.txt").write_text(child.stdout)
                (output / f"{name}-stderr.txt").write_text(child.stderr)
                assert child.returncode == 0, (mode, name, child.stderr)
                extent_receipt = json.loads(child.stdout)
                assert extent_receipt["draws"] == 4 and extent_receipt["strict_pixel_cuts"] == 4
                paths = list(extent_dir.glob("*.bin.json"))
                assert len(paths) == count, (mode, name, len(paths))
                assert not list(extent_dir.glob("*.bin"))
                for path in paths:
                    item = json.loads(path.read_text())
                    assert (item["width"], item["height"]) == (16, 16)
                if count == 0:
                    assert "Blit[ColorAttachmentDump" not in child.stderr
            # Source registration works without adding color/GPU readback.
            effect_only = {key: value for key, value in environment.items()
                           if not key.startswith("DXMT9_DUMP_COLOR_ATTACHMENT_")}
            child = subprocess.run([str(fixture), "--capture-observer", mode],
                                   env=effect_only, capture_output=True, text=True,
                                   timeout=30, check=False)
            (output / "effect-only-stdout.txt").write_text(child.stdout)
            (output / "effect-only-stderr.txt").write_text(child.stderr)
            assert child.returncode == 0, child.stderr
            sources = {row["seq"]: row for row in json.loads(child.stdout)["sources"]}
            check_trace(child.stderr)
            for name, overrides, warning in (
                ("malformed-scope", {"DXMT9_EFFECT_DRAW_TRACE_CAPTURE_SOURCES_ONLY": "yes"}, True),
                ("master-off", {"DXMT9_EFFECT_DRAW_TRACE": "0"}, False),
                ("master-off-malformed", {"DXMT9_EFFECT_DRAW_TRACE": "0",
                    "DXMT9_EFFECT_DRAW_TRACE_CAPTURE_SOURCES_ONLY": "yes"}, False),
                ("disjoint-seq", {"DXMT9_EFFECT_DRAW_TRACE_SEQ": "999999"}, False),
            ):
                child = subprocess.run([str(fixture), "--capture-observer", mode],
                                       env=dict(effect_only, **overrides),
                                       capture_output=True, text=True,
                                       timeout=30, check=False)
                (output / f"{name}-stdout.txt").write_text(child.stdout)
                (output / f"{name}-stderr.txt").write_text(child.stderr)
                assert child.returncode == 0, (mode, name, child.stderr)
                receipt = json.loads(child.stdout)
                assert receipt["draws"] == 4 and receipt["strict_pixel_cuts"] == 4
                assert "[dxmt9-effect-draw " not in child.stderr, (mode, name)
                assert "[dxmt9-effect-vs-volatile " not in child.stderr, (mode, name)
                assert ("invalid CAPTURE_SOURCES_ONLY" in child.stderr) == warning
            # Texture source scoping works independently of effect/color
            # observation, and unselected sources must not consume the budget.
            texture_environment = {key: value for key, value in os.environ.items()
                if not key.startswith(("DXMT9_DUMP_COLOR_ATTACHMENT_",
                                       "DXMT9_DUMP_DRAW_TEXTURE_",
                                       "DXMT9_EFFECT_DRAW_TRACE"))}
            texture_environment.update({
                "MTL_DEBUG_LAYER": "1",
                "DXMT9_DUMP_DRAW_TEXTURE_SLOTS": "0",
                "DXMT9_DUMP_DRAW_TEXTURE_REPEAT_PER_PASS": "1",
                "DXMT9_DUMP_DRAW_TEXTURE_SEQ_MIN": "1",
                "DXMT9_DUMP_DRAW_TEXTURE_SEQ_MAX": "64",
                "DXMT9_DUMP_DRAW_TEXTURE_MAX_DUMPS": "2",
                "DXMT9_DUMP_DRAW_TEXTURE_CAPTURE_SOURCES_ONLY": "1",
            })
            for name, overrides, count in (
                ("texture-captured", {}, 2),
                ("texture-issued", {"DXMT9_EFFECT_DRAW_TRACE": "1",
                    "DXMT9_EFFECT_DRAW_TRACE_BINDINGS": "1",
                    "DXMT9_EFFECT_DRAW_TRACE_GEOMETRY": "1",
                    "DXMT9_EFFECT_DRAW_TRACE_CAPTURE_SOURCES_ONLY": "1",
                    "DXMT9_EFFECT_DRAW_TRACE_INCLUDE_NON_ALPHA": "1",
                    "DXMT9_EFFECT_DRAW_TRACE_INCLUDE_UNTEXTURED": "1"}, 2),
                ("texture-all", {"DXMT9_DUMP_DRAW_TEXTURE_CAPTURE_SOURCES_ONLY": "0"}, 2),
                ("texture-invalid-scope", {"DXMT9_DUMP_DRAW_TEXTURE_CAPTURE_SOURCES_ONLY": "yes"}, 0),
                ("texture-unbounded", {"DXMT9_DUMP_DRAW_TEXTURE_MAX_DUMPS": "0"}, 0),
                ("texture-nonrepeat", {"DXMT9_DUMP_DRAW_TEXTURE_REPEAT_PER_PASS": "0"}, 0),
                ("texture-disjoint", {"DXMT9_DUMP_DRAW_TEXTURE_SEQ_MIN": "99999",
                    "DXMT9_DUMP_DRAW_TEXTURE_SEQ_MAX": "100000"}, 0),
            ):
                texture_dir = output / name
                child = subprocess.run([str(fixture), "--capture-texture-observer", mode],
                    env=dict(texture_environment, DXMT9_DUMP_DRAW_TEXTURE_DIR=str(texture_dir), **overrides),
                    capture_output=True, text=True, timeout=30, check=False)
                (output / f"{name}-stdout.txt").write_text(child.stdout)
                (output / f"{name}-stderr.txt").write_text(child.stderr)
                assert child.returncode == 0, (mode, name, child.stderr)
                receipt = json.loads(child.stdout)
                assert receipt["draws"] == 4 and receipt["strict_pixel_cuts"] == 4
                source_rows = {row["seq"]: row for row in receipt["sources"]}
                paths = list(texture_dir.glob("*.json"))
                assert len(paths) == count, (mode, name, len(paths))
                selected = set()
                sampling = [dict(re.findall(r"(\w+)=([^\s\]]*)", line))
                    for line in child.stderr.splitlines()
                    if line.startswith("[dxmt9-effect-issued-sampling ")]
                if name == "texture-issued":
                    assert len(sampling) == 2
                else:
                    assert not sampling
                for path in paths:
                    row = json.loads(path.read_text())
                    selected.add(row["seq"])
                    subresource, = row["subresources"]
                    assert (row["width"], row["height"], subresource["rowBytes"],
                            subresource["byteCount"]) == (2, 2, 8, 16)
                    assert (texture_dir / subresource["path"]).read_bytes() == b'\xff' * 16
                    if name in ("texture-captured", "texture-issued"):
                        source = source_rows[row["seq"]]
                        assert row["captureSourceOrdinal"] == source["source_ordinal"]
                        assert row["captureRawOrdinal"] == source["event_ordinal"]
                        assert all(row[key] > 0 for key in ("captureSourceOwner",
                            "captureSourceGeneration", "captureStorageGeneration"))
                    else:
                        assert "captureSourceOwner" not in row
                    if name == "texture-issued":
                        bound, = [item for item in sampling if int(item["seq"]) == row["seq"]]
                        assert int(bound["capture_source_ordinal"]) == row["captureSourceOrdinal"]
                        assert int(bound["capture_raw_ordinal"]) == row["captureRawOrdinal"]
                        descriptor = json.loads(bytes.fromhex(bound["json_utf8"]))
                        actual = descriptor["direct"]["texture_0"]
                        assert actual["status"] == "issued-descriptor"
                        assert actual["object_token"] == row["metalTextureObservationId"] > 0
                        assert (actual["width"], actual["height"], actual["levels"]) == (2, 2, 1)
                    else:
                        assert row["metalTextureObservationId"] == 0
                if name in ("texture-captured", "texture-issued"):
                    assert selected == set(source_rows)
                elif name == "texture-all":
                    assert selected != set(source_rows), "unscoped budget includes earlier uncaptured draw"
        print("capture_color_observer_metal: two modes, four strict pixel cuts each; "
              "exactly two source-qualified completed hashes per mode")
    except BaseException:
        print(f"Failure evidence retained: {root}", file=sys.stderr)
        raise
    else:
        shutil.rmtree(root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
