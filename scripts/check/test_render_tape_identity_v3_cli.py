#!/usr/bin/env python3
"""Actual native inspector and inspection-only bundle dispatch regressions."""

from __future__ import annotations

import contextlib
import hashlib
import importlib.util
import io
import json
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
from unittest import mock


def run(*args: object, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(arg) for arg in args], check=check, text=True, capture_output=True
    )


def reject(call, detail: str) -> None:
    try:
        call()
    except SystemExit as error:
        assert detail in str(error), str(error)
    else:
        raise AssertionError(f"expected rejection containing {detail!r}")


def load_runner(path: pathlib.Path):
    spec = importlib.util.spec_from_file_location("identity_v3_runner_tested", path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def decode_fields(description: str, data: bytes, offset: int = 0):
    fields = [field.split(":") for field in description.split()]
    layout = struct.Struct("<" + "".join(kind for _, kind in fields))
    values = layout.unpack_from(data, offset)
    decoded = {}
    for (name, kind), value in zip(fields, values):
        decoded[name] = str(value) if kind == "Q" else value.hex() if kind == "32s" else value
    return decoded, offset + layout.size


def assert_v3_exact_json(result: dict, identity: pathlib.Path) -> None:
    # Independent byte/layout expectations exercise every emitted raw field;
    # the CLI has already authenticated these bytes with the native validator.
    raw = identity.read_bytes()
    header, offset = decode_fields(
        "magic:Q events_bytes:Q frame_id:Q present_ordinal:Q capture_token:Q "
        "events_digest:32s version:I header_size:I capture_epoch:Q queue:Q "
        "baseline_sequence:Q baseline_initializer:Q baseline_completed_sequence:Q "
        "initializer_epoch:Q baseline_completed_initializer:Q baseline_completion_edge:Q "
        "baseline_kind:I authority:I", raw)
    tables = ("sources", "initializers", "commands", "dispositions", "coordinators", "settlements")
    for name in tables:
        header[name], offset = decode_fields("offset:I count:I entry_size:I", raw, offset)
    reserved, offset = decode_fields("reserved0:I reserved1:I reserved2:I reserved3:I", raw, offset)
    header.update(reserved)
    assert offset == header["header_size"] == 240
    assert int(header["frame_id"]) > 2**53
    descriptions = {
        "sources": "source_ordinal:Q seq_id:Q writer_capture_id:Q writer_generation:Q "
            "storage_generation:Q publication_edge:Q submission_edge:Q completion_edge:Q "
            "completed_sequence:Q cut_event_ordinal:Q command_count:I cut_kind:I cut_record:I reserved0:I",
        "initializers": "sequence:Q epoch:Q publication_edge:Q submission_edge:Q completion_edge:Q "
            "completed_sequence:Q cut_event_ordinal:Q cut_kind:I cut_record:I reserved0:I reserved1:I",
        "commands": "logical_pass_id:Q source_index:I physical_command_index:I kind:I draw_count:I "
            "membership:I dag_pass_index:I pass_kind:I coordinator_index:I reserved0:I reserved1:I",
        "dispositions": "event_ordinal:Q first_record:I record_count:I kind:I command_index:I "
            "first_subdraw:I subdraw_count:I coordinator_index:I reserved0:I",
        "coordinators": "event_ordinal:Q raw_ordinal:Q build_generation:Q dispatch_edge:Q "
            "release_ordinal:Q release_generation:Q _query_kind:I _query_generation:I _query_object_id:Q "
            "issue_cycle:Q issue_ordinal:Q device_epoch:Q writer_capture_id:Q writer_generation:Q "
            "storage_generation:Q prefix_sequence:Q prefix_initializer:Q prefix_initializer_epoch:Q "
            "record_index:I record_type:I flags:I query_type:I dispatch_result:i release_action:I "
            "release_writer:I prefix_kind:I source_index:I initializer_index:I command_end:I "
            "observation:I reserved0:I reserved1:I",
        "settlements": "event_ordinal:Q raw_ordinal:Q build_generation:Q replay_edge:Q record_count:I reserved0:I",
    }
    expected = {
        "schema": "dxmt9.render_tape.identity.v3", "valid": True,
        "inspection_only": True, "execution_supported": False,
        "production_qualified": False, "header": header,
    }
    for name in tables:
        table = header[name]
        rows = []
        for index in range(table["count"]):
            first = table["offset"] + index * table["entry_size"]
            row, end = decode_fields(descriptions[name], raw, first)
            assert end - first == table["entry_size"]
            if name in ("sources", "initializers"):
                row["phase"] = ("published" if row["submission_edge"] == "0" else
                                "submitted" if row["completion_edge"] == "0" else "completed")
            if name == "coordinators":
                row["query_identity"] = {
                    "kind": row.pop("_query_kind"), "generation": row.pop("_query_generation"),
                    "object_id": row.pop("_query_object_id"),
                }
            rows.append(row)
        expected["replay_settlements" if name == "settlements" else name] = rows
    assert result == expected


def pack(runner, tool: pathlib.Path, events: pathlib.Path,
         identity: pathlib.Path, output: pathlib.Path) -> None:
    args = runner.parser().parse_args([
        "pack", "--events", str(events), "--identity", str(identity),
        "--output-dir", str(output), "--validator", str(tool),
    ])
    with contextlib.redirect_stdout(io.StringIO()):
        assert runner.pack(args) == 0


def inspection_only_guards(runner, tool: pathlib.Path, bundle: pathlib.Path,
                           root: pathlib.Path) -> None:
    identity = bundle / "identity.bin"
    events = bundle / "events.bin"
    missing_provider = root / "must-never-run"
    with mock.patch.object(runner.subprocess, "run") as child:
        reject(lambda: runner.run_provider_replay(
            missing_provider, events, [], identity=identity), "inspection-only")
        reject(lambda: runner.run_policy_explore(
            tool, events, identity, []), "inspection-only")
        reject(lambda: runner.run_native_materialize(
            tool, events, identity, root / "never-materialized.bin", [],
            1, 0, 1), "inspection-only")
        child.assert_not_called()
    assert not (root / "never-materialized.bin").exists()
    for command in ("experiment", "cpu-experiment", "cpu-compare", "cpu-source-project"):
        output = root / f"never-staged-{command}"
        args = runner.parser().parse_args([
            command, str(bundle), "--output-dir", str(output),
            "--validator", str(tool), "--provider", str(missing_provider),
        ])
        with mock.patch.object(runner, "bundle_transaction") as transaction, \
             mock.patch.object(runner, "copy_input_owned") as copy_input, \
             mock.patch.object(runner.subprocess, "run") as child:
            reject(lambda: args.function(args), "inspection-only")
            transaction.assert_not_called()
            copy_input.assert_not_called()
            child.assert_not_called()
        assert not output.exists()
    output = root / "never-staged-cpu-policy-compare"
    args = runner.parser().parse_args([
        "cpu-policy-compare", str(bundle), "--output-dir", str(output),
        "--reference-policy", str(root / "missing-reference-policy.json"),
        "--candidate-policy", str(root / "missing-candidate-policy.json"),
    ])
    with mock.patch.object(runner.render_tape_cpu_policy, "cpu_policy_compare") as compare, \
         mock.patch.object(runner, "bundle_transaction") as transaction, \
         mock.patch.object(runner.subprocess, "run") as child:
        reject(lambda: args.function(args), "inspection-only")
        compare.assert_not_called()
        transaction.assert_not_called()
        child.assert_not_called()
    assert not output.exists()
    for command in ("policy-explore", "materialize"):
        output = root / f"native-never-{command}.bin"
        arguments = [tool, command, events, identity]
        if command == "materialize":
            arguments += [output, "--command-event-ordinal", 1,
                          "--first-record", 0, "--record-count", 1]
        rejected = run(*arguments, check=False)
        assert rejected.returncode != 0
        assert "inspection-only" in rejected.stderr
        assert not output.exists()


def rewrite_manifest(bundle: pathlib.Path, update) -> dict:
    manifest_path = bundle / "manifest.json"
    manifest = json.loads(manifest_path.read_text())
    update(manifest)
    manifest_path.write_text(json.dumps(manifest))
    return manifest


def assert_v2_unchanged(runner, tool: pathlib.Path, bundle_tool: pathlib.Path,
                        source: pathlib.Path, root: pathlib.Path) -> None:
    events, identity = source / "events-v2.bin", source / "identity-v2.bin"
    if not events.is_file():
        events = source / "events.bin"
    raw = identity.read_bytes()
    header = struct.unpack_from("<5Q32s14I", raw)
    authority, settlement_count = header[14], header[18]
    capture, provider = authority == 1, authority == 2
    queue_settled = capture and settlement_count > 0
    segments = []
    for index in range(header[11]):
        row = struct.unpack_from("<4Q4I", raw, header[10] + index * header[8])
        segments.append({"segment_index": index, "event_ordinal": row[0],
                         "source_ordinal": row[1], "seq_id": row[2],
                         "first_record": row[4], "record_count": row[5]})
    settlements = []
    if capture:
        for index in range(settlement_count):
            row = struct.unpack_from("<5Q2I", raw, header[17] + index * header[16])
            settlements.append({"event_ordinal": row[0], "raw_ordinal": row[1],
                                "build_generation": row[2], "first_source_ordinal": row[3],
                                "tail_seq_id": row[4], "source_count": row[5]})
    last = settlements[-1] if settlements else None
    expected = {
        "schema": runner.IDENTITY_SCHEMA, "valid": True, "authority": authority,
        "frame_id": header[2], "present_ordinal": header[3],
        "sources": header[11], "ranges": header[13],
        "completed_segment_count": header[11] if queue_settled else 0,
        "completion_evidence": ("queue-tail-fenced" if queue_settled else "none")
            if capture else "process-local-unverified" if provider else "not-queue-authenticated",
        "settlement_count": 0 if provider else settlement_count,
        "settlement_table_count": 0 if provider else settlement_count,
        "segments": segments, "event_settlement_table": settlements,
        "final_event_settlement": None if last is None else {
            key: last[key] for key in ("event_ordinal", "tail_seq_id", "source_count")},
    }
    assert json.loads(run(tool, "identity", events, identity).stdout) == expected
    bundle = root / "v2-bundle"
    pack(runner, tool, events, identity, bundle)
    manifest, *_ = runner.load_bundle(bundle)
    assert manifest["components"]["identity"]["schema"] == runner.IDENTITY_SCHEMA
    result = json.loads(run(sys.executable, bundle_tool, "inspect", bundle,
                            "--validator", tool).stdout)
    assert "identity_validation" not in result


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit(
            "usage: test_render_tape_identity_v3_cli.py "
            "<identity-v3-fixture> <native-tool> <bundle-tool>"
        )
    fixture, tool, bundle_tool = map(pathlib.Path, sys.argv[1:])
    runner = load_runner(bundle_tool)
    with tempfile.TemporaryDirectory(prefix="dxmt9-identity-v3-cli-") as directory:
        root = pathlib.Path(directory)
        source = root / "source"
        run(fixture, "--write-inspection-fixture", source)
        events, identity = source / "events.bin", source / "identity.bin"
        inspected = json.loads(run(tool, "identity", events, identity).stdout)
        assert inspected["schema"] == runner.IDENTITY_V3_SCHEMA
        assert inspected["valid"] is True
        assert inspected["inspection_only"] is True
        assert inspected["execution_supported"] is False
        assert inspected["production_qualified"] is False
        assert_v3_exact_json(inspected, identity)
        assert_v2_unchanged(runner, tool, bundle_tool, source, root)

        bundle = root / "bundle"
        pack(runner, tool, events, identity, bundle)
        manifest, *_ = runner.load_bundle(bundle, allow_identity_v3=True)
        assert manifest["components"]["identity"]["schema"] == runner.IDENTITY_V3_SCHEMA
        assert manifest["scope"]["production_capture"] is False
        assert manifest["scope"]["production_provider_replay"] is False
        reject(lambda: runner.load_bundle(bundle), "inspection-only")
        for command in ("validate", "inspect"):
            result = json.loads(run(sys.executable, bundle_tool, command, bundle,
                                    "--validator", tool).stdout)
            assert result["identity_validation"] == inspected
            assert result["scope"] == manifest["scope"]
        inspection_only_guards(runner, tool, bundle, root)

        # Metadata remains source data; inspection cannot silently change
        # authority or qualify/downgrade a future capture's declared scope.
        rewrite_manifest(bundle, lambda value: value["scope"].update(
            production_capture=True, production_provider_replay=True))
        declared = json.loads(run(sys.executable, bundle_tool, "inspect", bundle,
                                 "--validator", tool).stdout)
        assert declared["scope"]["production_capture"] is True
        assert declared["scope"]["production_provider_replay"] is True
        assert declared["identity_validation"]["production_qualified"] is False

        mismatched = root / "mismatched"
        shutil.copytree(bundle, mismatched)
        rewrite_manifest(mismatched, lambda value: value["components"]["identity"].update(
            schema=runner.IDENTITY_SCHEMA))
        reject(lambda: runner.load_bundle(mismatched, allow_identity_v3=True),
               "manifest schema does not match binary")
        inspection_only_guards(runner, tool, mismatched, root)

        retired = root / "retired-v1.bin"
        retired_bytes = bytearray(identity.read_bytes())
        struct.pack_into("<I", retired_bytes, 72, 1)
        retired.write_bytes(retired_bytes)
        with mock.patch.object(runner, "bundle_transaction") as transaction:
            reject(lambda: pack(runner, tool, events, retired, root / "retired-bundle"),
                   "version is unsupported: 1")
            transaction.assert_not_called()
        assert not (root / "retired-bundle").exists()
        assert run(tool, "identity", events, retired, check=False).returncode != 0

        damaged = root / "damaged-v3.bin"
        damaged_bytes = bytearray(identity.read_bytes())
        damaged_bytes[40] ^= 1  # bound events digest, not the dispatch prefix
        damaged.write_bytes(damaged_bytes)
        with mock.patch.object(runner, "bundle_transaction") as transaction:
            reject(lambda: pack(runner, tool, events, damaged, root / "damaged-bundle"),
                   "identity validation failed")
            transaction.assert_not_called()
        assert not (root / "damaged-bundle").exists()

        # A claimed v3 result from an old/misrouted validator cannot authorize
        # pack. The real native inspector is exercised above, not mocked.
        wrong_result = subprocess.CompletedProcess([], 0,
            json.dumps({"schema": runner.IDENTITY_SCHEMA, "valid": True}), "")
        with mock.patch.object(runner.subprocess, "run", return_value=wrong_result):
            reject(lambda: runner.run_identity_validator(tool, events, identity, []),
                   "validator schema does not match binary")
        assert hashlib.sha256(identity.read_bytes()).hexdigest() == \
            manifest["components"]["identity"]["sha256"]
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
