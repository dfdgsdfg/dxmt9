#!/usr/bin/env python3
"""End-to-end synthetic Render Tape GPU policy qualification contract."""

from __future__ import annotations

import csv
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
from contextlib import contextmanager


@contextmanager
def fixture_artifacts():
    root = pathlib.Path(tempfile.mkdtemp(prefix="dxmt9-render-tape-gpu-policy-"))
    try:
        yield root
    except BaseException:
        # Failed GPU bytes and receipts are counterexamples, not scratch data.
        print(f"GPU policy failure artifacts retained: {root}", file=sys.stderr)
        raise
    else:
        shutil.rmtree(root)


def run(*command: str, expected: int = 0,
        env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, check=False, capture_output=True,
                            text=True, timeout=180, env=env)
    assert result.returncode == expected, (
        command, result.returncode, result.stdout, result.stderr)
    return result


def digest(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read(path: pathlib.Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def assert_readback(path: pathlib.Path, oracle: pathlib.Path) -> None:
    assert path.is_file() and not path.is_symlink()
    assert path.read_bytes() == oracle.read_bytes()
    assert path.stat().st_size == oracle.stat().st_size
    assert digest(path) == digest(oracle)


def run_compare(command: list[str], output: pathlib.Path) -> None:
    result = subprocess.run(command, check=False, capture_output=True,
                            text=True, timeout=180)
    if result.returncode != 0:
        details = {"command": command, "returncode": result.returncode,
                   "stdout": result.stdout, "stderr": result.stderr}
        report_path = output / "result.json"
        if report_path.is_file():
            report = read(report_path)
            details["failures"] = report.get("failures")
            details["gpu_qualification"] = report.get("gpu_qualification")
            details["measurement"] = report.get("measurement")
            details["gpu_children"] = {}
            for side in ("reference", "candidate"):
                child_path = output / "gpu" / side / "result.json"
                if child_path.is_file():
                    child = read(child_path)
                    details["gpu_children"][side] = {
                        key: value for key, value in child.items()
                        if key in ("accepted", "provider_exit_code", "diagnostic",
                                   "loaded_images_stable", "readback_artifacts_valid",
                                   "validator_stable")
                    }
                    details["gpu_children"][side]["iterations"] = [
                        {"validity": row.get("provider_result", {}).get("validity"),
                         "path": row.get("path_counters")}
                        for row in child.get("result", {}).get("iterations", [])
                    ]
        raise AssertionError(details)


def texture_input_audit_cases(root, fixture, provider, validator, runner):
    source, bundle = root / "texture-source", root / "texture-bundle"
    run(str(fixture), "--write-texture-input-audit-fixture", str(source))
    pack = [sys.executable, str(runner), "pack", "--events", str(source / "events.bin"),
            "--output-dir", str(bundle), "--validator", str(validator), "--test-input",
            "--identity", str(source / "identity.bin"),
            "--output-rgba", str(source / "output.rgba"),
            "--source-rgba", str(source / "source.rgba")]
    for blob in sorted(source.glob("*.bin")):
        if blob.name not in ("events.bin", "identity.bin"):
            pack.extend(("--blob", str(blob)))
    run(*pack)
    command = [sys.executable, str(runner), "experiment", str(bundle),
               "--provider", str(provider), "--validator", str(validator),
               "--warmup", "0", "--repeat", "1", "--reset", "cold-device",
               "--replay-route", "compatibility", "--source-boundary-mode", "capture-source"]
    baseline = root / "texture-baseline"
    # Discover the fixture's authenticated plan; omission is not qualification.
    run(*command, "--output-dir", str(baseline), expected=1)
    receipt = read(baseline / "result.json")
    assert not receipt["accepted"] and not receipt["diagnostic"]
    assert receipt["provider_exit_code"] == 0
    plan = receipt["result"]["iterations"][0]["provider_result"]["source_replay"]["plan_sha256"]
    qualified = root / "texture-baseline-qualified"
    run(*command, "--output-dir", str(qualified), "--source-plan-sha256", plan)
    assert read(qualified / "result.json")["accepted"]
    data = source / "intervention.r32f"
    before = data.read_bytes()
    argument = f"9:43:1:{digest(data)}:{data}"
    output = root / "texture-intervention"
    run(*command, "--output-dir", str(output), "--source-plan-sha256", plan,
        "--audit-texture-input", argument, expected=1)
    receipt = read(output / "result.json")
    assert receipt["diagnostic"] and not receipt["accepted"]
    assert receipt["audit_texture_input"] and receipt["provider_exit_code"] == 0
    assert receipt["loaded_images_stable"] and receipt["readback_artifacts_valid"]
    assert data.read_bytes() == before == (output / "texture-input-audit.r32f").read_bytes()
    row, = receipt["result"]["iterations"]
    audit = row["provider_result"]["texture_input_audit"]
    assert all(audit[key] for key in ("requested", "validated", "attempted",
                                     "uploaded", "readback_matched"))
    assert audit["generation"] == 9 and audit["object_id"] == 43
    assert audit["after_captured_seq"] == 1 and audit["bytes"] == 16
    assert audit["before_sha256"] == digest(source / "audit-texture.bin")
    assert audit["after_sha256"] == audit["input_sha256"] == digest(data)
    assert row["provider_result"]["validity"]["expected_digest_matched"]
    forwarded = receipt["provider_command"][receipt["provider_command"].index("--audit-texture-input") + 1]
    assert forwarded != argument and "texture-input-audit.r32f" in forwarded
    # Public and native parsers reject malformed/empty options, before effects.
    for index, invalid in enumerate(("", "0:43:1:a:path", argument.replace("9:", "4294967296:", 1),
                                      argument.replace(digest(data), "a" * 64))):
        result = subprocess.run([*command, "--output-dir", str(root / f"texture-invalid-{index}"),
                                 "--source-plan-sha256", plan, "--audit-texture-input", invalid],
                                check=False, capture_output=True, text=True, timeout=30)
        assert result.returncode != 0
        assert not (root / f"texture-invalid-{index}").exists()
    run(str(provider), "experiment", str(source / "events.bin"),
        "--audit-texture-input", "", expected=2)
    # Deliberately reproduce a publication collision: completed diagnostic
    # evidence must be quarantined, not deleted with private staging.
    collision = root / "texture-publication-collision"
    environment = dict(os.environ, DXMT9_DUMP_COLOR_ATTACHMENT_DIR=str(collision / "observer"),
                       DXMT9_DUMP_COLOR_ATTACHMENT_CAPTURE_SOURCES_ONLY="0",
                       DXMT9_DUMP_COLOR_ATTACHMENT_HASH_ONLY="1",
                       DXMT9_DUMP_COLOR_ATTACHMENT_INDEX="0",
                       DXMT9_DUMP_COLOR_ATTACHMENT_AFTER_DRAW="0",
                       DXMT9_DUMP_COLOR_ATTACHMENT_MAX_DUMPS="16")
    failed = run(*command, "--output-dir", str(collision), "--source-plan-sha256", plan,
                 "--audit-texture-input", argument, env=environment, expected=1)
    assert "Directory not empty" in failed.stderr, failed.stderr
    retained, = list(root.glob(".texture-publication-collision.rejected-*"))
    retained_receipt = read(retained / "result.json")
    assert retained_receipt["diagnostic"] and not retained_receipt["accepted"]
    assert (retained / "texture-input-audit.r32f").read_bytes() == before
    assert (retained / "readback/iteration-000000-output.rgba").is_file()


def main() -> int:
    if len(sys.argv) != 6:
        raise SystemExit(
            "usage: test_render_tape_gpu_policy_cli.py <fixture> <cpu-provider> "
            "<gpu-provider> <validator> <runner>"
        )
    fixture, cpu_provider, gpu_provider, validator, runner = (
        pathlib.Path(value).resolve() for value in sys.argv[1:]
    )
    repo = runner.parents[2]
    policies = repo / "experiments" / "render-tape-policies"
    reference_policy = read(policies / "compatibility-exact.json")
    candidate_policy = read(policies / "compatibility-batch-4.json")

    with fixture_artifacts() as root:
        texture_input_audit_cases(root, fixture, gpu_provider, validator, runner)
        source = root / "source"
        bundle = root / "bundle"
        run(str(fixture), "--write-gpu-policy-fixture", str(source))
        pack = [sys.executable, str(runner), "pack",
                "--events", str(source / "events.bin"),
                "--output-dir", str(bundle),
                "--output-rgba", str(source / "output.rgba"),
                "--source-rgba", str(source / "source.rgba"),
                "--test-input", "--validator", str(validator)]
        for blob in sorted(source.glob("*.bin")):
            if blob.name != "events.bin":
                pack.extend(("--blob", str(blob)))
        run(*pack)

        manifest = read(bundle / "manifest.json")
        assert manifest["scope"] == {
            "output_oracle": True,
            "production_capture": False,
            "production_provider_replay": False,
            "source_oracle": True,
            "test_input": True,
        }
        for name in ("output.rgba", "source.rgba"):
            assert digest(bundle / name) == digest(source / name)

        # First qualify an actual A/A pair.  Each side still runs two fresh
        # real-GPU iterations; equality of a report is not the acceptance
        # predicate.
        aa_output = root / "aa-comparison"
        run_compare(
            [sys.executable, str(runner), "cpu-policy-compare", str(bundle),
             "--provider", str(cpu_provider), "--gpu-provider", str(gpu_provider),
             "--validator", str(validator), "--reference-policy",
             str(policies / "compatibility-exact.json"), "--candidate-policy",
             str(policies / "compatibility-exact.json"), "--repeat", "2",
             "--warmup", "1", "--gpu-qualify", "--output-dir", str(aa_output)],
            aa_output)
        aa_report = read(aa_output / "result.json")
        assert aa_report["accepted"] is True
        assert aa_report["gpu_evidence"] is True
        assert aa_report["qualification_scope"] == "synthetic-test-input"
        for name in ("reference", "candidate"):
            aa_qualification = aa_report["gpu_qualification"]["policies"][name]
            assert aa_qualification["accepted"] is True
            aa_receipt = read(aa_output / aa_qualification["receipt"])
            assert aa_receipt["result"]["completion"] == "real-gpu"
            assert len(aa_receipt["result"]["iterations"]) == 2
            assert all(
                iteration["provider_result"]["coverage"][
                    "draw_indexed_primitive_records"] == 32
                for iteration in aa_receipt["result"]["iterations"]
            )
            assert aa_report["measurement"][name]["accepted"] is True

        output = root / "comparison"
        run_compare(
            [sys.executable, str(runner), "cpu-policy-compare", str(bundle),
             "--provider", str(cpu_provider), "--gpu-provider", str(gpu_provider),
             "--validator", str(validator), "--reference-policy",
             str(policies / "compatibility-exact.json"), "--candidate-policy",
             str(policies / "compatibility-batch-4.json"), "--repeat", "2",
             "--warmup", "1", "--gpu-qualify", "--output-dir", str(output)],
            output)
        report = read(output / "result.json")
        assert report["accepted"] is True, report
        assert report["gpu_evidence"] is True
        assert report["qualification_scope"] == "synthetic-test-input"
        assert report["reference_policy"] == reference_policy
        assert report["candidate_policy"] == candidate_policy

        expected_environment = {
            "DXMT9_DRAW_CHUNK_COMMAND_LIMIT": "0",
            "DXMT9_DIRECT_SLOT_HEADROOM_BYTES": "0",
            "DXMT9_DIRECT_SLOT_AGGREGATE_HEADROOM_BYTES": "0",
        }
        expected_environment_candidate = {
            **expected_environment,
            "DXMT9_DRAW_CHUNK_COMMAND_LIMIT": "4",
        }
        gpu_receipts = {}
        for name, policy, environment in (
            ("reference", reference_policy, expected_environment),
            ("candidate", candidate_policy, expected_environment_candidate),
        ):
            qualification = report["gpu_qualification"]["policies"][name]
            assert qualification["accepted"] is True
            receipt = read(output / qualification["receipt"])
            gpu_receipts[name] = receipt
            assert receipt["accepted"] is True
            assert all(receipt["forced_environment"].get(key) == value
                       for key, value in environment.items())
            requested = receipt["requested"]
            assert requested["cpu_policy"] == policy
            assert requested["warmup"] == 0 and requested["repeat"] == 2
            assert requested["reset"] == "cold-device"
            evidence = receipt["result"]
            assert evidence["completion"] == "real-gpu"
            assert len(evidence["iterations"]) == 2
            for iteration in evidence["iterations"]:
                assert iteration["warmup"] is False
                assert iteration["path_counters"]["gpu_errors"] == 0
                provider_result = iteration["provider_result"]
                assert provider_result["coverage"][
                    "draw_indexed_primitive_records"] == 32
                assert provider_result["validity"]["oracle_mode"] == "strict"
                assert len(provider_result["intervals"]) == 1
                interval = provider_result["intervals"][0]
                validity = interval["validity"]
                assert validity["expected_digest_matched"] is True
                assert validity["output_readback"] is True
                assert validity["source_readback"] is True
                assert validity["output_sha256"] == digest(bundle / "output.rgba")
                assert validity["source_sha256"] == digest(bundle / "source.rgba")
                artifacts = iteration["readback_artifacts"]
                for kind, oracle in (("output", bundle / "output.rgba"),
                                     ("source", bundle / "source.rgba")):
                    metadata = artifacts[f"{kind}_rgba"]
                    assert metadata["bytes"] == oracle.stat().st_size
                    assert metadata["sha256"] == digest(oracle)
                assert_readback(
                    output / qualification["receipt"].replace("result.json", "") /
                    "readback" / artifacts["output_rgba"]["path"],
                    bundle / "output.rgba")
                assert_readback(
                    output / qualification["receipt"].replace("result.json", "") /
                    "readback" / artifacts["source_rgba"]["path"],
                    bundle / "source.rgba")

            measured = report["measurement"][name]
            assert measured["accepted"] is True
            assert measured["iterations"] == 2
            measured_receipt = read(output / measured["receipt"])
            assert measured_receipt["accepted"] is False
            assert measured_receipt["execution_valid"] is True
            assert len(measured_receipt["result"]["iterations"]) == 3

        # The source's compatibility batch is unchanged; command-limit policy
        # splits its final Slot publications, observed here as encode chunks.
        reference_chunks = [
            iteration["path_counters"]["encode_chunks"]
            for iteration in gpu_receipts["reference"]["result"]["iterations"]
        ]
        candidate_chunks = [
            iteration["path_counters"]["encode_chunks"]
            for iteration in gpu_receipts["candidate"]["result"]["iterations"]
        ]
        assert all(type(value) is int and value > 0
                   for value in reference_chunks + candidate_chunks)
        assert min(candidate_chunks) > max(reference_chunks)

        # A tampered original oracle must fail before GPU qualification and
        # therefore cannot create a measurement child or measurement receipt.
        tampered_bundle = root / "tampered-bundle"
        shutil.copytree(bundle, tampered_bundle)
        tampered = bytearray((tampered_bundle / "output.rgba").read_bytes())
        tampered[0] ^= 0x01
        (tampered_bundle / "output.rgba").write_bytes(tampered)
        tampered_output = root / "tampered-output"
        rejected = subprocess.run(
            [sys.executable, str(runner), "cpu-policy-compare", str(tampered_bundle),
             "--provider", str(cpu_provider), "--gpu-provider", str(gpu_provider),
             "--validator", str(validator), "--reference-policy",
             str(policies / "compatibility-exact.json"), "--candidate-policy",
             str(policies / "compatibility-batch-4.json"), "--repeat", "2",
             "--warmup", "1", "--gpu-qualify", "--output-dir", str(tampered_output)],
            check=False, capture_output=True, text=True, timeout=60)
        assert rejected.returncode != 0
        assert not tampered_output.exists() or not any(
            path.name == "measure" for path in tampered_output.rglob("measure"))

        # A separate visible-uniform fixture proves the GPU gate with three
        # real indexed draws whose A/B/A pixel regions come from PS constants
        # and whose VS transform generation is also exercised.  The original
        # patterned-clear fixture remains the bounded policy control above.
        visible_source = root / "visible-uniform-source"
        visible_bundle = root / "visible-uniform-bundle"
        run(str(fixture), "--write-gpu-visible-uniform-fixture",
            str(visible_source))
        visible_pack = [sys.executable, str(runner), "pack",
                        "--events", str(visible_source / "events.bin"),
                        "--output-dir", str(visible_bundle),
                        "--output-rgba", str(visible_source / "output.rgba"),
                        "--source-rgba", str(visible_source / "source.rgba"),
                        "--test-input", "--validator", str(validator)]
        for blob in sorted(visible_source.glob("*.bin")):
            if blob.name != "events.bin":
                visible_pack.extend(("--blob", str(blob)))
        run(*visible_pack)

        visible_compatibility = root / "visible-uniform-compatibility"
        run_compare(
            [sys.executable, str(runner), "cpu-policy-compare",
             str(visible_bundle), "--provider", str(cpu_provider),
             "--gpu-provider", str(gpu_provider), "--validator", str(validator),
             "--reference-policy", str(policies / "compatibility-exact.json"),
             "--candidate-policy", str(policies / "compatibility-exact.json"),
             "--repeat", "1", "--warmup", "0", "--gpu-qualify",
             "--output-dir", str(visible_compatibility)], visible_compatibility)
        compatibility_report = read(visible_compatibility / "result.json")
        assert compatibility_report["accepted"] is True
        compatibility_receipt = read(
            visible_compatibility /
            compatibility_report["gpu_qualification"]["policies"]["reference"][
                "receipt"])
        compatibility_iteration = compatibility_receipt["result"]["iterations"][0]
        assert compatibility_iteration["provider_result"]["coverage"][
            "draw_indexed_primitive_records"] == 3
        assert compatibility_iteration["provider_result"]["validity"][
            "expected_digest_matched"] is True
        assert_readback(
            visible_compatibility /
            compatibility_report["gpu_qualification"]["policies"]["reference"][
                "receipt"].replace("result.json", "") /
            "readback" /
            compatibility_iteration["readback_artifacts"]["output_rgba"]["path"],
            visible_bundle / "output.rgba")

        visible_production = root / "visible-uniform-production"
        run_compare(
            [sys.executable, str(runner), "cpu-policy-compare",
             str(visible_bundle), "--provider", str(cpu_provider),
             "--gpu-provider", str(gpu_provider), "--validator", str(validator),
             "--reference-policy", str(policies / "production-exact.json"),
             "--candidate-policy", str(policies / "production-headroom-5m.json"),
             "--repeat", "1", "--warmup", "0", "--gpu-qualify",
             "--output-dir", str(visible_production)], visible_production)
        production_report = read(visible_production / "result.json")
        assert production_report["accepted"] is True
        for name in ("reference", "candidate"):
            receipt = read(
                visible_production /
                production_report["gpu_qualification"]["policies"][name]["receipt"])
            iteration = receipt["result"]["iterations"][0]
            assert iteration["provider_result"]["coverage"][
                "draw_indexed_primitive_records"] == 3
            assert iteration["provider_result"]["validity"][
                "expected_digest_matched"] is True

        # A vertex probe path independently taints the run, even if its master
        # dependencies disable actual probing. Literal "0" is a directory.
        for index, (probe_variable, probe_path) in enumerate((
                ("DXMT9_PROBE_VERTEX_POSITION_DIR", "0"),
                ("DXMT9_PROBE_VERTEX_POSITION_DIR", str(root / "vertex-probe")),
                ("DXMT9_PROBE_NO_CONTRACT_SOURCE_SHA256", "0"),
                ("DXMT9_PROBE_NO_CONTRACT_SOURCE_SHA256", "a" * 64))):
            probe_output = root / f"visible-uniform-vertex-probe-{index}"
            run(sys.executable, str(runner), "experiment", str(visible_bundle),
                "--provider", str(gpu_provider), "--validator", str(validator),
                "--output-dir", str(probe_output), "--warmup", "0", "--repeat", "1",
                "--reset", "cold-device", "--replay-route", "production-slot",
                "--require-path", expected=1, env={
                    **os.environ, probe_variable: probe_path,
                    "DXMT9_EFFECT_DRAW_TRACE": "0", "DXMT_DUMP_SHADER_DIR": "",
                })
            receipt = read(probe_output / "result.json")
            assert receipt["diagnostic"] and not receipt["accepted"]
            assert receipt["provider_exit_code"] == 0
            assert receipt["result"]["iterations"][0]["provider_result"]["validity"][
                "expected_digest_matched"]

        # A summary-only readback is still diagnostic even with no raw dump
        # path.  Prove the observer actually ran and preserved correct pixels;
        # pixel failure must not be what rejects this receipt.
        roi_output = root / "visible-uniform-roi"
        roi_csv = root / "visible-uniform-roi.csv"
        run(sys.executable, str(runner), "experiment", str(visible_bundle),
            "--provider", str(gpu_provider), "--validator", str(validator),
            "--output-dir", str(roi_output), "--warmup", "0", "--repeat", "1",
            "--reset", "cold-device", "--replay-route", "production-slot",
            "--require-path", expected=1, env={
                **os.environ,
                "DXMT9_DUMP_COLOR_ATTACHMENT_ROI_SUMMARY_PATH": str(roi_csv),
                "DXMT9_DUMP_COLOR_ATTACHMENT_ROIS": "1,1,2,2:pixel",
                "DXMT9_DUMP_COLOR_ATTACHMENT_INDEX": "0",
                "DXMT9_DUMP_COLOR_ATTACHMENT_AFTER_DRAW": "0",
            })
        roi_receipt = read(roi_output / "result.json")
        assert roi_receipt["accepted"] is False
        assert roi_receipt["diagnostic"] is True
        assert roi_receipt["provider_exit_code"] == 0
        roi_iteration = roi_receipt["result"]["iterations"][0]
        assert roi_iteration["provider_result"]["validity"][
            "expected_digest_matched"] is True
        assert roi_iteration["path_counters"]["direct_lease_draws"] == 3
        assert roi_iteration["path_counters"]["gpu_errors"] == 0
        with roi_csv.open(newline="") as stream:
            roi_rows = list(csv.DictReader(stream))
        assert roi_rows and all(row["pixels"] == "1" and
                                row["after_draw"] == "0" and
                                row["readback_width"] == "1" and
                                row["readback_height"] == "1"
                                for row in roi_rows)

        for selected_frame in ("1", "2", "invalid"):
            frame_output = root / f"frame-dump-{selected_frame}"
            frame_dump = root / f"frame-dump-{selected_frame}-observer"
            run(sys.executable, str(runner), "experiment", str(visible_bundle),
                "--provider", str(gpu_provider), "--validator", str(validator),
                "--output-dir", str(frame_output), "--warmup", "0", "--repeat", "1",
                "--reset", "cold-device", "--replay-route", "production-slot",
                "--require-path", expected=1, env={
                    **os.environ,
                    "DXMT9_DUMP_COLOR_ATTACHMENT_DIR": str(frame_dump),
                    "DXMT9_DUMP_COLOR_ATTACHMENT_INDEX": "0",
                    "DXMT9_DUMP_COLOR_ATTACHMENT_FRAME": selected_frame,
                    "DXMT9_DUMP_COLOR_ATTACHMENT_MAX_DUMPS": "1",
                    "DXMT9_DUMP_COLOR_ATTACHMENT_AFTER_DRAW": "0",
                })
            frame_receipt = read(frame_output / "result.json")
            assert frame_receipt["diagnostic"] and not frame_receipt["accepted"]
            assert frame_receipt["provider_exit_code"] == 0
            frame_iteration = frame_receipt["result"]["iterations"][0]
            assert frame_iteration["provider_result"]["validity"][
                "expected_digest_matched"] is True
            assert frame_iteration["path_counters"]["gpu_errors"] == 0
            dumps = list(frame_dump.glob("*.bin.json"))
            if selected_frame == "1":
                assert len(dumps) == 1
                assert read(dumps[0])["encodedFrame"] == 1
                raw_dump_path = pathlib.Path(str(dumps[0]).removesuffix(".json"))
                raw_dump_meta = read(dumps[0])
                assert raw_dump_meta["rawBytesRetained"] is True
                assert raw_dump_meta["byteCount"] == raw_dump_path.stat().st_size
                assert raw_dump_meta["sha256"] == digest(raw_dump_path)
            else:
                assert not dumps, "unreached or malformed frame must not broaden capture"

        for label, hash_only, budget, admitted in (
            ("hash", "1", "257", True),
            ("hash-capture-only", "1", "257", False),
            ("hash-unbounded", "1", None, False),
            ("hash-over-budget", "1", "16385", False),
            ("hash-malformed", "invalid", "1", False),
            ("raw-over-budget", "0", "257", False),
        ):
            hash_output = root / label
            hash_dump = root / f"{label}-observer"
            hash_environment = {
                **os.environ,
                "DXMT9_DUMP_COLOR_ATTACHMENT_DIR": str(hash_dump),
                "DXMT9_DUMP_COLOR_ATTACHMENT_INDEX": "0",
                "DXMT9_DUMP_COLOR_ATTACHMENT_AFTER_DRAW": "0",
                "DXMT9_DUMP_COLOR_ATTACHMENT_HASH_ONLY": hash_only,
                "DXMT9_DUMP_COLOR_ATTACHMENT_CAPTURE_SOURCES_ONLY":
                    "1" if label == "hash-capture-only" else "0",
            }
            hash_environment.pop("DXMT9_DUMP_COLOR_ATTACHMENT_MAX_DUMPS", None)
            if budget is not None:
                hash_environment["DXMT9_DUMP_COLOR_ATTACHMENT_MAX_DUMPS"] = budget
            run(sys.executable, str(runner), "experiment", str(visible_bundle),
                "--provider", str(gpu_provider), "--validator", str(validator),
                "--output-dir", str(hash_output), "--warmup", "0", "--repeat", "1",
                "--reset", "cold-device", "--replay-route", "production-slot",
                "--require-path", expected=1, env=hash_environment)
            hash_receipt = read(hash_output / "result.json")
            assert hash_receipt["diagnostic"] and not hash_receipt["accepted"]
            assert hash_receipt["provider_exit_code"] == 0
            hash_iteration = hash_receipt["result"]["iterations"][0]
            assert hash_iteration["provider_result"]["validity"][
                "expected_digest_matched"] is True
            assert hash_iteration["path_counters"]["direct_lease_draws"] == 3
            assert hash_iteration["path_counters"]["gpu_errors"] == 0
            hash_dumps = list(hash_dump.glob("*.bin.json"))
            assert not list(hash_dump.glob("*.bin")), "hash-only must retain no raw images"
            if admitted:
                assert len(hash_dumps) == 1
                hash_meta = read(hash_dumps[0])
                assert hash_meta["rawBytesRetained"] is False
                for field in ("sha256", "byteCount", "rowBytes", "width", "height",
                              "format", "metalPixelFormat"):
                    assert hash_meta[field] == raw_dump_meta[field], field
            else:
                assert not hash_dumps, "malformed budget must not broaden observation"

        altered_source = root / "visible-uniform-altered-source"
        altered_bundle = root / "visible-uniform-altered-bundle"
        run(str(fixture), "--write-gpu-visible-uniform-altered-fixture",
            str(altered_source))
        altered_pack = [sys.executable, str(runner), "pack",
                        "--events", str(altered_source / "events.bin"),
                        "--output-dir", str(altered_bundle),
                        "--output-rgba", str(altered_source / "output.rgba"),
                        "--source-rgba", str(altered_source / "source.rgba"),
                        "--test-input", "--validator", str(validator)]
        for blob in sorted(altered_source.glob("*.bin")):
            if blob.name != "events.bin":
                altered_pack.extend(("--blob", str(blob)))
        run(*altered_pack)
        altered_output = root / "visible-uniform-altered-comparison"
        altered_result = subprocess.run(
            [sys.executable, str(runner), "cpu-policy-compare",
             str(altered_bundle), "--provider", str(cpu_provider),
             "--gpu-provider", str(gpu_provider), "--validator", str(validator),
             "--reference-policy", str(policies / "compatibility-exact.json"),
             "--candidate-policy", str(policies / "compatibility-exact.json"),
             "--repeat", "1", "--warmup", "0", "--gpu-qualify",
             "--output-dir", str(altered_output)],
            check=False, capture_output=True, text=True, timeout=180)
        assert altered_result.returncode != 0

        stale_vs_source = root / "visible-uniform-stale-vs-source"
        stale_vs_bundle = root / "visible-uniform-stale-vs-bundle"
        run(str(fixture), "--write-gpu-visible-uniform-stale-vs-fixture",
            str(stale_vs_source))
        stale_vs_pack = [sys.executable, str(runner), "pack",
                         "--events", str(stale_vs_source / "events.bin"),
                         "--output-dir", str(stale_vs_bundle),
                         "--output-rgba", str(stale_vs_source / "output.rgba"),
                         "--source-rgba", str(stale_vs_source / "source.rgba"),
                         "--test-input", "--validator", str(validator)]
        for blob in sorted(stale_vs_source.glob("*.bin")):
            if blob.name != "events.bin":
                stale_vs_pack.extend(("--blob", str(blob)))
        run(*stale_vs_pack)
        stale_vs_output = root / "visible-uniform-stale-vs-comparison"
        stale_vs_result = subprocess.run(
            [sys.executable, str(runner), "cpu-policy-compare",
             str(stale_vs_bundle), "--provider", str(cpu_provider),
             "--gpu-provider", str(gpu_provider), "--validator", str(validator),
             "--reference-policy", str(policies / "production-exact.json"),
             "--candidate-policy", str(policies / "production-exact.json"),
             "--repeat", "1", "--warmup", "0", "--gpu-qualify",
             "--output-dir", str(stale_vs_output)],
            check=False, capture_output=True, text=True, timeout=180)
        assert stale_vs_result.returncode != 0
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
