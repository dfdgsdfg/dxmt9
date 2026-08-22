#!/usr/bin/env python3

import json
import pathlib
import shutil
import subprocess
import sys
import tempfile


def run(*args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(args, check=check, text=True, capture_output=True)


def main() -> int:
    if len(sys.argv) != 5:
        raise SystemExit(
            "usage: test_render_tape_provider_cli.py <fixture> <provider> "
            "<validator> <runner>"
        )
    fixture = pathlib.Path(sys.argv[1])
    provider = pathlib.Path(sys.argv[2])
    validator = pathlib.Path(sys.argv[3])
    runner = pathlib.Path(sys.argv[4])
    bundle_tool = runner
    with tempfile.TemporaryDirectory(prefix="dxmt9-render-tape-provider-") as root:
        root_path = pathlib.Path(root)
        source = root_path / "source"
        bundle = root_path / "bundle"
        run(str(fixture), "--write-production-fixture", str(source))
        run(
            sys.executable,
            str(bundle_tool),
            "pack",
            "--events",
            str(source / "events.bin"),
            "--output-dir",
            str(bundle),
            "--validator",
            str(validator),
        )
        result = json.loads(
            run(
                sys.executable,
                str(runner),
                "provider-replay",
                str(bundle),
                "--provider",
                str(provider),
                "--warmup",
                "1",
                "--repeat",
                "2",
            ).stdout
        )
        assert result["status"] == "complete"
        assert result["provider_exit_code"] == 0
        assert result["archive_policy"] == "disabled"
        assert result["coverage"]["event_count"] == 4
        assert result["validity"]["output_readback"] is True
        assert result["validity"]["expected_digest_captured"] is True
        assert result["validity"]["expected_digest_matched"] is True
        assert result["validity"]["output_oracle_matched"] is True
        assert result["validity"]["output_non_degenerate"] is False
        assert result["validity"]["oracle_mode"] == "strict"
        assert result["oracle_accepted"] is True
        assert result["deterministic"] is True
        assert result["replay_policy"] == {
            "reset": "fresh-process-device",
            "warmup": 1,
            "repeat": 2,
        }
        assert len(result["warmup_runs"]) == 1
        assert len(result["runs"]) == 2
        assert result["runs"][0] == result["runs"][1]
        assert result["scope"] == {
            "output_oracle": True,
            "production_capture": False,
            "production_provider_replay": True,
            "source_oracle": False,
        }

        parallel_source = root_path / "parallel-source"
        parallel_bundle = root_path / "parallel-bundle"
        run(str(fixture), "--write-parallel-fixture", str(parallel_source))
        parallel_pack = [
            sys.executable,
            str(bundle_tool),
            "pack",
            "--events",
            str(parallel_source / "events.bin"),
            "--output-dir",
            str(parallel_bundle),
            "--validator",
            str(validator),
        ]
        for blob in sorted(parallel_source.glob("*.bin")):
            if blob.name != "events.bin":
                parallel_pack.extend(("--blob", str(blob)))
        run(*parallel_pack)
        parallel = json.loads(
            run(
                sys.executable,
                str(runner),
                "parallel-verify",
                str(parallel_bundle),
                "--provider",
                str(provider),
                "--validator",
                str(validator),
            ).stdout
        )
        assert parallel["oracle_accepted"] is True
        assert parallel["strict_identity_equal"] is True
        assert parallel["non_vacuous"] is True
        assert len(parallel["identity_runs"]) == 2
        assert len(parallel["parallel_runs"]) == 2
        assert all(
            run["partition_mode"] == {"requested": "identity", "resolved": "identity"}
            for run in parallel["identity_runs"]
        )
        assert all(
            run["partition_mode"] == {"requested": "parallel", "resolved": "parallel"}
            for run in parallel["parallel_runs"]
        )
        # This bounded child-economics fixture intentionally clips every
        # alternate draw and is therefore degenerate.  It remains a generic
        # structural/parallel fixture, not promotion evidence.
        assert all(
            run["validity"]["output_non_degenerate"] is False
            for run in parallel["identity_runs"] + parallel["parallel_runs"]
        )
        assert parallel["identity_replay"] == parallel["parallel_replay"]
        assert parallel["identity_partition_mode"] == {
            "requested": "identity",
            "resolved": "identity",
        }
        assert parallel["partition_mode"] == {
            "requested": "parallel",
            "resolved": "parallel",
        }
        counters = parallel["parallel_counters"]
        assert counters["selected"] > 0
        assert counters["children"] >= 2
        assert counters["draws"] > 0
        assert counters["worker_batches"] > 0
        assert counters["worker_tasks"] > 0
        assert counters["worker_active_peak"] > 0
        assert counters["gpu_command_buffer_errors"] == 0
        assert all(type(value) is int and value >= 0 for value in counters.values())
        assert len(parallel["parallel_counters_runs"]) == 2
        for run_counters in parallel["parallel_counters_runs"]:
            assert run_counters["selected"] > 0
            assert run_counters["children"] >= 2
            assert run_counters["draws"] > 0
            assert run_counters["worker_batches"] > 0
            assert run_counters["worker_tasks"] > 0
            assert run_counters["worker_active_peak"] > 0
            assert run_counters["gpu_command_buffer_errors"] == 0
            assert all(
                type(value) is int and value >= 0
                for value in run_counters.values()
            )

        degenerate_provider = root_path / "degenerate-provider.py"
        degenerate_provider.write_text(
            "#!/usr/bin/env python3\n"
            "import json\n"
            "import subprocess\n"
            "import sys\n"
            f"real = {str(provider.resolve())!r}\n"
            "completed = subprocess.run([real, *sys.argv[1:]], check=False, "
            "text=True, capture_output=True)\n"
            "result = json.loads(completed.stdout)\n"
            "result['validity']['output_non_degenerate'] = False\n"
            "print(json.dumps(result))\n"
            "sys.stderr.write(completed.stderr)\n"
            "raise SystemExit(completed.returncode)\n",
            encoding="utf-8",
        )
        degenerate_provider.chmod(0o755)
        production_parallel_bundle = root_path / "production-parallel-bundle"
        shutil.copytree(parallel_bundle, production_parallel_bundle)
        production_manifest = production_parallel_bundle / "manifest.json"
        production_manifest_data = json.loads(production_manifest.read_text())
        production_manifest_data["scope"]["production_capture"] = True
        production_manifest.write_text(
            json.dumps(production_manifest_data, sort_keys=True), encoding="utf-8"
        )
        degenerate = run(
            sys.executable,
            str(runner),
            "parallel-verify",
            str(production_parallel_bundle),
            "--provider",
            str(degenerate_provider),
            "--validator",
            str(validator),
            check=False,
        )
        assert degenerate.returncode != 0
        degenerate_result = json.loads(degenerate.stdout)
        assert degenerate_result["oracle_accepted"] is False
        assert "output_non_degenerate=true" in degenerate_result["failed_gate"]

        replay_fields = (
            "profile",
            "status",
            "failed_event",
            "requirements",
            "validity",
            "coverage",
            "conservation",
            "intervals",
        )
        assert all(
            parallel["identity_replay"] == {
                key: run.get(key)
                for key in replay_fields
            }
            for run in parallel["identity_runs"]
        )
        assert all(
            parallel["parallel_replay"] == {
                key: run.get(key)
                for key in replay_fields
            }
            for run in parallel["parallel_runs"]
        )

        mutating_provider = root_path / "mutating-provider.py"
        mutation_state = root_path / "mutating-provider.count"
        mutating_provider.write_text(
            "#!/usr/bin/env python3\n"
            "import json\n"
            "import pathlib\n"
            "import subprocess\n"
            "import sys\n"
            f"real = {str(provider.resolve())!r}\n"
            f"state = pathlib.Path({str(mutation_state)!r})\n"
            "count = int(state.read_text()) if state.exists() else 0\n"
            "state.write_text(str(count + 1), encoding='utf-8')\n"
            "completed = subprocess.run([real, *sys.argv[1:]], check=False, "
            "text=True, capture_output=True)\n"
            "result = json.loads(completed.stdout)\n"
            "if count == 3:\n"
            "    result['validity']['output_sha256'] = 'f' * 64\n"
            "print(json.dumps(result))\n"
            "sys.stderr.write(completed.stderr)\n"
            "raise SystemExit(completed.returncode)\n",
            encoding="utf-8",
        )
        mutating_provider.chmod(0o755)
        nondeterministic = run(
            sys.executable,
            str(runner),
            "parallel-verify",
            str(parallel_bundle),
            "--provider",
            str(mutating_provider),
            "--validator",
            str(validator),
            check=False,
        )
        assert nondeterministic.returncode != 0
        nondeterministic_result = json.loads(nondeterministic.stdout)
        assert nondeterministic_result["oracle_accepted"] is False
        assert "nondeterministic" in nondeterministic_result["failed_gate"]
        assert len(nondeterministic_result["identity_runs"]) == 2
        assert len(nondeterministic_result["parallel_runs"]) == 2

        mutation_state.unlink()
        mutating_provider.write_text(
            "#!/usr/bin/env python3\n"
            "import json\n"
            "import pathlib\n"
            "import subprocess\n"
            "import sys\n"
            f"real = {str(provider.resolve())!r}\n"
            f"state = pathlib.Path({str(mutation_state)!r})\n"
            "count = int(state.read_text()) if state.exists() else 0\n"
            "state.write_text(str(count + 1), encoding='utf-8')\n"
            "completed = subprocess.run([real, *sys.argv[1:]], check=False, "
            "text=True, capture_output=True)\n"
            "result = json.loads(completed.stdout)\n"
            "if count == 3:\n"
            "    result['parallel_counters']['selected'] += 1\n"
            "print(json.dumps(result))\n"
            "sys.stderr.write(completed.stderr)\n"
            "raise SystemExit(completed.returncode)\n",
            encoding="utf-8",
        )
        nondeterministic_counter = run(
            sys.executable,
            str(runner),
            "parallel-verify",
            str(parallel_bundle),
            "--provider",
            str(mutating_provider),
            "--validator",
            str(validator),
            check=False,
        )
        assert nondeterministic_counter.returncode != 0
        nondeterministic_counter_result = json.loads(
            nondeterministic_counter.stdout
        )
        assert nondeterministic_counter_result["oracle_accepted"] is False
        assert (
            nondeterministic_counter_result["failed_gate"]
            == "ExplicitParallel counter evidence is nondeterministic: selected"
        )

        inspected = json.loads(
            run(
                sys.executable,
                str(runner),
                "inspect",
                str(bundle),
                "--validator",
                str(validator),
            ).stdout
        )
        present_event = inspected["present_command_event"]
        assert present_event in inspected["command_event_indices"]

        reduced_a = root_path / "reduced-a"
        reduced = json.loads(
            run(
                sys.executable,
                str(runner),
                "reduce",
                str(bundle),
                "--output-dir",
                str(reduced_a),
                "--select-command-event",
                str(present_event),
                "--validator",
                str(validator),
                "--provider",
                str(provider),
            ).stdout
        )
        assert reduced["oracle_accepted"] is True
        assert reduced["selected_command_events"] == [present_event]
        reduced_manifest = json.loads(
            (reduced_a / "manifest.json").read_text(encoding="utf-8")
        )
        assert reduced_manifest["stage"] == "reduce"
        assert reduced_manifest["validity"]["provider_oracle"] is True
        assert reduced_manifest["reducer"]["selected_source_command_events"] == [
            present_event
        ]

        bisected = root_path / "bisected"
        bisect_result = json.loads(
            run(
                sys.executable,
                str(runner),
                "bisect",
                str(bundle),
                "--output-dir",
                str(bisected),
                "--validator",
                str(validator),
                "--provider",
                str(provider),
            ).stdout
        )
        assert bisect_result["oracle_accepted"] is True
        assert bisect_result["selected_command_events"] == [present_event]
        assert (reduced_a / "events.bin").read_bytes() == (
            bisected / "events.bin"
        ).read_bytes()

        rejected_output = root_path / "rejected"
        rejected = run(
            sys.executable,
            str(runner),
            "reduce",
            str(bundle),
            "--output-dir",
            str(rejected_output),
            "--select-command-event",
            "0",
            "--validator",
            str(validator),
            "--provider",
            str(provider),
            check=False,
        )
        assert rejected.returncode != 0
        assert not rejected_output.exists()

        excessive = run(
            sys.executable,
            str(runner),
            "provider-replay",
            str(bundle),
            "--provider",
            str(provider),
            "--repeat",
            "65",
            check=False,
        )
        assert excessive.returncode != 0
        assert "warmup + repeat" in excessive.stderr

        fake_provider = root_path / "fake-provider.py"
        fake_provider.write_text(
            "#!/usr/bin/env python3\n"
            "import json\n"
            "print(json.dumps({\n"
            "  'profile': 'frame-tape',\n"
            "  'status': 'complete',\n"
            "  'validity': {\n"
            "    'structurally_valid': True, 'digests_valid': True,\n"
            "    'output_readback': True, 'expected_digest_captured': True,\n"
            "    'expected_digest_matched': True, 'output_sha256': '0' * 64\n"
            "  },\n"
            "  'coverage': {'present_records': 1, 'present_outputs': 1},\n"
            "  'intervals': [{\n"
            "    'present_ordinal': 1, 'completion_ordinal': 1,\n"
            "    'validity': {\n"
            "      'output_readback': True, 'expected_digest_captured': True,\n"
            "      'expected_digest_matched': True, 'output_sha256': '0' * 64\n"
            "    }\n"
            "  }],\n"
            "  'conservation': {}\n"
            "}))\n",
            encoding="utf-8",
        )
        fake_provider.chmod(0o755)
        rejected_reduction_output = root_path / "rejected-provider-reduction"
        rejected_reduction = run(
            sys.executable,
            str(runner),
            "reduce",
            str(bundle),
            "--output-dir",
            str(rejected_reduction_output),
            "--select-command-event",
            str(present_event),
            "--validator",
            str(validator),
            "--provider",
            str(fake_provider),
            check=False,
        )
        assert rejected_reduction.returncode != 0
        assert "provider oracle rejected" in rejected_reduction.stderr
        assert not rejected_reduction_output.exists()
        weak_oracle = run(
            sys.executable,
            str(runner),
            "provider-replay",
            str(bundle),
            "--provider",
            str(fake_provider),
            check=False,
        )
        assert weak_oracle.returncode != 0
        weak_result = json.loads(weak_oracle.stdout)
        assert weak_result["status"] == "complete"
        assert weak_result["oracle_accepted"] is False
        assert weak_result["scope"]["production_provider_replay"] is False
        assert weak_result["scope"]["output_oracle"] is False

        sequence_source = root_path / "sequence-source"
        sequence_bundle = root_path / "sequence-bundle"
        run(str(fixture), "--write-sequence-fixture", str(sequence_source))
        run(
            sys.executable,
            str(bundle_tool),
            "pack",
            "--events",
            str(sequence_source / "events.bin"),
            "--output-dir",
            str(sequence_bundle),
            "--blob",
            str(sequence_source / "first.bin"),
            "--blob",
            str(sequence_source / "second.bin"),
            "--validator",
            str(validator),
        )
        sequence_manifest = json.loads(
            (sequence_bundle / "manifest.json").read_text(encoding="utf-8")
        )
        assert sequence_manifest["profile"] == "sequence-tape"
        sequence_inspect = json.loads(
            run(
                sys.executable,
                str(runner),
                "inspect",
                str(sequence_bundle),
                "--validator",
                str(validator),
            ).stdout
        )
        assert sequence_inspect["profile"] == "sequence-tape"
        assert sequence_inspect["present_completions"] == 2
        assert len(sequence_inspect["present_command_events"]) == 2
        sequence = json.loads(
            run(
                sys.executable,
                str(runner),
                "provider-replay",
                str(sequence_bundle),
                "--provider",
                str(provider),
                "--warmup",
                "1",
                "--repeat",
                "2",
            ).stdout
        )
        assert sequence["profile"] == "sequence-tape"
        assert sequence["archive_policy"] == "disabled"
        assert sequence["oracle_accepted"] is True
        assert sequence["deterministic"] is True
        assert sequence["coverage"]["present_records"] == 2
        assert sequence["coverage"]["present_outputs"] == 2
        assert sequence["coverage"]["seed_mutations"] == 2
        assert len(sequence["intervals"]) == 2
        assert sequence["intervals"][0]["completion_ordinal"] == 1
        assert sequence["intervals"][1]["completion_ordinal"] == 2
        assert (
            sequence["intervals"][0]["validity"]["output_sha256"]
            != sequence["intervals"][1]["validity"]["output_sha256"]
        )
        assert sequence["runs"][0] == sequence["runs"][1]

        sequence_reduce = run(
            sys.executable,
            str(runner),
            "reduce",
            str(sequence_bundle),
            "--output-dir",
            str(root_path / "sequence-reduced"),
            "--select-command-event",
            str(sequence_inspect["present_command_events"][0]),
            "--select-command-event",
            str(sequence_inspect["present_command_events"][1]),
            "--validator",
            str(validator),
            "--provider",
            str(provider),
            check=False,
        )
        assert sequence_reduce.returncode != 0
        assert not (root_path / "sequence-reduced").exists()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
