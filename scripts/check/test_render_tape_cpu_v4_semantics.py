#!/usr/bin/env python3
"""Private controlled CPU semantics launcher tests; the child is always mocked."""

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
import sys
import tempfile
import unittest
from unittest import mock


class ControlledCpuSemanticsTest(unittest.TestCase):
    def setUp(self):
        path = pathlib.Path(__file__).with_name("run_render_tape_cpu_v4_semantics.py")
        spec = importlib.util.spec_from_file_location("cpu_v4_semantics_tested", path)
        assert spec and spec.loader
        self.launcher = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.launcher)
        self.shared = self.launcher.load_bundle_tool()
        temporary = tempfile.TemporaryDirectory(prefix="dxmt9-cpu-v4-semantics-test-")
        self.addCleanup(temporary.cleanup)
        self.root = pathlib.Path(temporary.name).resolve()
        self.bundle = self.root / "bundle"
        self.bundle.mkdir()
        (self.bundle / "blobs").mkdir()
        self.binary = self.root / "dxmt9-mock-native"
        self.binary.write_bytes(b"subprocess is mocked, not executable code\n")
        self.binary.chmod(0o700)
        self.binary_bytes = self.binary.read_bytes()
        # Loader-only authenticated prefixes; never sent to a real renderer.
        identity = self.shared.IDENTITY_V4_BINDING_PREFIX.pack(
            self.shared.IDENTITY_MAGIC, 101, 4, 264, 102, 103).ljust(264, b"\0")
        components = {"blobs": []}
        for name, (filename, _option) in self.launcher.auth.COMPONENTS.items():
            raw = identity if name == "identity" else b"fixture"
            (self.bundle / filename).write_bytes(raw)
            components[name] = {"path": filename, "bytes": len(raw),
                                "sha256": hashlib.sha256(raw).hexdigest()}
        components["identity"]["schema"] = self.shared.IDENTITY_V4_SCHEMA
        components["execution_topology"]["schema"] = self.shared.EXECUTION_TOPOLOGY_SCHEMA
        action = self.shared.ACTION_PROFILE_HEADER.pack(
            self.shared.ACTION_PROFILE_MAGIC, 1, self.shared.ACTION_PROFILE_HEADER.size,
            1, 1, 1, 1, 1, 0, 101, 102, 103, 0, 0,
            *(components[name]["bytes"] for name in ("events", "identity", "execution_topology")),
            *(bytes.fromhex(components[name]["sha256"])
              for name in ("events", "identity", "execution_topology")))
        (self.bundle / "action_profile.bin").write_bytes(action)
        components["action_profile"] = {"path": "action_profile.bin", "bytes": len(action),
                                        "sha256": hashlib.sha256(action).hexdigest(),
                                        "schema": self.shared.ACTION_PROFILE_SCHEMA}
        self.manifest = {"schema": self.shared.SCHEMA, "profile": "frame-tape", "components": components,
                         "scope": {"capture_action_profile": "queue-observed",
                                   "action_profile_stable_exact": True}}
        self.save_manifest()
        self.plan = "1" * 64
        self.count = 0

    def save_manifest(self):
        (self.bundle / "manifest.json").write_text(json.dumps(self.manifest), encoding="utf-8")

    def native(self, contract="none", route="compatibility", headroom=None, draw_command_limit=0):
        policy = dict(zip(self.launcher.CAPACITY_FIELDS, headroom or (0, 0)))
        full_policy = self.launcher.replay_policy(argparse.Namespace(
            route=route, draw_command_limit=draw_command_limit, **policy))
        result = {"schema": self.launcher.NATIVE_SCHEMA, "mode": "controlled-cpu-v4-semantics",
                  "diagnostic": True, "complete": True, "phase": "logical-semantics-complete",
                  "failure": "", "failed_input_component": None, "failed_event": None,
                  "failed_event_ordinal": None, "iterations": 1, "max_wall_ms": 1000,
                  "producer_contract": contract,
                  "requested_route": route, "effective_route": route,
                  "capacity_policy": policy,
                  "replay_policy": full_policy,
                  "effective_stage_environment": self.launcher.expected_stage_environment(policy, draw_command_limit),
                  "buffer_preservation_contract": self.launcher.PRESERVATION_CONTRACT,
                  "native_producer_attestation_authenticated": False,
                  "requires_launcher_pe_attestation": contract == "pe-present-reservation-v1",
                  "input_admission_profile": "controlled-v4-raw256",
                  "metal_resource_device_created": True, "fixture_teardown_complete": True,
                  "authority_scope_valid": True,
                  "loaded_images": [{"path": str(self.binary), "bytes": len(self.binary_bytes),
                                     "sha256": hashlib.sha256(self.binary_bytes).hexdigest()}]}
        result.update({name: False for name in self.launcher.BROAD_FALSE})
        result["authentication"] = {name: True for name in (
            "owned_inputs_authenticated", "wire_valid", "general_valid", "pair_valid",
            "action_profile_authenticated", "stable_exact_action_witness",
            "original_oracle_bytes_authenticated_only", "plan_digest_matched")}
        result["authentication"].update(blob_count=0, blob_bytes=0)
        result["sha256"] = {name: self.manifest["components"][component]["sha256"]
                            for name, component in (("events", "events"), ("identity", "identity"),
                                                    ("execution_topology", "execution_topology"),
                                                    ("source", "source_oracle"), ("output", "output_oracle"),
                                                    ("action_profile", "action_profile"))}
        result["sha256"].update(expected_plan=self.plan, actual_plan=self.plan)
        result["action_witness"] = {"capture_token": 101, "capture_epoch": 102, "queue": 103,
                                    "construction_profile": 1, "arm_profile": 1, "freeze_profile": 1,
                                    "arm_revision": 0, "freeze_revision": 0}
        result["fixed_fixture_setup"] = {
            "cpu_ready_arena_replay_supported": route == "compatibility",
            "direct_chunk_slot_replay_supported": route == "production-slot",
            "render_workers_disabled_verified": True, "replay_offload_absent_verified": True,
            "exact_queue_action_profile_verified": True, "queue_construction_profile": 1,
            "queue_current_profile": 1, "queue_profile_revision": 0}
        result["input_admission"] = {"accepted": True}
        receipts = [{"record": {"event_ordinal": event, "record_index": 0},
                     "command_kind": 0, "identity_command_kind": 1,
                     "sequence": 1, "physical_command_index": 0, "subdraw_index": index, "failure": "",
                     "digest": "0123456789abcdef", "serialized": True, "source_rows_required": True,
                     "source_rows": {"status": "qualified", "qualified": True,
                                     "source_sequence": 1, "completed_sequence": 0,
                                     "planned_read_bytes": 64, "proved": True, "proved_bytes": 64}}
                    for index, event in enumerate((4, 5))]
        receipts.append({"record": {"event_ordinal": 6, "record_index": 0},
                         "command_kind": 1, "identity_command_kind": 2,
                         "sequence": 1, "physical_command_index": 1, "subdraw_index": None, "failure": "",
                         "digest": "fedcba9876543210", "serialized": True, "source_rows_required": False})
        execution = {"complete": True, "metadata_inventory_complete": True, "runtime_budgeted_attempt": True,
                     "source_progress_complete": True, "reset_ok": True, "failure": "",
                     "failed_event": None, "resource_version_failure": None,
                     "counters": {"draws": 2, "resources_created": 1, "resources_released": 1},
                     "consumer": {"worker_exited": True, "producer_closed": True, "cleanup_complete": True,
                                  "cleanup_failed": False, "failure": 0,
                                  "completion_policy": "immediate-fifo-cpu-acknowledgement",
                                  "observed": 1, "submitted": 1, "acknowledged": 1, "retired": 1,
                                  "receipts": 1, "commands": 2},
                     "provenance": {name: True for name in (
                         "canonical_traversal_complete", "raw_ack_coverage_complete",
                         "canonical_origin_mapping_complete", "controlled_cpu_complete",
                         "present_preparation_attempted", "present_preparation_completed")},
                     "query_projection": {"recorded_polls": 1, "replayed_polls": 1, "gpu_result_oracle": False},
                     "source_row_observation": {"qualified_draws": 2, "not_qualified_draws": 0,
                                                "shader_demand_complete": False, "future_preservation": False,
                                                "physical_fetch_complete": False},
                     "sources": [{"sequence": 1, "commands": [
                         {"physical_command": 0, "kind": 1, "draw_count": 2},
                         {"physical_command": 1, "kind": 2, "draw_count": 0}]}],
                     "logical_semantics": {
                         "requested": True, "admitted": True, "accepted": True, "complete": True,
                         "legal_candidate_nooverwrite_contract": True, "conditional_on_legal_nooverwrite": True,
                         "logical_stream_complete": True, "failure": "",
                         "digest_algorithm": "fnv1a64-logical-v4", "digest": "0123456789abcdef",
                         "sources": 1, "commands": 2, "draws": 2, "original_items": 3,
                         "source_rows_required": 2, "source_rows_qualified": 2,
                         "logical_buffer_hash_bytes": 128, "receipts": receipts}}
        execution["logical_semantics"].update({name: False for name in self.launcher.SEMANTIC_FALSE})
        production = route == "production-slot"
        execution["route_coverage"] = {
            "requested_route": route, "effective_route": route, "policy_selected": True,
            "observed": True, "monotonic": True, "complete": True,
            "direct_leases": int(production), "direct_draws": 2 if production else 0,
            "compatibility_draws": 0 if production else 2, "compatibility_batches": 0 if production else 1,
            "compatibility_unbatched_draws": 0, "fallback_draws": 0}
        result["execution"] = execution
        return result

    def argv(self, output, contract="none", preservation=True, route="compatibility", headroom=None,
             draw_command_limit=None):
        result = ["semantics", "--bundle", str(self.bundle), "--diagnostic", str(self.binary),
                  "--output-dir", str(output), "--expected-plan-digest", self.plan,
                  "--route", route,
                  "--producer-contract", contract, "--timeout-seconds", "10"]
        if preservation:
            result += ["--buffer-preservation-contract", self.launcher.PRESERVATION_CONTRACT]
        if headroom is not None:
            result += ["--slot-headroom-bytes", str(headroom[0]),
                       "--aggregate-headroom-bytes", str(headroom[1])]
        if draw_command_limit is not None:
            result += ["--draw-command-limit", str(draw_command_limit)]
        return result

    def run_case(self, response=None, returncode=0, contract="none", mutate=None, route="compatibility", headroom=None,
                 draw_command_limit=None):
        self.count += 1
        output = self.root / f"result-{self.count}"
        calls = []
        response = self.native(contract, route, headroom,
            0 if draw_command_limit is None else draw_command_limit) if response is None else response

        def child(argv, **kwargs):
            calls.append(argv)
            self.assertNotIn("env", kwargs)
            self.assertGreater(kwargs["timeout"], 0)
            self.assertLessEqual(kwargs["timeout"], 10)
            kwargs["stderr"].write(b"mock child diagnostics\n")
            if isinstance(response, BaseException):
                kwargs["stdout"].write(b"partial child output")
                raise response
            value = copy.deepcopy(response)
            if isinstance(value, dict):
                value["max_wall_ms"] = int(argv[argv.index("--max-wall-ms") + 1])
                raw = json.dumps(value).encode()
            else:
                raw = value
            kwargs["stdout"].write(raw)
            if mutate:
                mutate()
            return subprocess.CompletedProcess(argv, returncode)

        with mock.patch.object(sys, "argv", self.argv(output, contract, route=route, headroom=headroom,
                                                      draw_command_limit=draw_command_limit)), \
             contextlib.redirect_stdout(io.StringIO()), \
             mock.patch.object(self.launcher, "load_bundle_tool", return_value=self.shared), \
             mock.patch.object(self.shared, "load_bundle", wraps=self.shared.load_bundle) as loaded, \
             mock.patch.object(self.launcher.auth.subprocess, "run", side_effect=child):
            code = self.launcher.main()
        receipt = json.loads((output / "result.json").read_text())
        return code, receipt, calls, output, loaded

    def test_one_private_cpu_child_authenticates_and_reauthenticates_without_public_authority(self):
        code, receipt, calls, output, loaded = self.run_case()
        self.assertEqual(code, 0)
        self.assertTrue(receipt["accepted"])
        self.assertEqual(receipt["schema"], self.launcher.RECEIPT_SCHEMA)
        self.assertEqual(len(calls), 1)
        self.assertEqual(loaded.call_args_list, [mock.call(self.bundle, allow_identity_v4=True)] * 2)
        call = calls[0]
        self.assertEqual(call[1], "--controlled-cpu-v4-semantics")
        self.assertEqual(call[call.index("--expected-plan-digest") + 1], self.plan)
        self.assertEqual(call[call.index("--buffer-preservation-contract") + 1], self.launcher.PRESERVATION_CONTRACT)
        self.assertEqual(call[call.index("--route") + 1], "compatibility")
        for component, (_path, option) in self.launcher.auth.COMPONENTS.items():
            self.assertEqual(call[call.index(option) + 1], self.manifest["components"][component]["sha256"])
        self.assertNotIn("--controlled-cpu-v4-inventory", call)
        self.assertNotIn("--repeat", call)
        self.assertNotIn("--replay-route", call)
        self.assertNotIn("--publication-mode", call)
        self.assertEqual(receipt["route"], "compatibility")
        self.assertEqual(receipt["iterations"], 1)
        self.assertEqual(receipt["capacity_policy"], dict(zip(self.launcher.CAPACITY_FIELDS, (0, 0))))
        self.assertEqual(receipt["expected_stage_environment"], self.launcher.STAGE_ENVIRONMENT)
        self.assertEqual(receipt["effective_stage_environment"], self.launcher.STAGE_ENVIRONMENT)
        self.assertTrue(receipt["capacity_policy_valid"])
        self.assertTrue(receipt["stage_environment_valid"])
        self.assertTrue(receipt["replay_policy_valid"])
        self.assertEqual(receipt["replay_policy"], {"schema": self.launcher.REPLAY_POLICY_SCHEMA,
            "route": "compatibility", "draw_command_limit": 0, **receipt["capacity_policy"]})
        self.assertEqual(call[call.index("--draw-command-limit") + 1], "0")
        for option in ("--slot-headroom-bytes", "--aggregate-headroom-bytes"):
            self.assertEqual(call[call.index(option) + 1], "0")
        for name in self.launcher.BROAD_FALSE:
            self.assertIs(receipt[name], False)
        for name in ("manifest_authenticated", "bundle_reauthenticated", "binary_reauthenticated", "loaded_images_stable"):
            self.assertIs(receipt[name], True)
        self.assertTrue(receipt["logical_stream_complete"])
        for field in ("full_logical_semantic_oracle_complete", "original_semantic_equivalence_qualified",
                      "cross_policy_equivalence_qualified"):
            self.assertFalse(receipt[field])
            self.assertFalse(receipt["logical_semantics"][field])
        self.assertTrue(receipt["preservation_contract_is_caller_assumption"])
        self.assertEqual((output / "semantics.stderr").read_bytes(), b"mock child diagnostics\n")
        self.assertEqual(output.stat().st_mode & 0o077, 0)
        with self.assertRaisesRegex(SystemExit, "inspection-only"):
            self.shared.load_bundle(self.bundle)

    def test_capacity_helper_preserves_legacy_exact_defaults_and_enforces_strict_pairs(self):
        exact = dict(zip(self.launcher.CAPACITY_FIELDS, (0, 0)))
        for route in self.launcher.ROUTES:
            args = argparse.Namespace(route=route)
            self.assertEqual(self.launcher.capacity_policy(args), exact)
            args.slot_headroom_bytes = args.aggregate_headroom_bytes = 0
            self.assertEqual(self.launcher.capacity_policy(args), exact)
        for slot, aggregate in ((1024 * 1024, 1024 * 1024), (64 * 1024 * 1024, 512 * 1024 * 1024)):
            args = argparse.Namespace(route="production-slot", slot_headroom_bytes=slot,
                                      aggregate_headroom_bytes=aggregate)
            policy = self.launcher.capacity_policy(args)
            self.assertEqual(policy, dict(zip(self.launcher.CAPACITY_FIELDS, (slot, aggregate))))
            stage = self.launcher.expected_stage_environment(policy)
            self.assertEqual(stage["DXMT9_DIRECT_SLOT_HEADROOM_BYTES"], str(slot))
            self.assertEqual(stage["DXMT9_DIRECT_SLOT_AGGREGATE_HEADROOM_BYTES"], str(aggregate))
        for field in self.launcher.CAPACITY_FIELDS:
            with self.subTest(half_pair=field), self.assertRaises(self.launcher.Rejected):
                self.launcher.capacity_policy(argparse.Namespace(route="production-slot", **{field: 0}))
        pairs = ((True, 1), (1, True), (False, False), (1.0, 1), ("1", 1), (None, 0), (None, None),
                 (-1, 1), (0, 1), (1, 0), (2, 1), (64 * 1024 * 1024 + 1, 512 * 1024 * 1024),
                 (1024 * 1024 - 1, 1024 * 1024),
                 (1, 512 * 1024 * 1024 + 1))
        for slot, aggregate in pairs:
            with self.subTest(slot=slot, aggregate=aggregate), self.assertRaises(self.launcher.Rejected):
                self.launcher.capacity_policy(argparse.Namespace(route="production-slot",
                    slot_headroom_bytes=slot, aggregate_headroom_bytes=aggregate))
        with self.assertRaises(self.launcher.Rejected):
            self.launcher.capacity_policy(argparse.Namespace(route="compatibility",
                slot_headroom_bytes=1024 * 1024, aggregate_headroom_bytes=1024 * 1024))

    def test_full_replay_policy_helper_is_route_bound_and_rejects_mixed_or_noninteger_grouping(self):
        capacity = {"slot_headroom_bytes": 0, "aggregate_headroom_bytes": 0}
        for route in self.launcher.ROUTES:
            self.assertEqual(self.launcher.replay_policy(argparse.Namespace(route=route)),
                {"schema": self.launcher.REPLAY_POLICY_SCHEMA, "route": route, "draw_command_limit": 0, **capacity})
        for limit in (0, 3, 64):
            args = argparse.Namespace(route="compatibility", draw_command_limit=limit)
            policy = self.launcher.replay_policy(args)
            self.assertEqual(policy["draw_command_limit"], limit)
            self.assertEqual(self.launcher.expected_stage_environment(capacity, limit)["DXMT9_DRAW_CHUNK_COMMAND_LIMIT"],
                             str(limit))
        for limit in (False, True, None, "3", 3.0, -1, 1, 2, 65):
            with self.subTest(limit=limit), self.assertRaises(self.launcher.Rejected):
                self.launcher.replay_policy(argparse.Namespace(route="compatibility", draw_command_limit=limit))
        with self.assertRaises(self.launcher.Rejected):
            self.launcher.replay_policy(argparse.Namespace(route="production-slot", draw_command_limit=3))
        with self.assertRaises(self.launcher.Rejected):
            self.launcher.replay_policy(argparse.Namespace(route="production-slot", draw_command_limit=3,
                slot_headroom_bytes=1048576, aggregate_headroom_bytes=1048576))
        with self.assertRaises(self.launcher.Rejected):
            self.launcher.expected_stage_environment({"slot_headroom_bytes": 1048576,
                "aggregate_headroom_bytes": 1048576}, 3)

    def test_invalid_or_duplicate_grouping_cli_rejects_before_effects(self):
        variants = [(["--draw-command-limit", value], "compatibility")
                    for value in ("-1", "+3", "1", "2", "65", "3.0", "0x3", "3_0", " 3")]
        variants += [(["--draw-command-limit", "3"], "production-slot"),
                     (["--draw-command-limit", "3", "--slot-headroom-bytes", "1048576",
                       "--aggregate-headroom-bytes", "1048576"], "production-slot"),
                     (["--route", "compatibility"], "compatibility"),
                     (["--draw-command-limit", "0", "--draw-command-limit", "0"], "compatibility")]
        for option in ("--slot-headroom-bytes", "--aggregate-headroom-bytes"):
            variants.append((["--slot-headroom-bytes", "0", "--aggregate-headroom-bytes", "0", option, "0"],
                             "compatibility"))
        for index, (options, route) in enumerate(variants):
            output = self.root / f"grouping-invalid-{index}"
            with self.subTest(options=options), \
                 mock.patch.object(sys, "argv", self.argv(output, route=route) + options), \
                 mock.patch.object(self.launcher, "load_bundle_tool") as loader, \
                 mock.patch.object(self.launcher.auth.subprocess, "run") as child, \
                 contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                self.launcher.main()
            self.assertFalse(output.exists())
            loader.assert_not_called()
            child.assert_not_called()

    def test_grouping_policy_is_forwarded_and_validated_without_promoting_authority(self):
        for limit in (0, 3, 64):
            code, receipt, calls, _output, _loaded = self.run_case(draw_command_limit=limit)
            self.assertEqual(code, 0)
            self.assertTrue(receipt["accepted"])
            self.assertTrue(receipt["replay_policy_valid"])
            self.assertEqual(receipt["replay_policy"]["draw_command_limit"], limit)
            self.assertEqual(receipt["steps"]["semantics"]["native_result"]["replay_policy"], receipt["replay_policy"])
            self.assertEqual(receipt["effective_stage_environment"]["DXMT9_DRAW_CHUNK_COMMAND_LIMIT"], str(limit))
            self.assertEqual(calls[0][calls[0].index("--draw-command-limit") + 1], str(limit))
            self.assertTrue(receipt["logical_stream_complete"])
            for flag in self.launcher.BROAD_FALSE:
                self.assertIs(receipt[flag], False)

    def test_native_replay_policy_identity_and_grouping_pin_must_match(self):
        changes = (lambda n: n.pop("replay_policy"),
                   lambda n: n["replay_policy"].update(schema="dxmt9.render_tape.v4_replay_policy.v2"),
                   lambda n: n["replay_policy"].update(route="production-slot"),
                   lambda n: n["replay_policy"].update(draw_command_limit=False),
                   lambda n: n["replay_policy"].update(draw_command_limit=3),
                   lambda n: n["replay_policy"].update(extra=0),
                   lambda n: n["replay_policy"].pop("aggregate_headroom_bytes"),
                   lambda n: n["effective_stage_environment"].update(DXMT9_DRAW_CHUNK_COMMAND_LIMIT="3"))
        for change in changes:
            native = self.native()
            change(native)
            code, receipt, calls, _output, _loaded = self.run_case(native)
            self.assertEqual(code, 1)
            self.assertFalse(receipt["accepted"])
            self.assertFalse(receipt["replay_policy_valid"])
            self.assertEqual(len(calls), 1)
            self.assertIn("native_result", receipt["steps"]["semantics"])
        native = self.native(draw_command_limit=3)
        native["replay_policy"]["draw_command_limit"] = 64
        native["effective_stage_environment"]["DXMT9_DRAW_CHUNK_COMMAND_LIMIT"] = "64"
        code, receipt, _calls, _output, _loaded = self.run_case(native, draw_command_limit=3)
        self.assertEqual(code, 1)
        self.assertFalse(receipt["accepted"])

    def test_capacity_cli_invalid_pairs_fail_before_authentication_artifacts_or_spawn(self):
        variants = [(["--slot-headroom-bytes", "0"], "production-slot"),
                    (["--aggregate-headroom-bytes", "0"], "production-slot")]
        variants += [(["--slot-headroom-bytes", slot, "--aggregate-headroom-bytes", aggregate], "production-slot")
                     for slot, aggregate in (("0", "1"), ("1", "0"), ("2", "1"),
                         (str(64 * 1024 * 1024 + 1), str(512 * 1024 * 1024)),
                         ("1", str(512 * 1024 * 1024 + 1)), (str(1024 * 1024 - 1), str(1024 * 1024)),
                         ("+1", "1"), ("-1", "1"),
                         ("1.0", "1"), ("1e6", "1"), ("0x10", "16"), ("1_000", "1000"),
                         ("99999999999", "99999999999"))]
        variants += [(["--slot-headroom-bytes", "1048576", "--aggregate-headroom-bytes", "1048576"], "compatibility")]
        for index, (options, route) in enumerate(variants):
            output = self.root / f"capacity-invalid-{index}"
            with self.subTest(options=options, route=route), \
                 mock.patch.object(sys, "argv", self.argv(output, route=route) + options), \
                 mock.patch.object(self.launcher, "load_bundle_tool") as loader, \
                 mock.patch.object(self.launcher.auth.subprocess, "run") as child, \
                 contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                self.launcher.main()
            loader.assert_not_called()
            child.assert_not_called()
            self.assertFalse(output.exists())

    def test_bounded_capacity_is_forwarded_and_independently_bound_without_semantic_promotion(self):
        for route, headroom in (("compatibility", (0, 0)), ("production-slot", (0, 0)),
                                ("production-slot", (1024 * 1024, 1024 * 1024)),
                                ("production-slot", (64 * 1024 * 1024, 512 * 1024 * 1024))):
            with self.subTest(route=route, headroom=headroom):
                code, receipt, calls, _output, _loaded = self.run_case(route=route, headroom=headroom)
                self.assertEqual(code, 0)
                self.assertTrue(receipt["accepted"])
                policy = dict(zip(self.launcher.CAPACITY_FIELDS, headroom))
                self.assertEqual(receipt["capacity_policy"], policy)
                self.assertEqual(receipt["steps"]["semantics"]["native_result"]["capacity_policy"], policy)
                self.assertEqual(receipt["effective_stage_environment"], self.launcher.expected_stage_environment(policy))
                for option, value in zip(("--slot-headroom-bytes", "--aggregate-headroom-bytes"), headroom):
                    self.assertEqual(calls[0][calls[0].index(option) + 1], str(value))
                self.assertTrue(receipt["logical_stream_complete"])
                self.assertFalse(receipt["policy_qualified"])
                self.assertFalse(receipt["timing_eligible"])

    def test_native_capacity_and_actual_stage_environment_cannot_be_inferred_or_mismatched(self):
        for change in (lambda n: n.pop("capacity_policy"),
                       lambda n: n.update(capacity_policy={}),
                       lambda n: n["capacity_policy"].update(slot_headroom_bytes=False),
                       lambda n: n["capacity_policy"].update(aggregate_headroom_bytes=1),
                       lambda n: n["capacity_policy"].update(unrequested=0),
                       lambda n: n.pop("effective_stage_environment"),
                       lambda n: n["effective_stage_environment"].update(DXMT9_DIRECT_SLOT_HEADROOM_BYTES=""),
                       lambda n: n["effective_stage_environment"].update(DXMT9_DIRECT_SLOT_AGGREGATE_HEADROOM_BYTES=0)):
            native = self.native()
            change(native)
            code, receipt, calls, _output, _loaded = self.run_case(native)
            self.assertEqual(code, 1)
            self.assertFalse(receipt["accepted"])
            self.assertFalse(receipt["capacity_policy_valid"])
            self.assertEqual(len(calls), 1)
            self.assertIn("native_result", receipt["steps"]["semantics"])
        headroom = (1024 * 1024, 8 * 1024 * 1024)
        for variant in ("identity", "pin", "direct-work"):
            native = self.native(route="production-slot", headroom=headroom)
            if variant == "identity":
                native["capacity_policy"] = dict(zip(self.launcher.CAPACITY_FIELDS, (0, 0)))
                native["effective_stage_environment"] = dict(self.launcher.STAGE_ENVIRONMENT)
            elif variant == "pin":
                native["effective_stage_environment"]["DXMT9_DIRECT_SLOT_HEADROOM_BYTES"] = "0"
            else:
                native["execution"]["route_coverage"].update(direct_draws=0, direct_leases=0,
                    compatibility_draws=2, compatibility_batches=1, fallback_draws=2)
            code, receipt, _calls, _output, _loaded = self.run_case(native, route="production-slot", headroom=headroom)
            self.assertEqual(code, 1)
            self.assertFalse(receipt["accepted"])

    def test_preservation_opt_in_and_new_output_are_required_before_artifacts(self):
        output = self.root / "must-not-be-created"
        with mock.patch.object(sys, "argv", self.argv(output, preservation=False)), \
             contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            self.launcher.main()
        self.assertFalse(output.exists())
        output.mkdir()
        with mock.patch.object(sys, "argv", self.argv(output)), \
             contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            self.launcher.main()
        self.assertEqual(list(output.iterdir()), [])

    def test_route_must_be_explicit_before_artifacts(self):
        output = self.root / "missing-route"
        argv = self.argv(output)
        position = argv.index("--route")
        del argv[position:position + 2]
        with mock.patch.object(sys, "argv", argv), contextlib.redirect_stderr(io.StringIO()), \
             self.assertRaises(SystemExit):
            self.launcher.main()
        self.assertFalse(output.exists())

    def test_each_route_requires_matching_echoes_and_fixed_fixture_support(self):
        for route in self.launcher.ROUTES:
            with self.subTest(route=route):
                code, receipt, calls, _, _ = self.run_case(route=route)
                self.assertEqual(code, 0)
                self.assertTrue(receipt["accepted"])
                self.assertEqual(receipt["route"], route)
                self.assertEqual(receipt["requested_route"], route)
                self.assertEqual(receipt["effective_route"], route)
                self.assertEqual(calls[0][calls[0].index("--route") + 1], route)
                fixture = receipt["steps"]["semantics"]["native_result"]["fixed_fixture_setup"]
                self.assertIs(fixture["cpu_ready_arena_replay_supported"], route == "compatibility")
                self.assertIs(fixture["direct_chunk_slot_replay_supported"], route == "production-slot")
                for field in ("requested_route", "effective_route"):
                    for value in (None, "compatibility" if route == "production-slot" else "production-slot"):
                        native = self.native(route=route)
                        native[field] = value
                        code, rejected, _, _, _ = self.run_case(native, route=route)
                        self.assertEqual(code, 1)
                        self.assertFalse(rejected["accepted"])
                for field, value in (("cpu_ready_arena_replay_supported", route != "compatibility"),
                                     ("direct_chunk_slot_replay_supported", route != "production-slot")):
                    native = self.native(route=route)
                    native["fixed_fixture_setup"][field] = value
                    code, rejected, _, _, _ = self.run_case(native, route=route)
                    self.assertEqual(code, 1)
                    self.assertFalse(rejected["accepted"])

    def test_actual_route_coverage_allows_mixed_production_but_rejects_all_fallback(self):
        native = self.native(route="production-slot")
        coverage = native["execution"]["route_coverage"]
        coverage.update(direct_draws=1, compatibility_draws=1, compatibility_batches=1, fallback_draws=1)
        code, receipt, _, _, _ = self.run_case(native, route="production-slot")
        self.assertEqual(code, 0)
        self.assertEqual(receipt["route_coverage"]["direct_draws"], 1)
        self.assertEqual(receipt["route_coverage"]["fallback_draws"], 1)
        coverage.update(direct_leases=0, direct_draws=0, compatibility_draws=2, fallback_draws=2)
        code, receipt, _, _, _ = self.run_case(native, route="production-slot")
        self.assertEqual(code, 1)
        self.assertFalse(receipt["accepted"])
        self.assertEqual(receipt["failure"], "requested-direct-route-not-exercised")
        self.assertTrue(receipt["steps"]["semantics"]["native_result"]["complete"])
        self.assertEqual(receipt["steps"]["semantics"]["native_result"]["execution"]["route_coverage"]["compatibility_draws"], 2)
        self.assertTrue(receipt["loaded_images_stable"])

    def test_route_counter_evidence_requires_true_flags_valid_echoes_and_draw_conservation(self):
        for route in self.launcher.ROUTES:
            changes = [lambda c, field=field: c.update({field: False})
                       for field in ("policy_selected", "observed", "monotonic", "complete")]
            changes += [lambda c: c.update(requested_route="unknown"),
                        lambda c: c.update(effective_route=None),
                        lambda c: c.update(direct_draws=True),
                        lambda c: c.update(compatibility_draws=3),
                        lambda c: c.update(compatibility_unbatched_draws=3),
                        lambda c: c.update(compatibility_batches=3),
                        lambda c: c.update(fallback_draws=3),
                        lambda c: c.pop("direct_leases")]
            if route == "production-slot":
                changes.append(lambda c: c.update(direct_leases=0))
            else:
                changes += [lambda c: c.update(direct_leases=1), lambda c: c.update(fallback_draws=1)]
            for change in changes:
                native = self.native(route=route)
                change(native["execution"]["route_coverage"])
                code, receipt, _, _, _ = self.run_case(native, route=route)
                self.assertEqual(code, 1)
                self.assertFalse(receipt["accepted"])

    def test_selected_pe_contract_needs_authenticated_manifest_attestation(self):
        contract = "pe-present-reservation-v1"
        code, receipt, calls, _, _ = self.run_case(contract=contract)
        self.assertEqual(code, 1)
        self.assertFalse(receipt["accepted"])
        self.assertEqual(calls, [])
        self.manifest.update(producer={"path": "dxmt9-pe-render-tape"}, stage="production-capture")
        self.manifest["scope"].update(production_capture=True, source_oracle=True, output_oracle=True)
        self.save_manifest()
        code, receipt, calls, _, _ = self.run_case(contract=contract)
        self.assertEqual(code, 0)
        self.assertEqual(calls[0][calls[0].index("--producer-contract") + 1], contract)
        self.assertFalse(receipt["steps"]["semantics"]["native_result"]["native_producer_attestation_authenticated"])

    def test_unsafe_or_missing_authority_and_semantic_flags_fail_closed(self):
        for section, names, good in (
                (None, self.launcher.BROAD_FALSE, False),
                (None, ("diagnostic", "complete", "fixture_teardown_complete", "authority_scope_valid"), True),
                ("logical", ("requested", "admitted", "accepted", "complete", "legal_candidate_nooverwrite_contract",
                             "conditional_on_legal_nooverwrite", "logical_stream_complete"), True),
                ("logical", self.launcher.SEMANTIC_FALSE, False)):
            for name in names:
                for missing in (False, True):
                    with self.subTest(section=section, flag=name, missing=missing):
                        native = self.native()
                        owner = native if section is None else native["execution"]["logical_semantics"]
                        if missing:
                            del owner[name]
                        else:
                            owner[name] = not good
                        code, receipt, calls, _, _ = self.run_case(native)
                        self.assertEqual(code, 1)
                        self.assertFalse(receipt["accepted"])
                        self.assertEqual(len(calls), 1)

    def test_malformed_echo_authentication_and_conservation_are_rejected(self):
        changes = (
            lambda n: n.update(schema="dxmt9.controlled-cpu-v4-inventory.v1"),
            lambda n: n.update(mode="controlled-cpu-v4-inventory"),
            lambda n: n.update(buffer_preservation_contract="none"),
            lambda n: n.update(producer_contract="pe-present-reservation-v1"),
            lambda n: n.update(requested_route="production-slot"),
            lambda n: n.update(effective_route="production-slot"),
            lambda n: n.update(iterations=True),
            lambda n: n["sha256"].update(actual_plan="2" * 64),
            lambda n: n["sha256"].update(events="2" * 64),
            lambda n: n["authentication"].update(pair_valid=False),
            lambda n: n["action_witness"].update(queue=104),
            lambda n: n["fixed_fixture_setup"].update(direct_chunk_slot_replay_supported=True),
            lambda n: n["execution"]["consumer"].update(acknowledged=0),
            lambda n: n["execution"]["provenance"].update(raw_ack_coverage_complete=False),
            lambda n: n["execution"]["logical_semantics"].update(source_rows_qualified=1),
            lambda n: n["execution"]["logical_semantics"].update(original_items=4),
            lambda n: n["execution"]["logical_semantics"].update(commands=3),
            lambda n: n["execution"]["logical_semantics"].update(digest="1" * 64),
            lambda n: n["execution"]["logical_semantics"].update(digest_algorithm="sha256"),
            lambda n: n["execution"]["sources"][0]["commands"][0].update(draw_count=1),
            lambda n: n["execution"]["sources"][0]["commands"][0].update(physical_command=1),
            lambda n: n["execution"]["counters"].update(resources_released=0),
            lambda n: n["execution"]["query_projection"].update(replayed_polls=0),
            lambda n: n["execution"]["source_row_observation"].update(future_preservation=True),
        )
        for change in changes:
            native = self.native()
            change(native)
            code, receipt, _, _, _ = self.run_case(native)
            self.assertEqual(code, 1)
            self.assertFalse(receipt["accepted"])

    def test_each_original_receipt_needs_unique_locator_serialization_and_scoped_bytes(self):
        for change in (
                lambda rows: rows[0].update(serialized=False),
                lambda rows: rows[0].update(digest=""),
                lambda rows: rows[1].update(record=copy.deepcopy(rows[0]["record"])),
                lambda rows: rows.reverse(),
                lambda rows: rows[0]["record"].update(event_ordinal=True),
                lambda rows: rows[0].update(failure="serializer-failed"),
                lambda rows: rows[0].update(physical_command_index=3),
                lambda rows: rows[0].update(command_kind=9),
                lambda rows: rows[0].update(identity_command_kind=2),
                lambda rows: rows[0].pop("identity_command_kind"),
                lambda rows: rows[1].update(subdraw_index=0),
                lambda rows: rows[0].pop("source_rows_required"),
                lambda rows: rows[0]["source_rows"].update(status="batch-rejected"),
                lambda rows: rows[0]["source_rows"].update(qualified=False),
                lambda rows: rows[0]["source_rows"].update(proved=False),
                lambda rows: rows[0]["source_rows"].update(proved_bytes=63),
                lambda rows: rows[0]["source_rows"].update(completed_sequence=1)):
            native = self.native()
            change(native["execution"]["logical_semantics"]["receipts"])
            code, receipt, _, _, _ = self.run_case(native)
            self.assertEqual(code, 1)
            self.assertFalse(receipt["accepted"])

    def test_core_command_kinds_require_explicit_matching_identity_translation(self):
        for core_kind, identity_kind in self.launcher.CORE_TO_IDENTITY_COMMAND_KIND.items():
            native = self.native()
            if core_kind == 0:
                receipt = native["execution"]["logical_semantics"]["receipts"][0]
                command = native["execution"]["sources"][0]["commands"][0]
            else:
                receipt = native["execution"]["logical_semantics"]["receipts"][-1]
                command = native["execution"]["sources"][0]["commands"][-1]
            receipt.update(command_kind=core_kind, identity_command_kind=identity_kind)
            command["kind"] = identity_kind
            code, result, _, _, _ = self.run_case(native)
            self.assertEqual(code, 0)
            self.assertTrue(result["accepted"])
            receipt["identity_command_kind"] = core_kind
            code, result, _, _, _ = self.run_case(native)
            self.assertEqual(code, 1)
            self.assertFalse(result["accepted"])

    def test_reauthentication_catches_mutated_bundle_binary_and_loaded_images(self):
        original_events = (self.bundle / "events.bin").read_bytes()
        for kind in ("bundle", "binary", "image"):
            with self.subTest(kind=kind):
                native = self.native()
                if kind == "image":
                    image = self.root / "winemetal_mock.so"
                    image.write_bytes(b"original")
                    native["loaded_images"].append({"path": str(image), "bytes": 8,
                                                    "sha256": hashlib.sha256(b"original").hexdigest()})
                    mutate = lambda: image.write_bytes(b"modified")
                elif kind == "bundle":
                    mutate = lambda: (self.bundle / "events.bin").write_bytes(b"changed")
                else:
                    mutate = lambda: self.binary.write_bytes(b"different executable\n")
                code, receipt, _, _, _ = self.run_case(native, mutate=mutate)
                self.assertEqual(code, 1)
                self.assertFalse(receipt["accepted"])
                self.assertIn("reauthentication_failure", receipt)
                (self.bundle / "events.bin").write_bytes(original_events)
                self.binary.write_bytes(self.binary_bytes)

    def test_loaded_image_evidence_must_be_complete_unique_bounded_and_authentic(self):
        for change in (
                lambda n: n.pop("loaded_images"),
                lambda n: n.update(loaded_images=[]),
                lambda n: n["loaded_images"].append(copy.deepcopy(n["loaded_images"][0])),
                lambda n: n["loaded_images"][0].update(path="relative-image.so"),
                lambda n: n["loaded_images"][0].update(bytes=True),
                lambda n: n["loaded_images"][0].update(sha256="f" * 64)):
            native = self.native()
            change(native)
            code, receipt, _, _, _ = self.run_case(native)
            self.assertEqual(code, 1)
            self.assertFalse(receipt["loaded_images_stable"])

    def test_child_rejections_timeouts_and_malformed_json_preserve_owned_evidence(self):
        responses = ((self.native(), 4), (b'{"schema":1,"schema":2}', 0),
                     (b'{"value":NaN}', 0), (b'[]', 0),
                     (subprocess.TimeoutExpired("mock", 10), 0))
        for response, returncode in responses:
            code, receipt, calls, output, _ = self.run_case(response, returncode)
            self.assertEqual(code, 1)
            self.assertFalse(receipt["accepted"])
            self.assertEqual(len(calls), 1)
            self.assertTrue((output / "semantics.stdout").stat().st_size)
            self.assertEqual((output / "semantics.stderr").read_bytes(), b"mock child diagnostics\n")
            self.assertTrue(receipt["bundle_reauthenticated"])
        native = self.native()
        native["failure"] = "native-failure-preserved"
        native["loaded_images"] = []
        code, receipt, _, _, _ = self.run_case(native, 4)
        self.assertEqual(code, 1)
        self.assertIn("exit code 4", receipt["failure"])
        self.assertIn("reauthentication_failure", receipt)
        self.assertEqual(receipt["steps"]["semantics"]["native_result"]["failure"], "native-failure-preserved")

    def test_shared_deadline_and_result_byte_cap_fail_closed_after_child(self):
        expired = False
        original_guard = self.launcher.auth.require_time

        def guard(deadline):
            if expired:
                raise self.launcher.Rejected("shared diagnostic deadline exhausted")
            return original_guard(deadline)

        def expire():
            nonlocal expired
            expired = True

        with mock.patch.object(self.launcher.auth, "require_time", side_effect=guard):
            code, receipt, calls, output, _ = self.run_case(mutate=expire)
        self.assertEqual(code, 1)
        self.assertFalse(receipt["accepted"])
        self.assertEqual(len(calls), 1)
        self.assertIn("deadline", receipt["failure"])
        self.assertTrue((output / "semantics.stdout").stat().st_size)
        with mock.patch.object(self.launcher.auth, "MAX_RESULT_JSON_BYTES", 8):
            code, receipt, calls, output, _ = self.run_case()
        self.assertEqual(code, 1)
        self.assertEqual(len(calls), 1)
        self.assertFalse(receipt["accepted"])
        self.assertIn("byte cap", receipt["failure"])
        self.assertGreater((output / "semantics.stdout").stat().st_size, 8)

    def test_input_authentication_and_action_witness_reject_before_child(self):
        original = copy.deepcopy(self.manifest)
        response = self.native()
        for component in ("events", "identity", "execution_topology", "source_oracle", "output_oracle", "action_profile"):
            self.manifest = copy.deepcopy(original)
            self.manifest["components"][component]["sha256"] = "f" * 64
            self.save_manifest()
            code, receipt, calls, _, _ = self.run_case(response)
            self.assertEqual(code, 1)
            self.assertFalse(receipt["accepted"])
            self.assertEqual(calls, [])
            self.assertEqual(receipt["steps"]["semantics"]["status"], "skipped")
        self.manifest = copy.deepcopy(original)
        del self.manifest["components"]["action_profile"]
        (self.bundle / "action_profile.bin").unlink()
        self.manifest["scope"] = {}
        self.save_manifest()
        code, receipt, calls, _, _ = self.run_case(response)
        self.assertEqual(code, 1)
        self.assertEqual(calls, [])
        self.assertFalse(receipt["accepted"])


if __name__ == "__main__":
    unittest.main()
