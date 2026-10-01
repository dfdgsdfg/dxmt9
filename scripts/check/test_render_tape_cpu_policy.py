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
        "grammar": {"descriptor_kinds": {}, "record_types": {}, "control_kinds": {}},
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


def _query_native(module, policy, *, foreign_second_cycle=False):
    """Two Ready boundaries with identical aggregate drain accounting."""
    path = pathlib.Path(__file__).with_name("test_render_tape_query_receipts.py")
    spec = importlib.util.spec_from_file_location("cpu_policy_query_fixture", path)
    assert spec and spec.loader
    fixture_module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fixture_module)
    inspection, query = fixture_module.fixture(metal=False)
    identity = inspection["query_transcript"]["definitions"][0]["identity"]
    second_identity = ({**identity, "object_id": "43"}
                       if foreign_second_cycle else identity)
    inspection.update(events=22, objects=11, mutations=0, chunks=6, records=6, draws=2,
                      event_types=[2] * 11 + [5, 5, 5, 6, 6, 5, 5, 6, 3, 5, 7],
                      cpu_window_plan={"sources": [
                          {"event": event, "record_count": 1}
                          for event in (12, 13, 14, 17, 18, 21)]})
    transcript = inspection["query_transcript"]
    definition = transcript["definitions"][0]
    if foreign_second_cycle:
        transcript["definitions"].append({**definition, "event_ordinal": 2,
                                          "identity": second_identity})
    definitions = len(transcript["definitions"])
    inspection["grammar"]["descriptor_kinds"]["query"] = definitions
    inspection["grammar"]["record_types"]["query_issue"] = 4
    inspection["grammar"]["control_kinds"]["query_get_data"] = 3
    transcript.update(query_definitions=definitions, issue_records=4, recorded_polls=3,
                      issues=[{"event_ordinal": event, "record_index": 0,
                               "identity": issue_identity, "flags": flags,
                               "issue_cycle": cycle}
                              for event, flags, cycle, issue_identity in (
                                  (12, 2, 1, identity), (14, 1, 1, identity),
                                  (17, 2, 2, second_identity), (18, 1, 2, second_identity))])
    ready = transcript["polls"][0]
    ready.update(event_index=15, event_ordinal=16, issue_cycle=1)
    pending = {**copy.deepcopy(ready), "event_index": 14, "event_ordinal": 15,
               "captured_result": 1, "captured_disposition": 2,
               "defined_bytes": 0, "captured_hex": ""}
    second = {**copy.deepcopy(ready), "event_index": 18, "event_ordinal": 19,
              "identity": second_identity, "issue_cycle": 2}
    transcript["polls"] = [pending, ready, second]
    first_row = query["rows"][0]
    first_row.update(event_index=15, event_ordinal=16, issue_cycle=1,
                     local_issue_ordinal=2, local_sequence=1, local_initializer=0,
                     completed_sequence=1, completed_initializer=0)
    pending_row = {**copy.deepcopy(first_row), **pending,
                   "replay_result": 1, "oracle_result": 1, "replay_hex": "",
                   "oracle_polls": 0, "result_required": False,
                   "readiness_satisfied": False, "result_substituted": False,
                   "supplemental": False, "completed_sequence": 0}
    second_row = {**copy.deepcopy(first_row), **second, "replay_result": 0,
                  "oracle_polls": 0, "supplemental": False,
                  "local_issue_ordinal": 4,
                  "local_query_identity": 100 if foreign_second_cycle else 99}
    query.update(query_definitions=definitions, issue_records=4, replayed_issue_records=4,
                 recorded_polls=3, replayed_polls=3, captured_pending=1, captured_ready=2,
                 replay_pending=2, replay_ready=1, required_results=2,
                 readiness_required=2, readiness_satisfied=2, substituted_results=2,
                 supplemental_drains=1, rows=[pending_row, first_row, second_row])
    verification = _native(module, policy)
    verification.update(completion="controlled-query-ready-obligation",
                        requested_completion="after-command",
                        input_counts={key: inspection[key] for key in (
                            "events", "objects", "mutations", "chunks", "records", "draws")})
    verification["input_counts"].update(query_definitions=definitions,
                                        query_issues=4, query_polls=3)
    for row in verification["iterations"]:
        trace = row["trace"][0]
        trace.update(first_event=13, last_event=13, event=13, released_at_event=16)
        next_trace = {**copy.deepcopy(trace), "submission": 2, "source_generation": 2,
                      "storage_generation": 2, "event_submission": 2,
                      "first_event": 21, "last_event": 21, "event": 21,
                      "released_at_event": 22}
        row.update(query_replay=copy.deepcopy(query), query_semantic_digest="21",
                   command_count=6, draw_count=2, resources_created=11, resources_released=11,
                   submitted=2, completed=2, reclaimed=2, deferred_releases=2,
                   source_summary_commands=6, source_summary_draws=2,
                   direct_draws=2 if policy["replay_route"] == "production-slot" else 0,
                   compatibility_draws=2 if policy["replay_route"] == "compatibility" else 0,
                   trace=[trace, next_trace], draw_observations=[
                       {"ordinal": ordinal, "publication_event": event,
                        "cumulative_semantic_digest": "11"}
                       for ordinal, event in ((1, 13), (2, 21))])
    measurement = copy.deepcopy(verification)
    measurement.update(run_mode="measure", phase_accounting="iteration-only")
    for row in measurement["iterations"]:
        row.update(full_oracle_calls=0, semantic_digest="0", draw_observations=[],
                   logical_resource_oracle_complete=False, buffer_backing_oracle_complete=False,
                   buffer_backing_observed=False, buffer_snapshot_observations=0,
                   buffer_live_binding_observations=0, buffer_binding_observations=0,
                   query_semantic_digest="0", phase_ns={key: 0 for key in row["phase_ns"]})
        row["query_replay"].update(mode="measurement", rows=[], transcript_wall_ns=0,
                                    oracle_wall_ns=0, events_sha256=inspection["events_sha256"])
    return inspection, verification, measurement


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
        query = copy.deepcopy(verified)
        query.update(completion="controlled-query-ready-obligation",
                     requested_completion="after-command")
        query["input_counts"]["query_definitions"] = 1
        for row in query["iterations"]:
            row.update(query_replay={"present": True}, query_semantic_digest="21")
        assert module._basic_result_shape(
            query, batch4, repeat=2, measurement=False)[0]
        for requested in (None, "before-next-work", True):
            broken = copy.deepcopy(query)
            broken["requested_completion"] = requested
            assert not module._basic_result_shape(
                broken, batch4, repeat=2, measurement=False)[0]
        assert module.compare_verification(query, query, repeat=2)["accepted"]
        stale_query = copy.deepcopy(query)
        stale_query["iterations"][0]["query_semantic_digest"] = "22"
        query_comparison = module.compare_verification(query, stale_query, repeat=2)
        assert not query_comparison["accepted"]
        assert query_comparison["failures"][0]["name"] == "query-semantic-digest-mismatch"
        for missing in (None, "", "0"):
            stale_query["iterations"][0]["query_semantic_digest"] = missing
            assert not module.compare_verification(stale_query, stale_query, repeat=2)["accepted"]
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

    # No high-frequency measurement observations are needed: the cold policy
    # gate joins its logical releases to strict detailed query qualification.
    for foreign_second_cycle in (False, True):
        inspection, verified, measured = _query_native(
            module, compatibility, foreign_second_cycle=foreign_second_cycle)
        assert module.verify_receipt(
            verified, compatibility, repeat=2, input_inspection=inspection)[0]
        assert module.measure_receipt(
            measured, compatibility, total=2, warmup=0, verification=verified,
            input_inspection=inspection)[0]
        for affected in ((0,), (0, 1)):
            wrong = copy.deepcopy(measured)
            for index in affected:
                wrong["iterations"][index]["trace"][0]["released_at_event"] = 19
            # Standalone execution_valid remains structural-only; it cannot
            # independently qualify timing or query-cycle attribution.
            assert module.execution_receipt(
                wrong, compatibility, repeat=2, input_inspection=inspection)[0]
            accepted, reason, _ = module.measure_receipt(
                wrong, compatibility, total=2, warmup=0, verification=verified,
                input_inspection=inspection)
            assert not accepted and reason == "measurement-qualified-query-release-mismatch"
        missing_end = copy.deepcopy(inspection)
        missing_end["query_transcript"]["issues"][1]["flags"] = 2
        assert not module.measure_receipt(
            measured, compatibility, total=2, warmup=0, verification=verified,
            input_inspection=missing_end)[0], "Ready without identity/cycle END"
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            image = root / "image.metallib"
            image.write_bytes(b"query image")
            verification_artifact = _artifact(verified, accepted=True, image_path=image)
            measurement_artifact = _artifact(measured, accepted=False, image_path=image)
            for artifact in (verification_artifact, measurement_artifact):
                artifact["input_inspection"] = inspection
                artifact["forced_environment"].update(
                    DXMT9_DRAW_CHUNK_COMMAND_LIMIT="0", DXMT9_DIRECT_SLOT_HEADROOM_BYTES="0",
                    DXMT9_DIRECT_SLOT_AGGREGATE_HEADROOM_BYTES="0")
            bundle = root / "input"
            bundle.mkdir()
            (bundle / "events.bin").write_bytes(b"query test input")
            policy_file = root / "policy.json"
            policy_file.write_bytes(module.canonical_policy(compatibility))
            def query_child(command, timeout):
                mode = command[command.index("--run-mode") + 1]
                artifact = copy.deepcopy(verification_artifact if mode == "verify"
                                         else measurement_artifact)
                if mode == "measure":
                    artifact["result"]["iterations"][0]["trace"][0]["released_at_event"] = 19
                output = pathlib.Path(command[command.index("--output-dir") + 1])
                output.mkdir(parents=True)
                (output / "result.json").write_text(json.dumps(artifact))
                return artifact
            args = argparse.Namespace(bundle=bundle, reference_policy=policy_file,
                candidate_policy=policy_file, repeat=2, warmup=0,
                output_dir=root / "query-pair-rejected", provider=root / "unused-provider",
                validator=root / "unused-validator", timeout_seconds=1)
            with mock.patch.object(module, "_child", side_effect=query_child), \
                    mock.patch.object(module, "_final_identity_check", return_value=True):
                assert module.cpu_policy_compare(args) == 1
            report = json.loads((args.output_dir / "result.json").read_text())
            assert report["accepted"] is False and report["identity_bound"] is True
            for name in ("reference", "candidate"):
                assert report["measurement"][name]["accepted"] is False
                assert report["measurement"][name]["reason"] == \
                    "measurement-qualified-query-release-mismatch"
                assert report["measurement"][name]["median_wall_ns"] is None

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
