#!/usr/bin/env python3
"""V4 aggregate scheduling and authority tests; no real child is executed."""

from __future__ import annotations

import argparse
import contextlib
import copy
import hashlib
import importlib.util
import io
import json
import pathlib
import subprocess
import unittest
from unittest import mock


def load_module(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def fingerprint(path):
    raw = path.read_bytes()
    return {"bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}


class RenderTapeV4ExperimentTest(unittest.TestCase):
    def setUp(self):
        check = pathlib.Path(__file__).resolve().parent
        fixtures = load_module(check / "test_render_tape_cpu_v4_execute.py", "experiment_native_fixture")
        self.fixture = fixtures.ControlledCpuExecuteTest()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.root = self.fixture.root
        self.qualification = self.root / "qualification"
        self.qualification.mkdir()
        self.bundle = self.qualification / "input"
        self.fixture.bundle.rename(self.bundle)
        self.fixture.bundle = self.fixture.fixture.bundle = self.bundle
        self.manifest = self.fixture.manifest
        self.manifest.update(producer={"path": "dxmt9-pe-render-tape"}, stage="production-capture")
        self.manifest["scope"].update(production_capture=True, source_oracle=True, output_oracle=True)
        self.fixture.fixture.save_manifest()
        self.runner = self.fixture.shared
        self.experiment = load_module(check.parent / "tools/render_tape_v4_experiment.py", "experiment_tested")
        self.paired, _execute = self.experiment.load_dependencies()
        # Keep the actual independent execution-only native/image validators.
        self.execute = self.fixture.launcher
        self.action = self.runner.load_action_profile(self.bundle, self.manifest)
        self.gpu_binary = self.root / "mock-gpu-native"
        self.gpu_binary.write_bytes(b"mock GPU executable; never executed\n")
        self.gpu_binary.chmod(0o700)
        self.qualification_receipt = self.qualification / "result.json"
        self.qualification_receipt.write_text(json.dumps({"fixture": "authority supplied by mocked revalidator"}))
        self.policies = self.paired.route_capacity_policies(argparse.Namespace())
        self.replays = self.paired.route_replay_policies(argparse.Namespace(), self.policies)
        self.qualified_children = []
        for route in self.experiment.ROUTES:
            directory = self.qualification / f"cpu-{route}"
            directory.mkdir()
            native = self.fixture.native(self.experiment.PRODUCER_CONTRACT, route)
            native["capacity_policy"] = dict(self.policies[route])
            native["replay_policy"] = dict(self.replays[route])
            native["effective_stage_environment"] = self.paired.stage_environment(
                "cpu", self.policies[route], self.replays[route]["draw_command_limit"])
            self.canonical_counts = self.paired.canonical_execution_counts(native)
            path = directory / "result.json"
            path.write_text(json.dumps({"capacity_policy": dict(self.policies[route]),
                "replay_policy": dict(self.replays[route]),
                "expected_stage_environment": self.paired.stage_environment(
                    "cpu", self.policies[route], self.replays[route]["draw_command_limit"]),
                "steps": {"semantics": {"status": "exited", "returncode": 0,
                "timed_out": False, "native_result": native}}}))
            self.qualified_children.append(path)
        self.case_count = 0

    def context(self, environment=None):
        return {
            "receipt": {"input_manifest": {"path": "manifest.json", **fingerprint(self.bundle / "manifest.json")},
                        "canonical_execution_counts": copy.deepcopy(self.canonical_counts)},
            "owned_input": self.bundle, "manifest": copy.deepcopy(self.manifest),
            "action": copy.deepcopy(self.action), "plan_sha256": self.fixture.plan,
            "cpu_binary": self.fixture.binary, "gpu_binary": self.gpu_binary,
            "stage_executables": {"cpu": fingerprint(self.fixture.binary), "gpu": fingerprint(self.gpu_binary)},
            "shared_runtime_images": {str(path): fingerprint(path) for path in self.fixture.modules},
            "environment": dict(environment or {}),
            "producer_contract": self.experiment.PRODUCER_CONTRACT,
            "buffer_preservation_contract": self.experiment.PRESERVATION_CONTRACT,
            "cpu_stage_environment": dict(self.execute.STAGE_ENVIRONMENT),
            "capacity_policies": copy.deepcopy(self.policies),
            "replay_policies": copy.deepcopy(self.replays),
            "cpu_stage_environments": self.paired.stage_environments(self.policies, self.replays)["cpu"],
            "qualification_images": {str(path): fingerprint(path)
                                     for path in (self.qualification_receipt, *self.qualified_children)},
        }

    def arguments(self, output, **changes):
        values = {"qualification_dir": self.qualification, "output_dir": output,
                  "diagnostic": None, "order": "compatibility,production-slot", "warmup": 1, "repeat": 2,
                  "producer_contract": self.experiment.PRODUCER_CONTRACT,
                  "buffer_preservation_contract": self.experiment.PRESERVATION_CONTRACT,
                  "timeout_seconds": 30}
        values.update(changes)
        return argparse.Namespace(**values)

    def child_receipt(self, context, route, directory):
        native = self.fixture.native(self.experiment.PRODUCER_CONTRACT, route)
        native["capacity_policy"] = dict(context["capacity_policies"][route])
        native["replay_policy"] = dict(context["replay_policies"][route])
        native["effective_stage_environment"] = copy.deepcopy(context["cpu_stage_environments"][route])
        execution = native["execution"]
        return {
            "schema": self.execute.RECEIPT_SCHEMA, "status": "complete", "accepted": True, "diagnostic": True,
            **{name: False for name in self.execute.FALSE_CLAIMS},
            **{name: True for name in ("execution_complete", "manifest_authenticated", "bundle_reauthenticated",
                "binary_reauthenticated", "loaded_images_stable", "conditional_on_legal_nooverwrite",
                "preservation_contract_is_caller_assumption", "capacity_policy_valid", "stage_environment_valid",
                "replay_policy_valid")},
            "bundle": str(context["owned_input"]), "output_dir": str(directory),
            "diagnostic_binary": str(context["cpu_binary"]), "route": route,
            "requested_route": route, "effective_route": route, "iterations": 1,
            "expected_plan_digest": context["plan_sha256"],
            "producer_contract": context["producer_contract"],
            "buffer_preservation_contract": context["buffer_preservation_contract"],
            "input_manifest": copy.deepcopy(context["receipt"]["input_manifest"]),
            "input_components": copy.deepcopy(context["manifest"]["components"]),
            "action_profile_validation": copy.deepcopy(context["action"]),
            "diagnostic_executable": copy.deepcopy(context["stage_executables"]["cpu"]),
            "environment": copy.deepcopy(context["environment"]),
            "capacity_policy": copy.deepcopy(context["capacity_policies"][route]),
            "replay_policy": copy.deepcopy(context["replay_policies"][route]),
            "expected_stage_environment": copy.deepcopy(context["cpu_stage_environments"][route]),
            "effective_stage_environment": copy.deepcopy(context["cpu_stage_environments"][route]),
            "native_max_wall_ms": native["max_wall_ms"], "execution": copy.deepcopy(execution),
            "clocks": copy.deepcopy(execution["clocks"]),
            "route_coverage": copy.deepcopy(execution["route_coverage"]),
            "loaded_images": copy.deepcopy(native["loaded_images"]),
            "steps": {"execute": {"status": "exited", "returncode": 0, "timed_out": False,
                                   "native_result": native}},
        }

    @staticmethod
    def sync_native_projections(child):
        native = child["steps"]["execute"]["native_result"]
        child["execution"] = copy.deepcopy(native["execution"])
        child["clocks"] = copy.deepcopy(native["execution"]["clocks"])
        child["route_coverage"] = copy.deepcopy(native["execution"]["route_coverage"])
        child["loaded_images"] = copy.deepcopy(native["loaded_images"])

    def run_case(self, *, context=None, qualification_effect=None, edit_child=None, after_child=None,
                 response_kind=None, outer_returncode=0, environment=None, candidate_policy=None,
                 grouping_limit=None, **arguments):
        self.case_count += 1
        output = self.root / f"experiment-{self.case_count}"
        args = self.arguments(output, **arguments)
        environment = dict(environment or {})
        if candidate_policy is not None:
            self.policies = self.paired.validate_route_capacity_policies(
                {"compatibility": self.paired.capacity_policy(), "production-slot": candidate_policy})
        if candidate_policy is not None or grouping_limit is not None:
            self.replays = self.paired.route_replay_policies(argparse.Namespace(
                draw_command_limit=0 if grouping_limit is None else grouping_limit), self.policies)
            for route, path in zip(self.experiment.ROUTES, self.qualified_children):
                qualified = json.loads(path.read_text())
                qualified["capacity_policy"] = dict(self.policies[route])
                qualified["replay_policy"] = dict(self.replays[route])
                qualified["expected_stage_environment"] = self.paired.stage_environment(
                    "cpu", self.policies[route], self.replays[route]["draw_command_limit"])
                native = qualified["steps"]["semantics"]["native_result"]
                native["capacity_policy"] = dict(self.policies[route])
                native["replay_policy"] = dict(self.replays[route])
                native["effective_stage_environment"] = self.paired.stage_environment(
                    "cpu", self.policies[route], self.replays[route]["draw_command_limit"])
                path.write_text(json.dumps(qualified))
        context = self.context(environment) if context is None else context
        calls = []

        def child(argv, **kwargs):
            # This is the execution-only Python launcher, not the observed
            # semantic endpoint or native executable. Its one native receipt
            # is synthetic, independently revalidated by the aggregate.
            self.assertTrue(revalidated.called)
            calls.append(list(argv))
            self.assertNotIn("env", kwargs)
            self.assertEqual(pathlib.Path(argv[0]).name, "run_python.sh")
            self.assertEqual(pathlib.Path(argv[1]).name, "run_render_tape_cpu_v4_execute.py")
            self.assertEqual(argv[argv.index("--bundle") + 1], str(self.bundle))
            policy = context["capacity_policies"][argv[argv.index("--route") + 1]]
            self.assertEqual(int(argv[argv.index("--slot-headroom-bytes") + 1]), policy["slot_headroom_bytes"])
            self.assertEqual(int(argv[argv.index("--aggregate-headroom-bytes") + 1]), policy["aggregate_headroom_bytes"])
            self.assertEqual(int(argv[argv.index("--draw-command-limit") + 1]),
                             context["replay_policies"][argv[argv.index("--route") + 1]]["draw_command_limit"])
            self.assertGreater(kwargs["timeout"], 0)
            self.assertLessEqual(kwargs["timeout"], self.experiment.MAX_CHILD_TIMEOUT_SECONDS)
            directory = pathlib.Path(argv[argv.index("--output-dir") + 1])
            directory.mkdir()
            route = argv[argv.index("--route") + 1]
            receipt = self.child_receipt(context, route, directory)
            if edit_child:
                edit_child(receipt, len(calls) - 1, argv)
            child_path = directory / "result.json"
            (directory / "execute.stdout").write_text(json.dumps(receipt["steps"]["execute"]["native_result"]))
            (directory / "execute.stderr").write_text("retained native diagnostics\n")
            if response_kind == "missing":
                pass
            elif response_kind == "duplicate":
                child_path.write_bytes(b'{"accepted":true,"accepted":true}')
            elif response_kind == "nonfinite":
                child_path.write_bytes(b'{"accepted":NaN}')
            elif response_kind == "oversized":
                with child_path.open("wb") as stream:
                    stream.truncate(self.experiment.MAX_CHILD_RECEIPT_BYTES + 1)
            elif response_kind == "symlink":
                raw_path = directory / "raw-result.json"
                raw_path.write_text(json.dumps(receipt))
                child_path.symlink_to(raw_path)
            else:
                child_path.write_text(json.dumps(receipt))
            kwargs["stdout"].write(json.dumps({"accepted": receipt["accepted"], "receipt": str(child_path)}).encode())
            kwargs["stderr"].write(b"retained launcher diagnostics\n")
            if after_child:
                after_child(receipt, len(calls) - 1, argv, child_path)
            return subprocess.CompletedProcess(argv, outer_returncode)

        effect = qualification_effect if qualification_effect is not None else lambda *args, **kwargs: context
        with mock.patch.dict(self.experiment.os.environ, environment, clear=True), \
             mock.patch.object(self.experiment, "load_dependencies", return_value=(self.paired, self.execute)), \
             mock.patch.object(self.paired, "revalidate_paired_qualification", side_effect=effect) as revalidated, \
             mock.patch.object(self.execute.auth.subprocess, "run", side_effect=child), \
             mock.patch.object(self.runner, "copy_input_owned") as copied, \
             contextlib.redirect_stdout(io.StringIO()):
            code = self.experiment.experiment(args, self.runner)
        copied.assert_not_called()
        receipt = json.loads((output / "result.json").read_text())
        return code, receipt, calls, output, revalidated

    def assert_rejected(self, **kwargs):
        code, receipt, calls, output, revalidated = self.run_case(**kwargs)
        self.assertEqual(code, 1)
        self.assertFalse(receipt["accepted"])
        self.assertFalse(receipt["measurement_complete"])
        self.assertEqual(receipt["status"], "rejected")
        self.assertFalse(receipt["summary"]["comparison_ready"])
        self.assertIsNone(receipt["summary"]["comparison"])
        return receipt, calls, output, revalidated

    def test_forward_and_reverse_route_order_fresh_launcher_and_shared_input(self):
        for order in ("compatibility,production-slot", "production-slot,compatibility"):
            with self.subTest(order=order):
                code, receipt, calls, output, revalidated = self.run_case(order=order)
                self.assertEqual(code, 0)
                self.assertTrue(receipt["accepted"])
                self.assertEqual(revalidated.call_count, 2)
                self.assertEqual(len(calls), 6)
                self.assertEqual([row["route"] for row in receipt["iterations"]], order.split(",") * 3)
                self.assertEqual(len({row["run_id"] for row in receipt["iterations"]}), 6)
                self.assertTrue(all((output / row["receipt"]).is_file() for row in receipt["iterations"]))
                self.assertFalse(receipt["summary"]["comparison_ready"])
                self.assertTrue(receipt["summary"]["smoke_sample_count"])
                for arm in receipt["summary"]["arms"]:
                    self.assertEqual(arm["warmups_accepted"], 1)
                    self.assertEqual(arm["body_wall"]["samples"], 2)
                    self.assertEqual(arm["body_process_cpu"]["median_ns"], 58)
                    self.assertEqual(arm["body_wall"]["median_ns"], 29)
                for name in self.experiment.FALSE_CLAIMS:
                    self.assertIs(receipt[name], False)
                self.assertIsNone(receipt["summary"]["fps"])
                self.assertIsNone(receipt["summary"]["gpu_time"])
                self.assertIsNone(receipt["summary"]["policy_ranking"])

    def test_aa_routes_have_distinct_aggregate_arms_without_global_native_id_claim(self):
        for route in self.experiment.ROUTES:
            code, receipt, calls, _output, _revalidated = self.run_case(order=f"{route},{route}")
            self.assertEqual(code, 0)
            self.assertTrue(receipt["same_route_control"])
            self.assertEqual({row["arm_id"] for row in receipt["iterations"]}, {"arm-0", "arm-1"})
            self.assertEqual(len({row["run_id"] for row in receipt["iterations"]}), len(calls))
            # A real fresh-process native queue/capture allocator can reuse its
            # local values. Run/arm identity, not fabricated native uniqueness,
            # distinguishes A/A observations.
            self.assertEqual(len({tuple(row["native_arm"].values()) for row in receipt["iterations"]}), 1)

    def test_positive_candidate_pair_forward_reverse_and_production_aa_derive_capacity(self):
        candidate = self.paired.capacity_policy(5 * 1024 * 1024, 64 * 1024 * 1024)
        for order in ("compatibility,production-slot", "production-slot,compatibility",
                      "production-slot,production-slot"):
            with self.subTest(order=order):
                code, receipt, _calls, _output, _revalidated = self.run_case(
                    order=order, candidate_policy=candidate)
                self.assertEqual(code, 0)
                self.assertFalse(receipt["capacity_matrix_complete"])
                for row in receipt["iterations"]:
                    expected = candidate if row["route"] == "production-slot" else self.paired.capacity_policy()
                    self.assertEqual(row["capacity_policy"], expected)
                for arm in receipt["arms"]:
                    self.assertEqual(arm["capacity_policy"], receipt["capacity_policies"][arm["route"]])

    def test_capacity_wrapper_native_half_policy_or_stage_rebinding_rejects(self):
        candidate = self.paired.capacity_policy(5 * 1024 * 1024, 64 * 1024 * 1024)
        for kind in ("wrapper", "native", "half-policy", "expected-pins", "actual-pins"):
            def corrupt(child, _index, _argv, kind=kind):
                # Forward order begins with exact Compatibility, so any
                # positive rebinding of this first child must reject too.
                native = child["steps"]["execute"]["native_result"]
                if kind == "wrapper":
                    child["capacity_policy"] = dict(candidate)
                elif kind == "native":
                    native["capacity_policy"] = dict(candidate)
                elif kind == "half-policy":
                    del child["capacity_policy"]["aggregate_headroom_bytes"]
                elif kind == "expected-pins":
                    child["expected_stage_environment"] = self.paired.stage_environment("cpu", candidate)
                else:
                    native["effective_stage_environment"] = self.paired.stage_environment("cpu", candidate)
            with self.subTest(kind=kind):
                _receipt, calls, _output, _revalidated = self.assert_rejected(
                    edit_child=corrupt, candidate_policy=candidate)
                self.assertEqual(len(calls), 1)

    def test_invalid_or_unsupported_qualified_capacity_mapping_launches_zero_children(self):
        for kind in ("missing", "half", "unsupported-route", "compatibility-positive"):
            context = self.context()
            if kind == "missing":
                del context["capacity_policies"]
            elif kind == "half":
                del context["capacity_policies"]["production-slot"]["aggregate_headroom_bytes"]
            elif kind == "unsupported-route":
                context["capacity_policies"]["unknown"] = self.paired.capacity_policy()
            else:
                context["capacity_policies"]["compatibility"] = self.paired.capacity_policy(
                    5 * 1024 * 1024, 64 * 1024 * 1024)
            _receipt, calls, _output, _revalidated = self.assert_rejected(context=context)
            self.assertEqual(calls, [])

    def test_capacity_route_identity_mutation_after_samples_invalidates_aggregate(self):
        context = self.context()
        changed = copy.deepcopy(context)
        changed["capacity_policies"]["production-slot"] = self.paired.capacity_policy(
            5 * 1024 * 1024, 64 * 1024 * 1024)
        changed["cpu_stage_environments"] = self.paired.stage_environments(changed["capacity_policies"])["cpu"]
        receipt, calls, _output, _revalidated = self.assert_rejected(
            context=context, qualification_effect=[context, changed])
        self.assertEqual(len(calls), 6)
        self.assertFalse(receipt["final_qualification_revalidated"])

    def test_grouping_limits_are_derived_per_route_including_compatibility_aa(self):
        for limit in (3, 64):
            for order in ("compatibility,production-slot", "compatibility,compatibility"):
                code, receipt, _calls, _output, _revalidated = self.run_case(order=order, grouping_limit=limit)
                self.assertEqual(code, 0)
                self.assertFalse(receipt["replay_policy_matrix_complete"])
                for row in receipt["iterations"]:
                    expected = limit if row["route"] == "compatibility" else 0
                    self.assertEqual(row["replay_policy"]["draw_command_limit"], expected)
                    self.assertEqual(row["effective_stage_environment"]["DXMT9_DRAW_CHUNK_COMMAND_LIMIT"], str(expected))

    def test_grouped_wrapper_native_full_identity_and_pins_are_independent_gates(self):
        for kind in ("wrapper", "native", "pins", "bool-limit", "missing"):
            def corrupt(child, _index, _argv, kind=kind):
                native = child["steps"]["execute"]["native_result"]
                if kind == "wrapper":
                    child["replay_policy"]["draw_command_limit"] = 0
                elif kind == "native":
                    native["replay_policy"]["draw_command_limit"] = 0
                elif kind == "pins":
                    native["effective_stage_environment"]["DXMT9_DRAW_CHUNK_COMMAND_LIMIT"] = "0"
                elif kind == "bool-limit":
                    child["replay_policy"]["draw_command_limit"] = True
                else:
                    del child["replay_policy"]
            receipt, calls, _output, _revalidated = self.assert_rejected(edit_child=corrupt, grouping_limit=3)
            self.assertEqual(len(calls), 1)
            self.assertFalse(receipt["accepted"])

    def test_final_full_grouping_identity_rebinding_invalidates_accepted_rows(self):
        self.run_case(grouping_limit=3)
        context = self.context()
        changed = copy.deepcopy(context)
        changed["replay_policies"]["compatibility"]["draw_command_limit"] = 64
        changed["cpu_stage_environments"] = self.paired.stage_environments(
            changed["capacity_policies"], changed["replay_policies"])["cpu"]
        receipt, calls, _output, _revalidated = self.assert_rejected(
            context=context, qualification_effect=[context, changed])
        self.assertEqual(len(calls), 6)
        self.assertTrue(all(row["accepted"] for row in receipt["iterations"]))
        self.assertIn("reauthentication_failure", receipt)

    def test_68_children_and_128_child_bound_include_warmups(self):
        for warmup, repeat, total in ((2, 32, 68), (32, 32, 128)):
            code, receipt, calls, _output, _revalidated = self.run_case(warmup=warmup, repeat=repeat)
            self.assertEqual(code, 0)
            self.assertEqual(len(calls), total)
            self.assertEqual(len(receipt["iterations"]), total)
            self.assertTrue(receipt["summary"]["comparison_ready"])
            for arm in receipt["summary"]["arms"]:
                self.assertEqual(arm["body_wall"]["samples"], repeat)
                self.assertEqual(arm["warmups_accepted"], warmup)

    def test_invalid_order_counts_contracts_and_time_controls_reject_before_children(self):
        changes = (
            {"order": "compatibility"}, {"order": "compatibility,unknown"}, {"order": None},
            {"warmup": -1}, {"warmup": True}, {"warmup": 33, "repeat": 32},
            {"repeat": 0}, {"repeat": 33}, {"repeat": True}, {"repeat": 1.5},
            {"producer_contract": "none"}, {"buffer_preservation_contract": "none"},
            {"timeout_seconds": float("nan")}, {"timeout_seconds": float("inf")},
            {"timeout_seconds": 0}, {"timeout_seconds": 7201}, {"timeout_seconds": True},
        )
        for values in changes:
            with self.subTest(values=values), mock.patch.object(self.experiment, "load_dependencies") as loaded:
                with self.assertRaises(SystemExit):
                    self.experiment.experiment(self.arguments(self.root / "invalid-output", **values), self.runner)
                loaded.assert_not_called()

    def test_wall_cpu_summaries_are_body_only_and_warmups_excluded(self):
        def clocks(child, index, argv):
            execution = child["steps"]["execute"]["native_result"]["execution"]
            production = argv[argv.index("--route") + 1] == "production-slot"
            wall, cpu = (20, 120) if production else (10, 40)
            if index < 4:
                wall = cpu = 1000000
            execution["clocks"]["body"].update(wall_ns=wall, process_cpu_ns=cpu)
            execution["clocks"]["setup"].update(wall_ns=999999, process_cpu_ns=999999)
            execution["clocks"]["teardown"].update(wall_ns=888888, process_cpu_ns=888888)
            self.sync_native_projections(child)

        code, receipt, calls, _output, _revalidated = self.run_case(warmup=2, repeat=16, edit_child=clocks)
        self.assertEqual(code, 0)
        self.assertEqual(len(calls), 36)
        summary = receipt["summary"]
        self.assertTrue(summary["comparison_ready"])
        self.assertEqual(summary["comparison"]["body_wall"]["arm_1_over_arm_0_median_ratio"], 2)
        self.assertEqual(summary["comparison"]["body_process_cpu"]["arm_1_over_arm_0_median_ratio"], 3)
        self.assertEqual(summary["arms"][0]["body_wall"]["max_ns"], 10)
        self.assertEqual(summary["arms"][1]["body_process_cpu"]["median_ns"], 120)

    def test_integer_median_and_nearest_rank_p95_are_explicit(self):
        summary = self.experiment.clock_summary([4, 1, 2, 3])
        self.assertEqual(summary, {"samples": 4, "min_ns": 1, "median_ns": 2, "p95_ns": 4, "max_ns": 4})
        self.assertEqual(self.experiment.clock_summary(list(range(1, 21)))["p95_ns"], 19)
        self.assertIsNone(self.experiment.clock_summary([])["median_ns"])

    def test_missing_or_rejected_qualification_launches_zero_children(self):
        receipt, calls, output, revalidated = self.assert_rejected(
            qualification_effect=self.paired.Rejected("authoritative qualification rejected"))
        self.assertFalse(receipt["qualification_revalidated"])
        self.assertEqual(calls, [])
        self.assertEqual(revalidated.call_count, 1)
        self.assertTrue((output / "result.json").is_file())
        self.assertTrue(all(row["status"] == "skipped" for row in receipt["iterations"]))
        with mock.patch.object(self.experiment, "load_dependencies") as loaded:
            with self.assertRaises(SystemExit):
                self.experiment.experiment(self.arguments(self.root / "missing-output",
                    qualification_dir=self.root / "missing-qualification"), self.runner)
            loaded.assert_not_called()

    def test_enabled_validation_hud_and_undeclared_renderer_settings_launch_zero_children(self):
        for name in self.experiment.VALIDATION_SETTINGS:
            receipt, calls, _output, revalidated = self.assert_rejected(environment={name: "1"})
            self.assertEqual(calls, [])
            self.assertEqual(revalidated.call_count, 0)
            self.assertIn("validation or HUD", receipt["failure"])
        self.assert_rejected(environment={"DXMT9_UNDECLARED_SETTING": "1"})
        code, receipt, _calls, _output, _revalidated = self.run_case(
            environment={name: "0" for name in self.experiment.VALIDATION_SETTINGS})
        self.assertEqual(code, 0)
        self.assertTrue(receipt["accepted"])

    def test_context_environment_contract_stage_or_requested_binary_mismatch_launches_zero_children(self):
        for key, value in (("environment", {"MTL_HUD_ENABLED": "0"}),
                           ("producer_contract", "none"), ("buffer_preservation_contract", "none"),
                           ("cpu_stage_environment", {})):
            context = self.context()
            context[key] = value
            _receipt, calls, _output, _revalidated = self.assert_rejected(context=context)
            self.assertEqual(calls, [])
        _receipt, calls, _output, _revalidated = self.assert_rejected(diagnostic=self.gpu_binary)
        self.assertEqual(calls, [])

    def test_final_reauthentication_invalidates_changed_context_preserving_successful_children(self):
        fields = ("manifest", "action", "stage_executables", "shared_runtime_images", "environment",
                  "cpu_stage_environment", "qualification_images", "plan_sha256")
        for field in fields:
            with self.subTest(field=field):
                initial = self.context()
                changed = copy.deepcopy(initial)
                if isinstance(changed[field], dict):
                    changed[field]["changed"] = True
                else:
                    changed[field] = "f" * 64
                receipt, calls, output, revalidated = self.assert_rejected(
                    context=initial, qualification_effect=[initial, changed])
                self.assertEqual(revalidated.call_count, 2)
                self.assertEqual(len(calls), 6)
                self.assertTrue(all(row["accepted"] for row in receipt["iterations"]))
                self.assertFalse(receipt["final_qualification_revalidated"])
                self.assertIn("reauthentication_failure", receipt)
                self.assertTrue(all((output / row["receipt"]).is_file() for row in receipt["iterations"]))

    def test_mutable_context_alias_cannot_rewrite_initial_authority_snapshot(self):
        context = self.context()
        def mutate(_child, index, _argv, _path):
            if index == 5:
                context["qualification_images"][str(self.qualification_receipt)]["sha256"] = "e" * 64
        receipt, _calls, _output, _revalidated = self.assert_rejected(context=context, after_child=mutate)
        self.assertIn("evidence changed", receipt["reauthentication_failure"])

    def test_final_revalidator_rejection_preserves_artifacts_and_disables_comparison(self):
        context = self.context()
        receipt, calls, output, revalidated = self.assert_rejected(context=context,
            qualification_effect=[context, self.paired.Rejected("owned input or module changed")], repeat=16)
        self.assertEqual(len(calls), 34)
        self.assertEqual(revalidated.call_count, 2)
        self.assertTrue(all((output / row["receipt"]).exists() for row in receipt["iterations"]))
        self.assertIn("owned input or module changed", receipt["reauthentication_failure"])

    def test_final_execution_receipt_reauthentication_rejects_late_mutation(self):
        first = []
        def mutate(_child, index, _argv, path):
            if index == 0:
                first.append(path)
            if index == 5:
                first[0].write_text(first[0].read_text() + "\n")
        receipt, calls, output, _revalidated = self.assert_rejected(after_child=mutate)
        self.assertEqual(len(calls), 6)
        self.assertTrue(receipt["final_qualification_revalidated"])
        self.assertFalse(receipt["executions_reauthenticated"])
        self.assertIn("child receipt changed", receipt["reauthentication_failure"])
        self.assertTrue((output / receipt["iterations"][0]["receipt"]).exists())

    def test_accepted_wrapper_cannot_hide_invalid_native_lifecycle_omission_clock_query_or_route(self):
        changes = (("execution_only", "source_row_reads_omitted", False),
                   ("consumer", "acknowledged", 0), ("query_projection", "replayed_polls", 0),
                   ("route_coverage", "complete", False), ("provenance", "raw_ack_coverage_complete", False))
        for group, name, value in changes:
            def corrupt(child, _index, _argv, group=group, name=name, value=value):
                child["steps"]["execute"]["native_result"]["execution"][group][name] = value
                self.sync_native_projections(child)
            receipt, calls, output, _revalidated = self.assert_rejected(edit_child=corrupt)
            self.assertEqual(len(calls), 1)
            self.assertEqual(receipt["iterations"][0]["status"], "rejected")
            self.assertTrue((output / receipt["iterations"][0]["receipt"]).is_file())
        for value in (True, -1, 1.5):
            def corrupt_clock(child, _index, _argv, value=value):
                child["steps"]["execute"]["native_result"]["execution"]["clocks"]["body"]["wall_ns"] = value
                self.sync_native_projections(child)
            self.assert_rejected(edit_child=corrupt_clock)

    def test_child_input_binary_contract_configuration_and_projection_binding_rejects(self):
        changes = (("bundle", "different-input"), ("diagnostic_binary", "different-native"),
                   ("output_dir", "different-child"), ("diagnostic", False),
                   ("producer_contract", "none"), ("buffer_preservation_contract", "none"),
                   ("environment", {"MTL_HUD_ENABLED": "0"}), ("effective_stage_environment", {}),
                   ("input_manifest", {}), ("input_components", {}), ("action_profile_validation", {}),
                   ("diagnostic_executable", {}), ("clocks", {}), ("execution", {}),
                   ("route_coverage", {}), ("loaded_images", []), ("iterations", True))
        for name, value in changes:
            def corrupt(child, _index, _argv, name=name, value=value):
                child[name] = value
            _receipt, calls, _output, _revalidated = self.assert_rejected(edit_child=corrupt)
            self.assertEqual(len(calls), 1)

    def test_native_timeout_missing_timeout_and_additional_native_step_reject(self):
        for kind in ("true", "missing", "extra-step"):
            def corrupt(child, _index, _argv, kind=kind):
                if kind == "true":
                    child["steps"]["execute"]["timed_out"] = True
                elif kind == "missing":
                    del child["steps"]["execute"]["timed_out"]
                else:
                    child["steps"]["second-execution"] = copy.deepcopy(child["steps"]["execute"])
            receipt, calls, _output, _revalidated = self.assert_rejected(edit_child=corrupt)
            self.assertEqual(len(calls), 1)
            self.assertIn("one actual native process", receipt["failure"])

    def test_selfconsistent_reduced_original_work_cannot_pass_complete_native_flags(self):
        cases = ("events-commands", "draws", "resources", "query", "provenance", "plan")
        for kind in cases:
            def reduce_work(child, _index, _argv, kind=kind):
                native = child["steps"]["execute"]["native_result"]
                execution = native["execution"]
                if kind == "events-commands":
                    execution["counters"].update(events=7, commands=2)
                elif kind == "draws":
                    execution["counters"]["draws"] = 1
                    execution["execution_only"]["draws"] = 1
                    execution["route_coverage"]["compatibility_draws"] = 1
                elif kind == "resources":
                    execution["counters"].update(resources_created=0, resources_released=0)
                elif kind == "query":
                    execution["query_projection"].update(present=False, definitions=0, issues=0,
                        checkpoints=0, recorded_polls=0, replayed_polls=0, rows=0)
                elif kind == "provenance":
                    execution["provenance"].update(replayed_events=2, dispatched_records=2)
                else:
                    native["plan_counts"].update(events=7, records=2)
                self.sync_native_projections(child)
                # These mutations remain locally conserving and complete to
                # the true execution-only validator; the independent paired
                # original-work join must be what catches lost original work.
                self.execute.validate_native(native, argparse.Namespace(route="compatibility",
                    expected_plan_digest=self.fixture.plan, producer_contract=self.experiment.PRODUCER_CONTRACT,
                    buffer_preservation_contract=self.experiment.PRESERVATION_CONTRACT),
                    self.manifest, self.action, native["max_wall_ms"])
            with self.subTest(kind=kind):
                receipt, calls, _output, _revalidated = self.assert_rejected(edit_child=reduce_work)
                self.assertEqual(len(calls), 1)
                self.assertIn("original-work count differs", receipt["failure"])

    def test_cheap_original_mutation_and_texture_effect_counts_are_not_diagnostic_omissions(self):
        for name in ("observed_mutations", "observed_texture_uploads"):
            def corrupt(child, _index, _argv, name=name):
                child["steps"]["execute"]["native_result"]["execution"]["counters"][name] = 1
                self.sync_native_projections(child)
            receipt, _calls, _output, _revalidated = self.assert_rejected(edit_child=corrupt)
            self.assertIn("original-work count differs", receipt["failure"])

    def test_physical_candidate_rebatching_may_differ_from_original_source_count(self):
        def rebatch(child, _index, _argv):
            execution = child["steps"]["execute"]["native_result"]["execution"]
            execution["execution_only"].update(sources=2, submitted=2, acknowledged=2, retired=2)
            execution["consumer"].update(observed=2, submitted=2, acknowledged=2, retired=2,
                                         receipts=2, lifecycle_actions=8)
            second = copy.deepcopy(execution["consumer_receipts"][0])
            second["identity"].update(sequence=2, source_index=1, storage_first_page=1)
            execution["consumer_receipts"].append(second)
            self.sync_native_projections(child)
        code, receipt, _calls, _output, _revalidated = self.run_case(edit_child=rebatch)
        self.assertEqual(code, 0)
        self.assertTrue(receipt["accepted"])
        self.assertEqual(receipt["qualified_original_work"]["plan_counts"]["sources"], 1)
        self.assertEqual(receipt["iterations"][0]["execution_only"]["sources"], 2)

    def test_original_cpu_receipt_missing_fingerprint_or_tampered_bytes_launches_zero_children(self):
        for kind in ("missing-fingerprint", "changed-bytes"):
            context = self.context()
            path = self.qualified_children[0]
            original = path.read_bytes()
            if kind == "missing-fingerprint":
                del context["qualification_images"][str(path)]
            else:
                path.write_bytes(original + b"\n")
            try:
                _receipt, calls, _output, _revalidated = self.assert_rejected(context=context)
                self.assertEqual(calls, [])
            finally:
                path.write_bytes(original)

    def test_loaded_module_fingerprint_missing_module_and_different_set_reject(self):
        for kind in ("fingerprint", "missing", "extra"):
            def corrupt(child, _index, _argv, kind=kind):
                images = child["steps"]["execute"]["native_result"]["loaded_images"]
                if kind == "fingerprint":
                    images[1]["sha256"] = "f" * 64
                elif kind == "missing":
                    images.pop()
                else:
                    images.append({"path": str(self.gpu_binary), **fingerprint(self.gpu_binary)})
                self.sync_native_projections(child)
            self.assert_rejected(edit_child=corrupt)

    def test_failed_warmup_preserves_raw_receipt_native_logs_and_unstarted_rows(self):
        receipt, calls, output, _revalidated = self.assert_rejected(outer_returncode=4)
        self.assertEqual(len(calls), 1)
        first = receipt["iterations"][0]
        self.assertEqual(first["phase"], "warmup")
        self.assertFalse(receipt["measurement_started"])
        self.assertEqual(first["status"], "rejected")
        self.assertTrue(all(row["status"] == "skipped" for row in receipt["iterations"][1:]))
        self.assertIn("retained native", (output / first["run_id"] / "execute.stderr").read_text())
        self.assertIn("retained launcher", (output / f"{first['run_id']}.stderr").read_text())
        self.assertTrue((output / first["receipt"]).exists())

    def test_missing_duplicate_nonfinite_oversized_and_symlink_receipt_rejects(self):
        for kind in ("missing", "duplicate", "nonfinite", "oversized", "symlink"):
            with self.subTest(kind=kind):
                receipt, calls, output, _revalidated = self.assert_rejected(response_kind=kind)
                self.assertEqual(len(calls), 1)
                self.assertEqual(receipt["iterations"][0]["status"], "rejected")
                self.assertTrue((output / receipt["iterations"][0]["run_id"]).is_dir())

    def test_output_and_qualification_path_ownership_checks(self):
        alias = self.root / "qualification-alias"
        alias.symlink_to(self.qualification, target_is_directory=True)
        for values in ({"qualification_dir": alias}, {"output_dir": self.bundle / "experiment"},
                       {"output_dir": self.qualification}, {"output_dir": alias}):
            args = self.arguments(self.root / "owned-output", **values)
            with self.assertRaises(SystemExit):
                self.experiment.experiment(args, self.runner)


if __name__ == "__main__":
    unittest.main()
