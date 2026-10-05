#!/usr/bin/env python3
"""Public paired-adapter regressions; every native/launcher child is mocked.

Loader-only immutable prefixes exercise Python authentication, not actual V4
provenance or renderer qualification. Real native admission remains independent.
"""

from __future__ import annotations

import argparse
import contextlib
import copy
import hashlib
import importlib.util
import io
import json
import os
import pathlib
import subprocess
import tempfile
import time
import unittest
from unittest import mock


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class PairedQualificationTest(unittest.TestCase):
    def setUp(self):
        scripts = pathlib.Path(__file__).resolve().parents[1]
        self.paired = load(scripts / "tools/render_tape_v4_paired.py", "paired_tested")
        fixtures = load(scripts / "check/test_render_tape_cpu_v4_semantics.py", "paired_cpu_fixtures")
        self.fixture = fixtures.ControlledCpuSemanticsTest()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.cpu, self.runner = self.fixture.launcher, self.fixture.shared
        self.auth = self.cpu.auth
        self.root, self.bundle, self.cpu_binary = self.fixture.root, self.fixture.bundle, self.fixture.binary
        self.gpu_binary = self.root / "dxmt9-mock-gpu-native"
        self.gpu_binary.write_bytes(b"mocked GPU child only\n")
        self.gpu_binary.chmod(0o700)
        self.modules = []
        for name in self.paired.REQUIRED_RUNTIME_IMAGES:
            path = self.root / name
            path.write_bytes(name.encode())
            self.modules.append(path)
        self.fixture.manifest.update(producer={"path": "dxmt9-pe-render-tape"}, stage="production-capture")
        self.fixture.manifest["scope"].update(production_capture=True, output_oracle=True, source_oracle=True)
        self.fixture.save_manifest()
        self.plan = self.fixture.plan
        self.case = 0
        self.environment_patch = mock.patch.dict(os.environ, {}, clear=True)
        self.environment_patch.start()
        self.addCleanup(self.environment_patch.stop)

    def images(self, binary):
        return [{"path": str(path), "bytes": path.stat().st_size,
                 "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                for path in sorted([binary, *self.modules])]

    def gpu_native(self, *, replay=False, route="compatibility", output=None, policy=None, draw_command_limit=0):
        policy = self.paired.capacity_policy() if policy is None else policy
        full_policy = self.paired.replay_policy(route, policy, draw_command_limit)
        result = {"schema": self.auth.NATIVE_SCHEMA, "diagnostic": True,
                  "policy_qualified": False, "timing_eligible": False,
                  "producer_contract": self.paired.PRODUCER_CONTRACT,
                  "publication_mode": "semantic-mapping",
                  "action_profile": {"authenticated": True, "stable_exact": True},
                  "status": "complete" if replay else "inspection-complete",
                  "qualified": replay, "execution_count": int(replay), "device_created": replay,
                  "plan_sha256": self.plan, "semantic_mapping_qualified": replay,
                  "exact_source_diagnostic_complete": False,
                  "loaded_images": self.images(self.gpu_binary),
                  "capacity_policy": dict(policy),
                  "replay_policy": full_policy if replay else {**full_policy, "route": "inspection"},
                  "effective_stage_environment": self.paired.stage_environment("gpu", policy, draw_command_limit) if replay else {}}
        result.update({name: replay for name in self.auth.GATES})
        if replay:
            result.update(expected_plan_sha256=self.plan, replay_route=route,
                          path_counters={"observed": True, "monotonic": True,
                                         "direct_leases": int(route == "production-slot"),
                                         "direct_lease_draws": 2 if route == "production-slot" else 0})
            artifacts = {"requested": True, "attempted": True, "directory": str(output), "failure": "none"}
            for name in ("source", "output"):
                entry = self.fixture.manifest["components"][f"{name}_oracle"]
                artifacts[name] = {"attempted": True, "written": True, "bytes": entry["bytes"],
                                   "written_bytes": entry["bytes"], "sha256": entry["sha256"], "failure": "none"}
            result["readback_artifacts"] = artifacts
        return result

    def cpu_native(self, route, policy=None, draw_command_limit=0):
        policy = self.paired.capacity_policy() if policy is None else policy
        native = self.fixture.native(contract=self.paired.PRODUCER_CONTRACT, route=route)
        native["loaded_images"] = self.images(self.cpu_binary)
        native["capacity_policy"] = dict(policy)
        native["replay_policy"] = self.paired.replay_policy(route, policy, draw_command_limit)
        native["effective_stage_environment"] = self.paired.stage_environment("cpu", policy, draw_command_limit)
        counters = native["execution"]["counters"]
        counters.update(events=7, commands=5, observed_mutations=1, observed_texture_uploads=0,
                        record_contexts=5, buffer_versions_observed=1, buffer_versions_unavailable=0)
        native["execution"]["query_projection"].update(definitions=1, issues=1, checkpoints=0, rows=1)
        if route == "production-slot":
            execution = native["execution"]
            semantic = execution["logical_semantics"]
            semantic.update(sources=3, commands=3, logical_buffer_hash_bytes=384)
            for name in ("observed", "submitted", "acknowledged", "retired", "receipts"):
                execution["consumer"][name] = 3
            execution["consumer"]["commands"] = 3
            execution["sources"] = []
            for index, receipt in enumerate(semantic["receipts"]):
                sequence = index + 1
                receipt.update(sequence=sequence, physical_command_index=0,
                               subdraw_index=0 if index < 2 else None)
                execution["sources"].append({"sequence": sequence, "commands": [
                    {"physical_command": 0, "kind": 1 if index < 2 else 2, "draw_count": int(index < 2)}]})
                if index < 2:
                    receipt["source_rows"].update(source_sequence=sequence, completed_sequence=sequence - 1)
        return native

    def args(self, output, **changes):
        result = argparse.Namespace(bundle=self.bundle, output_dir=output, cpu_diagnostic=self.cpu_binary,
            gpu_diagnostic=self.gpu_binary, producer_contract=self.paired.PRODUCER_CONTRACT,
            buffer_preservation_contract=self.paired.PRESERVATION_CONTRACT, timeout_seconds=60.0)
        for key, value in changes.items():
            setattr(result, key, value)
        return result

    def child_receipt(self, stage, route, bundle, output, argv):
        policy = self.paired.capacity_policy(int(argv[argv.index("--slot-headroom-bytes") + 1]),
                                             int(argv[argv.index("--aggregate-headroom-bytes") + 1]))
        limit = int(argv[argv.index("--draw-command-limit") + 1])
        replay = self.paired.replay_policy(route, policy, limit)
        raw = (bundle / "manifest.json").read_bytes()
        binary = self.cpu_binary if stage == "cpu" else self.gpu_binary
        result = {"schema": self.cpu.RECEIPT_SCHEMA if stage == "cpu" else self.auth.RECEIPT_SCHEMA,
                  "status": "complete", "accepted": True, "manifest_authenticated": True,
                  "bundle_reauthenticated": True, "binary_reauthenticated": True, "loaded_images_stable": True,
                  "producer_contract": self.paired.PRODUCER_CONTRACT, "route": route,
                  "capacity_policy": dict(policy),
                  "capacity_policy_valid": True, "native_capacity_policy_valid": True, "stage_environment_valid": True,
                  "replay_policy": replay, "replay_policy_valid": True, "native_replay_policy_valid": True,
                  "expected_stage_environment": self.paired.stage_environment(stage, policy, limit),
                  "effective_stage_environment": self.paired.stage_environment(stage, policy, limit),
                  "policy_qualified": False, "timing_eligible": False, "environment": {},
                  "input_manifest": {"path": "manifest.json", "bytes": len(raw),
                                     "sha256": hashlib.sha256(raw).hexdigest()},
                  "bundle": str(bundle), "output_dir": str(output), "diagnostic_binary": str(binary),
                  "expected_plan_digest": self.plan,
                  "diagnostic_executable": {"bytes": binary.stat().st_size, "sha256": self.runner.digest(binary)}}
        def step(native):
            return {"status": "exited", "returncode": 0, "timed_out": False, "native_result": native}
        if stage == "cpu":
            native = self.cpu_native(route, policy, limit)
            result.update(buffer_preservation_contract=self.paired.PRESERVATION_CONTRACT,
                          conditional_on_legal_nooverwrite=True, logical_stream_complete=True,
                          requested_route=route, effective_route=route, iterations=1,
                          native_max_wall_ms=native["max_wall_ms"],
                          steps={"semantics": step(native)})
        else:
            native = self.gpu_native(replay=True, route=route, output=output, policy=policy, draw_command_limit=limit)
            result.update(action_profile_reauthenticated=True, readback_artifacts_valid=True,
                          stage_environment_valid=True, publication_mode="semantic-mapping", plan_sha256=self.plan,
                          steps={"inspect": step(self.gpu_native(route=route, policy=policy, draw_command_limit=limit)),
                                 "replay": step(native)})
            for name in ("source", "output"):
                (output / f"replay-{name}.rgba").write_bytes((bundle / f"{name}.rgba").read_bytes())
        return result

    def run_case(self, mutate=None, failure=None, after=None, **arguments):
        self.case += 1
        output = self.root / f"paired-{self.case}"
        calls = []

        def run(argv, **kwargs):
            calls.append(argv)
            self.assertIsInstance(argv, list)
            self.assertNotIn("shell", kwargs)
            self.assertNotIn("env", kwargs)
            self.assertGreater(kwargs["timeout"], 0)
            self.assertLessEqual(kwargs["timeout"], 600)
            kwargs["stderr"].write(f"raw child {len(calls)} stderr\n".encode())
            if len(calls) == 1:
                native = self.gpu_native()
                if mutate:
                    mutate("plan", native, argv)
                kwargs["stdout"].write(json.dumps(native).encode())
                return subprocess.CompletedProcess(argv, 0)
            bundle = pathlib.Path(argv[argv.index("--bundle") + 1])
            child_output = pathlib.Path(argv[argv.index("--output-dir") + 1])
            child_output.mkdir(mode=0o700)
            route = argv[argv.index("--route") + 1]
            stage = "cpu" if "cpu_v4_semantics" in argv[1] else "gpu"
            child = self.child_receipt(stage, route, bundle, child_output, argv)
            name = child_output.name
            if mutate:
                mutate(name, child, argv)
            (child_output / "result.json").write_text(json.dumps(child), encoding="utf-8")
            if failure == name:
                kwargs["stdout"].write(b'{"status":"rejected"}')
                return subprocess.CompletedProcess(argv, 7)
            if after:
                after(name, child_output, bundle)
            kwargs["stdout"].write(json.dumps({"accepted": True, "receipt": str(child_output / "result.json")}).encode())
            return subprocess.CompletedProcess(argv, 0)

        with mock.patch.object(self.paired, "_load_cpu_launcher", return_value=self.cpu), \
             mock.patch.object(self.runner, "copy_input_owned", side_effect=lambda source, target: __import__("shutil").copytree(source, target)), \
             mock.patch.object(self.auth.subprocess, "run", side_effect=run), contextlib.redirect_stdout(io.StringIO()):
            code = self.paired.paired_qualification(self.args(output, **arguments), runner=self.runner)
        receipt = json.loads((output / "result.json").read_text(encoding="utf-8"))
        return code, receipt, calls, output

    def test_complete_pair_ignores_physical_partitions_and_revalidates_after_atomic_rename(self):
        code, receipt, calls, output = self.run_case()
        self.assertEqual(code, 0)
        self.assertEqual(len(calls), 7)
        self.assertTrue(receipt["paired_qualification_complete"])
        self.assertFalse(receipt["timing_eligible"])
        self.assertFalse(receipt["policy_qualified"])
        self.assertEqual(receipt["iterations_per_route_per_backend"], {"cpu": 1, "gpu": 2})
        self.assertEqual(receipt["limits"]["child_receipt_bytes"], self.paired.MAX_CHILD_RECEIPT_BYTES)
        self.assertEqual(receipt["limits"]["native_result_json_bytes"], self.auth.MAX_RESULT_JSON_BYTES)
        self.assertEqual(set(receipt["children"]), {name for _stage, _route, name in self.paired._child_specs()})
        historical = pathlib.Path(receipt["execution_staging_directory"])
        self.assertFalse(historical.exists())
        self.assertTrue((output / "input").is_dir())
        for argv in calls[1:]:
            self.assertEqual(argv[argv.index("--expected-plan-digest") + 1], self.plan)
            self.assertEqual(pathlib.Path(argv[argv.index("--bundle") + 1]), historical / "input")
        with mock.patch.object(self.paired, "_load_cpu_launcher", return_value=self.cpu):
            context = self.paired.revalidate_paired_qualification(output, runner=self.runner)
        self.assertEqual(context["owned_input"], output / "input")
        self.assertEqual(context["plan_sha256"], self.plan)
        self.assertEqual(context["cpu_binary"], self.cpu_binary)
        self.assertEqual(len(context["qualification_images"]), 7)

    def test_wrapper_receipt_cap_is_independent_of_the_native_stdout_cap(self):
        self.assertEqual(self.paired.MAX_CHILD_RECEIPT_BYTES, 64 * 1024 * 1024)
        self.assertEqual(self.auth.MAX_RESULT_JSON_BYTES, 16 * 1024 * 1024)
        path = self.root / "expanded-wrapper.json"
        # Scale both independent bounds down without creating huge test files.
        raw = json.dumps({"expanded_receipt_evidence": "x" * 2048}).encode()
        path.write_bytes(raw)
        with mock.patch.object(self.paired, "MAX_CHILD_RECEIPT_BYTES", 4096), \
             mock.patch.object(self.auth, "MAX_RESULT_JSON_BYTES", 1024):
            evidence = {}
            result = self.paired._read_json(self.auth, path, evidence=evidence)
            self.assertEqual(result["expanded_receipt_evidence"], "x" * 2048)
            self.assertEqual(evidence, {"bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()})
            with self.assertRaisesRegex(self.auth.Rejected, "byte cap exceeded"):
                self.auth.child_json(path)
            path.write_bytes(b'{"expanded_receipt_evidence":"' + b"x" * 4096 + b'"}')
            with self.assertRaisesRegex(self.auth.Rejected, "byte cap exceeded"):
                self.paired._read_json(self.auth, path)

    def test_positive_capacity_candidate_is_route_bound_and_revalidated(self):
        candidate = self.paired.capacity_policy(5 * 1024 * 1024, 64 * 1024 * 1024)
        code, receipt, calls, output = self.run_case(**candidate)
        self.assertEqual(code, 0)
        self.assertEqual(receipt["capacity_policies"],
                         {"compatibility": self.paired.capacity_policy(), "production-slot": candidate})
        self.assertFalse(receipt["capacity_matrix_complete"])
        for argv in calls[1:]:
            route = argv[argv.index("--route") + 1]
            expected = candidate if route == "production-slot" else self.paired.capacity_policy()
            self.assertEqual(int(argv[argv.index("--slot-headroom-bytes") + 1]), expected["slot_headroom_bytes"])
            self.assertEqual(int(argv[argv.index("--aggregate-headroom-bytes") + 1]), expected["aggregate_headroom_bytes"])
        with mock.patch.object(self.paired, "_load_cpu_launcher", return_value=self.cpu):
            context = self.paired.revalidate_paired_qualification(output, runner=self.runner)
        self.assertEqual(context["capacity_policies"], receipt["capacity_policies"])
        self.assertEqual(context["cpu_stage_environments"]["production-slot"],
                         self.paired.stage_environment("cpu", candidate))

    def test_invalid_capacity_and_half_pairs_reject_before_loading_children(self):
        candidates = ({"slot_headroom_bytes": 0}, {"aggregate_headroom_bytes": 0},
                      {"slot_headroom_bytes": 1024 * 1024},
                      {"slot_headroom_bytes": True, "aggregate_headroom_bytes": True},
                      {"slot_headroom_bytes": 1, "aggregate_headroom_bytes": 1024 * 1024},
                      {"slot_headroom_bytes": 65 * 1024 * 1024, "aggregate_headroom_bytes": 128 * 1024 * 1024},
                      {"slot_headroom_bytes": 5 * 1024 * 1024, "aggregate_headroom_bytes": 4 * 1024 * 1024},
                      {"slot_headroom_bytes": 5 * 1024 * 1024, "aggregate_headroom_bytes": 513 * 1024 * 1024})
        for candidate in candidates:
            with self.subTest(candidate=candidate), mock.patch.object(self.paired, "_load_cpu_launcher") as loaded:
                with self.assertRaises(SystemExit):
                    self.paired.paired_qualification(self.args(self.root / "invalid-capacity", **candidate), self.runner)
                loaded.assert_not_called()
        self.assertEqual(self.paired.route_capacity_policies(self.args(self.root / "default")),
                         self.paired.route_capacity_policies(self.args(self.root / "explicit", **self.paired.capacity_policy())))

    def test_capacity_native_wrapper_or_environment_mismatch_rejects(self):
        candidate = self.paired.capacity_policy(5 * 1024 * 1024, 64 * 1024 * 1024)
        for stage in ("cpu", "gpu"):
            for group in ("wrapper-policy", "native-policy", "expected-pins", "effective-pins"):
                def mutate(name, child, _argv, stage=stage, group=group):
                    if name != ("cpu-production-slot" if stage == "cpu" else "gpu-production-slot-1"):
                        return
                    native = child["steps"]["semantics" if stage == "cpu" else "replay"]["native_result"]
                    if group == "wrapper-policy":
                        child["capacity_policy"] = self.paired.capacity_policy()
                    elif group == "native-policy":
                        native["capacity_policy"] = self.paired.capacity_policy()
                    elif group == "expected-pins":
                        child["expected_stage_environment"] = self.paired.stage_environment(stage, self.paired.capacity_policy())
                    else:
                        native["effective_stage_environment"] = self.paired.stage_environment(stage, self.paired.capacity_policy())
                with self.subTest(stage=stage, group=group):
                    code, receipt, _calls, output = self.run_case(mutate=mutate, **candidate)
                    self.assertEqual(code, 1)
                    self.assertFalse(receipt["accepted"])
                    self.assertTrue((output / "cpu-production-slot/result.json").exists())

    def test_revalidator_rejects_missing_capacity_unsupported_route_and_rebinding(self):
        candidate = self.paired.capacity_policy(5 * 1024 * 1024, 64 * 1024 * 1024)
        for kind in ("missing", "unsupported-route", "compatibility-positive", "rebind-candidate"):
            code, _receipt, _calls, output = self.run_case(**candidate)
            self.assertEqual(code, 0)
            path = output / "result.json"
            receipt = json.loads(path.read_text())
            if kind == "missing":
                del receipt["capacity_policies"]
            elif kind == "unsupported-route":
                receipt["capacity_policies"]["other"] = self.paired.capacity_policy()
            elif kind == "compatibility-positive":
                receipt["capacity_policies"]["compatibility"] = candidate
            else:
                receipt["capacity_policies"]["production-slot"] = self.paired.capacity_policy()
                receipt["expected_stage_environment"] = self.paired.stage_environments(receipt["capacity_policies"])
            path.write_text(json.dumps(receipt))
            with mock.patch.object(self.paired, "_load_cpu_launcher", return_value=self.cpu):
                with self.assertRaises(self.paired.Rejected):
                    self.paired.revalidate_paired_qualification(output, runner=self.runner)

    def test_capacity_environment_overrides_stay_undeclared_even_exact_zero(self):
        for name in self.paired.CAPACITY_SELECTORS:
            for value in ("0", str(5 * 1024 * 1024)):
                with self.assertRaises(self.paired.Rejected):
                    self.paired.inherited_environment({name: value})

    def test_grouping_limits_three_and_sixtyfour_bind_only_compatibility(self):
        for limit in (3, 64):
            code, receipt, calls, output = self.run_case(draw_command_limit=limit)
            self.assertEqual(code, 0)
            self.assertEqual(receipt["replay_policies"]["compatibility"]["draw_command_limit"], limit)
            self.assertEqual(receipt["replay_policies"]["production-slot"]["draw_command_limit"], 0)
            self.assertFalse(receipt["replay_policy_matrix_complete"])
            for argv in calls[1:]:
                route = argv[argv.index("--route") + 1]
                self.assertEqual(int(argv[argv.index("--draw-command-limit") + 1]), limit if route == "compatibility" else 0)
            with mock.patch.object(self.paired, "_load_cpu_launcher", return_value=self.cpu):
                context = self.paired.revalidate_paired_qualification(output, runner=self.runner)
            self.assertEqual(context["replay_policies"], receipt["replay_policies"])
            for stage in ("cpu", "gpu"):
                self.assertEqual(receipt["expected_stage_environment"][stage]["compatibility"]["DXMT9_DRAW_CHUNK_COMMAND_LIMIT"], str(limit))
                self.assertEqual(receipt["expected_stage_environment"][stage]["compatibility"]["DXMT9_CHUNK_DRAW_PAYLOAD_ARENA_LIMIT_BYTES"], "0")

    def test_grouping_bounds_and_mixed_positive_axes_reject_before_children(self):
        for value in (True, None, -1, 1, 2, 65, 3.5):
            with mock.patch.object(self.paired, "_load_cpu_launcher") as loaded, self.assertRaises(SystemExit):
                self.paired.paired_qualification(self.args(self.root / "bad-group", draw_command_limit=value), self.runner)
            loaded.assert_not_called()
        with mock.patch.object(self.paired, "_load_cpu_launcher") as loaded, self.assertRaises(SystemExit):
            self.paired.paired_qualification(self.args(self.root / "mixed-policy", draw_command_limit=3,
                **self.paired.capacity_policy(1024 * 1024, 64 * 1024 * 1024)), self.runner)
        loaded.assert_not_called()

    def test_grouping_wrapper_native_inspector_and_pin_identity_mismatch_rejects(self):
        for stage in ("cpu", "gpu"):
            for kind in ("wrapper", "native", "pins", "inspection"):
                if kind == "inspection" and stage == "cpu":
                    continue
                def mutate(name, child, _argv, stage=stage, kind=kind):
                    if name != ("cpu-compatibility" if stage == "cpu" else "gpu-compatibility-1"):
                        return
                    exact = self.paired.replay_policy("compatibility", self.paired.capacity_policy())
                    native = child["steps"]["semantics" if stage == "cpu" else "replay"]["native_result"]
                    if kind == "wrapper":
                        child["replay_policy"] = exact
                    elif kind == "native":
                        native["replay_policy"] = exact
                    elif kind == "pins":
                        native["effective_stage_environment"]["DXMT9_DRAW_CHUNK_COMMAND_LIMIT"] = "0"
                    else:
                        child["steps"]["inspect"]["native_result"]["replay_policy"] = {**exact, "route": "inspection"}
                code, receipt, _calls, _output = self.run_case(mutate=mutate, draw_command_limit=3)
                self.assertEqual(code, 1)
                self.assertFalse(receipt["accepted"])

    def test_revalidation_rejects_full_policy_rebinding_missing_identity_and_illegal_production_grouping(self):
        for kind in ("missing", "rebound", "production-positive", "bool-limit"):
            code, _receipt, _calls, output = self.run_case(draw_command_limit=3)
            self.assertEqual(code, 0)
            path = output / "result.json"
            receipt = json.loads(path.read_text())
            if kind == "missing":
                del receipt["replay_policies"]
            elif kind == "rebound":
                receipt["replay_policies"]["compatibility"]["draw_command_limit"] = 0
                receipt["expected_stage_environment"] = self.paired.stage_environments(
                    receipt["capacity_policies"], receipt["replay_policies"])
            else:
                receipt["replay_policies"]["production-slot"]["draw_command_limit"] = 3 if kind == "production-positive" else False
            path.write_text(json.dumps(receipt))
            with mock.patch.object(self.paired, "_load_cpu_launcher", return_value=self.cpu), self.assertRaises(self.paired.Rejected):
                self.paired.revalidate_paired_qualification(output, runner=self.runner)

    def test_overcap_wrapper_receipt_rejects_admission_and_revalidation(self):
        def mutate(name, child, _argv):
            if name == "cpu-compatibility":
                child["mock_expanded_receipt_evidence"] = "x" * (64 * 1024)
        with mock.patch.object(self.paired, "MAX_CHILD_RECEIPT_BYTES", 64 * 1024):
            code, receipt, calls, output = self.run_case(mutate=mutate)
        self.assertEqual(code, 1)
        self.assertEqual(len(calls), 2)
        self.assertIn("byte cap exceeded", receipt["failure"])
        self.assertGreater((output / "cpu-compatibility/result.json").stat().st_size, 64 * 1024)
        self.assertFalse(receipt["paired_qualification_complete"])

        code, _receipt, _calls, output = self.run_case()
        self.assertEqual(code, 0)
        child_path = output / "cpu-compatibility/result.json"
        cap = max(path.stat().st_size for path in output.rglob("result.json")) + 1
        child_path.write_bytes(child_path.read_bytes() + b" " * cap)
        with mock.patch.object(self.paired, "_load_cpu_launcher", return_value=self.cpu), \
             mock.patch.object(self.paired, "MAX_CHILD_RECEIPT_BYTES", cap):
            with self.assertRaisesRegex(self.paired.Rejected, "byte cap exceeded"):
                self.paired.revalidate_paired_qualification(output, runner=self.runner)

    def test_every_original_item_must_match_not_only_final_digest(self):
        def mutate(name, child, _argv):
            if name == "cpu-production-slot":
                semantic = child["steps"]["semantics"]["native_result"]["execution"]["logical_semantics"]
                semantic["receipts"][1]["digest"] = "1111111111111111"
        code, receipt, calls, _output = self.run_case(mutate=mutate)
        self.assertEqual(code, 1)
        self.assertEqual(len(calls), 3)
        self.assertFalse(receipt["logical_streams_equal"])
        self.assertIn("original item mismatch", receipt["failure"])

    def test_locator_kind_and_canonical_count_mismatch_rejects_before_gpu(self):
        for field in ("event_ordinal", "record_index", "command_kind", "identity_command_kind", "events"):
            def mutate(name, child, _argv, selected=field):
                if name != "cpu-production-slot":
                    return
                native = child["steps"]["semantics"]["native_result"]
                if selected == "events":
                    native["execution"]["counters"][selected] += 1
                elif selected in ("event_ordinal", "record_index"):
                    native["execution"]["logical_semantics"]["receipts"][0]["record"][selected] += 1
                else:
                    native["execution"]["logical_semantics"]["receipts"][0][selected] += 1
            with self.subTest(field=field):
                code, receipt, calls, _output = self.run_case(mutate=mutate)
                self.assertEqual(code, 1)
                self.assertEqual(len(calls), 3)
                self.assertFalse(receipt["paired_qualification_complete"])

    def test_each_gpu_iteration_requires_original_queries_and_both_exact_pixels(self):
        for name in ("gpu-compatibility-1", "gpu-compatibility-2", "gpu-production-slot-1", "gpu-production-slot-2"):
            for gate in ("query_qualified", "source_exact", "output_exact"):
                def mutate(current, child, _argv, target=name, selected=gate):
                    if current == target:
                        child["steps"]["replay"]["native_result"][selected] = False
                with self.subTest(name=name, gate=gate):
                    code, receipt, _calls, output = self.run_case(mutate=mutate)
                    self.assertEqual(code, 1)
                    self.assertFalse(receipt["paired_qualification_complete"])
                    self.assertTrue((output / name / "result.json").is_file())

    def test_direct_path_pins_shared_images_and_post_auth_are_independent_gates(self):
        for variant in ("direct", "pins", "image", "byte", "binary", "older-pixels", "older-receipt"):
            def mutate(name, child, _argv, selected=variant):
                if name == "gpu-production-slot-1":
                    native = child["steps"]["replay"]["native_result"]
                    if selected == "direct":
                        native["path_counters"]["direct_lease_draws"] = 0
                    elif selected == "pins":
                        native["effective_stage_environment"]["DXMT9_PREWARM"] = "enabled"
                    elif selected == "image":
                        native["loaded_images"] = [row for row in native["loaded_images"] if not row["path"].endswith("winemetal_dxmt9.so")]
            def after(name, output, _bundle, selected=variant):
                if name == "gpu-production-slot-2":
                    if selected == "byte":
                        (output / "replay-source.rgba").write_bytes(b"changed")
                    elif selected == "binary":
                        self.cpu_binary.write_bytes(b"changed binary bytes")
                    elif selected == "older-pixels":
                        (output.parent / "gpu-compatibility-1/replay-output.rgba").write_bytes(b"changed")
                    elif selected == "older-receipt":
                        child_path = output.parent / "cpu-compatibility/result.json"
                        earlier = json.loads(child_path.read_text())
                        earlier["expected_plan_digest"] = "f" * 64
                        child_path.write_text(json.dumps(earlier))
            with self.subTest(variant=variant):
                saved = self.cpu_binary.read_bytes()
                code, receipt, _calls, _output = self.run_case(mutate=mutate, after=after)
                self.cpu_binary.write_bytes(saved)
                self.assertEqual(code, 1)
                self.assertFalse(receipt["paired_qualification_complete"])

    def test_child_failure_preserves_all_raw_receipts_and_starts_no_younger_child(self):
        code, receipt, calls, output = self.run_case(failure="cpu-production-slot")
        self.assertEqual(code, 1)
        self.assertEqual(len(calls), 3)
        self.assertIn("exit code 7", receipt["failure"])
        self.assertTrue((output / "cpu-production-slot/result.json").is_file())
        self.assertIn("raw child 3", (output / "cpu-production-slot.stderr").read_text())
        self.assertFalse(any(name.startswith("gpu-") for name in receipt["children"]))

    def test_secondary_log_cap_failure_does_not_mask_original_child_failure(self):
        check_logs = self.paired._validate_child_logs
        def check(staging, name):
            if name == "cpu-production-slot":
                raise self.paired.Rejected("paired child log byte cap exceeded")
            return check_logs(staging, name)
        with mock.patch.object(self.paired, "_validate_child_logs", side_effect=check):
            code, receipt, calls, _output = self.run_case(failure="cpu-production-slot")
        self.assertEqual(code, 1)
        self.assertEqual(len(calls), 3)
        self.assertIn("exit code 7", receipt["failure"])
        self.assertEqual(receipt["secondary_failures"], ["paired child log byte cap exceeded"])

    def test_finite_environment_and_explicit_contracts_reject_before_child(self):
        for values in ({"DXMT_UNKNOWN": "1"}, {"DYLD_INSERT_LIBRARIES": "/tmp/inject.dylib"},
                       {"DYLD_LIBRARY_PATH": f"{self.root}:"}, {"MTL_UNKNOWN": "1"}):
            with self.subTest(environment=values), mock.patch.dict(os.environ, values, clear=True):
                with self.assertRaises(self.paired.Rejected):
                    self.paired.inherited_environment(dict(os.environ))
        self.assertEqual(self.paired.inherited_environment({"UNRELATED_SECRET": "not-recorded", "MTL_DEBUG_LAYER": "1"}),
                         {"MTL_DEBUG_LAYER": "1"})
        for field, value in (("producer_contract", "none"), ("buffer_preservation_contract", "none"),
                             ("timeout_seconds", True), ("timeout_seconds", float("inf")), ("timeout_seconds", 1801)):
            args = self.args(self.root / "unused")
            setattr(args, field, value)
            with self.subTest(field=field), mock.patch.object(self.paired, "_load_cpu_launcher") as loader:
                with self.assertRaises(SystemExit):
                    self.paired.paired_qualification(args, runner=self.runner)
                loader.assert_not_called()

    def test_revalidation_does_not_trust_accepted_bits_after_child_or_pixel_tampering(self):
        for variant in ("digest", "pixels", "image"):
            code, _receipt, _calls, output = self.run_case()
            self.assertEqual(code, 0)
            child_path = output / "cpu-production-slot/result.json"
            if variant == "digest":
                child = json.loads(child_path.read_text())
                child["steps"]["semantics"]["native_result"]["execution"]["logical_semantics"]["receipts"][0]["digest"] = "2222222222222222"
                child_path.write_text(json.dumps(child))
            elif variant == "pixels":
                (output / "gpu-compatibility-2/replay-output.rgba").write_bytes(b"changed")
            else:
                self.modules[0].write_bytes(b"changed runtime module")
            with mock.patch.object(self.paired, "_load_cpu_launcher", return_value=self.cpu):
                with self.assertRaises((self.paired.Rejected, self.auth.Rejected)):
                    self.paired.revalidate_paired_qualification(output, runner=self.runner)
            if variant == "image":
                self.modules[0].write_bytes(self.modules[0].name.encode())

    def test_inspection_and_step_lifecycle_are_independently_revalidated(self):
        variants = [("status", "complete"), ("semantic_mapping_qualified", True),
                    ("exact_source_diagnostic_complete", True), *[(gate, True) for gate in self.auth.GATES]]
        for field, value in variants:
            with self.subTest(inspect_field=field):
                code, _receipt, _calls, output = self.run_case()
                self.assertEqual(code, 0)
                child_path = output / "gpu-compatibility-1/result.json"
                child = json.loads(child_path.read_text())
                child["steps"]["inspect"]["native_result"][field] = value
                child_path.write_text(json.dumps(child))
                with mock.patch.object(self.paired, "_load_cpu_launcher", return_value=self.cpu):
                    with self.assertRaises(self.paired.Rejected):
                        self.paired.revalidate_paired_qualification(output, runner=self.runner)
        for stage in ("aggregate", "private"):
            for field, value in (("status", "child-timeout"), ("returncode", True), ("timed_out", True)):
                with self.subTest(stage=stage, field=field):
                    code, _receipt, _calls, output = self.run_case()
                    self.assertEqual(code, 0)
                    path = output / ("result.json" if stage == "aggregate" else "cpu-compatibility/result.json")
                    receipt = json.loads(path.read_text())
                    receipt["steps"]["cpu-compatibility" if stage == "aggregate" else "semantics"][field] = value
                    path.write_text(json.dumps(receipt))
                    with mock.patch.object(self.paired, "_load_cpu_launcher", return_value=self.cpu):
                        with self.assertRaises(self.paired.Rejected):
                            self.paired.revalidate_paired_qualification(output, runner=self.runner)

    def test_revalidation_rejects_bool_iterations_and_normalizes_private_failures(self):
        code, _receipt, _calls, output = self.run_case()
        self.assertEqual(code, 0)
        path = output / "result.json"
        receipt = json.loads(path.read_text())
        receipt["iterations_per_route_per_backend"]["cpu"] = True
        path.write_text(json.dumps(receipt))
        with mock.patch.object(self.paired, "_load_cpu_launcher", return_value=self.cpu):
            with self.assertRaises(self.paired.Rejected):
                self.paired.revalidate_paired_qualification(output, runner=self.runner)
        receipt["iterations_per_route_per_backend"]["cpu"] = 1
        path.write_text(json.dumps(receipt))
        with mock.patch.object(self.paired, "_load_cpu_launcher", return_value=self.cpu), \
             mock.patch.object(self.cpu, "authenticate", side_effect=self.auth.Rejected("private admission rejected")):
            with self.assertRaisesRegex(self.paired.Rejected, "private admission rejected"):
                self.paired.revalidate_paired_qualification(output, runner=self.runner)


if __name__ == "__main__":
    unittest.main()
