#!/usr/bin/env python3

import json
import copy
import importlib.util
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile


def run(*args: str, check: bool = True, env=None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(args, check=check, text=True, capture_output=True, env=env)


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

        experiment_output = root_path / "experiment"
        experiment_command = [
            sys.executable, str(runner), "experiment", str(bundle),
            "--provider", str(provider), "--validator", str(validator),
            "--warmup", "1", "--repeat", "2", "--output-dir", str(experiment_output),
        ]
        experiment = json.loads(run(*experiment_command).stdout)
        assert experiment["accepted"] is True
        receipt = json.loads((experiment_output / "result.json").read_text())
        prepared = receipt["result"]
        assert prepared["prepare_count"] == 1
        assert prepared["reset"] == "cold-device"
        assert prepared["backend"] == "metal"
        assert len(prepared["iterations"]) == 3
        assert receipt["readback_artifacts_valid"] is True
        assert (experiment_output / "readback").is_dir()
        # The textured projection fixture issues one non-indexed UP draw.
        # Observe the same real Metal route with/without the cold logger;
        # do not let indexed-only trace coverage masquerade as all draws.
        nonindexed = root_path / "nonindexed-trace-source"
        run(str(fixture), "--write-projection-fixture", str(nonindexed))
        trace_command = [str(provider), "experiment", str(nonindexed / "events.bin"),
                         "--blob", str(nonindexed / "seed.bin"),
                         "--identity", str(nonindexed / "identity.bin"),
                         "--warmup", "0", "--repeat", "1",
                         "--replay-route", "compatibility"]
        trace_results = []
        for enabled in (False, True):
            environment = dict(os.environ, DXMT9_EFFECT_DRAW_TRACE=str(int(enabled)),
                               DXMT9_EFFECT_DRAW_TRACE_INCLUDE_NON_ALPHA="1",
                               DXMT9_EFFECT_DRAW_TRACE_INCLUDE_UNTEXTURED="1")
            observed = run(*trace_command, env=environment)
            draw_rows = [line for line in observed.stderr.splitlines()
                         if "[dxmt9-effect-draw " in line]
            assert len(draw_rows) == int(enabled), draw_rows
            if enabled:
                assert " indexed=0 " in draw_rows[0], draw_rows[0]
            observed_iteration, = json.loads(observed.stdout)["iterations"]
            assert observed_iteration["path_counters"]["gpu_errors"] == 0
            observed_provider = observed_iteration["provider_result"]
            assert observed_provider["coverage"]["draw_primitive_up_records"] == 1
            # This historical fixture names the attachment but carries no
            # captured pixel digest. Prove observer invariance against the
            # observer-off result, not independent pixel qualification.
            assert observed_provider["validity"]["output_readback"] is True
            assert observed_provider["validity"]["output_bytes"] == 16 * 16 * 4
            trace_results.append(observed_provider["validity"]["output_sha256"])
        assert trace_results[0] == trace_results[1]
        audit_output = root_path / "initial-seed-audit"
        audited = run(*experiment_command[:-1], str(audit_output),
                      "--audit-initial-seeds", check=False)
        assert audited.returncode == 1  # Diagnostics cannot qualify a policy.
        audit_receipt = json.loads((audit_output / "result.json").read_text())
        assert audit_receipt["diagnostic"] is True
        assert audit_receipt["accepted"] is False
        assert audit_receipt["readback_artifacts_valid"] is True
        for iteration in audit_receipt["result"]["iterations"]:
            audit = iteration["provider_result"]["initial_seed_audit"]
            assert audit["requested"] is True and audit["attempted"] is True
            assert audit["diagnostic"] is True and audit["perturbative"] is True
            assert audit["count"] == len(audit["entries"])
        drain_output = root_path / "fresh-seed-drain-audit"
        drained = run(*experiment_command[:-1], str(drain_output),
                      "--audit-fresh-seed-drains", check=False)
        assert drained.returncode == 1
        drain_receipt = json.loads((drain_output / "result.json").read_text())
        assert drain_receipt["diagnostic"] is True
        assert drain_receipt["accepted"] is False
        assert drain_receipt["readback_artifacts_valid"] is True
        assert drain_receipt["result"]["audit_fresh_seed_drains"] is True
        for iteration in drain_receipt["result"]["iterations"]:
            audit = iteration["provider_result"]["fresh_seed_drain_audit"]
            assert audit["requested"] is True and audit["diagnostic"] is True
            # The bounded provider pre-creates this fixture's objects. It must
            # not manufacture the general provider's chronological witness.
            assert audit["attempted"] is False and audit["complete"] is False
            assert audit["inspected_mutations"] == audit["mutation_count"] == 0
            # This is explicitly not non-vacuous bypass evidence.
            assert audit["bypassed_drains"] == 0
        prestage_output = root_path / "texture-prestage-audit"
        prestaged = run(*experiment_command[:-1], str(prestage_output),
                        "--audit-texture-prestage", check=False)
        assert prestaged.returncode == 1
        prestage_receipt = json.loads((prestage_output / "result.json").read_text())
        assert prestage_receipt["diagnostic"] is True
        assert prestage_receipt["accepted"] is False
        assert prestage_receipt["readback_artifacts_valid"] is True
        assert prestage_receipt["result"]["audit_texture_prestage"] is True
        for iteration in prestage_receipt["result"]["iterations"]:
            audit = iteration["provider_result"]["texture_prestage_audit"]
            assert audit["requested"] is True and audit["diagnostic"] is True
            assert audit["attempted"] is False and audit["complete"] is False
            assert audit["candidate_objects"] == audit["initializer_flushes"] == 0
        for extra in (("--reset", "warm-device"), ("--audit-fresh-seed-drains",),
                      ("--audit-initial-seeds",)):
            invalid_output = root_path / ("invalid-prestage-" + extra[0][2:])
            invalid = run(*experiment_command[:-1], str(invalid_output),
                          "--audit-texture-prestage", *extra, check=False)
            assert invalid.returncode != 0
            assert not invalid_output.exists()  # Rejected before staging/effects.
        assert receipt["provider"]["sha256"]
        assert (experiment_output / "input" / "events.bin").read_bytes() == (bundle / "events.bin").read_bytes()
        spec = importlib.util.spec_from_file_location("render_tape_runner", runner)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        assert module.experiment_renderer_environment({
            "DXMT9_TEST": "1", "DXMT_TEST": "0", "MTL_DEBUG_LAYER": "1",
            "MTL_SHADER_VALIDATION": "1", "MTL_CAPTURE_ENABLED": "0",
            "METAL_CAPTURE_ENABLED": "1", "MTL_HUD_ENABLED": "1",
            "ACCOUNT_TOKEN": "do-not-record",
            "HOME": "/not-renderer-provenance",
        }) == {"DXMT9_TEST": "1", "DXMT_TEST": "0", "MTL_DEBUG_LAYER": "1",
               "MTL_SHADER_VALIDATION": "1", "MTL_CAPTURE_ENABLED": "0",
               "METAL_CAPTURE_ENABLED": "1", "MTL_HUD_ENABLED": "1"}
        for layer in ("", "0", "1", "unknown"):
            for shader in ("", "0", "1", "unknown"):
                assert module.experiment_metal_validation_enabled({
                    "MTL_DEBUG_LAYER": layer, "MTL_SHADER_VALIDATION": shader,
                }) is (layer not in ("", "0") or shader not in ("", "0"))
        validation_output = root_path / "validation-experiment"
        # Test the wrapper gate independently of Metal's validation runtime:
        # replay a known-good native receipt/readback set with an env-checking
        # provider double. This is not a validation-enabled GPU oracle.
        validation_provider = root_path / "validation-provider.py"
        validation_provider.write_text(
            "#!/usr/bin/env python3\n"
            "import json, os, pathlib, shutil, sys\n"
            "assert os.environ['MTL_DEBUG_LAYER'] == '1'\n"
            "assert os.environ['MTL_SHADER_VALIDATION'] == '1'\n"
            f"result = json.loads({json.dumps(prepared)!r})\n"
            "target = pathlib.Path(sys.argv[sys.argv.index('--readback-dir') + 1])\n"
            f"shutil.copytree({str(experiment_output / 'readback')!r}, target)\n"
            "print(json.dumps(result))\n", encoding="utf-8")
        validation_provider.chmod(0o755)
        validation_command = [*experiment_command[:-1], str(validation_output)]
        validation_command[validation_command.index("--provider") + 1] = str(validation_provider)
        validation_run = subprocess.run(
            validation_command,
            env={**os.environ, "MTL_DEBUG_LAYER": "1", "MTL_SHADER_VALIDATION": "1"},
            capture_output=True, text=True, timeout=60)
        assert validation_run.returncode == 1, validation_run.stderr
        validation_receipt = json.loads((validation_output / "result.json").read_text())
        assert validation_receipt["accepted"] is False
        assert validation_receipt["diagnostic"] is True
        assert validation_receipt["metal_validation_diagnostic"] is True
        assert validation_receipt["renderer_environment"]["MTL_DEBUG_LAYER"] == "1"
        assert validation_receipt["renderer_environment"]["MTL_SHADER_VALIDATION"] == "1"
        assert validation_receipt["provider_exit_code"] == 0, (
            validation_receipt["result"],
            (validation_output / "provider.stderr").read_text()[:3000])
        assert module.experiment_accepts(validation_receipt["result"], warmup=1,
            repeat=2, reset="cold-device", route="compatibility",
            require_path=False, non_degenerate=False)
        for index, iteration in enumerate(prepared["iterations"]):
            artifacts = iteration["readback_artifacts"]
            for kind in ("output", "source"):
                evidence = artifacts[f"{kind}_rgba"]
                assert evidence["path"] == f"iteration-{index:06d}-{kind}.rgba"
                artifact = experiment_output / "readback" / evidence["path"]
                assert artifact.stat().st_size == evidence["bytes"]
                assert module.digest(artifact) == evidence["sha256"]
        accept_options = dict(warmup=1, repeat=2, reset="cold-device",
                              route="compatibility", require_path=False, non_degenerate=False)
        assert module.experiment_accepts(prepared, **accept_options)
        for change in ("partial", "reset", "stale-output", "gpu-error", "zero-encode", "observed-path", "pixel-determinism", "timing"):
            invalid = copy.deepcopy(prepared)
            if change == "partial":
                invalid["iterations"].pop()
            elif change == "reset":
                invalid["iterations"][1]["reset_ok"] = False
            elif change == "stale-output":
                invalid["iterations"][1]["provider_result"]["intervals"][0]["validity"]["output_sha256"] = "0" * 64
            elif change == "gpu-error":
                invalid["iterations"][1]["path_counters"]["gpu_errors"] = 1
            elif change == "zero-encode":
                invalid["iterations"][1]["path_counters"]["encode_chunks"] = 0
            elif change == "observed-path":
                invalid["iterations"][1]["observed_replay_path"] = "direct-slot"
            elif change == "pixel-determinism":
                invalid["pixel_deterministic"] = False
            else:
                invalid["iterations"][1]["replay_ns"] = -1
            assert not module.experiment_accepts(invalid, **accept_options), change
        rejected_output = root_path / "empty-candidate"
        rejected = run(*experiment_command[:-1], str(rejected_output), "--require-path", check=False)
        assert rejected.returncode != 0
        assert json.loads((rejected_output / "result.json").read_text())["accepted"] is False
        assert (rejected_output / "provider.stdout").is_file()
        assert (rejected_output / "provider.stderr").is_file()

        oracle_failed_provider = root_path / "oracle-failed-experiment-provider.py"
        oracle_failed_provider.write_text(
            "#!/usr/bin/env python3\n"
            "import json\n"
            "import subprocess\n"
            "import sys\n"
            f"real = {str(provider.resolve())!r}\n"
            "completed = subprocess.run([real, *sys.argv[1:]], check=False, "
            "text=True, capture_output=True)\n"
            "result = json.loads(completed.stdout)\n"
            "result['status'] = 'failed'\n"
            "print(json.dumps(result))\n"
            "sys.stderr.write(completed.stderr)\n"
            "raise SystemExit(1)\n",
            encoding="utf-8",
        )
        oracle_failed_provider.chmod(0o755)
        oracle_failed_output = root_path / "oracle-failed-experiment"
        oracle_failed = run(
            sys.executable, str(runner), "experiment", str(bundle),
            "--provider", str(oracle_failed_provider), "--validator", str(validator),
            "--warmup", "0", "--repeat", "1",
            "--output-dir", str(oracle_failed_output), check=False,
        )
        assert oracle_failed.returncode != 0
        oracle_failed_receipt = json.loads(
            (oracle_failed_output / "result.json").read_text()
        )
        assert oracle_failed_receipt["accepted"] is False
        assert oracle_failed_receipt["readback_artifacts_valid"] is True
        assert oracle_failed_receipt["result"]["status"] == "failed"
        assert (oracle_failed_output / "readback" /
                "iteration-000000-output.rgba").is_file()

        for corruption in ("missing", "corrupt"):
            corrupt_provider = root_path / f"{corruption}-readback-provider.py"
            corrupt_provider.write_text(
                "#!/usr/bin/env python3\n"
                "import json\n"
                "import pathlib\n"
                "import subprocess\n"
                "import sys\n"
                f"real = {str(provider.resolve())!r}\n"
                f"mode = {corruption!r}\n"
                "completed = subprocess.run([real, *sys.argv[1:]], check=False, "
                "text=True, capture_output=True)\n"
                "result = json.loads(completed.stdout)\n"
                "directory = pathlib.Path(sys.argv[sys.argv.index('--readback-dir') + 1])\n"
                "target = directory / 'iteration-000000-output.rgba'\n"
                "target.unlink() if mode == 'missing' else target.write_bytes(b'corrupt')\n"
                "print(json.dumps(result))\n"
                "sys.stderr.write(completed.stderr)\n"
                "raise SystemExit(completed.returncode)\n",
                encoding="utf-8",
            )
            corrupt_provider.chmod(0o755)
            corrupt_output = root_path / f"{corruption}-readback"
            corrupt = run(
                sys.executable, str(runner), "experiment", str(bundle),
                "--provider", str(corrupt_provider), "--validator", str(validator),
                "--warmup", "0", "--repeat", "1",
                "--output-dir", str(corrupt_output), check=False,
            )
            assert corrupt.returncode != 0
            corrupt_receipt = json.loads(
                (corrupt_output / "result.json").read_text()
            )
            assert corrupt_receipt["accepted"] is False
            assert corrupt_receipt["readback_artifacts_valid"] is False

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
        parallel_inspect = json.loads(
            run(
                sys.executable,
                str(runner),
                "inspect",
                str(parallel_bundle),
                "--validator",
                str(validator),
            ).stdout
        )
        assert parallel_inspect["buffer_definitions"] == [
            {
                "event": 3,
                "identity": {"kind": 2, "generation": 4, "object_id": "49"},
                "usage": 0,
                "pool": 0,
                "size": 84,
            },
            {
                "event": 4,
                "identity": {"kind": 2, "generation": 4, "object_id": "50"},
                "usage": 0,
                "pool": 0,
                "size": 6,
            },
        ]
        assert parallel_inspect["buffer_mutations"] == [
            {
                "event": 7,
                "identity": {"kind": 2, "generation": 4, "object_id": "49"},
                "mutation_kind": 2,
                "disposition": 0,
                "byte_offset": 0,
                "byte_size": 84,
            },
            {
                "event": 8,
                "identity": {"kind": 2, "generation": 4, "object_id": "50"},
                "mutation_kind": 2,
                "disposition": 0,
                "byte_offset": 0,
                "byte_size": 6,
            },
        ]
        assert parallel_inspect["buffer_destroys"] == []
        route_outputs = []
        for route in ("compatibility", "production-slot"):
            route_output = root_path / ("experiment-" + route)
            route_run = run(
                sys.executable, str(runner), "experiment", str(parallel_bundle),
                "--provider", str(provider), "--validator", str(validator),
                "--output-dir", str(route_output), "--warmup", "1", "--repeat", "2",
                "--reset", "warm-device", "--replay-route", route,
                *(("--require-path",) if route == "compatibility" else ()))
            assert json.loads(route_run.stdout)["accepted"] is True
            route_result = json.loads((route_output / "result.json").read_text())["result"]
            if route == "production-slot":
                # This 128-draw fixture takes production's pre-effect fallback;
                # matching pixels must not qualify it as executed Direct work.
                assert all(item["observed_replay_path"] == "compatibility"
                           for item in route_result["iterations"])
                assert not module.experiment_accepts(
                    route_result, warmup=1, repeat=2, reset="warm-device",
                    route=route, require_path=True, non_degenerate=True)
            route_outputs.append([
                item["provider_result"]["validity"]["output_sha256"]
                for item in route_result["iterations"]])
        assert route_outputs[0] == route_outputs[1]
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
            "identity_evidence",
            "source_boundary_audit",
            "source_replay",
            "audit_batch_cuts",
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
