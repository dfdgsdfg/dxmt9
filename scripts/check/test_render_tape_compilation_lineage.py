#!/usr/bin/env python3
"""Actual Metal source/options lineage remains diagnostic GPU evidence."""

from __future__ import annotations

import hashlib
import json
import os
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile


def main() -> int:
    if len(sys.argv) != 5:
        raise SystemExit("usage: test_render_tape_compilation_lineage.py "
                         "<fixture> <provider> <validator> <runner>")
    fixture, provider, validator, runner = map(pathlib.Path, sys.argv[1:])
    root = pathlib.Path(tempfile.mkdtemp(prefix="dxmt9-compiled-shader-lineage-"))

    def run(*args: str, expected: int = 0, env=None):
        result = subprocess.run(args, text=True, capture_output=True,
                                timeout=90, env=env)
        assert result.returncode == expected, (args, result.returncode,
                                               result.stdout, result.stderr)
        return result

    try:
        source, bundle = root / "source", root / "bundle"
        run(str(fixture), "--write-gpu-visible-uniform-fixture", str(source))
        command = [sys.executable, str(runner), "pack", "--events",
                   str(source / "events.bin"), "--output-dir", str(bundle),
                   "--source-rgba", str(source / "source.rgba"),
                   "--output-rgba", str(source / "output.rgba"),
                   "--test-input", "--validator", str(validator)]
        for path in sorted(source.glob("*.bin")):
            if path.name != "events.bin":
                command.extend(("--blob", str(path)))
        run(*command)
        shaders, output = root / "shaders", root / "observed"
        environment = dict(os.environ, DXMT_DUMP_SHADER_DIR=str(shaders),
                           DXMT9_EFFECT_DRAW_TRACE="1",
                           DXMT9_EFFECT_DRAW_TRACE_BINDINGS="1",
                           DXMT9_EFFECT_DRAW_TRACE_PAYLOAD_BYTES="1",
                           DXMT9_EFFECT_DRAW_TRACE_GEOMETRY="1",
                           DXMT9_EFFECT_DRAW_TRACE_GEOMETRY_MAX_REFS="0",
                           DXMT9_EFFECT_DRAW_TRACE_INCLUDE_NON_ALPHA="1",
                           DXMT9_EFFECT_DRAW_TRACE_INCLUDE_UNTEXTURED="1")
        run(sys.executable, str(runner), "experiment", str(bundle),
            "--provider", str(provider), "--validator", str(validator),
            "--warmup", "0", "--repeat", "1", "--require-path",
            "--output-dir", str(output), expected=1, env=environment)
        report = json.loads((output / "result.json").read_text())
        assert report["diagnostic"] is True and report["accepted"] is False
        assert report["provider_exit_code"] == 0
        iteration = report["result"]["iterations"][0]
        assert iteration["path_counters"]["gpu_errors"] == 0
        assert iteration["provider_result"]["validity"]["expected_digest_matched"] is True
        def check_bound_constants(stderr: str, mode: str = "direct") -> None:
            draws = [dict(re.findall(r"(\w+)=([^\s\]]*)", line))
                     for line in stderr.splitlines()
                     if line.startswith("[dxmt9-effect-draw ")]
            assert len(draws) == 3, len(draws)
            expected_vs = ((1., 1., 1., 1.), (0.5, 1., 1., 1.), (1., 1., 1., 1.))
            expected_ps = ((0.88, 0.25, 0.25, 1.), (0.25, 0.88, 0.5, 1.),
                           (0.88, 0.25, 0.25, 1.))
            for index, row in enumerate(draws):
                assert row["bound_cbuf_mode"] == mode
                for stage in ("vs", "ps", "ffpvs", "ffpps"):
                    key = f"bound_{stage}_issued_binding"
                    if mode == "direct":
                        assert row[key] == "match", (stage, row)
                    else:
                        assert key not in row  # Direct witness cannot qualify a table binding.
                for stage, expected in (("vs", expected_vs), ("ps", expected_ps)):
                    assert row[f"bound_{stage}_readable"] == "1"
                    raw = bytes.fromhex(row[f"bound_{stage}_bytes"])
                    assert len(raw) == int(row[f"bound_{stage}_size"])
                    assert hashlib.sha256(raw).hexdigest() == row[f"bound_{stage}_sha256"]
                    assert raw[:16] == struct.pack("<4f", *expected[index])
                    assert raw[:16] == bytes.fromhex(row[f"{stage}_float_bytes"])[:16]
                for stage in ("ffpvs", "ffpps"):
                    assert row[f"bound_{stage}_readable"] == "1"
                    raw = bytes.fromhex(row[f"bound_{stage}_bytes"])
                    assert len(raw) == int(row[f"bound_{stage}_size"])
                    assert hashlib.sha256(raw).hexdigest() == row[f"bound_{stage}_sha256"]
            volatile = [dict(re.findall(r"(\w+)=([^\s\]]*)", line))
                        for line in stderr.splitlines()
                        if line.startswith("[dxmt9-effect-vs-volatile ")]
            assert len(volatile) == 3
            assert [r["ordinal"] for r in volatile] == [r["ordinal"] for r in draws]
            assert all(len(bytes.fromhex(row["bytes"])) == 80 for row in volatile)
            issued = [dict(re.findall(r"(\w+)=([^\s\]]*)", line))
                      for line in stderr.splitlines()
                      if line.startswith("[dxmt9-effect-issued-geometry ")]
            original = [dict(re.findall(r"(\w+)=([^\s\]]*)", line))
                        for line in stderr.splitlines()
                        if line.startswith("[dxmt9-effect-geometry-bytes ")]
            assert len(issued) == len(original) == 3
            assert [row["ordinal"] for row in issued] == [row["ordinal"] for row in draws]
            raster = [dict(re.findall(r"(\w+)=([^\s\]]*)", line))
                      for line in stderr.splitlines()
                      if line.startswith("[dxmt9-effect-issued-raster ")]
            assert len(raster) == 3
            assert [(r["seq"], r["ordinal"]) for r in raster] == [
                (r["seq"], r["ordinal"]) for r in draws]
            sampling = [dict(re.findall(r"(\w+)=([^\s\]]*)", line))
                        for line in stderr.splitlines()
                        if line.startswith("[dxmt9-effect-issued-sampling ")]
            assert [(r["seq"], r["ordinal"]) for r in sampling] == [
                (r["seq"], r["ordinal"]) for r in draws]
            for row in sampling:
                descriptor = json.loads(bytes.fromhex(row["json_utf8"]))
                assert descriptor["schema"] == "dxmt9.metal_sampling_binding.v1"
                assert descriptor["scope"] == "issued-descriptors-not-contents-or-shader-consumption"
            depth = [dict(re.findall(r"(\w+)=([^\s\]]*)", line))
                     for line in stderr.splitlines()
                     if line.startswith("[dxmt9-effect-issued-depth ")]
            assert [(r["seq"], r["ordinal"]) for r in depth] == [
                (r["seq"], r["ordinal"]) for r in draws]
            for row in depth:
                descriptor = json.loads(bytes.fromhex(row["json_utf8"]))
                assert descriptor["schema"] == "dxmt9.metal_depth_stencil_binding.v1"
                assert descriptor["scope"] == "issued-descriptors-not-attachment-contents-or-gpu-tests"
                assert descriptor["depth_stencil"]["status"] == "issued-descriptor"
                assert descriptor["stencil_reference_known"] is True
                assert descriptor["stencil_reference"] == 0
            for row in raster:
                assert row["indexed"] == "1"
                assert all(row[field] == "1" for field in (
                    "viewport_known", "scissor_known", "raster_known", "pipeline_known"))
                assert struct.unpack("<6d", bytes.fromhex(row["viewport_bytes"])) == (
                    0., 0., 16., 16., 0., 1.)
                assert struct.unpack("<4Q", bytes.fromhex(row["scissor_bytes"])) == (0, 0, 16, 16)
                assert struct.unpack("<3f", bytes.fromhex(row["depth_bias_bytes"])) == (0., 0., 0.)
                assert len(bytes.fromhex(row["raster_modes"])) == 16
                label = bytes.fromhex(row["pipeline_label_utf8"]).decode()
                assert re.fullmatch(r"pso_h[0-9a-f]{16}(?:_vso0x[0-9a-f]+)?", label)
                assert row["pipeline_creation_known"] == "1", row
                digest = row["pipeline_creation_sha256"]
                raw_creation = (shaders / "pipelines" / (digest + ".json")).read_bytes()
                assert hashlib.sha256(raw_creation).hexdigest() == digest
                creation = json.loads(raw_creation)
                assert creation["schema"] == "dxmt9.metal_pipeline_creation.v1"
                assert creation["vertex_function"] == "dxmt9_vs"
                assert creation["fragment_function"] == "dxmt9_fs"
                assert creation["compile_options"] == {
                    "profile_version": 1, "language_version": 131076,
                    "math_mode": 2, "math_functions": 0, "preserve_invariance": False}
                assert creation["function_constants"] is None
                assert creation["sample_count"] == 1 and creation["rasterization"] is True
                assert len(creation["colors"]) == 8 and len(creation["colors"][0]) == 9
                assert len(creation["vertex_buffer_mutability"]) == 31
                for stage, field in (("vs", "vertex_source_sha256"), ("fs", "fragment_source_sha256")):
                    sha = row[f"pipeline_{stage}_sha256"]
                    assert sha == creation[field]
                    source_bytes = (shaders / "compiled" / (sha + ".metal")).read_bytes()
                    assert hashlib.sha256(source_bytes).hexdigest() == sha
                    if stage == "vs":
                        assert b"position [[position, invariant]]" in source_bytes
                    assert any(path.read_bytes() == source_bytes for path in
                               shaders.glob(f"translated-{stage}-*.metal"))
            def fnv(raw: bytes) -> str:
                value = 14695981039346656037
                for byte in raw:
                    value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
                return f"0x{value:x}"
            index_bytes = (source / "index.bin").read_bytes()
            vertex_bytes = (source / "vertex.bin").read_bytes()
            for row, expected in zip(issued, original):
                assert row["index_readable"] == row["stream0_readable"] == row["volatile_known"] == "1"
                assert row["index_hash"] == expected["index_hash"]
                assert row["stream0_hash"] == expected["stream0_hash"]
                first = int(expected["index_start_byte"])
                size = int(expected["index_byte_count"])
                assert row["index_hash"] == fnv(index_bytes[first:first + size])
                first = int(expected["stream0_start_byte"])
                size = int(expected["stream0_byte_count"])
                assert row["stream0_hash"] == fnv(vertex_bytes[first:first + size])
                assert row["instance_count"] == "1" and row["base_instance"] == "0"

        check_bound_constants((output / "provider.stderr").read_text())
        direct_output = root / "observed-direct"
        run(sys.executable, str(runner), "experiment", str(bundle),
            "--provider", str(provider), "--validator", str(validator),
            "--warmup", "0", "--repeat", "1", "--require-path",
            "--replay-route", "production-slot", "--output-dir", str(direct_output),
            expected=1, env=environment)
        direct_report = json.loads((direct_output / "result.json").read_text())
        direct_iteration, = direct_report["result"]["iterations"]
        assert direct_report["diagnostic"] and not direct_report["accepted"]
        assert direct_iteration["path_counters"]["direct_lease_draws"] == 3
        assert direct_iteration["provider_result"]["validity"]["expected_digest_matched"]
        check_bound_constants((direct_output / "provider.stderr").read_text())
        table_output = root / "observed-argument-table"
        run(sys.executable, str(runner), "experiment", str(bundle),
            "--provider", str(provider), "--validator", str(validator),
            "--warmup", "0", "--repeat", "1", "--require-path",
            "--replay-route", "production-slot", "--output-dir", str(table_output),
            expected=1, env=dict(environment, DXMT9_ARGBUF_DIRECT_CBUF="0"))
        table_report = json.loads((table_output / "result.json").read_text())
        table_iteration, = table_report["result"]["iterations"]
        assert table_report["diagnostic"] and not table_report["accepted"]
        assert table_iteration["path_counters"]["direct_lease_draws"] == 3
        assert table_iteration["provider_result"]["validity"]["expected_digest_matched"]
        check_bound_constants((table_output / "provider.stderr").read_text(), "argbuf")
        # A real binding without the actual source/creation publication chain
        # must remain unknown even if its mutable label looks identical.
        unpublished_env = dict(environment)
        unpublished_env.pop("DXMT_DUMP_SHADER_DIR", None)
        unpublished_output = root / "unpublished-pipeline-lineage"
        run(sys.executable, str(runner), "experiment", str(bundle),
            "--provider", str(provider), "--validator", str(validator),
            "--warmup", "0", "--repeat", "1", "--require-path",
            "--output-dir", str(unpublished_output), expected=1, env=unpublished_env)
        unpublished = [dict(re.findall(r"(\w+)=([^\s\]]*)", line)) for line in
                       (unpublished_output / "provider.stderr").read_text().splitlines()
                       if line.startswith("[dxmt9-effect-issued-raster ")]
        assert len(unpublished) == 3
        assert all(row["pipeline_known"] == "1" and row["pipeline_creation_known"] == "0"
                   and "pipeline_creation_sha256" not in row for row in unpublished)
        blocked_dump = root / "blocked-shader-directory"
        blocked_dump.write_bytes(b"occupied by test")
        blocked_output = root / "failed-pipeline-publication"
        run(sys.executable, str(runner), "experiment", str(bundle),
            "--provider", str(provider), "--validator", str(validator),
            "--warmup", "0", "--repeat", "1", "--require-path",
            "--output-dir", str(blocked_output), expected=1,
            env=dict(environment, DXMT_DUMP_SHADER_DIR=str(blocked_dump)))
        blocked_report = json.loads((blocked_output / "result.json").read_text())
        assert blocked_report["result"]["iterations"][0]["provider_result"]["validity"]["expected_digest_matched"]
        blocked = [dict(re.findall(r"(\w+)=([^\s\]]*)", line)) for line in
                   (blocked_output / "provider.stderr").read_text().splitlines()
                   if line.startswith("[dxmt9-effect-issued-raster ")]
        assert len(blocked) == 3
        assert all(row["pipeline_known"] == "1" and row["pipeline_creation_known"] == "0"
                   and "pipeline_creation_sha256" not in row for row in blocked)
        # A separate fixture reads POSITION from stream 1 / Metal slot 6, using
        # the second half of the same immutable VB. Changing only that half
        # leaves stream-0/index witnesses identical while changing pixels.
        def fnv(raw: bytes) -> str:
            value = 14695981039346656037
            for byte in raw:
                value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
            return f"0x{value:x}"

        extra_witnesses = {}
        extra_geometry = {}
        extra_inputs = {}
        for altered in (False, True):
            name = "extra-altered" if altered else "extra"
            extra_source, extra_bundle = root / (name + "-source"), root / (name + "-bundle")
            flag = "--write-gpu-visible-extra-stream-" + ("altered-fixture" if altered else "fixture")
            run(str(fixture), flag, str(extra_source))
            pack = [sys.executable, str(runner), "pack", "--events",
                    str(extra_source / "events.bin"), "--output-dir", str(extra_bundle),
                    "--source-rgba", str(extra_source / "source.rgba"),
                    "--output-rgba", str(extra_source / "output.rgba"),
                    "--test-input", "--validator", str(validator)]
            for path in sorted(extra_source.glob("*.bin")):
                if path.name != "events.bin":
                    pack.extend(("--blob", str(path)))
            run(*pack)
            for route in ("compatibility", "production-slot"):
                extra_output = root / (name + "-" + route)
                run(sys.executable, str(runner), "experiment", str(extra_bundle),
                    "--provider", str(provider), "--validator", str(validator),
                    "--warmup", "0", "--repeat", "1", "--require-path",
                    "--replay-route", route, "--output-dir", str(extra_output),
                    expected=1, env=environment)
                extra_report = json.loads((extra_output / "result.json").read_text())
                item, = extra_report["result"]["iterations"]
                assert item["path_counters"]["gpu_errors"] == 0
                assert item["provider_result"]["validity"]["expected_digest_matched"] is not altered
                if route == "production-slot":
                    assert item["path_counters"]["direct_lease_draws"] == 3
                stderr = (extra_output / "provider.stderr").read_text()
                rows = [dict(re.findall(r"(\w+)=([^\s\]]*)", line))
                        for line in stderr.splitlines()
                        if line.startswith("[dxmt9-effect-issued-stream ")]
                assert len(rows) == 3, (extra_output, rows)
                raw = (extra_source / "vertex.bin").read_bytes()
                extra_inputs[altered] = raw
                geometry = [dict(re.findall(r"(\w+)=([^\s\]]*)", line))
                            for line in stderr.splitlines()
                            if line.startswith("[dxmt9-effect-issued-geometry ")]
                assert len(geometry) == 3
                assert all(row["index_readable"] == row["stream0_readable"] == "1"
                           for row in geometry)
                extra_geometry[altered, route] = [
                    (row["index_hash"], row["stream0_hash"]) for row in geometry]
                for i, row in enumerate(rows):
                    assert row["stream"] == "1" and row["metal_slot"] == "6"
                    assert row["binding_known"] == row["readable"] == "1"
                    assert row["stride"] == "28" and row["divisor"] == "0"
                    assert int(row["binding_offset"]) == 12 * 28
                    assert int(row["min_element"]) == i * 4
                    assert int(row["max_element"]) == i * 4 + 3
                    first, size = int(row["start_byte"]), int(row["byte_count"])
                    assert size == 4 * 28 and first == (12 + i * 4) * 28
                    assert row["hash"] == fnv(raw[first:first + size])
                    assert row["layout_scope"] == "expected-declaration-stride"
                extra_witnesses[altered, route] = rows
            if not altered:
                # Run the existing off/filter negatives against a genuinely
                # used extra stream, not a fixture that cannot emit its row.
                bundle = extra_bundle
        for route in ("compatibility", "production-slot"):
            assert extra_geometry[False, route] == extra_geometry[True, route]
            assert all(a["hash"] != b["hash"] for a, b in zip(
                extra_witnesses[False, route], extra_witnesses[True, route]))
        assert extra_inputs[False][:12 * 28] == extra_inputs[True][:12 * 28]
        for name, overrides in (
            ("issued-filtered", {"DXMT9_EFFECT_DRAW_TRACE_PRIMITIVE_COUNT": "1"}),
            ("issued-master-off", {"DXMT9_EFFECT_DRAW_TRACE": "0"}),
            ("issued-bindings-off", {"DXMT9_EFFECT_DRAW_TRACE_BINDINGS": "0"}),
            ("issued-geometry-off", {"DXMT9_EFFECT_DRAW_TRACE_GEOMETRY": "0"}),
            ("issued-capture-missing", {"DXMT9_EFFECT_DRAW_TRACE_CAPTURE_SOURCES_ONLY": "1"}),
        ):
            filtered = root / name
            run(sys.executable, str(runner), "experiment", str(bundle),
                "--provider", str(provider), "--validator", str(validator),
                "--warmup", "0", "--repeat", "1", "--require-path",
                "--replay-route", "production-slot", "--output-dir", str(filtered),
                expected=1, env=dict(environment, **overrides))
            filtered_report = json.loads((filtered / "result.json").read_text())
            assert filtered_report["result"]["iterations"][0]["provider_result"]["validity"]["expected_digest_matched"]
            assert "[dxmt9-effect-issued-geometry " not in (filtered / "provider.stderr").read_text()
            assert "[dxmt9-effect-issued-raster " not in (filtered / "provider.stderr").read_text()
            assert "[dxmt9-effect-issued-stream " not in (filtered / "provider.stderr").read_text()
            assert "[dxmt9-effect-issued-sampling " not in (filtered / "provider.stderr").read_text()
            assert "[dxmt9-effect-issued-depth " not in (filtered / "provider.stderr").read_text()
            assert "_issued_binding=" not in (filtered / "provider.stderr").read_text()
        compiled = shaders / "compiled"
        receipts = sorted(compiled.glob("attempt-*.json"))
        assert receipts, "the actual Metal compiler seam must execute"
        attempts = set()
        compiled_sources = set()
        for path in receipts:
            row = json.loads(path.read_text())
            assert row["schema"] == "dxmt9.metal_shader_compilation.v1"
            assert row["compile_options"] == {
                "profile_version": 1, "language_version": 131076,
                "math_mode": 2, "math_functions": 0, "preserve_invariance": False}
            assert row["success"] is True
            assert row["architecture"] in ("arm64", "x86_64")
            assert row["runtime_os"] and row["device_name"]
            identity = row["process_id"], row["attempt"]
            assert identity not in attempts
            attempts.add(identity)
            source_bytes = (compiled / (row["source_sha256"] + ".metal")).read_bytes()
            assert len(source_bytes) == row["source_bytes"]
            assert hashlib.sha256(source_bytes).hexdigest() == row["source_sha256"]
            compiled_sources.add(source_bytes)
        # At least one translated shader in each stage actually reached Metal;
        # regenerated attribution source alone is not compilation evidence.
        for stage in ("translated-vs", "translated-fs"):
            translated = list(shaders.glob(stage + "-*.metal"))
            assert any(path.read_bytes() in compiled_sources for path in translated), stage
        assert "[dxmt9-compiled-shader]" not in (output / "provider.stderr").read_text()
    except BaseException:
        print(f"Compilation-lineage failure artifacts retained: {root}", file=sys.stderr)
        raise
    else:
        shutil.rmtree(root)
    print("render_tape_compilation_lineage passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
