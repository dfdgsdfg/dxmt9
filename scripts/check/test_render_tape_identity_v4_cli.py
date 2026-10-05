#!/usr/bin/env python3
"""Native V4 pair inspection, manifest authentication and execution guards."""

from __future__ import annotations

import contextlib
import copy
import hashlib
import io
import json
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
from unittest import mock

from test_render_tape_identity_v3_cli import (
    decode_fields, inspection_only_guards, load_runner, reject, rewrite_manifest, run,
)


def assert_v4_exact_json(result: dict, identity: pathlib.Path,
                         topology: pathlib.Path) -> None:
    raw = identity.read_bytes()
    header, offset = decode_fields(
        "magic:Q events_bytes:Q frame_id:Q present_ordinal:Q capture_token:Q "
        "events_digest:32s version:I header_size:I capture_epoch:Q queue:Q "
        "baseline_sequence:Q baseline_initializer:Q baseline_completed_sequence:Q "
        "initializer_epoch:Q baseline_completed_initializer:Q baseline_completion_edge:Q "
        "baseline_kind:I authority:I", raw)
    tables = ("sources", "initializers", "commands", "dispositions", "coordinators",
              "settlements", "attempts")
    for name in tables:
        header[name], offset = decode_fields("offset:I count:I entry_size:I", raw, offset)
    extra, offset = decode_fields(
        "required_execution_topology_version:I reserved0:I reserved1:I reserved2:I reserved3:I "
        "coordinator_domain:I reserved4:I",
        raw, offset)
    header.update(extra)
    assert offset == header["header_size"] == 264
    assert header["coordinators"]["entry_size"] == 208
    source_description = (
        "source_ordinal:Q seq_id:Q writer_capture_id:Q writer_generation:Q "
        "storage_generation:Q publication_edge:Q submission_edge:Q completion_edge:Q "
        "completed_sequence:Q cut_event_ordinal:Q command_count:I cut_kind:I cut_record:I reserved0:I"
    )
    descriptions = {
        "initializers": "sequence:Q epoch:Q publication_edge:Q submission_edge:Q completion_edge:Q "
            "completed_sequence:Q cut_event_ordinal:Q cut_kind:I cut_record:I reserved0:I reserved1:I",
        "commands": "source_index:I physical_command_index:I kind:I draw_count:I coordinator_index:I reserved0:I",
        "dispositions": "event_ordinal:Q first_record:I record_count:I kind:I command_index:I "
            "first_subdraw:I subdraw_count:I coordinator_index:I reserved0:I",
        "settlements": "event_ordinal:Q raw_ordinal:Q replay_edge:Q record_count:I reserved0:I",
    }
    coordinator_description = (
        "event_ordinal:Q raw_ordinal:Q build_generation:Q dispatch_edge:Q "
        "release_ordinal:Q release_generation:Q _query_kind:I _query_generation:I _query_object_id:Q "
        "issue_cycle:Q issue_ordinal:Q device_epoch:Q writer_capture_id:Q writer_generation:Q "
        "storage_generation:Q prefix_sequence:Q prefix_initializer:Q prefix_initializer_epoch:Q "
        "record_index:I record_type:I flags:I query_type:I dispatch_result:i release_action:I "
        "release_writer:I prefix_kind:I source_index:I initializer_index:I command_end:I "
        "observation:I reserved0:I reserved1:I"
    )
    producer_description = "frame:Q source:Q last_source:Q sequence:Q"
    expected = {
        "schema": "dxmt9.render_tape.identity.v4", "valid": True, "pair_valid": True,
        "inspection_only": True, "execution_supported": False,
        "production_qualified": False, "header": header,
    }
    for name in tables:
        table = header[name]
        rows = []
        for index in range(table["count"]):
            first = table["offset"] + index * table["entry_size"]
            if name == "sources":
                lifecycle, end = decode_fields(source_description, raw, first)
                lifecycle["phase"] = "completed"
                row, end = decode_fields(
                    "raw_ordinal:Q publication_build_generation:Q publication_control_index:I reserved0:I",
                    raw, end)
                row["lifecycle"] = lifecycle
            elif name == "attempts":
                row, end = decode_fields(
                    "event_ordinal:Q raw_ordinal:Q attempt_generation:Q begin_edge:Q terminal_edge:Q effect_edge:Q",
                    raw, first)
                row["identity"], end = decode_fields(producer_description, raw, end)
                receipt_identity, end = decode_fields(producer_description, raw, end)
                receipt, end = decode_fields(
                    "queue_sequence:Q build_generation:Q source_generation:Q storage_generation:Q "
                    "kind:I control_index:I command_count:I reserved0:I", raw, end)
                receipt["identity"] = receipt_identity
                row["receipt"] = receipt
                tail, end = decode_fields(
                    "first_record:I record_count:I outcome:I staged_command_count:I", raw, end)
                row.update(tail)
            elif name == "coordinators":
                provenance, end = decode_fields(coordinator_description, raw, first)
                provenance["query_identity"] = {
                    "kind": provenance.pop("_query_kind"), "generation": provenance.pop("_query_generation"),
                    "object_id": provenance.pop("_query_object_id"),
                }
                row, end = decode_fields("admission_edge:Q authority:I reserved0:I", raw, end)
                row["provenance"] = provenance
            else:
                row, end = decode_fields(descriptions[name], raw, first)
                if name == "initializers":
                    row["phase"] = "completed"
            assert end - first == table["entry_size"]
            rows.append(row)
        output_name = {"settlements": "replay_settlements", "attempts": "replay_attempts"}.get(name, name)
        expected[output_name] = rows
    topology_raw = topology.read_bytes()
    topology_header, offset = decode_fields(
        "magic:Q identity_bytes:Q events_bytes:Q capture_token:Q capture_epoch:Q queue:Q "
        "identity_digest:32s events_digest:32s version:I header_size:I", topology_raw)
    for name in ("plans", "placements"):
        topology_header[name], offset = decode_fields("offset:I count:I entry_size:I", topology_raw, offset)
    reserved, offset = decode_fields("reserved0:I reserved1:I", topology_raw, offset)
    topology_header.update(reserved)
    assert offset == topology_header["header_size"] == 152
    topology_expected = {
        "schema": "dxmt9.render_tape.execution_topology.v1", "valid": True,
        "header": topology_header,
    }
    for name, description in {
        "plans": "plan_id:Q acceptance_edge:Q route:I fallback_reason:I first_placement:I "
            "placement_count:I executed_count:I reserved0:I",
        "placements": "plan_index:I command_index:I outcome:I execution_ordinal:I dag_pass_index:I "
            "pass_kind:I omission_reason:I reserved0:I",
    }.items():
        table = topology_header[name]
        rows = []
        for index in range(table["count"]):
            first = table["offset"] + index * table["entry_size"]
            row, end = decode_fields(description, topology_raw, first)
            assert end - first == table["entry_size"]
            rows.append(row)
        topology_expected[name] = rows
    expected["execution_topology"] = topology_expected
    assert result == expected
    assert all("membership" not in row and "logical_pass_id" not in row for row in result["commands"])


def pack_pair(runner, tool: pathlib.Path, source: pathlib.Path,
              output: pathlib.Path, *, topology: pathlib.Path | None = None) -> None:
    arguments = ["pack", "--events", str(source / "events.bin"),
                 "--identity", str(source / "identity.bin"),
                 "--output-dir", str(output), "--validator", str(tool)]
    if topology is not None:
        arguments += ["--execution-topology", str(topology)]
    args = runner.parser().parse_args(arguments)
    with contextlib.redirect_stdout(io.StringIO()):
        assert runner.pack(args) == 0


def inspect_pair(runner, tool: pathlib.Path, bundle: pathlib.Path) -> None:
    args = runner.parser().parse_args(["inspect", str(bundle), "--validator", str(tool)])
    with contextlib.redirect_stdout(io.StringIO()):
        assert runner.validate_or_inspect(args) == 0


def write_action_profile(runner, bundle: pathlib.Path, *, raw: bytes | None = None,
                         profiles=(1, 1, 1), revisions=(0, 0)) -> bytes:
    if raw is None:
        identity = (bundle / "identity.bin").read_bytes()
        token = struct.unpack_from("<Q", identity, 32)[0]
        epoch, queue = struct.unpack_from("<QQ", identity, 80)
        header = bytearray(200)
        struct.pack_into("<8sII", header, 0, b"DXMTRAP1", 1, 200)
        struct.pack_into("<6I5Q", header, 16, 1, 1, *profiles, 0, token, epoch, queue, *revisions)
        for filename, size_offset, digest_offset in (
            ("events.bin", 80, 104), ("identity.bin", 88, 136),
            ("execution_topology.bin", 96, 168),
        ):
            component = (bundle / filename).read_bytes()
            struct.pack_into("<Q", header, size_offset, len(component))
            header[digest_offset:digest_offset + 32] = hashlib.sha256(component).digest()
        raw = bytes(header)
    path = bundle / "action_profile.bin"
    path.write_bytes(raw)
    rewrite_manifest(bundle, lambda value: value["components"].update(action_profile={
        "path": "action_profile.bin", "schema": "dxmt9.render_tape.action_profile.v1",
        "bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest(),
    }))
    rewrite_manifest(bundle, lambda value: value.setdefault("scope", {}).update(
        capture_action_profile="queue-observed",
        action_profile_stable_exact=profiles == (1, 1, 1) and revisions == (0, 0)))
    return raw


def check_action_profile_bundles(runner, tool: pathlib.Path, bundle: pathlib.Path,
                                root: pathlib.Path, inspected: dict) -> None:
    # Action-profile admission itself is native-free: fixtures are bound to the
    # real pair's bytes, while inspection subprocess results are mocked here.
    def clone(name: str) -> pathlib.Path:
        target = root / f"action-profile-{name}"
        shutil.copytree(bundle, target)
        return target

    def loaded_profile(target: pathlib.Path) -> dict:
        manifest, *_ = runner.load_bundle(target, allow_identity_v4=True)
        return runner.load_action_profile(target, manifest)

    assert loaded_profile(bundle) == {
        "present": False, "evidence": "legacy-unknown", "stable_exact": False, "witness": None,
    }
    legacy_without_scope = clone("legacy-without-scope")
    rewrite_manifest(legacy_without_scope, lambda value: value.pop("scope"))
    assert loaded_profile(legacy_without_scope)["evidence"] == "legacy-unknown"
    assert loaded_profile(legacy_without_scope)["stable_exact"] is False
    legacy = clone("legacy-declared")
    rewrite_manifest(legacy, lambda value: value["scope"].update(
        capture_action_profile="legacy-unknown", action_profile_stable_exact=False,
        production_capture=True, policy_qualified=True))
    assert loaded_profile(legacy)["evidence"] == "legacy-unknown"
    assert loaded_profile(legacy)["stable_exact"] is False
    inspection_only_guards(runner, tool, legacy, root)

    stable = clone("stable-exact")
    original = write_action_profile(runner, stable)
    result = loaded_profile(stable)
    assert result["present"] is True and result["evidence"] == "queue-observed"
    assert result["stable_exact"] is True
    assert result["witness"]["capture_token"] == inspected["header"]["capture_token"]
    assert result["witness"]["capture_epoch"] == inspected["header"]["capture_epoch"]
    assert result["witness"]["queue"] == inspected["header"]["queue"]
    assert result["witness"]["arm_revision"] == result["witness"]["freeze_revision"] == "0"
    manifest = json.loads((stable / "manifest.json").read_text())
    for command in ("validate", "inspect"):
        args = runner.parser().parse_args([command, str(stable), "--validator", str(tool)])
        output = io.StringIO()
        with mock.patch.object(runner, "run_identity_validator", return_value=copy.deepcopy(inspected)), \
             mock.patch.object(runner, "run_validator", return_value={"valid": True}), \
             contextlib.redirect_stdout(output):
            assert runner.validate_or_inspect(args) == 0
        reported = json.loads(output.getvalue())
        assert reported["action_profile_validation"] == result
        assert reported["scope"] == manifest["scope"]
        assert reported["identity_validation"]["inspection_only"] is True
        assert reported["identity_validation"]["execution_supported"] is False
        assert reported["identity_validation"]["production_qualified"] is False
    assert json.loads((stable / "manifest.json").read_text()) == manifest
    inspection_only_guards(runner, tool, stable, root)

    for name, profiles, revisions in (
        ("normal", (0, 0, 0), (0, 0)),
        ("post-arm-change", (1, 1, 0), (0, 1)),
        ("aba", (1, 1, 1), (0, 2)),
        ("pre-arm-change", (1, 1, 1), (2, 2)),
    ):
        target = clone(name)
        write_action_profile(runner, target, profiles=profiles, revisions=revisions)
        evidence = loaded_profile(target)
        assert evidence["evidence"] == "queue-observed" and evidence["stable_exact"] is False
        inspection_only_guards(runner, tool, target, root)

    mutations = [
        ("magic", 0, "8s", b"NOTVALID", "header is unsupported"),
        ("version", 8, "I", 2, "header is unsupported"),
        ("header-size", 12, "I", 199, "header is unsupported"),
        ("witness-version", 16, "I", 2, "witness is inconsistent"),
        ("unknown-evidence", 20, "I", 0, "witness is inconsistent"),
        ("construction-profile", 24, "I", 2, "witness is inconsistent"),
        ("arm-profile", 28, "I", 2, "witness is inconsistent"),
        ("freeze-profile", 32, "I", 2, "witness is inconsistent"),
        ("reserved", 36, "I", 1, "witness is inconsistent"),
        ("construction-inconsistent", 24, "I", 0, "witness is inconsistent"),
        ("equal-revision-inconsistent", 32, "I", 0, "witness is inconsistent"),
        ("revision-backward", 64, "2Q", (2, 1), "witness is inconsistent"),
        ("arm-parity-inconsistent", 64, "2Q", (1, 2), "witness is inconsistent"),
        ("freeze-parity-inconsistent", 72, "Q", 1, "witness is inconsistent"),
        ("zero-events-size", 80, "Q", 0, "component sizes must be nonzero"),
        ("zero-identity-size", 88, "Q", 0, "component sizes must be nonzero"),
        ("zero-topology-size", 96, "Q", 0, "component sizes must be nonzero"),
    ]
    for name, offset in (("token", 40), ("epoch", 48), ("queue", 56)):
        mutations.append((name, offset, "Q", struct.unpack_from("<Q", original, offset)[0] + 1,
                          "capture binding mismatch"))
    for name, size_offset, digest_offset in (("events", 80, 104), ("identity", 88, 136),
                                           ("execution_topology", 96, 168)):
        mutations.append((f"{name}-size", size_offset, "Q", 1, f"{name} binding mismatch"))
        mutations.append((f"{name}-digest", digest_offset, "32s", b"\0" * 32, f"{name} binding mismatch"))
    for name, offset, kind, value, detail in mutations:
        target = clone(name)
        damaged = bytearray(original)
        values = value if isinstance(value, tuple) else (value,)
        struct.pack_into("<" + kind, damaged, offset, *values)
        write_action_profile(runner, target, raw=bytes(damaged))
        reject(lambda: loaded_profile(target), detail)

    for name, update, detail in (
        ("schema", lambda value: value["components"]["action_profile"].update(schema="unsupported"), "schema is unsupported"),
        ("path", lambda value: value["components"]["action_profile"].update(path="../action_profile.bin"), "path must be canonical"),
        ("size", lambda value: value["components"]["action_profile"].update(bytes=199), "size mismatch"),
        ("digest", lambda value: value["components"]["action_profile"].update(sha256="0" * 64), "digest mismatch"),
        ("missing-profile-scope", lambda value: value["scope"].pop("capture_action_profile"), "requires scope.capture_action_profile"),
        ("missing-stable-scope", lambda value: value["scope"].pop("action_profile_stable_exact"), "requires scope.action_profile_stable_exact"),
        ("wrong-profile-scope", lambda value: value["scope"].update(capture_action_profile="legacy-unknown"), "contradicts scope.capture_action_profile"),
        ("wrong-stable-scope", lambda value: value["scope"].update(action_profile_stable_exact=False), "contradicts scope.action_profile_stable_exact"),
        ("truthy-stable-scope", lambda value: value["scope"].update(action_profile_stable_exact=1), "contradicts scope.action_profile_stable_exact"),
    ):
        target = root / f"action-profile-bad-{name}"
        shutil.copytree(stable, target)
        rewrite_manifest(target, update)
        reject(lambda: loaded_profile(target), detail)
    for name, changes, detail in (
        ("fake-known", {"capture_action_profile": "queue-observed"}, "contradicts scope.capture_action_profile"),
        ("fake-stable", {"action_profile_stable_exact": True}, "contradicts scope.action_profile_stable_exact"),
        ("fake-truthy-stable", {"action_profile_stable_exact": 1}, "contradicts scope.action_profile_stable_exact"),
    ):
        target = clone(name)
        rewrite_manifest(target, lambda value: value["scope"].update(changes))
        reject(lambda: loaded_profile(target), detail)

    target = clone("orphan")
    write_action_profile(runner, target)
    rewrite_manifest(target, lambda value: [value["components"].pop(name)
                                           for name in ("identity", "execution_topology")])
    reject(lambda: loaded_profile(target), "orphan action profile")
    for name, raw in (("short", original[:-1]), ("long", original + b"\0")):
        target = clone(name)
        write_action_profile(runner, target, raw=raw)
        reject(lambda: loaded_profile(target), "exact 200-byte header")
    target = clone("digest-tamper")
    write_action_profile(runner, target)
    damaged = bytearray(original)
    damaged[0] ^= 1
    (target / "action_profile.bin").write_bytes(damaged)
    reject(lambda: loaded_profile(target), "action profile digest mismatch")
    target = clone("missing-component-file")
    write_action_profile(runner, target)
    (target / "action_profile.bin").unlink()
    reject(lambda: loaded_profile(target), "component is missing")
    target = clone("unlisted-component-file")
    (target / "action_profile.bin").write_bytes(original)
    reject(lambda: loaded_profile(target), "unlisted top-level")
    for name, filename in (("events", "events.bin"), ("identity", "identity.bin"),
                           ("execution_topology", "execution_topology.bin")):
        target = clone(f"rebound-{name}")
        write_action_profile(runner, target)
        path = target / filename
        damaged = bytearray(path.read_bytes())
        damaged[-1] ^= 1
        path.write_bytes(damaged)
        rewrite_manifest(target, lambda value: value["components"][name].update(sha256=runner.digest(path)))
        reject(lambda: loaded_profile(target), f"action profile {name} binding mismatch")


def check_coordinator_pair_fixtures(runner, tool: pathlib.Path, bundle_tool: pathlib.Path,
                                    source: pathlib.Path, root: pathlib.Path) -> None:
    for authority, name in ((1, "session-coordinator"), (2, "ordinary-coordinator")):
        fixture = source / name
        events, identity = fixture / "events.bin", fixture / "identity.bin"
        topology = fixture / "execution_topology.bin"
        inspected = json.loads(run(tool, "identity", events, identity,
                                   "--execution-topology", topology).stdout)
        assert_v4_exact_json(inspected, identity, topology)
        assert inspected["header"]["coordinator_domain"] == authority
        assert inspected["coordinators"]
        for row in inspected["coordinators"]:
            assert row["authority"] == authority and row["reserved0"] == 0
            provenance = row["provenance"]
            assert provenance["reserved0"] == provenance["reserved1"] == 0
            if authority == 1:
                assert row["admission_edge"] == "0"
                assert int(provenance["release_ordinal"]) > 0
                assert int(provenance["release_generation"]) > 0
            else:
                assert int(row["admission_edge"]) > 0
                assert provenance["release_ordinal"] == provenance["release_generation"] == "0"
                assert provenance["release_action"] == provenance["release_writer"] == 0
        bundle = root / f"{name}-bundle"
        pack_pair(runner, tool, fixture, bundle, topology=topology)
        for command in ("validate", "inspect"):
            result = json.loads(run(sys.executable, bundle_tool, command, bundle,
                                    "--validator", tool).stdout)
            assert result["identity_validation"] == inspected
        inspection_only_guards(runner, tool, bundle, root)

        # Rebind every damaged identity honestly in its companion: the native
        # typed grammar, not a stale digest, rejects authority/lifecycle lies.
        coordinator_offset = inspected["header"]["coordinators"]["offset"]
        mutations = (
            ("unknown-authority", coordinator_offset + 200, "I", 3),
            ("wrong-domain", coordinator_offset + 200, "I", 3 - authority),
            ("reserved-authority", coordinator_offset + 204, "I", 1),
            ("reserved-provenance", coordinator_offset + 184, "I", 1),
            ("false-admission", coordinator_offset + 192, "Q", 1 if authority == 1 else 0),
            ("false-release", coordinator_offset + 32, "Q", 0 if authority == 1 else 1),
            ("unknown-domain", 256, "I", 3),
            ("reserved-header", 260, "I", 1),
            ("retired-header-size", 76, "I", 256),
        )
        for mutation, offset, kind, value in mutations:
            identity_bytes = bytearray(identity.read_bytes())
            struct.pack_into("<" + kind, identity_bytes, offset, value)
            damaged_identity = root / f"{name}-{mutation}-identity.bin"
            damaged_identity.write_bytes(identity_bytes)
            topology_bytes = bytearray(topology.read_bytes())
            topology_bytes[48:80] = hashlib.sha256(identity_bytes).digest()
            damaged_topology = root / f"{name}-{mutation}-topology.bin"
            damaged_topology.write_bytes(topology_bytes)
            rejected = run(tool, "identity", events, damaged_identity,
                           "--execution-topology", damaged_topology, check=False)
            assert rejected.returncode != 0 and "identity v4 failed" in rejected.stderr
            reject(lambda: runner.run_identity_validator(tool, events, damaged_identity, [],
                execution_topology=damaged_topology), "identity validation failed")


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit("usage: test_render_tape_identity_v4_cli.py <identity-v4-fixture> <native-tool> <bundle-tool>")
    fixture, tool, bundle_tool = map(pathlib.Path, sys.argv[1:])
    runner = load_runner(bundle_tool)
    with tempfile.TemporaryDirectory(prefix="dxmt9-identity-v4-cli-") as directory:
        root = pathlib.Path(directory)
        source = root / "source"
        run(fixture, "--write-inspection-fixture", source)
        events, identity = source / "events.bin", source / "identity.bin"
        topology = source / "execution_topology.bin"
        inspected = json.loads(run(tool, "identity", events, identity,
                                   "--execution-topology", topology).stdout)
        assert_v4_exact_json(inspected, identity, topology)
        assert int(inspected["header"]["frame_id"]) > 2**53
        assert runner.identity_binary_schema(identity) == runner.IDENTITY_V4_SCHEMA
        assert runner.run_identity_validator(tool, events, identity, [],
                                             execution_topology=topology) == inspected
        missing = run(tool, "identity", events, identity, check=False)
        assert missing.returncode != 0 and "requires execution topology" in missing.stderr
        duplicate = run(tool, "identity", events, identity, "--execution-topology", topology,
                        "--execution-topology", topology, check=False)
        assert duplicate.returncode != 0
        with mock.patch.object(runner, "bundle_transaction") as transaction:
            reject(lambda: pack_pair(runner, tool, source, root / "missing-pair"),
                   "requires execution topology")
            transaction.assert_not_called()
        assert not (root / "missing-pair").exists()
        orphan_args = runner.parser().parse_args([
            "pack", "--events", str(events), "--execution-topology", str(topology),
            "--output-dir", str(root / "orphan-pair"), "--validator", str(tool),
        ])
        with mock.patch.object(runner, "bundle_transaction") as transaction, \
             mock.patch.object(runner.subprocess, "run") as child:
            reject(lambda: runner.pack(orphan_args), "orphan execution topology")
            transaction.assert_not_called()
            child.assert_not_called()

        bundle = root / "bundle"
        pack_pair(runner, tool, source, bundle, topology=topology)
        manifest, *_ = runner.load_bundle(bundle, allow_identity_v4=True)
        assert manifest["components"]["execution_topology"] == {
            "path": runner.EXECUTION_TOPOLOGY_NAME, "schema": runner.EXECUTION_TOPOLOGY_SCHEMA,
            "bytes": topology.stat().st_size, "sha256": runner.digest(topology),
        }
        reject(lambda: runner.load_bundle(bundle), "inspection-only")
        for command in ("validate", "inspect"):
            result = json.loads(run(sys.executable, bundle_tool, command, bundle,
                                    "--validator", tool).stdout)
            assert result["identity_validation"] == inspected
            assert result["scope"] == manifest["scope"]
        inspection_only_guards(runner, tool, bundle, root)
        check_action_profile_bundles(runner, tool, bundle, root, inspected)
        rewrite_manifest(bundle, lambda value: value["scope"].update(
            production_capture=True, production_provider_replay=True))
        declared = json.loads(run(sys.executable, bundle_tool, "inspect", bundle,
                                 "--validator", tool).stdout)
        assert declared["scope"]["production_provider_replay"] is True
        assert declared["identity_validation"]["production_qualified"] is False
        inspection_only_guards(runner, tool, bundle, root)

        def clone(name: str) -> pathlib.Path:
            target = root / name
            shutil.copytree(bundle, target)
            return target

        for name, update, detail in (
            ("missing-declaration", lambda v: v["components"].pop("execution_topology"), "requires execution topology"),
            ("bad-topology-schema", lambda v: v["components"]["execution_topology"].update(schema="unsupported"), "schema is unsupported"),
            ("bad-topology-path", lambda v: v["components"]["execution_topology"].update(path="../execution_topology.bin"), "path must be canonical"),
            ("bad-topology-size", lambda v: v["components"]["execution_topology"].update(bytes=1), "size mismatch"),
            ("bad-topology-digest", lambda v: v["components"]["execution_topology"].update(sha256="0" * 64), "digest mismatch"),
            ("unknown-component", lambda v: v["components"].update(unknown={}), "unknown components"),
            ("orphan-topology", lambda v: v["components"].pop("identity"), "orphan execution topology"),
            ("wrong-identity-schema", lambda v: v["components"]["identity"].update(schema=runner.IDENTITY_V3_SCHEMA), "manifest schema does not match binary"),
        ):
            target = clone(name)
            rewrite_manifest(target, update)
            reject(lambda: runner.load_bundle(target, allow_identity_v3=True, allow_identity_v4=True), detail)
        target = clone("missing-file")
        (target / "execution_topology.bin").unlink()
        reject(lambda: runner.load_bundle(target, allow_identity_v4=True), "component is missing")
        target = clone("unlisted-file")
        (target / "unknown.bin").write_bytes(b"unlisted")
        reject(lambda: runner.load_bundle(target, allow_identity_v4=True), "unlisted top-level")

        # Rehashing altered companions gets past manifest authentication but
        # never the genuine native pair/row validator.
        for index, offset in enumerate((8, 16, 24, 32, 40, 48, 80, 112, 120, 144)):
            target = clone(f"invalid-topology-{index}")
            path = target / "execution_topology.bin"
            damaged = bytearray(path.read_bytes())
            damaged[offset] ^= 1
            path.write_bytes(damaged)
            rewrite_manifest(target, lambda v: v["components"]["execution_topology"].update(
                sha256=runner.digest(path)))
            runner.load_bundle(target, allow_identity_v4=True)
            reject(lambda: inspect_pair(runner, tool, target), "identity validation failed")
        alternate = source / "identity-alternate.bin"
        alternate_topology = source / "execution_topology-alternate.bin"
        assert run(tool, "identity", events, alternate, "--execution-topology", alternate_topology).returncode == 0
        cross_pair = run(tool, "identity", events, alternate, "--execution-topology", topology, check=False)
        assert cross_pair.returncode != 0 and "topology validation failed" in cross_pair.stderr
        damaged_identity = root / "damaged-identity.bin"
        identity_bytes = bytearray(identity.read_bytes())
        identity_bytes[40] ^= 1
        damaged_identity.write_bytes(identity_bytes)
        malformed = run(tool, "identity", events, damaged_identity,
                        "--execution-topology", topology, check=False)
        assert malformed.returncode != 0 and "identity v4 failed" in malformed.stderr

        # Optimizer omission does not erase an actual submitted source's
        # publication/completion evidence, even when its only command is omitted.
        omitted_path = root / "fully-omitted-topology.bin"
        omitted = bytearray(topology.read_bytes())
        plan_offset = struct.unpack_from("<I", omitted, 120)[0]
        placement_offset = struct.unpack_from("<I", omitted, 132)[0]
        struct.pack_into("<I", omitted, plan_offset + 32, 2)  # executedCount
        struct.pack_into("<6I", omitted, placement_offset + 8,
                         3, 0xffffffff, 0xffffffff, 0, 1, 0)
        struct.pack_into("<I", omitted, placement_offset + 2 * 32 + 12, 1)
        omitted_path.write_bytes(omitted)
        omitted_inspection = json.loads(run(tool, "identity", events, identity,
                                            "--execution-topology", omitted_path).stdout)
        assert_v4_exact_json(omitted_inspection, identity, omitted_path)
        assert omitted_inspection["sources"] == inspected["sources"]
        omitted_bundle = root / "submitted-omitted-bundle"
        pack_pair(runner, tool, source, omitted_bundle, topology=omitted_path)
        inspect_pair(runner, tool, omitted_bundle)

        # A true inline/no-submit terminal remains unsupported. Bind its altered
        # bytes honestly to the companion so rejection is lifecycle-based, not
        # merely a stale full-identity digest or fabricated completion fence.
        inline_identity = root / "inline-identity.bin"
        inline_bytes = bytearray(identity.read_bytes())
        source_offset = inspected["header"]["sources"]["offset"]
        struct.pack_into("<3Q", inline_bytes, source_offset + 48, 0, 0, 0)
        inline_identity.write_bytes(inline_bytes)
        inline_topology = root / "inline-topology.bin"
        inline_companion = bytearray(omitted)
        inline_companion[48:80] = hashlib.sha256(inline_bytes).digest()
        inline_topology.write_bytes(inline_companion)
        unsupported = run(tool, "identity", events, inline_identity,
                          "--execution-topology", inline_topology, check=False)
        assert unsupported.returncode != 0 and "identity v4 failed" in unsupported.stderr

        target = clone("cross-pair")
        (target / "identity.bin").write_bytes(alternate.read_bytes())
        rewrite_manifest(target, lambda v: v["components"]["identity"].update(
            sha256=runner.digest(target / "identity.bin")))
        reject(lambda: inspect_pair(runner, tool, target), "identity validation failed")

        # A source changed during copy cannot publish the previously admitted
        # pair. The staged pair is revalidated before any final artifact.
        real_copy = runner.copy_flushed
        output = root / "changed-during-copy"

        def change_topology(source_path, target_path):
            real_copy(source_path, target_path)
            if target_path.name == runner.EXECUTION_TOPOLOGY_NAME:
                damaged = bytearray(target_path.read_bytes())
                damaged[48] ^= 1
                target_path.write_bytes(damaged)

        with mock.patch.object(runner, "copy_flushed", side_effect=change_topology):
            reject(lambda: pack_pair(runner, tool, source, output, topology=topology),
                   "identity validation failed")
        assert not output.exists()
        assert not list(root.glob(".changed-during-copy.staging*"))

        # An old or unsafe validator cannot turn inspection into qualification.
        for update, detail in (
            (lambda v: v.update(execution_supported=True), "unsafe scope"),
            (lambda v: v.update(pair_valid=False), "complete pair"),
            (lambda v: v["execution_topology"]["header"].update(identity_bytes="1"), "pair binding mismatch"),
            (lambda v: v["execution_topology"]["header"].update(queue="1"), "capture binding mismatch"),
            (lambda v: v["header"].update(header_size=256), "typed coordinator evidence"),
            (lambda v: v["header"].update(coordinator_domain=3), "typed coordinator evidence"),
            (lambda v: v["header"].update(reserved4=1), "typed coordinator evidence"),
            (lambda v: v["header"]["coordinators"].update(entry_size=192), "typed coordinator evidence"),
            (lambda v: v["header"]["coordinators"].update(count=1), "typed coordinator evidence"),
        ):
            result = copy.deepcopy(inspected)
            update(result)
            response = subprocess.CompletedProcess([], 0, json.dumps(result), "")
            with mock.patch.object(runner.subprocess, "run", return_value=response):
                reject(lambda: runner.run_identity_validator(tool, events, identity, [],
                    execution_topology=topology), detail)
        assert hashlib.sha256(topology.read_bytes()).hexdigest() == \
            manifest["components"]["execution_topology"]["sha256"]
        check_coordinator_pair_fixtures(runner, tool, bundle_tool, source, root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
