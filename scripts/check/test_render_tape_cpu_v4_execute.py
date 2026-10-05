#!/usr/bin/env python3
"""Observer-light controlled CPU launcher tests; no native child is executed."""

from __future__ import annotations

import contextlib
import copy
import hashlib
import importlib.util
import io
import json
import pathlib
import subprocess
import sys
import unittest
from unittest import mock


def load_sibling(name: str, module_name: str):
    path = pathlib.Path(__file__).with_name(name)
    spec = importlib.util.spec_from_file_location(module_name, path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class ControlledCpuExecuteTest(unittest.TestCase):
    def setUp(self):
        # Reuse the authenticated immutable bundle fixture, not its semantic
        # validator or tests. Only this suite's mocked child is launched.
        fixtures = load_sibling("test_render_tape_cpu_v4_semantics.py", "cpu_execute_bundle_fixture")
        self.fixture = fixtures.ControlledCpuSemanticsTest()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.launcher = load_sibling("run_render_tape_cpu_v4_execute.py", "cpu_execute_tested")
        self.root, self.bundle = self.fixture.root, self.fixture.bundle
        self.binary, self.shared = self.fixture.binary, self.fixture.shared
        self.plan, self.manifest = self.fixture.plan, self.fixture.manifest
        self.modules = []
        for name in ("libdxmt9_native.dylib", "winemetal_dxmt9.so"):
            path = self.root / name
            path.write_bytes(f"mock authenticated loaded runtime: {name}\n".encode())
            self.modules.append(path)
        self.case_count = 0

    def native(self, contract="none", route="compatibility", headroom=None, draw_command_limit=0):
        native = self.fixture.native(contract, route, headroom, draw_command_limit)
        native.update(schema=self.launcher.NATIVE_SCHEMA, mode="controlled-cpu-v4-execute",
                      phase="execution-only-complete",
                      effective_stage_environment=self.launcher.expected_stage_environment(
                          native["capacity_policy"], draw_command_limit))
        for path in self.modules:
            raw = path.read_bytes()
            native["loaded_images"].append({"path": str(path), "bytes": len(raw),
                                            "sha256": hashlib.sha256(raw).hexdigest()})
        execution = native["execution"]
        execution.update(execution_mode="execution-only", metadata_inventory_complete=False,
                         sources=[], consumer_receipts=[{
                             "identity": {"queue": 201, "capture_token": 202, "capture_epoch": 203,
                                          "sequence": 1, "source_index": 0, "source_generation": 1,
                                          "storage_first_page": 0, "storage_page_count": 1,
                                          "storage_generation": 1},
                             "submitted": True, "acknowledged": True, "retired": True,
                             "cleanup_complete": True}],
                         execution_only={"requested": True, "complete": True,
                                         "lifecycle_coverage_complete": True,
                                         **{name: True for name in self.launcher.OMISSIONS},
                                         "sources": 1, "commands": 2, "draws": 2,
                                         "submitted": 1, "acknowledged": 1, "retired": 1},
                         clocks={"clock_domain": "steady-wall-and-process-cpu",
                                 "process_cpu_clock": "CLOCK_PROCESS_CPUTIME_ID",
                                 "scope": "executor-run-only",
                                 "body_scope": "after-setup-and-arm-through-consumer-join",
                                 "body_excludes_setup_and_teardown": True,
                                 **{name: {"started": True, "valid": True, "complete": True,
                                           "wall_ns": value, "process_cpu_ns": value * 2}
                                    for name, value in (("setup", 17), ("body", 29), ("teardown", 11))}},
                         expected_buffer_budget={"limit_bytes": 0, "charged_bytes": 0,
                                                 "high_water_bytes": 0, "physical_byte_proof": False},
                         initial_setup={"owners": 0, "seeds": 0, "aliases": 0, "created": 0,
                                        "prepared": 0, "activated": 0, "staged_seeds": 0,
                                        "consumed_seeds": 0, "released_hidden": 0, "seed_bytes": 0,
                                        "allocation_bytes": 0, "host_checkpoint_joined": True},
                         buffer_version_projection={"scope": "producer-instantaneous-current-allocation-metadata",
                                                    "logical_epoch_authenticated_by_revision": False,
                                                    "backing_retained": False, "old_ring_epoch_proven": False,
                                                    "physical_byte_proof": False, "rows": []})
        logical = execution["logical_semantics"]
        logical.update({name: False for name in (
            "requested", "admitted", "accepted", "complete", "legal_candidate_nooverwrite_contract",
            "conditional_on_legal_nooverwrite", "logical_stream_complete")})
        logical.update({name: 0 for name in (
            "sources", "commands", "draws", "original_items", "source_rows_required",
            "source_rows_qualified", "logical_buffer_hash_bytes")})
        logical.update(receipts=[], digest="0000000000000000")
        execution["consumer"].update(phase=3, lifecycle_actions=4,
                                     active_identity={name: 0 for name in self.launcher.IDENTITY_FIELDS})
        execution["counters"].update(events=8, commands=3, observed_mutations=0,
                                     observed_texture_uploads=0, record_contexts=0, setup_submissions=0,
                                     setup_acknowledgements=0, buffer_versions_observed=0,
                                     buffer_versions_unavailable=0)
        execution["provenance"].update(replayed_events=3, dispatched_records=3, acknowledged_raws=1,
                                       source_obligations_registered=1, source_obligations_qualified=1)
        execution["query_projection"].update(present=True, definitions=1, issues=1, checkpoints=0, rows=1)
        execution["source_row_observation"].update(qualified_draws=0, not_qualified_draws=0)
        native["plan_counts"] = {"events": 8, "records": 3, "sources": 1}
        return native

    def argv(self, output, contract="none", route="compatibility", headroom=None, draw_command_limit=None):
        result = self.fixture.argv(output, contract, route=route, headroom=headroom,
                                   draw_command_limit=draw_command_limit)
        result[0] = "execute"
        return result

    def run_case(self, response=None, returncode=0, contract="none", route="compatibility", mutate=None, headroom=None,
                 draw_command_limit=None):
        self.case_count += 1
        output = self.root / f"execute-result-{self.case_count}"
        response = self.native(contract, route, headroom,
            0 if draw_command_limit is None else draw_command_limit) if response is None else response
        calls = []

        def child(argv, **kwargs):
            calls.append(argv)
            self.assertNotIn("env", kwargs)
            self.assertEqual(argv[1], "--controlled-cpu-v4-execute")
            self.assertGreater(kwargs["timeout"], 0)
            self.assertLessEqual(kwargs["timeout"], 10)
            kwargs["stderr"].write(b"mock execution diagnostics\n")
            if isinstance(response, BaseException):
                kwargs["stdout"].write(b"partial child output")
                raise response
            native = copy.deepcopy(response)
            if isinstance(native, dict):
                native["max_wall_ms"] = int(argv[argv.index("--max-wall-ms") + 1])
                raw = json.dumps(native).encode()
            else:
                raw = native
            kwargs["stdout"].write(raw)
            if mutate:
                mutate()
            return subprocess.CompletedProcess(argv, returncode)

        with mock.patch.object(sys, "argv", self.argv(output, contract, route, headroom, draw_command_limit)), \
             contextlib.redirect_stdout(io.StringIO()), \
             mock.patch.object(self.launcher, "load_bundle_tool", return_value=self.shared), \
             mock.patch.object(self.shared, "load_bundle", wraps=self.shared.load_bundle) as loaded, \
             mock.patch.object(self.launcher.auth.subprocess, "run", side_effect=child):
            code = self.launcher.main()
        receipt = json.loads((output / "result.json").read_text())
        return code, receipt, calls, output, loaded

    def assert_rejected(self, native):
        code, receipt, calls, _output, _loaded = self.run_case(native)
        self.assertEqual(code, 1)
        self.assertFalse(receipt["accepted"])
        self.assertFalse(receipt["execution_complete"])
        self.assertEqual(receipt["status"], "rejected")
        self.assertEqual(len(calls), 1)
        self.assertIn("native_result", receipt["steps"]["execute"])
        for name in self.launcher.FALSE_CLAIMS:
            self.assertIs(receipt[name], False)
        return receipt

    def test_one_fresh_child_preserves_actual_scoped_clocks_without_qualification(self):
        code, receipt, calls, output, loaded = self.run_case()
        self.assertEqual(code, 0)
        self.assertTrue(receipt["accepted"])
        self.assertTrue(receipt["execution_complete"])
        self.assertEqual(receipt["schema"], self.launcher.RECEIPT_SCHEMA)
        self.assertEqual(len(calls), 1)
        self.assertEqual(loaded.call_args_list, [mock.call(self.bundle, allow_identity_v4=True)] * 2)
        for name in ("manifest_authenticated", "bundle_reauthenticated", "binary_reauthenticated", "loaded_images_stable"):
            self.assertIs(receipt[name], True)
        for name in self.launcher.FALSE_CLAIMS:
            self.assertIs(receipt[name], False)
        call = calls[0]
        for option in ("--controlled-cpu-v4-semantics", "--controlled-cpu-v4-inventory", "--repeat", "--inspect-plan"):
            self.assertNotIn(option, call)
        self.assertEqual(call[call.index("--bundle") + 1], str(self.bundle))
        self.assertEqual(call[call.index("--expected-plan-digest") + 1], self.plan)
        self.assertEqual(receipt["clocks"]["body"]["wall_ns"], 29)
        self.assertEqual(receipt["clocks"]["body"]["process_cpu_ns"], 58)
        self.assertEqual(receipt["effective_stage_environment"], self.launcher.STAGE_ENVIRONMENT)
        self.assertEqual(receipt["expected_stage_environment"], self.launcher.STAGE_ENVIRONMENT)
        self.assertEqual(receipt["capacity_policy"], {"slot_headroom_bytes": 0, "aggregate_headroom_bytes": 0})
        self.assertTrue(receipt["capacity_policy_valid"])
        self.assertTrue(receipt["stage_environment_valid"])
        self.assertTrue(receipt["replay_policy_valid"])
        self.assertEqual(receipt["replay_policy"], {"schema": self.launcher.REPLAY_POLICY_SCHEMA,
            "route": "compatibility", "draw_command_limit": 0, **receipt["capacity_policy"]})
        self.assertEqual(call[call.index("--draw-command-limit") + 1], "0")
        for option in ("--slot-headroom-bytes", "--aggregate-headroom-bytes"):
            self.assertEqual(call[call.index(option) + 1], "0")
        self.assertEqual((output / "execute.stderr").read_bytes(), b"mock execution diagnostics\n")
        self.assertEqual(output.stat().st_mode & 0o077, 0)

    def test_execution_capacity_admission_precedes_artifacts_authentication_and_spawn(self):
        invalid = (("--slot-headroom-bytes", "0"), ("--aggregate-headroom-bytes", "0"),
                   ("--slot-headroom-bytes", "0", "--aggregate-headroom-bytes", "1048576"),
                   ("--slot-headroom-bytes", "1048576", "--aggregate-headroom-bytes", "0"),
                   ("--slot-headroom-bytes", "1048575", "--aggregate-headroom-bytes", "1048576"),
                   ("--slot-headroom-bytes", "67108865", "--aggregate-headroom-bytes", "536870912"),
                   ("--slot-headroom-bytes", "1048576", "--aggregate-headroom-bytes", "536870913"),
                   ("--slot-headroom-bytes", "+1048576", "--aggregate-headroom-bytes", "1048576"))
        for index, options in enumerate(invalid):
            output = self.root / f"invalid-execution-capacity-{index}"
            with mock.patch.object(sys, "argv", self.argv(output, route="production-slot") + list(options)), \
                 mock.patch.object(self.launcher, "load_bundle_tool") as loader, \
                 mock.patch.object(self.launcher.auth.subprocess, "run") as child, \
                 contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                self.launcher.main()
            self.assertFalse(output.exists())
            loader.assert_not_called()
            child.assert_not_called()
        output = self.root / "compatibility-positive-capacity"
        with mock.patch.object(sys, "argv", self.argv(output, headroom=(1048576, 1048576))), \
             contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            self.launcher.main()
        self.assertFalse(output.exists())

    def test_execution_full_policy_invalid_values_routes_mixed_and_duplicates_reject_before_effects(self):
        invalid = [(["--draw-command-limit", value], "compatibility")
                   for value in ("-1", "+3", "1", "2", "65", "3.0", "0x3", "3_0", " 3")]
        invalid += [(["--draw-command-limit", "3"], "production-slot"),
                    (["--draw-command-limit", "3", "--slot-headroom-bytes", "1048576",
                      "--aggregate-headroom-bytes", "1048576"], "production-slot"),
                    (["--draw-command-limit", "0", "--draw-command-limit", "0"], "compatibility"),
                    (["--route", "compatibility"], "compatibility")]
        for option in ("--slot-headroom-bytes", "--aggregate-headroom-bytes"):
            invalid.append((["--slot-headroom-bytes", "0", "--aggregate-headroom-bytes", "0", option, "0"],
                            "compatibility"))
        for index, (options, route) in enumerate(invalid):
            output = self.root / f"invalid-execution-full-policy-{index}"
            with mock.patch.object(sys, "argv", self.argv(output, route=route) + options), \
                 mock.patch.object(self.launcher, "load_bundle_tool") as loader, \
                 mock.patch.object(self.launcher.auth.subprocess, "run") as child, \
                 contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                self.launcher.main()
            self.assertFalse(output.exists())
            loader.assert_not_called()
            child.assert_not_called()

    def test_execution_grouping_identity_is_forwarded_and_does_not_grant_semantic_authority(self):
        for limit in (0, 3, 64):
            code, receipt, calls, _output, _loaded = self.run_case(draw_command_limit=limit)
            self.assertEqual(code, 0)
            self.assertTrue(receipt["accepted"])
            self.assertTrue(receipt["replay_policy_valid"])
            self.assertEqual(receipt["replay_policy"]["draw_command_limit"], limit)
            self.assertEqual(receipt["replay_policy"], receipt["steps"]["execute"]["native_result"]["replay_policy"])
            self.assertEqual(receipt["effective_stage_environment"]["DXMT9_DRAW_CHUNK_COMMAND_LIMIT"], str(limit))
            self.assertEqual(calls[0][calls[0].index("--draw-command-limit") + 1], str(limit))
            for name in self.launcher.FALSE_CLAIMS:
                self.assertIs(receipt[name], False)
            self.assertEqual(receipt["clocks"]["body"]["wall_ns"], 29)

    def test_execution_native_full_policy_and_grouping_pin_cannot_be_forged(self):
        changes = (lambda n: n.pop("replay_policy"),
                   lambda n: n["replay_policy"].update(schema="dxmt9.render_tape.v4_replay_policy.v2"),
                   lambda n: n["replay_policy"].update(route="production-slot"),
                   lambda n: n["replay_policy"].update(draw_command_limit=False),
                   lambda n: n["replay_policy"].update(draw_command_limit=3),
                   lambda n: n["replay_policy"].update(extra=0),
                   lambda n: n["replay_policy"].pop("slot_headroom_bytes"),
                   lambda n: n["effective_stage_environment"].update(DXMT9_DRAW_CHUNK_COMMAND_LIMIT="3"))
        for change in changes:
            native = self.native()
            change(native)
            self.assert_rejected(native)
        native = self.native(draw_command_limit=3)
        native["replay_policy"]["draw_command_limit"] = 64
        native["effective_stage_environment"]["DXMT9_DRAW_CHUNK_COMMAND_LIMIT"] = "64"
        code, receipt, _calls, _output, _loaded = self.run_case(native, draw_command_limit=3)
        self.assertEqual(code, 1)
        self.assertFalse(receipt["accepted"])
        self.assertTrue(receipt["steps"]["execute"]["native_result"]["complete"])

    def test_execution_capacity_is_forwarded_and_bound_without_adding_semantic_authority(self):
        for headroom in ((0, 0), (1048576, 1048576), (67108864, 536870912)):
            with self.subTest(headroom=headroom):
                code, receipt, calls, _output, _loaded = self.run_case(route="production-slot", headroom=headroom)
                self.assertEqual(code, 0)
                self.assertTrue(receipt["accepted"])
                self.assertEqual(receipt["capacity_policy"], dict(zip(self.launcher.semantics.CAPACITY_FIELDS, headroom)))
                self.assertEqual(receipt["expected_stage_environment"], receipt["effective_stage_environment"])
                for option, value in zip(("--slot-headroom-bytes", "--aggregate-headroom-bytes"), headroom):
                    self.assertEqual(calls[0][calls[0].index(option) + 1], str(value))
                for name in self.launcher.FALSE_CLAIMS:
                    self.assertIs(receipt[name], False)
                self.assertEqual(receipt["clocks"]["body"]["wall_ns"], 29)

    def test_execution_native_capacity_policy_and_actual_pins_need_exact_identity(self):
        for change in (lambda n: n.pop("capacity_policy"),
                       lambda n: n.update(capacity_policy={}),
                       lambda n: n["capacity_policy"].update(slot_headroom_bytes=False),
                       lambda n: n["capacity_policy"].update(aggregate_headroom_bytes=1),
                       lambda n: n["capacity_policy"].update(extra=0),
                       lambda n: n["effective_stage_environment"].update(DXMT9_DIRECT_SLOT_HEADROOM_BYTES="1"),
                       lambda n: n["effective_stage_environment"].update(DXMT9_DIRECT_SLOT_AGGREGATE_HEADROOM_BYTES=0)):
            native = self.native()
            change(native)
            self.assert_rejected(native)
        headroom = (1048576, 8 * 1024 * 1024)
        for variant in ("policy", "pin", "direct-work"):
            native = self.native(route="production-slot", headroom=headroom)
            if variant == "policy":
                native["capacity_policy"] = {"slot_headroom_bytes": 0, "aggregate_headroom_bytes": 0}
                native["effective_stage_environment"] = dict(self.launcher.STAGE_ENVIRONMENT)
            elif variant == "pin":
                native["effective_stage_environment"]["DXMT9_DIRECT_SLOT_HEADROOM_BYTES"] = "0"
            else:
                native["execution"]["route_coverage"].update(direct_draws=0, direct_leases=0,
                    compatibility_draws=2, compatibility_batches=1, fallback_draws=2)
            code, receipt, _calls, _output, _loaded = self.run_case(native, route="production-slot", headroom=headroom)
            self.assertEqual(code, 1)
            self.assertFalse(receipt["accepted"])
            self.assertTrue(receipt["steps"]["execute"]["native_result"]["complete"])

    def test_every_omission_must_be_explicit_and_true(self):
        for name in self.launcher.OMISSIONS:
            for value in (False, None, 1):
                with self.subTest(name=name, value=value):
                    native = self.native()
                    native["execution"]["execution_only"][name] = value
                    self.assert_rejected(native)
            with self.subTest(name=name, missing=True):
                native = self.native()
                del native["execution"]["execution_only"][name]
                self.assert_rejected(native)

    def test_diagnostic_tables_cannot_be_hidden_or_populated(self):
        for path, name in (((), "sources"), (("logical_semantics",), "receipts"),
                           (("buffer_version_projection",), "rows")):
            for value in (None, [{}], ""):
                with self.subTest(path=path, name=name, value=value):
                    native = self.native()
                    table = native["execution"]
                    for part in path:
                        table = table[part]
                    table[name] = value
                    self.assert_rejected(native)
        for group, name in (("counters", "record_contexts"), ("counters", "buffer_versions_observed"),
                            ("expected_buffer_budget", "charged_bytes"),
                            ("logical_semantics", "logical_buffer_hash_bytes"),
                            ("source_row_observation", "qualified_draws")):
            native = self.native()
            native["execution"][group][name] = 1
            self.assert_rejected(native)

    def test_clock_scopes_require_complete_exact_nonnegative_bounded_integers(self):
        for scope in ("setup", "body", "teardown"):
            for name in ("started", "valid", "complete"):
                native = self.native()
                native["execution"]["clocks"][scope][name] = False
                self.assert_rejected(native)
            for name in ("wall_ns", "process_cpu_ns"):
                for value in (-1, True, 1.5, self.launcher.U64_MAX + 1, None):
                    with self.subTest(scope=scope, name=name, value=value):
                        native = self.native()
                        native["execution"]["clocks"][scope][name] = value
                        self.assert_rejected(native)
                native = self.native()
                del native["execution"]["clocks"][scope][name]
                self.assert_rejected(native)
        for name in ("clock_domain", "process_cpu_clock", "scope", "body_scope"):
            native = self.native()
            native["execution"]["clocks"][name] = "unscoped"
            self.assert_rejected(native)
        native = self.native()
        native["execution"]["clocks"]["body_excludes_setup_and_teardown"] = False
        self.assert_rejected(native)

    def test_route_lifecycle_ack_query_provenance_and_reset_tampering_rejects(self):
        changes = (
            ("consumer", "phase", 1), ("consumer", "acknowledged", 0),
            ("consumer", "retired", 0), ("consumer", "cleanup_complete", False),
            ("consumer", "lifecycle_actions", 3), ("consumer", "commands", 1),
            ("execution_only", "lifecycle_coverage_complete", False),
            ("execution_only", "draws", 1), ("query_projection", "replayed_polls", 0),
            ("provenance", "raw_ack_coverage_complete", False),
            ("provenance", "source_obligations_qualified", 0),
            ("initial_setup", "host_checkpoint_joined", False),
            ("counters", "resources_released", 0), ("counters", "setup_acknowledgements", 1),
            ("route_coverage", "complete", False), ("route_coverage", "effective_route", "production-slot"),
        )
        for group, name, value in changes:
            with self.subTest(group=group, name=name):
                native = self.native()
                native["execution"][group][name] = value
                self.assert_rejected(native)
        for name in ("reset_ok", "source_progress_complete", "runtime_budgeted_attempt"):
            native = self.native()
            native["execution"][name] = False
            self.assert_rejected(native)
        native = self.native()
        native["execution"]["consumer_receipts"][0]["acknowledged"] = False
        self.assert_rejected(native)
        native = self.native()
        native["execution"]["consumer"]["active_identity"]["sequence"] = 1
        self.assert_rejected(native)

    def test_direct_route_requires_real_direct_work_but_accepts_natural_mixed_fallback(self):
        native = self.native(route="production-slot")
        native["execution"]["route_coverage"].update(direct_draws=1, compatibility_draws=1,
                                                    compatibility_batches=1, fallback_draws=1)
        code, receipt, _calls, _output, _loaded = self.run_case(native, route="production-slot")
        self.assertEqual(code, 0)
        self.assertTrue(receipt["accepted"])
        native["execution"]["route_coverage"].update(direct_draws=0, direct_leases=0,
                                                    compatibility_draws=2, fallback_draws=2)
        code, receipt, _calls, _output, _loaded = self.run_case(native, route="production-slot")
        self.assertEqual(code, 1)
        self.assertEqual(receipt["failure"], "requested-direct-route-not-exercised")

    def test_candidate_rebatching_does_not_change_original_source_obligation_count(self):
        native = self.native()
        execution = native["execution"]
        execution["execution_only"].update(sources=2, submitted=2, acknowledged=2, retired=2)
        execution["consumer"].update(observed=2, submitted=2, acknowledged=2, retired=2,
                                     receipts=2, lifecycle_actions=8)
        second = copy.deepcopy(execution["consumer_receipts"][0])
        second["identity"].update(sequence=2, source_index=1, storage_first_page=1)
        execution["consumer_receipts"].append(second)
        code, receipt, _calls, _output, _loaded = self.run_case(native)
        self.assertEqual(code, 0)
        self.assertTrue(receipt["accepted"])
        self.assertEqual(receipt["execution"]["execution_only"]["sources"], 2)
        self.assertEqual(receipt["execution"]["provenance"]["source_obligations_qualified"], 1)

    def test_module_and_input_reauthentication_rejects_missing_or_changed_evidence(self):
        for index in (0, 1, 2):
            native = self.native()
            native["loaded_images"].pop(index)
            receipt = self.assert_rejected(native)
            self.assertFalse(receipt["loaded_images_stable"])
        native = self.native()
        native["loaded_images"][1]["sha256"] = "f" * 64
        self.assert_rejected(native)
        for path in (self.binary, self.modules[0], self.bundle / "events.bin"):
            original = path.read_bytes()
            code, receipt, _calls, _output, _loaded = self.run_case(
                mutate=lambda path=path: path.write_bytes(b"changed after child"))
            self.assertEqual(code, 1)
            self.assertFalse(receipt["accepted"])
            self.assertIn("reauthentication_failure", receipt)
            path.write_bytes(original)

    def test_failure_and_partial_child_artifacts_survive(self):
        native = self.native()
        native.update(complete=False, phase="actual-replay", failure="native-owned-failure", failed_event=7)
        code, receipt, _calls, output, _loaded = self.run_case(native, returncode=4)
        self.assertEqual(code, 1)
        self.assertFalse(receipt["accepted"])
        self.assertEqual(receipt["steps"]["execute"]["native_result"]["failure"], "native-owned-failure")
        self.assertEqual(receipt["steps"]["execute"]["returncode"], 4)
        self.assertEqual(json.loads((output / "execute.stdout").read_bytes())["failed_event"], 7)
        timeout = subprocess.TimeoutExpired("mock", 1)
        code, receipt, _calls, output, _loaded = self.run_case(timeout)
        self.assertEqual(code, 1)
        self.assertTrue(receipt["steps"]["execute"]["timed_out"])
        self.assertEqual((output / "execute.stdout").read_bytes(), b"partial child output")

    def test_contract_hashes_and_selected_pe_attestation_are_authenticated(self):
        for name, value in (("producer_contract", "pe-present-reservation-v1"),
                            ("buffer_preservation_contract", "none"),
                            ("requires_launcher_pe_attestation", True)):
            native = self.native()
            native[name] = value
            self.assert_rejected(native)
        for name in ("events", "identity", "execution_topology", "source", "output",
                     "action_profile", "expected_plan", "actual_plan"):
            native = self.native()
            native["sha256"][name] = "f" * 64
            self.assert_rejected(native)
        contract = "pe-present-reservation-v1"
        code, receipt, calls, _output, _loaded = self.run_case(contract=contract)
        self.assertEqual(code, 1)
        self.assertFalse(receipt["accepted"])
        self.assertEqual(calls, [])
        self.manifest.update(producer={"path": "dxmt9-pe-render-tape"}, stage="production-capture")
        self.manifest["scope"].update(production_capture=True, source_oracle=True, output_oracle=True)
        self.fixture.save_manifest()
        code, receipt, calls, _output, _loaded = self.run_case(contract=contract)
        self.assertEqual(code, 0)
        self.assertTrue(receipt["accepted"])
        self.assertEqual(calls[0][calls[0].index("--producer-contract") + 1], contract)
        self.assertFalse(receipt["steps"]["execute"]["native_result"]["native_producer_attestation_authenticated"])

    def test_duplicate_json_keys_and_nonfinite_clocks_reject_without_losing_raw_output(self):
        for raw in (b'{"schema":"first","schema":"second"}',
                    b'{"wall_ns":NaN}', b'{"process_cpu_ns":Infinity}'):
            code, receipt, calls, output, _loaded = self.run_case(raw)
            self.assertEqual(code, 1)
            self.assertFalse(receipt["accepted"])
            self.assertEqual(len(calls), 1)
            self.assertEqual((output / "execute.stdout").read_bytes(), raw)

    def test_required_optins_and_effective_environment_cannot_be_inferred(self):
        for option in ("--route", "--expected-plan-digest", "--buffer-preservation-contract"):
            output = self.root / f"missing-{option[2:]}"
            argv = self.argv(output)
            index = argv.index(option)
            del argv[index:index + 2]
            with mock.patch.object(sys, "argv", argv), contextlib.redirect_stderr(io.StringIO()), \
                 self.assertRaises(SystemExit):
                self.launcher.main()
            self.assertFalse(output.exists())
        for name in self.launcher.STAGE_ENVIRONMENT:
            native = self.native()
            native["effective_stage_environment"].pop(name)
            self.assert_rejected(native)
        native = self.native()
        native["effective_stage_environment"]["DXMT_PERF_COUNTERS"] = "0"
        self.assert_rejected(native)
        for name in self.launcher.BROAD_FALSE:
            native = self.native()
            native[name] = True
            self.assert_rejected(native)


if __name__ == "__main__":
    unittest.main()
