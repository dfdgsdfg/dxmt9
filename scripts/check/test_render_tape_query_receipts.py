#!/usr/bin/env python3
"""Native-free negative pins for captured query experiment receipts."""

from __future__ import annotations

import copy
import importlib.util
import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    "query_receipt_runner", ROOT / "scripts/tools/run_dxmt9_render_tape.py")
assert spec and spec.loader
RUNNER = importlib.util.module_from_spec(spec)
spec.loader.exec_module(RUNNER)


def fixture(*, metal=True):
    identity = {"kind": 5, "generation": 3, "object_id": "42"}
    poll = {"event_index": 4, "event_ordinal": 5, "identity": identity,
            "versioned": True, "issue_cycle": 2, "flags": 1,
            "has_output": True, "data_size": 4, "captured_result": 0,
            "captured_disposition": 1, "captured_completion_ordinal": 1234,
            "defined_bytes": 4, "captured_hex": "07000000"}
    inspection = {"schema": "dxmt9.render_tape.v2", "valid": True,
                  "events_sha256": "e" * 64,
                  "grammar": {"descriptor_kinds": {"query": 1},
                              "record_types": {"query_issue": 2},
                              "control_kinds": {"query_get_data": 1}},
                  "query_transcript": {
                      "schema": "dxmt9.render_tape.query_transcript.v2", "complete": True,
                      "query_definitions": 1, "issue_records": 2, "recorded_polls": 1,
                      "definitions": [{"event_ordinal": 1, "identity": identity,
                                       "versioned": True, "type": 9, "data_bytes": 4,
                                       "initial_state": 0, "initial_issue_cycle": 0}],
                      "issues": [{"event_ordinal": 2, "record_index": index,
                                  "identity": identity, "flags": flags, "issue_cycle": 2}
                                 for index, flags in enumerate((2, 1))],
                      "polls": [poll]}}
    row = {key: copy.deepcopy(value) for key, value in poll.items()
           if key not in ("versioned", "captured_disposition", "captured_completion_ordinal")}
    row.update(replay_result=1, oracle_result=0, replay_hex="07000000" if metal else "00000000",
               local_query_identity=99, local_issue_ordinal=7, local_device_epoch=8,
               local_queue=1, local_sequence=2, local_initializer=1,
               completed_sequence=2, completed_initializer=1, oracle_polls=1,
               result_required=True, result_matched=metal, readiness_satisfied=True,
               result_substituted=not metal, supplemental=True)
    evidence = {"schema": "dxmt9.render_tape.query_replay.v1", "mode": "qualification",
                "backend": "metal-real" if metal else "cpu-controlled-production-queue",
                "present": True, "transcript_complete": True, "oracle_complete": True,
                "actual_metal": metal, "gpu_result_oracle": metal,
                "query_definitions": 1, "issue_records": 2, "replayed_issue_records": 2,
                "recorded_polls": 1, "replayed_polls": 1,
                "captured_pending": 0, "captured_ready": 1,
                "replay_pending": 1, "replay_ready": 0, "required_results": 1,
                "matched_results": int(metal), "readiness_required": 1,
                "readiness_satisfied": 1, "substituted_results": int(not metal),
                "supplemental_polls": 1, "supplemental_drains": 0,
                "transcript_wall_ns": 10, "oracle_wall_ns": 20,
                "max_oracle_polls": 4, "max_oracle_wall_ns": 5000000000 if metal else 0,
                "failure": "none", "rows": [row]}
    if not metal:
        evidence["oracle_wall_ns"] = 0
    return inspection, evidence


def checkpoint_fixture(*, metal=False):
    inspection, evidence = fixture(metal=metal)
    transcript = inspection["query_transcript"]
    definition = transcript["definitions"][0]
    definition.update(type=8, schema_version=3, initial_state=2, initial_issue_cycle=47,
        checkpoint={"checkpoint_version": 1, "status": 1, "query_type": 8, "active": 0,
                    "identity": copy.deepcopy(definition["identity"]), "issue_cycle": 47,
                    "core_query_identity": 101, "device_epoch": 7, "issue_ordinal": 63,
                    "raw_ordinal": 11, "queue_identity": 909, "end_sequence": 8,
                    "initializer_sequence": 3, "completed_sequence": 8,
                    "completed_initializer": 3, "result_code": 0, "reserved0": 0})
    poll = transcript["polls"][0]
    poll.update(issue_cycle=47, defined_bytes=0, captured_hex="")
    second_poll = {**copy.deepcopy(poll), "event_index": 6, "event_ordinal": 7, "issue_cycle": 48}
    transcript.update(issue_records=1, recorded_polls=2, polls=[poll, second_poll],
                      issues=[{"event_ordinal": 6, "record_index": 0,
                               "identity": copy.deepcopy(definition["identity"]),
                               "flags": 1, "issue_cycle": 48}])
    inspection["grammar"]["record_types"]["query_issue"] = 1
    inspection["grammar"]["control_kinds"]["query_get_data"] = 2
    row = evidence["rows"][0]
    row.update(issue_cycle=47, defined_bytes=0, captured_hex="", replay_hex="",
               replay_result=0, oracle_result=0, local_issue_ordinal=1,
               local_sequence=0, local_initializer=0, completed_sequence=0,
               completed_initializer=0, oracle_polls=0, result_matched=False,
               result_substituted=False, supplemental=False)
    second_row = {**copy.deepcopy(row), "event_index": 6, "event_ordinal": 7, "issue_cycle": 48,
                  "local_issue_ordinal": 2, "local_sequence": 1, "completed_sequence": 1}
    setup = {key: copy.deepcopy(row[key]) for key in (
        "identity", "local_query_identity", "local_issue_ordinal", "local_device_epoch",
        "local_queue", "local_sequence", "local_initializer", "completed_sequence", "completed_initializer")}
    setup.update(captured_issue_cycle=47, captured_issue_ordinal=63, captured_device_epoch=7,
                 setup_polls=1, setup_issued=True, completed=True, activated=True)
    evidence.update(issue_records=1, replayed_issue_records=1, recorded_polls=2, replayed_polls=2,
                    captured_ready=2, replay_ready=2, replay_pending=0, readiness_required=2,
                    readiness_satisfied=2, required_results=0, matched_results=0,
                    substituted_results=0, supplemental_polls=0, oracle_wall_ns=0,
                    rows=[row, second_row], checkpoint_definitions=1, checkpoint_setup_ends=1,
                    checkpoint_setup_publications=0, checkpoint_setup_polls=1,
                    checkpoint_setup_waits=0, checkpoint_completed=1, checkpoint_activated=1,
                    checkpoint_setup_wall_ns=17, checkpoints=[setup])
    return inspection, evidence


def checkpoint_receipts():
    for metal in (False, True):
        inspection, evidence = checkpoint_fixture(metal=metal)
        def accepts(value=evidence, source=inspection, **kwargs):
            return RUNNER.query_replay_accepts(value, source, backend=evidence["backend"], **kwargs)
        assert accepts(), "completed EVENT imports a new local END, including an empty prefix"
        zero_input, zero_evidence = checkpoint_fixture(metal=metal)
        zero_input["grammar"]["record_types"]["query_issue"] = 0
        zero_input["grammar"]["control_kinds"]["query_get_data"] = 0
        zero_input["query_transcript"].update(issue_records=0, recorded_polls=0, issues=[], polls=[])
        zero_evidence.update(issue_records=0, replayed_issue_records=0, recorded_polls=0,
                             replayed_polls=0, captured_ready=0, replay_ready=0,
                             readiness_required=0, readiness_satisfied=0, rows=[],
                             gpu_result_oracle=False)
        assert accepts(zero_evidence, zero_input), "setup is not a captured result/readiness comparison"
        bad = copy.deepcopy(zero_evidence)
        bad["checkpoints"] = []
        assert not accepts(bad, zero_input), "unused imported state still requires setup/activation proof"
        for field in ("checkpoint_definitions", "checkpoint_setup_ends", "checkpoint_completed",
                      "checkpoint_activated", "checkpoint_setup_publications", "checkpoint_setup_polls",
                      "checkpoint_setup_waits", "checkpoint_setup_wall_ns", "checkpoints"):
            bad = copy.deepcopy(evidence)
            bad.pop(field)
            assert not accepts(bad), ("missing setup receipt", field)
        for field in ("checkpoint_definitions", "checkpoint_setup_ends", "checkpoint_completed",
                      "checkpoint_activated"):
            for count in (0, 2):
                bad = copy.deepcopy(evidence)
                bad[field] = count
                assert not accepts(bad), ("non-conserved restore", field, count)
        for field, value in (("captured_issue_cycle", 48), ("captured_issue_ordinal", 47),
                             ("captured_device_epoch", 8), ("local_query_identity", 0),
                             ("local_issue_ordinal", 0), ("local_issue_ordinal", 2),
                             ("local_device_epoch", 0), ("local_queue", 0), ("setup_polls", 0),
                             ("setup_issued", False), ("completed", False), ("activated", False)):
            bad = copy.deepcopy(evidence)
            bad["checkpoints"][0][field] = value
            assert not accepts(bad), ("invalid local setup", field, value)
        bad = copy.deepcopy(evidence)
        bad["checkpoints"][0]["local_sequence"] = 1
        assert not accepts(bad), "publication without completed local obligations"
        bad = copy.deepcopy(evidence)
        bad["checkpoints"].append(copy.deepcopy(bad["checkpoints"][0]))
        assert not accepts(bad), "duplicate checkpoint restoration"
        bad = copy.deepcopy(evidence)
        bad["rows"][0]["local_issue_ordinal"] = 2
        assert not accepts(bad), "initial Ready needs exact retained setup END authority"
        bad = copy.deepcopy(evidence)
        bad["rows"][1]["local_issue_ordinal"] = 1
        assert not accepts(bad), "reissue cannot consume the old checkpoint incarnation"
        for field, value in (("schema_version", 2), ("type", 9), ("initial_state", 1),
                             ("initial_issue_cycle", 48), ("checkpoint", None)):
            bad = copy.deepcopy(inspection)
            bad["query_transcript"]["definitions"][0][field] = value
            assert not accepts(source=bad), ("unauthenticated checkpoint descriptor", field)
        for field, value in (("issue_cycle", 48), ("device_epoch", 0), ("issue_ordinal", 0),
                             ("issue_ordinal", 46),
                             ("completed_sequence", 7), ("completed_initializer", 2),
                             ("active", 1), ("status", 5), ("reserved0", 1)):
            bad = copy.deepcopy(inspection)
            bad["query_transcript"]["definitions"][0]["checkpoint"][field] = value
            assert not accepts(source=bad), ("invalid captured checkpoint", field)
        bad = copy.deepcopy(inspection)
        bad["query_transcript"]["definitions"][0]["checkpoint"]["identity"]["generation"] += 1
        assert not accepts(source=bad), "checkpoint must bind exact captured query generation"
        measured = copy.deepcopy(evidence)
        measured.update(mode="measurement", rows=[], checkpoints=[], events_sha256=inspection["events_sha256"])
        assert accepts(measured, measurement=True), "measurement retains cheap setup conservation"
        for field in ("checkpoint_setup_ends", "checkpoint_completed", "checkpoint_activated"):
            bad = copy.deepcopy(measured)
            bad[field] = 0
            assert not accepts(bad, measurement=True), ("measurement loses setup proof", field)
        bad = copy.deepcopy(measured)
        bad["checkpoints"] = evidence["checkpoints"]
        assert not accepts(bad, measurement=True), "measurement contains detailed setup observer rows"
    empty_input = {"schema": "dxmt9.render_tape.v2", "valid": True,
                   "grammar": {"descriptor_kinds": {}, "record_types": {}, "control_kinds": {}}}
    empty = {"present": False, "query_definitions": 0, "issue_records": 0, "recorded_polls": 0, "rows": []}
    assert RUNNER.query_replay_accepts(empty, empty_input, backend="cpu-controlled-production-queue")
    empty["checkpoint_definitions"] = 1
    assert not RUNNER.query_replay_accepts(empty, empty_input, backend="cpu-controlled-production-queue"), \
        "query-free coverage cannot launder imported checkpoint declarations"


def main():
    checkpoint_receipts()
    for metal in (False, True):
        inspection, evidence = fixture(metal=metal)
        backend = evidence["backend"]
        def accepts(value, source=inspection, **kwargs):
            return RUNNER.query_replay_accepts(value, source, backend=backend, **kwargs)
        assert accepts(evidence), backend
        for zero_kind in ("definitions-only", "issued-without-polls", "pending-only"):
            zero_input, zero_evidence = fixture(metal=metal)
            transcript = zero_input["query_transcript"]
            zero_evidence.update(gpu_result_oracle=False, captured_ready=0,
                                 required_results=0, matched_results=0, substituted_results=0,
                                 readiness_required=0, readiness_satisfied=0,
                                 supplemental_polls=0, supplemental_drains=0, oracle_wall_ns=0)
            if zero_kind == "pending-only":
                transcript["polls"][0].update(captured_result=1, captured_disposition=2,
                                               defined_bytes=0, captured_hex="")
                zero_evidence.update(captured_pending=1)
                zero_evidence["rows"][0].update(captured_result=1, defined_bytes=0,
                    captured_hex="", replay_hex="", oracle_result=1, oracle_polls=0,
                    result_required=False, readiness_satisfied=False,
                    result_matched=False, result_substituted=False, supplemental=False)
            else:
                transcript.update(recorded_polls=0, polls=[])
                zero_input["grammar"]["control_kinds"]["query_get_data"] = 0
                zero_evidence.update(recorded_polls=0, replayed_polls=0, captured_pending=0,
                                     replay_pending=0, replay_ready=0, rows=[],
                                     max_oracle_polls=0, max_oracle_wall_ns=0)
                if zero_kind == "definitions-only":
                    transcript.update(issue_records=0, issues=[])
                    zero_input["grammar"]["record_types"]["query_issue"] = 0
                    zero_evidence.update(issue_records=0, replayed_issue_records=0)
            assert accepts(zero_evidence, zero_input), (backend, zero_kind)
            assert not accepts(None, zero_input), "nonzero definitions need an explicit receipt"
            for missing in ("rows", "recorded_polls", "query_definitions", "transcript_complete"):
                broken = copy.deepcopy(zero_evidence)
                broken.pop(missing)
                assert not accepts(broken, zero_input), (backend, zero_kind, missing)
            for field in ("recorded_polls", "issue_records", "readiness_required",
                          "supplemental_polls", "supplemental_drains"):
                broken = copy.deepcopy(zero_evidence)
                broken[field] += 1
                assert not accepts(broken, zero_input), (backend, zero_kind, field)
            broken_input = copy.deepcopy(zero_input)
            broken_input["query_transcript"]["complete"] = False
            assert not accepts(zero_evidence, broken_input), "incomplete zero-obligation input"
            if metal:
                broken = copy.deepcopy(zero_evidence)
                broken["gpu_result_oracle"] = True
                assert not accepts(broken, zero_input), "vacuous GPU query-result proof"
            else:
                measured_zero = copy.deepcopy(zero_evidence)
                measured_zero.update(mode="measurement", rows=[], transcript_wall_ns=0,
                                     events_sha256=zero_input["events_sha256"])
                assert accepts(measured_zero, zero_input, measurement=True)
        assert not accepts(None), "omitted query receipt"
        assert not accepts(evidence, {}), "missing independent inspection"
        for key in evidence:
            bad = copy.deepcopy(evidence)
            bad.pop(key)
            assert not accepts(bad), (backend, "missing", key)
        for key, value in (("issue_cycle", 1), ("event_ordinal", 6),
                           ("flags", 0), ("data_size", 8), ("captured_result", 1),
                           ("captured_hex", "06000000"), ("local_query_identity", 0),
                           ("completed_sequence", 1), ("completed_initializer", 0),
                           ("readiness_satisfied", False), ("oracle_result", 1),
                           ("supplemental", False), ("oracle_polls", 0)):
            bad = copy.deepcopy(evidence)
            bad["rows"][0][key] = value
            assert not accepts(bad), (backend, key)
        bad = copy.deepcopy(evidence)
        bad["rows"].append(copy.deepcopy(bad["rows"][0]))
        assert not accepts(bad), "duplicate observation"
        bad = copy.deepcopy(inspection)
        bad["query_transcript"]["polls"][0]["versioned"] = False
        assert not accepts(evidence, bad), "legacy insufficient payload"
        bad = copy.deepcopy(inspection)
        bad["query_transcript"]["complete"] = False
        assert not accepts(evidence, bad), "uncaptured query prefix"
        bad = copy.deepcopy(inspection)
        bad["query_transcript"]["issues"][-1]["flags"] = 2
        assert not accepts(evidence, bad), "Ready without an identity/cycle END"
        bad = copy.deepcopy(inspection)
        bad["query_transcript"]["issues"][-1]["issue_cycle"] += 1
        assert not accepts(evidence, bad), "Ready bound to another issue cycle"
        bad = copy.deepcopy(evidence)
        bad["gpu_result_oracle"] = not metal
        assert not accepts(bad), "controlled completion cannot claim GPU samples"
        for limit in ("poll-budget", "wall-budget", True, None):
            bad = copy.deepcopy(evidence)
            bad["progress_limit"] = limit
            assert not accepts(bad), "progress exhaustion cannot qualify"
        if metal:
            bad = copy.deepcopy(evidence)
            bad["rows"][0]["replay_hex"] = "06000000"
            assert not accepts(bad), "actual Metal query result mismatch"
        else:
            measured = copy.deepcopy(evidence)
            measured.update(mode="measurement", rows=[], events_sha256=inspection["events_sha256"])
            assert accepts(measured, measurement=True)
            bad = copy.deepcopy(measured)
            bad["rows"] = evidence["rows"]
            assert not accepts(bad, measurement=True), "measurement observer pollution"
            bad = copy.deepcopy(measured)
            bad["events_sha256"] = "a" * 64
            assert not accepts(bad, measurement=True), "changed qualification input"
        # A later reissue has a distinct local issue identity even when its
        # source query object is retained. Reusing the prior receipt is stale.
        reissued = copy.deepcopy(evidence)
        reissue_input = copy.deepcopy(inspection)
        reissue_input["grammar"]["record_types"]["query_issue"] = 4
        reissue_input["grammar"]["control_kinds"]["query_get_data"] = 2
        transcript = reissue_input["query_transcript"]
        transcript.update(issue_records=4, recorded_polls=2)
        transcript["issues"] += [{**copy.deepcopy(row), "event_ordinal": 6,
                                   "issue_cycle": 3} for row in transcript["issues"]]
        next_poll = copy.deepcopy(transcript["polls"][0])
        next_poll.update(event_index=6, event_ordinal=7, issue_cycle=3)
        transcript["polls"].append(next_poll)
        next_row = copy.deepcopy(reissued["rows"][0])
        next_row.update(event_index=6, event_ordinal=7, issue_cycle=3, local_issue_ordinal=8)
        reissued["rows"].append(next_row)
        reissued.update(issue_records=4, replayed_issue_records=4,
                        recorded_polls=2, replayed_polls=2, captured_ready=2,
                        replay_pending=2, required_results=2, readiness_required=2,
                        readiness_satisfied=2, matched_results=2 if metal else 0,
                        substituted_results=0 if metal else 2, supplemental_polls=2)
        assert accepts(reissued, reissue_input), "valid reissue"
        reissued["rows"][1]["local_issue_ordinal"] = 7
        assert not accepts(reissued, reissue_input), "reissue using stale result authority"
        # Event queries define readiness only; their application output memory
        # is not a sample-result obligation even on captured success.
        event_input = copy.deepcopy(inspection)
        event_input["query_transcript"]["definitions"][0]["type"] = 8
        event_input["query_transcript"]["polls"][0].update(defined_bytes=0, captured_hex="")
        event = copy.deepcopy(evidence)
        event.update(required_results=0, matched_results=0, substituted_results=0)
        event["rows"][0].update(defined_bytes=0, captured_hex="", replay_hex="",
                                result_matched=False, result_substituted=False)
        assert accepts(event, event_input), "Event defined no application output bytes"
        event["rows"][0]["captured_hex"] = "a5a5a5a5"
        assert not accepts(event, event_input), "untouched Event caller memory"
        if not metal:
            boundary_input = copy.deepcopy(inspection)
            boundary_input["event_types"] = [2, 5, 5, 5, 6, 5, 7]
            boundary_input["cpu_window_plan"] = {"sources": [
                {"event": event, "record_count": 1} for event in (2, 3, 4, 6)]}
            boundary_input["query_transcript"]["issues"][1]["event_ordinal"] = 4
            boundary_input["query_transcript"]["issues"][1]["record_index"] = 0
            assert RUNNER._cpu_general_query_release_event(
                boundary_input, evidence, 3, 2) == 5, "disclosed Ready oracle release"
            no_extra_probe = copy.deepcopy(evidence)
            no_extra_probe["rows"][0]["supplemental"] = False
            assert RUNNER._cpu_general_query_release_event(
                boundary_input, no_extra_probe, 3, 2) == 6, "next non-query work drains"
            foreign = copy.deepcopy(evidence)
            foreign["rows"][0]["local_sequence"] = 1
            assert RUNNER._cpu_general_query_release_event(
                boundary_input, foreign, 3, 2) == 6, "older END does not release later work"
            new_work = copy.deepcopy(boundary_input)
            new_work["cpu_window_plan"]["sources"][2]["record_count"] = 2
            assert RUNNER._cpu_general_query_release_event(
                new_work, evidence, 3, 2) == 4, "mixed query/work cannot suppress release boundary"
            measured_progress = copy.deepcopy(evidence)
            measured_progress.update(rows=[], supplemental_drains=1)
            assert RUNNER._cpu_general_query_release_event(
                boundary_input, measured_progress, 3, 2, 5) == 5
            assert RUNNER._cpu_general_query_release_event(
                boundary_input, measured_progress, 3, 2, 4) is None, "measured early release"
            assert RUNNER._cpu_general_query_release_event(
                boundary_input, measured_progress, 3, 2, 7) is None, "missed required work boundary"
            measured_progress["supplemental_drains"] = 0
            assert RUNNER._cpu_general_query_release_event(
                boundary_input, measured_progress, 3, 2, 5) is None, "undisclosed oracle drain"
    zero = {"schema": "dxmt9.render_tape.v2", "valid": True,
            "grammar": {"descriptor_kinds": {}, "record_types": {}, "control_kinds": {}}}
    assert RUNNER.query_replay_accepts(None, zero, backend="metal-real")
    assert not RUNNER.query_replay_accepts(None, {}, backend="metal-real")
    print("query receipt contract cases passed")


if __name__ == "__main__":
    main()
