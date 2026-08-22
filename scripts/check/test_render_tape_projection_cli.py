#!/usr/bin/env python3
"""Regression coverage for the bounded Render Tape v2 projection CLI."""

from __future__ import annotations

import hashlib
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile


TEXTURE_DIGEST = "8d70d691c822d55638b6e7fd54cd94170c87d19eb1f628b757506ede5688d297"
SHADER_DIGEST = "9f64a747e1b97f131fabb6b447296c9b6f0201e79fb3c5356e6c77e89b6a806a"


def run(*args: object, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(arg) for arg in args],
        check=check,
        capture_output=True,
        text=True,
    )


def main() -> int:
    if len(sys.argv) != 6:
        raise SystemExit(
            "usage: test_render_tape_projection_cli.py <fixture> <tool> "
            "<bundle-tool> <provider-fixture> <provider>"
        )
    fixture = pathlib.Path(sys.argv[1])
    tool = pathlib.Path(sys.argv[2])
    bundle_tool = pathlib.Path(sys.argv[3])
    provider_fixture = pathlib.Path(sys.argv[4])
    provider = pathlib.Path(sys.argv[5])
    with tempfile.TemporaryDirectory(prefix="dxmt9-render-tape-project-") as root:
        tape = pathlib.Path(root) / "events.bin"
        identity = pathlib.Path(root) / "identity.bin"
        run(fixture, "--write-fixture", tape, identity)
        before = hashlib.sha256(tape.read_bytes()).hexdigest()
        common = (
            "--verified-blob",
            f"{SHADER_DIGEST}:4",
            "--verified-blob",
            f"{TEXTURE_DIGEST}:4",
        )
        completed = run(
            tool,
            "project",
            tape,
            "--command-event-ordinal",
            6,
            "--first-record",
            1,
            "--record-count",
            2,
            *common,
        )
        artifact = json.loads(completed.stdout)
        assert artifact["schema"] == "dxmt9.render_tape.projection.v1"
        assert artifact["status"] == "projection-ready"
        assert artifact["scope"] == {
            "profile": "frame-tape",
            "claim": "structural-projection-readiness-only",
            "wire_bytes_rewritten": False,
            "legacy_mini_replay_manifest": False,
            "provider_replay": False,
            "gt2_replay": False,
        }
        assert artifact["source"]["sha256"] == before
        assert artifact["selector"] == {
            "command_event_ordinal": 6,
            "first_record_index": 1,
            "record_count": 2,
        }
        assert artifact["bootstrap"] == {
            "derived": True,
            "selected_record_full_snapshot": False,
            "coverage_mask": (1 << 23) - 1,
        }
        assert [(entry["event_ordinal"], entry["record_index"])
                for entry in artifact["draws"]] == [(6, 1), (6, 2)]
        assert artifact["boundaries"]["clear"]["record_index"] == 0
        assert artifact["boundaries"]["present"]["record_index"] == 3
        assert [entry["identity"]["generation"]
                for entry in artifact["objects"]] == [1, 3, 7]
        assert [entry["digest"] for entry in artifact["blob_references"]] == [
            SHADER_DIGEST,
            TEXTURE_DIGEST,
        ]
        assert artifact["conservation"] == {
            "selected_draws": 2,
            "excluded_records": 2,
            "objects": 3,
            "blob_references": 2,
            "excluded_coordinator_events": 3,
        }
        encoded = completed.stdout
        for unsupported_key in (
            "frame_id",
            "source_ordinal",
            "source_seq_id",
            "logical_pass_id",
        ):
            assert unsupported_key not in encoded
        assert hashlib.sha256(tape.read_bytes()).hexdigest() == before

        rejected = run(
            tool,
            "project",
            tape,
            "--command-event-ordinal",
            6,
            "--first-record",
            0,
            "--record-count",
            2,
            *common,
            check=False,
        )
        assert rejected.returncode == 1
        assert rejected.stdout == ""
        assert "status=non-draw-record" in rejected.stderr

        identity_result = json.loads(run(
            tool, "identity", tape, identity, *common,
        ).stdout)
        assert identity_result == {
            "schema": "dxmt9.render_tape.identity.v2",
            "valid": True,
            "authority": 1,
            "frame_id": 42,
            "present_ordinal": 6,
            "sources": 1,
            "ranges": 1,
            "completed_segment_count": 0,
            "completion_evidence": "none",
            "settlement_count": 0,
            "settlement_table_count": 0,
            "segments": [{
                "segment_index": 0,
                "event_ordinal": 6,
                "source_ordinal": 101,
                "seq_id": 501,
                "first_record": 0,
                "record_count": 4,
            }],
        }
        projected_events = pathlib.Path(root) / "projected.bin"
        materialized = json.loads(run(
            tool, "materialize", tape, identity, projected_events,
            "--command-event-ordinal", 6,
            "--first-record", 1,
            "--record-count", 2,
            *common,
        ).stdout)
        assert materialized["schema"] == "dxmt9.render_tape.v2"
        assert materialized["logical_pass_id"] == 9001
        assert materialized["referenced_blobs"] == [SHADER_DIGEST, TEXTURE_DIGEST]
        assert json.loads(run(
            tool, "validate", projected_events, *common,
        ).stdout)["valid"] is True
        materialized_bytes = projected_events.read_bytes()
        rejected = run(
            tool, "materialize", tape, identity, projected_events,
            "--command-event-ordinal", 6,
            "--first-record", 1,
            "--record-count", 2,
            *common, check=False,
        )
        assert rejected.returncode != 0
        assert projected_events.read_bytes() == materialized_bytes
        assert not list(pathlib.Path(root).glob(".projected-events.bin.staging-*"))

        shader_blob = pathlib.Path(root) / "shader.bin"
        texture_blob = pathlib.Path(root) / "texture.bin"
        shader_blob.write_bytes(bytes((1, 2, 3, 4)))
        texture_blob.write_bytes(bytes((0xAA, 0xBB, 0xCC, 0xDD)))
        source_bundle = pathlib.Path(root) / "source-bundle"
        run(
            sys.executable, bundle_tool, "pack", "--events", tape,
            "--identity", identity, "--blob", shader_blob, "--blob", texture_blob,
            "--output-dir", source_bundle, "--validator", tool,
        )
        fake_provider = pathlib.Path(root) / "fake-provider.py"
        pixels = bytes((1, 2, 3, 255, 4, 5, 6, 255))
        pixel_sha = hashlib.sha256(pixels).hexdigest()
        fake_provider.write_text(
            "#!/usr/bin/env python3\n"
            "import json,pathlib,sys\n"
            f"pixels={pixels!r}\n"
            "args=sys.argv[1:]\n"
            "has_expected='--expected-rgba' in args or 'source-bundle' in args[1]\n"
            "if '--output-rgba' in args:\n"
            "  out=args[args.index('--output-rgba')+1]\n"
            "  pathlib.Path(out).write_bytes(pixels)\n"
            f"sha='{pixel_sha}'\n"
            "valid={'structurally_valid':True,'digests_valid':True,'output_readback':True,"
            "'expected_digest_captured':has_expected,'expected_digest_matched':has_expected,"
            "'expected_pixels_compared':False,'pixel_envelope_matched':False,"
            "'output_oracle_matched':has_expected,"
            "'oracle_mode':'strict' if has_expected else 'rejected',"
            "'output_non_degenerate':True,'output_bytes':len(pixels),'output_sha256':sha}\n"
            "result={'schema':'dxmt9.render_tape.provider_replay.v2',"
            "'archive_policy':'disabled','profile':'frame-tape','status':'complete',"
            "'failed_event':4294967295,"
            "'requirements':{'output_width':2,'output_height':1,'output_format':21},"
            "'validity':valid,'coverage':{'present_records':1,'present_source_mappings':1,"
            "'present_outputs':1,'seed_mutations':1},'conservation':{'input_blobs':2,"
            "'referenced_blobs':2,'objects_created':3,'objects_released':3,"
            "'present_ordinal':6,'completion_ordinal':9},'intervals':[{'present_ordinal':6,"
            "'completion_ordinal':9,'validity':valid}]}\n"
            "print(json.dumps(result,sort_keys=True))\n",
            encoding="utf-8",
        )
        fake_provider.chmod(0o755)

        identity_replay = json.loads(run(
            sys.executable, bundle_tool, "provider-replay", source_bundle,
            "--provider", fake_provider, "--validator", tool, "--repeat", 1,
        ).stdout)
        assert identity_replay["oracle_accepted"] is True

        claiming_provider = pathlib.Path(root) / "claiming-provider.py"
        claiming_provider.write_text(
            fake_provider.read_text(encoding="utf-8").replace(
                "has_expected='--expected-rgba' in args or 'source-bundle' in args[1]",
                "has_expected=True",
            ),
            encoding="utf-8",
        )
        claiming_provider.chmod(0o755)
        claimed_output = pathlib.Path(root) / "claimed-output"
        rejected = run(
            sys.executable, bundle_tool, "executable-project", source_bundle,
            "--output-dir", claimed_output, "--command-event-ordinal", 6,
            "--first-record", 1, "--record-count", 2,
            "--validator", tool, "--provider", claiming_provider, check=False,
        )
        assert rejected.returncode != 0 and not claimed_output.exists()
        assert not list(pathlib.Path(root).glob(".claimed-output.staging-*"))

        existing_output = pathlib.Path(root) / "existing-output"
        existing_output.mkdir()
        rejected = run(
            sys.executable, bundle_tool, "executable-project", source_bundle,
            "--output-dir", existing_output, "--command-event-ordinal", 6,
            "--first-record", 1, "--record-count", 2,
            "--validator", tool, "--provider", fake_provider, check=False,
        )
        assert rejected.returncode != 0
        assert existing_output.is_dir() and not list(existing_output.iterdir())
        assert not list(pathlib.Path(root).glob(".existing-output.staging-*"))

        dangling_output = pathlib.Path(root) / "dangling-output"
        dangling_target = pathlib.Path(root) / "missing-output-target"
        dangling_output.symlink_to(dangling_target, target_is_directory=True)
        rejected = run(
            sys.executable, bundle_tool, "executable-project", source_bundle,
            "--output-dir", dangling_output, "--command-event-ordinal", 6,
            "--first-record", 1, "--record-count", 2,
            "--validator", tool, "--provider", fake_provider, check=False,
        )
        assert rejected.returncode != 0
        assert dangling_output.is_symlink() and not dangling_target.exists()
        assert not list(pathlib.Path(root).glob(".dangling-output.staging-*"))

        output_bundle = pathlib.Path(root) / "projected-bundle"
        executed = json.loads(run(
            sys.executable, bundle_tool, "executable-project", source_bundle,
            "--output-dir", output_bundle, "--command-event-ordinal", 6,
            "--first-record", 1, "--record-count", 2,
            "--validator", tool, "--provider", fake_provider,
        ).stdout)
        assert executed["oracle_accepted"] is True
        manifest = json.loads((output_bundle / "manifest.json").read_text())
        assert manifest["schema"] == "dxmt9.render_tape.bundle.v2"
        assert manifest["stage"] == "executable-projection"
        assert manifest["scope"] == {
            "production_capture": False,
            "production_provider_replay": True,
            "output_oracle": True,
            "executable_projection": True,
            "source_full_frame_oracle_copied": False,
        }
        assert manifest["components"]["identity"]["schema"] == (
            "dxmt9.render_tape.identity.v2"
        )
        assert (output_bundle / "identity.bin").is_file()
        assert (output_bundle / "output.rgba").read_bytes() == pixels
        assert not list(pathlib.Path(root).glob(".projected-bundle.staging-*"))

        provider_source = pathlib.Path(root) / "provider-source"
        provider_bundle = pathlib.Path(root) / "provider-bundle"
        run(provider_fixture, "--write-projection-fixture", provider_source)
        provider_events = provider_source / "events.bin"
        provider_before = hashlib.sha256(provider_events.read_bytes()).hexdigest()
        seed_sha = hashlib.sha256(
            (provider_source / "seed.bin").read_bytes()
        ).hexdigest()
        provider_plan = json.loads(run(
            tool, "project", provider_events,
            "--command-event-ordinal", 6,
            "--first-record", 0,
            "--record-count", 1,
            "--verified-blob", f"{seed_sha}:16",
        ).stdout)
        assert provider_plan["bootstrap"]["derived"] is True
        assert (
            provider_plan["bootstrap"]["selected_record_full_snapshot"] is False
        )
        assert (
            hashlib.sha256(provider_events.read_bytes()).hexdigest()
            == provider_before
        )
        run(
            sys.executable, bundle_tool, "pack",
            "--events", provider_events,
            "--identity", provider_source / "identity.bin",
            "--blob", provider_source / "seed.bin",
            "--output-dir", provider_bundle,
            "--validator", tool,
        )
        real_output = pathlib.Path(root) / "provider-projected-bundle"
        real_projection = json.loads(run(
            sys.executable, bundle_tool, "executable-project", provider_bundle,
            "--output-dir", real_output,
            "--command-event-ordinal", 6,
            "--first-record", 0,
            "--record-count", 1,
            "--validator", tool,
            "--provider", provider,
        ).stdout)
        assert real_projection["oracle_accepted"] is True
        real_manifest = json.loads((real_output / "manifest.json").read_text())
        assert real_manifest["validity"] == {
            "fresh_process_repeats": 2,
            "provider_replay": True,
            "strict_projected_oracle": True,
            "structural": True,
        }
        assert real_manifest["scope"]["production_provider_replay"] is True
        assert real_manifest["projection"]["command_event_ordinal"] == 6
        assert real_manifest["projection"]["first_record"] == 0
        assert real_manifest["projection"]["record_count"] == 1
        assert len(real_manifest["projection"]["provider_runs"]) == 2
        assert (real_manifest["projection"]["provider_runs"][0] ==
                real_manifest["projection"]["provider_runs"][1])

        real_replay = json.loads(run(
            sys.executable, bundle_tool, "provider-replay", real_output,
            "--provider", provider,
            "--validator", tool,
            "--warmup", 1,
            "--repeat", 2,
        ).stdout)
        assert real_replay["oracle_accepted"] is True
        assert real_replay["deterministic"] is True
        assert real_replay["replay_policy"] == {
            "reset": "fresh-process-device",
            "warmup": 1,
            "repeat": 2,
        }
        assert len(real_replay["warmup_runs"]) == 1
        assert len(real_replay["runs"]) == 2
        assert real_replay["runs"][0] == real_replay["runs"][1]
        evidence = real_replay["runs"][0]["identity_evidence"]
        assert evidence["authority"] == "derived-projection"
        assert evidence["derived_sidecar"] is True
        assert evidence["segment_count"] == evidence["provenance_segment_count"]
        assert evidence["completed_segment_count"] == 0
        assert evidence["completion_evidence"] == "not-queue-authenticated"
        assert evidence["settlement_count"] == evidence["settlement_table_count"]
        assert evidence["settlement_count"] == 1
        assert len(evidence["segments"]) == evidence["segment_count"]
        assert all(
            row["source_ordinal"] > 0 and row["seq_id"] > 0
            for row in evidence["segments"]
        )
        assert evidence["event_settlement_table"] == []
        assert len(evidence["derived_settlement_table"]) == 1
        assert evidence["final_event_settlement"] is None

        retired_v1 = pathlib.Path(root) / "retired-v1-identity.bin"
        retired_bytes = bytearray((provider_source / "identity.bin").read_bytes())
        retired_bytes[72:76] = (1).to_bytes(4, "little")
        retired_v1.write_bytes(retired_bytes)
        retired = run(
            provider, "replay", provider_events,
            "--identity", retired_v1,
            "--blob", provider_source / "seed.bin", check=False,
        )
        assert retired.returncode != 0
        retired_result = json.loads(retired.stdout)
        assert retired_result["identity_evidence"]["rejected"] is True
        assert retired_result["identity_evidence"]["rejection_reason"] == (
            "invalid-header"
        )

        damaged_identity = pathlib.Path(root) / "damaged-identity.bin"
        damaged = bytearray(identity.read_bytes())
        damaged[40] ^= 1
        damaged_identity.write_bytes(damaged)
        native_failed_output = pathlib.Path(root) / "must-not-exist.bin"
        rejected = run(
            tool, "materialize", tape, damaged_identity,
            native_failed_output,
            "--command-event-ordinal", 6, "--first-record", 1,
            "--record-count", 2, *common, check=False,
        )
        assert rejected.returncode != 0
        assert not native_failed_output.exists()

        damaged_bundle = pathlib.Path(root) / "damaged-bundle"
        shutil.copytree(source_bundle, damaged_bundle)
        damaged_component = bytearray((damaged_bundle / "identity.bin").read_bytes())
        damaged_component[40] ^= 1
        (damaged_bundle / "identity.bin").write_bytes(damaged_component)
        failed_output = pathlib.Path(root) / "failed-bundle"
        rejected = run(
            sys.executable, bundle_tool, "executable-project", damaged_bundle,
            "--output-dir", failed_output, "--command-event-ordinal", 6,
            "--first-record", 1, "--record-count", 2,
            "--validator", tool, "--provider", fake_provider, check=False,
        )
        assert rejected.returncode != 0
        assert not failed_output.exists()
        assert not list(pathlib.Path(root).glob(".failed-bundle.staging-*"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
