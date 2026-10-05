#!/usr/bin/env python3
"""Cross-qualification regressions; all fresh execution children are mocked."""

from __future__ import annotations

import argparse
import contextlib
import copy
import importlib.util
import io
import json
import pathlib
import shutil
import subprocess
import unittest
from unittest import mock


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class CapacityExperimentTest(unittest.TestCase):
    def setUp(self):
        check = pathlib.Path(__file__).resolve().parent
        fixtures = load(check / "test_render_tape_v4_experiment.py", "capacity_experiment_fixture")
        self.base = fixtures.RenderTapeV4ExperimentTest()
        self.base.setUp()
        self.addCleanup(self.base.doCleanups)
        self.fingerprint = fixtures.fingerprint
        self.root, self.runner = self.base.root, self.base.runner
        self.experiment, self.paired, self.execute = self.base.experiment, self.base.paired, self.base.execute
        self.capacity = load(check.parent / "tools/render_tape_v4_capacity_experiment.py", "capacity_experiment_tested")
        self.directories = (self.base.qualification, self.root / "qualification-b")
        self.directories[1].mkdir()
        shutil.copytree(self.base.bundle, self.directories[1] / "input")
        self.contexts = [self.base.context(), copy.deepcopy(self.base.context())]
        self.contexts[1]["owned_input"] = self.directories[1] / "input"
        self.case = 0
        self.write_qualification(0)
        self.set_policy(1, self.paired.capacity_policy(1024 * 1024, 64 * 1024 * 1024))

    def write_qualification(self, arm, edit_native=None):
        context, directory = self.contexts[arm], self.directories[arm]
        policies = context["capacity_policies"]
        limit = context["replay_policies"]["compatibility"]["draw_command_limit"]
        replays = self.paired.route_replay_policies(argparse.Namespace(draw_command_limit=limit), policies)
        context["replay_policies"] = replays
        context["cpu_stage_environments"] = self.paired.stage_environments(policies, replays)["cpu"]
        context["receipt"].update(capacity_policies=copy.deepcopy(policies),
            replay_policies=copy.deepcopy(replays), expected_stage_environment=self.paired.stage_environments(policies, replays))
        paths = [directory / "result.json"]
        paths[0].write_text(json.dumps(context["receipt"]))
        for route in self.paired.ROUTES:
            child_dir = directory / f"cpu-{route}"
            child_dir.mkdir(exist_ok=True)
            native = self.base.fixture.native(self.experiment.PRODUCER_CONTRACT, route)
            native["capacity_policy"] = copy.deepcopy(policies[route])
            native["replay_policy"] = copy.deepcopy(replays[route])
            native["effective_stage_environment"] = copy.deepcopy(context["cpu_stage_environments"][route])
            # The revalidator is the mocked authority seam. Preserve genuine
            # fixture original-item rows for the real cross-stream comparison,
            # while retaining the cheap original-work counts of the CPU fixture.
            semantic = self.base.fixture.fixture.native(self.experiment.PRODUCER_CONTRACT, route)
            native["execution"]["logical_semantics"] = semantic["execution"]["logical_semantics"]
            if edit_native:
                edit_native(native)
            child = {"capacity_policy": copy.deepcopy(policies[route]),
                     "replay_policy": copy.deepcopy(replays[route]),
                     "expected_stage_environment": copy.deepcopy(context["cpu_stage_environments"][route]),
                     "steps": {"semantics": {"status": "exited", "returncode": 0,
                                              "timed_out": False, "native_result": native}}}
            path = child_dir / "result.json"
            path.write_text(json.dumps(child))
            paths.append(path)
        context["qualification_images"] = {str(path): self.fingerprint(path) for path in paths}

    def set_policy(self, arm, policy):
        self.contexts[arm]["capacity_policies"]["production-slot"] = policy
        self.write_qualification(arm)

    def set_grouping(self, arm, limit):
        self.contexts[arm]["capacity_policies"] = self.paired.route_capacity_policies(argparse.Namespace())
        self.contexts[arm]["replay_policies"] = self.paired.route_replay_policies(
            argparse.Namespace(draw_command_limit=limit), self.contexts[arm]["capacity_policies"])
        self.write_qualification(arm)

    def args(self, output, **changes):
        values = {"qualification_a": self.directories[0], "qualification_b": self.directories[1],
                  "output_dir": output, "diagnostic": None, "route": "production-slot", "warmup": 2, "repeat": 16,
                  "timeout_seconds": 120, "producer_contract": self.experiment.PRODUCER_CONTRACT,
                  "buffer_preservation_contract": self.experiment.PRESERVATION_CONTRACT}
        values.update(changes)
        return argparse.Namespace(**values)

    def run_case(self, *, edit_child=None, after_child=None, qualification_effect=None,
                 outer_returncode=0, environment=None, **changes):
        self.case += 1
        output = self.root / f"capacity-result-{self.case}"
        args = self.args(output, **changes)
        selected = (pathlib.Path(args.qualification_a).resolve(), pathlib.Path(args.qualification_b).resolve())
        lookup = dict(zip(self.directories, self.contexts))
        shared_input = lookup[selected[0]]["owned_input"]
        calls = []

        def revalidate(path, *_args, **_kwargs):
            return lookup[path]

        def child(argv, **kwargs):
            self.assertTrue(revalidated.called)
            self.assertNotIn("env", kwargs)
            self.assertEqual(pathlib.Path(argv[0]).name, "run_python.sh")
            self.assertEqual(pathlib.Path(argv[1]).name, "run_render_tape_cpu_v4_execute.py")
            self.assertEqual(argv[argv.index("--route") + 1], args.route)
            self.assertEqual(argv[argv.index("--bundle") + 1], str(shared_input))
            directory = pathlib.Path(argv[argv.index("--output-dir") + 1])
            directory.mkdir()
            arm = int(directory.name.rsplit("-", 1)[1])
            context = {**lookup[selected[arm]], "owned_input": shared_input}
            policy = context["capacity_policies"][args.route]
            self.assertEqual(int(argv[argv.index("--slot-headroom-bytes") + 1]), policy["slot_headroom_bytes"])
            self.assertEqual(int(argv[argv.index("--aggregate-headroom-bytes") + 1]), policy["aggregate_headroom_bytes"])
            self.assertEqual(int(argv[argv.index("--draw-command-limit") + 1]),
                             context["replay_policies"][args.route]["draw_command_limit"])
            receipt = self.base.child_receipt(context, args.route, directory)
            calls.append(list(argv))
            if edit_child:
                edit_child(receipt, len(calls) - 1, arm)
            path = directory / "result.json"
            path.write_text(json.dumps(receipt))
            (directory / "execute.stderr").write_text("retained native capacity diagnostics\n")
            kwargs["stdout"].write(json.dumps({"accepted": True, "receipt": str(path)}).encode())
            kwargs["stderr"].write(b"retained launcher diagnostics\n")
            if after_child:
                after_child(len(calls) - 1, arm, path)
            return subprocess.CompletedProcess(argv, outer_returncode)

        effect = qualification_effect if qualification_effect is not None else revalidate
        with mock.patch.dict(self.capacity.os.environ, dict(environment or {}), clear=True), \
             mock.patch.object(self.capacity, "load_dependencies", return_value=(self.experiment, self.paired, self.execute)), \
             mock.patch.object(self.paired, "revalidate_paired_qualification", side_effect=effect) as revalidated, \
             mock.patch.object(self.execute.auth.subprocess, "run", side_effect=child), \
             mock.patch.object(self.runner, "copy_input_owned") as copied, \
             contextlib.redirect_stdout(io.StringIO()):
            code = self.capacity.capacity_experiment(args, self.runner)
        copied.assert_not_called()
        return code, json.loads((output / "result.json").read_text()), calls, output, revalidated

    def assert_rejected(self, **kwargs):
        code, receipt, calls, output, revalidated = self.run_case(**kwargs)
        self.assertEqual(code, 1)
        self.assertEqual(receipt["status"], "rejected")
        self.assertFalse(receipt["accepted"])
        self.assertFalse(receipt["measurement_complete"])
        self.assertFalse(receipt["summary"]["comparison_ready"])
        self.assertIsNone(receipt["summary"]["comparison"])
        return receipt, calls, output, revalidated

    def test_exact_vs_one_and_five_mib_candidates_separate_body_clocks_and_exclude_warmups(self):
        for slot in (1024 * 1024, 5 * 1024 * 1024):
            self.set_policy(1, self.paired.capacity_policy(slot, 64 * 1024 * 1024))
            def clocks(child, index, arm):
                native = child["steps"]["execute"]["native_result"]
                body = native["execution"]["clocks"]["body"]
                body.update(wall_ns=10 if arm == 0 else 20, process_cpu_ns=40 if arm == 0 else 120)
                if index < 4:
                    body.update(wall_ns=999999, process_cpu_ns=999999)
                self.base.sync_native_projections(child)
            code, receipt, calls, output, revalidated = self.run_case(edit_child=clocks)
            self.assertEqual(code, 0)
            self.assertEqual(len(calls), 36)
            self.assertEqual(revalidated.call_count, 4)
            self.assertTrue(receipt["candidate_logical_streams_equal"])
            self.assertTrue(receipt["summary"]["comparison_ready"])
            self.assertEqual(receipt["summary"]["comparison"]["body_wall"]["arm_1_over_arm_0_median_ratio"], 2)
            self.assertEqual(receipt["summary"]["comparison"]["body_process_cpu"]["arm_1_over_arm_0_median_ratio"], 3)
            self.assertEqual(receipt["summary"]["arms"][0]["body_wall"]["max_ns"], 10)
            self.assertTrue(all((output / row["receipt"]).is_file() for row in receipt["iterations"]))
            for name in self.experiment.FALSE_CLAIMS:
                self.assertIs(receipt[name], False)
            self.assertFalse(receipt["capacity_matrix_complete"])
            for name in ("fps", "gpu_time", "policy_ranking"):
                self.assertIsNone(receipt["summary"][name])

    def test_reverse_directory_order_and_aa_keep_bound_arm_policy_identity(self):
        code, receipt, _calls, _output, _revalidated = self.run_case(
            qualification_a=self.directories[1], qualification_b=self.directories[0])
        self.assertEqual(code, 0)
        self.assertEqual(receipt["arms"][0]["capacity_policy"]["slot_headroom_bytes"], 1024 * 1024)
        self.assertEqual(receipt["shared_execution_input"], str(self.contexts[1]["owned_input"]))
        self.set_policy(0, self.paired.capacity_policy(5 * 1024 * 1024, 64 * 1024 * 1024))
        code, receipt, calls, _output, _revalidated = self.run_case(qualification_b=self.directories[0])
        self.assertEqual(code, 0)
        self.assertTrue(receipt["same_qualification_control"])
        self.assertTrue(receipt["same_capacity_control"])
        self.assertEqual({row["arm_id"] for row in receipt["iterations"]}, {"arm-0", "arm-1"})
        self.assertEqual(len({row["run_id"] for row in receipt["iterations"]}), len(calls))

    def test_compatibility_exact_vs_l3_l64_and_reverse_compare_full_policy_not_capacity_only(self):
        for limit in (3, 64):
            self.set_grouping(1, limit)
            for reverse in (False, True):
                changes = {"route": "compatibility"}
                if reverse:
                    changes.update(qualification_a=self.directories[1], qualification_b=self.directories[0])
                code, receipt, calls, _output, revalidated = self.run_case(**changes)
                self.assertEqual(code, 0)
                self.assertEqual(len(calls), 36)
                self.assertEqual(revalidated.call_count, 4)
                self.assertTrue(receipt["summary"]["comparison_ready"])
                self.assertTrue(receipt["same_capacity_control"])
                self.assertFalse(receipt["same_replay_policy_control"])
                self.assertIn("compatibility-replay-policy", receipt["replay_comparison_scope"])
                expected = [limit, 0] if reverse else [0, limit]
                self.assertEqual([arm["replay_policy"]["draw_command_limit"] for arm in receipt["arms"]], expected)
                for row in receipt["iterations"]:
                    self.assertEqual(row["replay_policy"]["draw_command_limit"], expected[row["arm_index"]])
                    self.assertEqual(row["replay_policy"]["route"], "compatibility")

    def test_grouped_compatibility_aa_keeps_distinct_arms(self):
        self.set_grouping(0, 3)
        code, receipt, calls, _output, _revalidated = self.run_case(
            route="compatibility", qualification_b=self.directories[0])
        self.assertEqual(code, 0)
        self.assertTrue(receipt["same_replay_policy_control"])
        self.assertEqual({row["arm_id"] for row in receipt["iterations"]}, {"arm-0", "arm-1"})
        self.assertEqual(len({row["run_id"] for row in receipt["iterations"]}), len(calls))

    def test_grouping_full_identity_and_positive_axes_conflicts_reject_before_children(self):
        saved = copy.deepcopy(self.contexts[1])
        for kind in ("production-group", "mixed-axes", "wrong-schema", "missing", "bool-limit"):
            self.contexts[1] = copy.deepcopy(saved)
            full = self.contexts[1]["replay_policies"]
            if kind == "production-group":
                full["production-slot"]["draw_command_limit"] = 3
            elif kind == "mixed-axes":
                full["compatibility"]["draw_command_limit"] = 3
            elif kind == "wrong-schema":
                full["compatibility"]["schema"] = "older-unbound-policy"
            elif kind == "missing":
                del self.contexts[1]["replay_policies"]
            else:
                full["compatibility"]["draw_command_limit"] = False
            _receipt, calls, _output, _revalidated = self.assert_rejected(route="compatibility")
            self.assertEqual(calls, [])
        self.contexts[1] = saved

    def test_grouped_child_full_policy_mismatch_and_final_group_rebinding_reject(self):
        self.set_grouping(1, 3)
        def corrupt(child, _index, _arm):
            child["replay_policy"]["draw_command_limit"] = 3
        _receipt, calls, _output, _revalidated = self.assert_rejected(route="compatibility", edit_child=corrupt)
        self.assertEqual(len(calls), 1)
        def change_group(index, _arm, _path):
            if index == 35:
                self.set_grouping(1, 64)
        receipt, calls, output, _revalidated = self.assert_rejected(route="compatibility", after_child=change_group)
        self.assertEqual(len(calls), 36)
        self.assertTrue(all(row["accepted"] for row in receipt["iterations"]))
        self.assertFalse(receipt["final_qualifications_revalidated"])
        self.assertTrue(all((output / row["receipt"]).is_file() for row in receipt["iterations"]))

    def test_configurable_warmups_and_bounded_maximum_rounds(self):
        code, receipt, calls, _output, _revalidated = self.run_case(warmup=32, repeat=32)
        self.assertEqual(code, 0)
        self.assertEqual(len(calls), 128)
        self.assertEqual(receipt["summary"]["arms"][0]["body_wall"]["samples"], 32)
        self.assertEqual(receipt["summary"]["arms"][0]["warmups_accepted"], 32)

    def test_counts_and_final_digest_alone_cannot_replace_cross_candidate_item_equality(self):
        def change_item(native):
            native["execution"]["logical_semantics"]["receipts"][1]["digest"] = "1111111111111111"
        self.write_qualification(1, change_item)
        receipt, calls, _output, revalidated = self.assert_rejected()
        self.assertEqual(calls, [])
        self.assertEqual(revalidated.call_count, 2)
        self.assertIn("original item mismatch", receipt["failure"])

    def test_mismatched_original_data_work_images_environment_or_contracts_launch_zero_children(self):
        cases = ("manifest", "action", "plan_sha256", "stage_executables", "shared_runtime_images", "environment",
                 "producer_contract", "buffer_preservation_contract")
        saved = copy.deepcopy(self.contexts[1])
        for field in cases:
            self.contexts[1] = copy.deepcopy(saved)
            value = self.contexts[1][field]
            if isinstance(value, dict):
                value["changed"] = True
            else:
                self.contexts[1][field] = "changed"
            with self.subTest(field=field):
                _receipt, calls, _output, _revalidated = self.assert_rejected()
                self.assertEqual(calls, [])
        self.contexts[1] = saved
        self.contexts[1]["receipt"]["canonical_execution_counts"]["counters"]["draws"] -= 1
        _receipt, calls, _output, _revalidated = self.assert_rejected()
        self.assertEqual(calls, [])

    def test_missing_pair_authority_and_validation_or_hud_reject_before_children(self):
        for arm in (0, 1):
            def revalidate(path, *_args, arm=arm, **_kwargs):
                if path == self.directories[arm]:
                    raise self.paired.Rejected("independent original GPU/pair authority rejected")
                return self.contexts[self.directories.index(path)]
            _receipt, calls, _output, _revalidated = self.assert_rejected(qualification_effect=revalidate)
            self.assertEqual(calls, [])
        for name in self.experiment.VALIDATION_SETTINGS:
            _receipt, calls, _output, revalidated = self.assert_rejected(environment={name: "1"})
            self.assertEqual(calls, [])
            self.assertEqual(revalidated.call_count, 0)

    def test_wrong_child_arm_policy_and_selfconsistent_reduced_work_reject_and_retain(self):
        for kind in ("policy", "counts", "timeout"):
            def corrupt(child, _index, _arm, kind=kind):
                native = child["steps"]["execute"]["native_result"]
                if kind == "policy":
                    child["capacity_policy"] = self.contexts[1]["capacity_policies"]["production-slot"]
                elif kind == "counts":
                    native["execution"]["counters"].update(events=7, commands=2)
                    self.base.sync_native_projections(child)
                else:
                    child["steps"]["execute"]["timed_out"] = True
            receipt, calls, output, _revalidated = self.assert_rejected(edit_child=corrupt)
            self.assertEqual(len(calls), 1)
            self.assertEqual(receipt["iterations"][0]["phase"], "warmup")
            self.assertTrue(all(row["status"] == "skipped" for row in receipt["iterations"][1:]))
            self.assertTrue((output / receipt["iterations"][0]["receipt"]).exists())

    def test_final_policy_rebinding_invalidates_complete_samples_without_discarding_artifacts(self):
        def change_policy(index, _arm, _path):
            if index == 35:
                self.set_policy(1, self.paired.capacity_policy(5 * 1024 * 1024, 64 * 1024 * 1024))
        receipt, calls, output, revalidated = self.assert_rejected(after_child=change_policy)
        self.assertEqual(len(calls), 36)
        self.assertEqual(revalidated.call_count, 4)
        self.assertTrue(all(row["accepted"] for row in receipt["iterations"]))
        self.assertFalse(receipt["final_qualifications_revalidated"])
        self.assertIn("reauthentication_failure", receipt)
        self.assertTrue(all((output / row["receipt"]).is_file() for row in receipt["iterations"]))

    def test_final_shared_input_or_qualification_receipt_mutation_rejects(self):
        def change_bytes(index, _arm, _path):
            if index == 35:
                path = self.directories[0] / "cpu-production-slot/result.json"
                path.write_text(path.read_text() + "\n")
        receipt, calls, _output, _revalidated = self.assert_rejected(after_child=change_bytes)
        self.assertEqual(len(calls), 36)
        self.assertIn("reauthentication_failure", receipt)

    def test_failed_warmup_retains_raw_launcher_native_and_receipt_artifacts(self):
        receipt, calls, output, _revalidated = self.assert_rejected(outer_returncode=4)
        self.assertEqual(len(calls), 1)
        first = receipt["iterations"][0]
        self.assertFalse(receipt["measurement_started"])
        self.assertIn("retained launcher", (output / f"{first['run_id']}.stderr").read_text())
        self.assertIn("retained native", (output / first["run_id"] / "execute.stderr").read_text())
        self.assertTrue((output / first["receipt"]).is_file())

    def test_invalid_controls_and_unsafe_paths_reject_before_effects(self):
        controls = ({"warmup": -1}, {"warmup": 0}, {"warmup": 1}, {"warmup": True},
                    {"warmup": 33, "repeat": 32}, {"repeat": 15}, {"repeat": 33}, {"repeat": True},
                    {"timeout_seconds": float("nan")}, {"timeout_seconds": True}, {"timeout_seconds": 7201},
                    {"producer_contract": "none"}, {"buffer_preservation_contract": "none"},
                    {"qualification_b": self.root / "missing"}, {"output_dir": self.directories[1] / "inside"},
                    {"route": "unknown"})
        with mock.patch.object(self.capacity, "load_dependencies", return_value=(self.experiment, self.paired, self.execute)), \
             mock.patch.object(self.paired, "revalidate_paired_qualification") as revalidated:
            for control in controls:
                with self.subTest(control=control), self.assertRaises(SystemExit):
                    self.capacity.capacity_experiment(self.args(self.root / "invalid-capacity", **control), self.runner)
            revalidated.assert_not_called()
        args = self.capacity.parser().parse_args(["--qualification-a", str(self.directories[0]),
            "--qualification-b", str(self.directories[1]), "--output-dir", str(self.root / "parsed"),
            "--producer-contract", self.experiment.PRODUCER_CONTRACT,
            "--buffer-preservation-contract", self.experiment.PRESERVATION_CONTRACT])
        self.assertEqual((args.warmup, args.repeat, args.timeout_seconds), (2, 16, 3600))
        self.assertEqual(args.route, "production-slot")


if __name__ == "__main__":
    unittest.main()
