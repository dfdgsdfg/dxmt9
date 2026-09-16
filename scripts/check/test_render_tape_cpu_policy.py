#!/usr/bin/env python3
"""Native-free tests for the CPU policy qualification boundary."""

from __future__ import annotations

import copy
import argparse
import hashlib
import importlib.util
import json
import pathlib
import subprocess
import tempfile
from unittest import mock


def _load():
    path = pathlib.Path(__file__).resolve().parents[1] / "tools" / "render_tape_cpu_policy.py"
    spec = importlib.util.spec_from_file_location("render_tape_cpu_policy_tested", path)
    module = importlib.util.module_from_spec(spec)
    assert spec and spec.loader
    spec.loader.exec_module(module)
    return module


def _inspection():
    return {
        "schema": "dxmt9.render_tape.v2", "valid": True,
        "events": 1, "objects": 1, "mutations": 0, "chunks": 1,
        "records": 1, "draws": 1, "record_types": {"generate_mipmaps": 0},
        "event_types": [5],
    }


def _native(module, policy, mode="verify", repeat=2, source=1):
    measurement = mode == "measure"
    rows = []
    for index in range(repeat):
        rows.append({
            "index": index, "complete": True, "reset_ok": True,
            "failure": "", "fault_triggered": False,
            "full_oracle_calls": 0 if measurement else 1,
            "semantic_digest": "0" if measurement else "11",
            "draw_observations": [] if measurement else [{
                "ordinal": 1, "publication_event": 1,
                "cumulative_semantic_digest": "11"}],
            "logical_resource_oracle_complete": not measurement,
            "physical_texture_oracle": False, "draw_count": 1,
            "resources_created": 1, "resources_released": 1,
            "submitted": 1, "completed": 1, "reclaimed": 1,
            "source_summary_commands": source, "source_summary_draws": source,
            "direct_draws": 1 if policy["replay_route"] == "production-slot" else 0,
            "compatibility_draws": 0 if policy["replay_route"] == "production-slot" else 1,
            "slot_provisioned": 1 if policy["slot_headroom_bytes"] else 0,
            "slot_provision_reused": 0, "slot_provision_failed": 0,
            "capacity_lease_denied": 0, "draw_limit_publishes": 0,
            "phase_ns": {"setup": 0 if measurement else 1,
                         "replay": 0 if measurement else 1,
                         "observe": 0 if measurement else 1,
                         "completion": 0 if measurement else 1,
                         "cleanup": 0 if measurement else 1},
            "native_cpu_ns": 2, "elapsed_ns": 3,
            "command_count": 1, "pending_peak": 1, "deferred_releases": 0,
            "destroys_with_pending": 0, "retained_snapshots_after_destroy": 0,
            "mipmap_command_count": 0,
            "buffer_backing_oracle_complete": not measurement,
            "buffer_backing_observed": not measurement,
            "buffer_snapshot_observations": 0 if measurement else 1,
            "buffer_live_binding_observations": 0,
            "buffer_binding_observations": 0 if measurement else 1,
            "buffer_backing_oracle": "not-applicable" if measurement else "real-metal-cpu-visible",
            "trace": [{"iteration": index + 1, "submission": 1, "source": 1,
                       "source_generation": 1, "storage_first_page": 0,
                       "storage_page_count": 1, "storage_generation": 1,
                       "first_event": 1, "last_event": 1, "event": 1,
                       "event_submission": 1, "released_at_event": 1,
                       "submitted": True, "completed": True}],
        })
    return {
        "schema": "dxmt9.render_tape.cpu_experiment.v1",
        "grammar": "general-frame", "semantic_projection": "final-slot-general-v2",
        "attachment_contents": "backend-command-owned-v1", "gpu_readback": False,
        "backend": "cpu-metal-resources",
        "resource_execution": "cpu-visible-metal-no-gpu-submit", "status": "complete",
        "reset_policy": "fresh-device", "cache_policy": "fresh-runtime",
        "run_mode": mode, "phase_accounting": "iteration-only" if measurement else "scoped-observations",
        "runtime_perf_counters": True, "replay_route": policy["replay_route"],
        "publication": "per-command", "completion": "after-command", "repeat": repeat,
        "fault": "none", "max_pending": 1, "immutable_input_owned": True,
        "deterministic": True, "shared_native_resource_device": True,
        "native_cpu_clock": "process", "completion_plan_sha256": None,
        "cpu_policy": policy,
        "effective_cpu_policy": {"reset": "fresh-device", "cpu_ready_arena": False,
                                  "draw_command_limit": policy["draw_command_limit"],
                                  "draw_payload_limit": 0, "early_prefix": False,
                                  "gpu_submit": False},
        "input_counts": {"events": 1, "objects": 1, "mutations": 0,
                         "chunks": 1, "records": 1, "draws": 1},
        "iterations": rows,
    }


def _artifact(result, *, accepted, image_path):
    image = image_path.read_bytes()
    digest = hashlib.sha256(image).hexdigest()
    return {
        "schema": "dxmt9.render_tape.cpu_experiment_artifact.v1",
        "accepted": accepted, "execution_valid": True, "provider_exit_code": 0,
        "semantic_qualified": accepted, "diagnostic": False,
        "metal_validation_diagnostic": False,
        "loaded_images_stable": True, "gpu_evidence": False,
        "provider": {"path": "/provider", "sha256": "a" * 64},
        "validator": {"path": "/validator", "sha256": "b" * 64},
        "input_components": {"events": {"path": "events.bin", "bytes": 1,
                                           "sha256": "c" * 64}, "blobs": []},
        "input_inspection": _inspection(), "renderer_environment": {},
        "forced_environment": {"DXMT_PERF_COUNTERS": "1"},
        "result": {**result, "loaded_images": [{"path": str(image_path),
                                                   "bytes": len(image), "sha256": digest}]},
    }


def main() -> int:
    module = _load()
    compatibility = module.default_policy("compatibility")
    batch4 = {**compatibility, "draw_command_limit": 4}
    production = module.default_policy("production-slot")
    headroom = {**production, "slot_headroom_bytes": 1 << 20,
                "aggregate_headroom_bytes": 1 << 20}
    assert module.validate_policy(batch4) == batch4
    assert module.validate_policy(headroom) == headroom
    for broken in (
            {**compatibility, "draw_command_limit": True},
            {**compatibility, "slot_headroom_bytes": 1.0},
            {**compatibility, "unknown": 1},
            {**production, "slot_headroom_bytes": 1 << 20,
             "aggregate_headroom_bytes": 0}):
        try:
            module.validate_policy(broken)
        except module.PolicyError:
            pass
        else:
            raise AssertionError("malformed policy accepted")

    inspection = _inspection()
    # Exercise the real shared native-result gate; no predicate bypass.
    def receipt_contract_cases():
        verified = _native(module, batch4, repeat=2)
        assert module.verify_receipt(
            verified, batch4, repeat=2, input_inspection=inspection)[0]
        for field, value in (("attachment_contents", None),
                             ("attachment_contents", "cpu-shadow"),
                             ("gpu_readback", None), ("gpu_readback", True)):
            stale = copy.deepcopy(verified)
            if value is None:
                stale.pop(field)
            else:
                stale[field] = value
            assert not module.verify_receipt(
                stale, batch4, repeat=2, input_inspection=inspection)[0]
        broken = copy.deepcopy(verified)
        broken["iterations"][0]["full_oracle_calls"] = 0
        assert not module.verify_receipt(
            broken, batch4, repeat=2, input_inspection=inspection)[0]
        changed = copy.deepcopy(verified)
        changed["iterations"][0]["draw_observations"][0][
            "cumulative_semantic_digest"] = "12"
        assert not module.compare_verification(verified, changed, repeat=2)["accepted"]
        changed = copy.deepcopy(verified)
        changed["iterations"][0]["source_summary_commands"] = 99
        assert module.compare_verification(verified, changed, repeat=2)["accepted"]

        measured = _native(module, batch4, mode="measure", repeat=3, source=1)
        ok, _, rows = module.measure_receipt(
            measured, batch4, total=3, warmup=1, verification=verified,
            input_inspection=inspection)
        assert ok and len(rows) == 2
        forged = copy.deepcopy(measured)
        forged["iterations"][0]["full_oracle_calls"] = 1
        assert not module.measure_receipt(
            forged, batch4, total=3, warmup=1, verification=verified,
            input_inspection=inspection)[0]

        with tempfile.TemporaryDirectory() as temporary:
            image = pathlib.Path(temporary) / "image.metallib"
            image.write_bytes(b"image")
            verification_artifact = _artifact(verified, accepted=True, image_path=image)
            measurement_artifact = _artifact(measured, accepted=False, image_path=image)
            assert module.verify_receipt(verification_artifact, batch4, repeat=2)[0]
            assert module.execution_receipt(measurement_artifact, batch4, repeat=3)[0]
            assert module.bindings_match(
                verification_artifact, measurement_artifact, batch4)
            missing = copy.deepcopy(measurement_artifact)
            del missing["validator"]
            assert not module.bindings_match(
                verification_artifact, missing, batch4)
            failed = copy.deepcopy(measurement_artifact)
            failed["execution_valid"] = False
            assert not module.execution_receipt(failed, batch4, repeat=3)[0]
            for field in ("input_components", "provider", "validator", "renderer_environment",
                          "forced_environment", "input_inspection"):
                broken = copy.deepcopy(measurement_artifact)
                del broken[field]
                assert not module.bindings_match(verification_artifact, broken, batch4), field
            for field, value in (("cache_policy", "warm"), ("reset_policy", "warm"),
                                 ("run_mode", "verify")):
                broken = copy.deepcopy(measurement_artifact)
                broken["result"][field] = value
                assert not module.measure_receipt(
                    broken, batch4, total=3, warmup=1,
                    verification=verification_artifact)[0], field
            for field in ("submitted", "source_summary_commands", "slot_provisioned"):
                broken = copy.deepcopy(measurement_artifact)
                broken["result"]["iterations"][0][field] += 1
                assert not module.measure_receipt(
                    broken, batch4, total=3, warmup=1,
                    verification=verification_artifact)[0], field
            # A mismatched verification pair must not start measurement at all.
            root = pathlib.Path(temporary)
            bundle = root / "input"
            bundle.mkdir()
            (bundle / "events.bin").write_bytes(b"owned test input")
            policy_file = root / "policy.json"
            policy_file.write_bytes(module.canonical_policy(batch4))
            calls = []
            def verification_child(command, timeout):
                assert command[command.index("--run-mode") + 1] == "verify"
                result = copy.deepcopy(verification_artifact)
                if calls:
                    result["provider"]["sha256"] = "d" * 64
                calls.append(command)
                output = pathlib.Path(command[command.index("--output-dir") + 1])
                output.mkdir(parents=True)
                (output / "result.json").write_text(json.dumps(result))
                return result
            args = argparse.Namespace(bundle=bundle, reference_policy=policy_file,
                candidate_policy=policy_file, repeat=2, warmup=1,
                output_dir=root / "pair-rejected", provider=root / "unused-provider",
                validator=root / "unused-validator", timeout_seconds=1)
            with mock.patch.object(module, "_child", side_effect=verification_child):
                assert module.cpu_policy_compare(args) == 1
            assert len(calls) == 2
            assert not (args.output_dir / "measure").exists()
            rejected = json.loads((args.output_dir / "result.json").read_text())
            assert rejected["accepted"] is False and rejected["measurement"] == {}
            image.write_bytes(b"changed")
            assert not module.bindings_match(verification_artifact, measurement_artifact, batch4)
    receipt_contract_cases()

    with tempfile.TemporaryDirectory() as temporary:
        policy = pathlib.Path(temporary) / "policy.json"
        policy.write_text(json.dumps({**compatibility, "schema": "stale"}))
        try:
            module.load_policy(policy)
        except module.PolicyError:
            pass
        else:
            raise AssertionError("stale policy accepted")
        timed_out = pathlib.Path(temporary) / "timeout"
        with mock.patch.object(module.subprocess, "run", side_effect=subprocess.TimeoutExpired(
                ["child"], 1, output=b"partial output", stderr=b"partial error")):
            receipt = module._child(["child", "--output-dir", str(timed_out)], 1)
        assert not receipt["accepted"] and receipt["compare_wrapper_timeout"]
        assert (timed_out / "command.stdout").read_bytes() == b"partial output"
        assert (timed_out / "command.stderr").read_bytes() == b"partial error"
        assert json.loads((timed_out / "result.json").read_text())["accepted"] is False
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
