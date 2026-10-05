#!/usr/bin/env python3
"""CPU-only launcher regressions; native execution/evidence is always mocked."""

from __future__ import annotations

import contextlib
import copy
import hashlib
import importlib.util
import io
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import time
import types
import unittest
from unittest import mock


class RepeatNativeFixture(dict):
    """Full native files stay separate from the compact mocked stdout manifest."""

    def refresh(self):
        self.native_files = {}
        for row, native in zip(self["iterations"], self.results):
            raw = json.dumps(native).encode()
            self.native_files[row["result_path"]] = raw
            row.update(result_bytes=len(raw), result_sha256=hashlib.sha256(raw).hexdigest())


class CaptureDiagnosticTest(unittest.TestCase):
    def setUp(self):
        path = pathlib.Path(__file__).with_name("run_render_tape_v4_capture_diagnostic.py")
        spec = importlib.util.spec_from_file_location("capture_diagnostic_tested", path)
        assert spec and spec.loader
        self.launcher = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.launcher)
        self.shared = self.launcher.load_bundle_tool()
        temporary = tempfile.TemporaryDirectory(prefix="dxmt9-v4-diagnostic-test-")
        self.addCleanup(temporary.cleanup)
        self.root = pathlib.Path(temporary.name).resolve()
        self.bundle = self.root / "bundle"
        self.bundle.mkdir()
        self.binary = self.root / "mock-native"
        self.binary.write_bytes(b"mocked subprocess only\n")
        self.binary.chmod(0o700)
        self.binary_bytes = self.binary.read_bytes()
        self.runtime_images = []
        for path in (self.binary, self.root / "libdxmt9_native.dylib", self.root / "winemetal_dxmt9.so"):
            if path != self.binary:
                path.write_bytes(f"mocked loaded module {path.name}\n".encode())
            raw = path.read_bytes()
            self.runtime_images.append({"path": str(path), "bytes": len(raw),
                                        "sha256": hashlib.sha256(raw).hexdigest()})
        # A loader-only dispatch prefix, not a native-valid V4 capture or query.
        identity = self.shared.IDENTITY_DISPATCH_PREFIX.pack(self.shared.IDENTITY_MAGIC, 4, 264).ljust(264, b"\0")
        components = {"blobs": []}
        for name, (filename, _option) in self.launcher.COMPONENTS.items():
            raw = identity if name == "identity" else b"fixture"
            (self.bundle / filename).write_bytes(raw)
            components[name] = {"path": filename, "bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}
        components["identity"]["schema"] = self.shared.IDENTITY_V4_SCHEMA
        components["execution_topology"]["schema"] = self.shared.EXECUTION_TOPOLOGY_SCHEMA
        self.manifest = {"schema": self.shared.SCHEMA, "profile": "frame-tape", "components": components}
        self.save_manifest()
        self.plan = "1" * 64
        self.count = 0

    def save_manifest(self):
        (self.bundle / "manifest.json").write_text(json.dumps(self.manifest), encoding="utf-8")

    def add_production_pe_provenance(self):
        self.manifest["producer"] = {"path": "dxmt9-pe-render-tape"}
        self.manifest["stage"] = "production-capture"
        self.manifest["scope"] = {"production_capture": True, "output_oracle": True, "source_oracle": True}
        self.save_manifest()

    def add_action_profile(self, profile=1, revision=0):
        token, epoch, queue = 101, 102, 103
        identity = self.shared.IDENTITY_V4_BINDING_PREFIX.pack(
            self.shared.IDENTITY_MAGIC, token, 4, 264, epoch, queue).ljust(264, b"\0")
        (self.bundle / "identity.bin").write_bytes(identity)
        component = self.manifest["components"]["identity"]
        component.update(bytes=len(identity), sha256=hashlib.sha256(identity).hexdigest())
        components = self.manifest["components"]
        raw = self.shared.ACTION_PROFILE_HEADER.pack(
            self.shared.ACTION_PROFILE_MAGIC, 1, self.shared.ACTION_PROFILE_HEADER.size,
            1, 1, profile, profile, profile, 0, token, epoch, queue, revision, revision,
            *(components[name]["bytes"] for name in ("events", "identity", "execution_topology")),
            *(bytes.fromhex(components[name]["sha256"])
              for name in ("events", "identity", "execution_topology")))
        (self.bundle / self.shared.ACTION_PROFILE_NAME).write_bytes(raw)
        components["action_profile"] = {"path": self.shared.ACTION_PROFILE_NAME,
                                        "schema": self.shared.ACTION_PROFILE_SCHEMA,
                                        "bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}
        self.manifest.setdefault("scope", {}).update(
            capture_action_profile="queue-observed", action_profile_stable_exact=(profile == 1 and revision == 0))
        self.save_manifest()

    def native(self, replay=False, producer_contract="none", publication_mode="exact-source-diagnostic",
               capacity_policy=None, route="production-slot", draw_command_limit=0):
        semantic = publication_mode == "semantic-mapping"
        policy = copy.deepcopy(capacity_policy) if capacity_policy is not None else self.launcher.make_capacity_policy(0, 0)
        result = {"schema": self.launcher.NATIVE_SCHEMA, "diagnostic": True,
                  "policy_qualified": False, "timing_eligible": False,
                  "producer_contract": producer_contract,
                  "publication_mode": publication_mode,
                  "semantic_mapping_qualified": replay and semantic,
                  "exact_source_diagnostic_complete": replay and not semantic,
                  "status": "complete" if replay else "inspection-complete",
                  "plan_sha256": self.plan, "execution_count": int(replay),
                  "device_created": replay, "qualified": replay,
                  "loaded_images": copy.deepcopy(self.runtime_images),
                  "capacity_policy": policy,
                  "replay_policy": self.launcher.make_replay_policy(
                      route if replay else "inspection", policy, draw_command_limit),
                  "effective_stage_environment": self.launcher.expected_stage_environment(
                      policy, draw_command_limit) if replay else {}}
        if semantic:
            result["action_profile"] = {"authenticated": True, "stable_exact": True}
        result.update({gate: replay for gate in self.launcher.GATES})
        if replay:
            result.update(expected_plan_sha256=self.plan, replay_route=route)
            result["path_counters"] = {"observed": True, "monotonic": True,
                                       "direct_leases": 2 if route == "production-slot" else 0,
                                       "direct_lease_draws": 4 if route == "production-slot" else 0,
                                       "compatibility_draws": 6 if route == "production-slot" else 10}
            result["readback_artifacts"] = {"requested": True, "attempted": True,
                "directory": str(self.root / f"result-{self.count + 1}"), "failure": "none"}
            for name in ("source", "output"):
                component = self.manifest["components"][f"{name}_oracle"]
                result["readback_artifacts"][name] = {
                    "attempted": True, "written": True, "bytes": component["bytes"],
                    "written_bytes": component["bytes"], "sha256": component["sha256"], "failure": "none"}
        return result

    def timing_measurements(self, *, prepared_observed=True, runtime_observed=True, monotonic=True, version=1):
        prepared = {"observed": prepared_observed,
            "scope": "owned-input-preparation-and-prepared-replay-phases;excludes-file-loading-factory-creation-device-teardown",
            "zero_semantics": "not-entered-or-below-clock-resolution",
            "prepare_wall_scope": ("owned-tape-blob-copy-general-provider-preflight-and-immutable-v4-preparation"
                                   if version == 2 else "owned-tape-blob-copy-and-provider-preflight"),
            "resource_setup_scope": "object-bootstrap-and-mutations-including-synchronous-drains",
            "chunk_replay_scope": "provider-import-resolve-dispatch-including-synchronous-drains;not-encode-thread",
            "readback_drain_scope": "flush-completion-readback-format-packing-digest;not-gpu-only",
            "cleanup_scope": "provider-flush-and-release;not-device-teardown",
            "reset_scope": "fresh-device-no-warm-reset",
            "replay_wall_scope": "replay-general-plan-after-policy-admission-and-reset;not-entire-provider-call"}
        for index, field in enumerate(("prepare_wall_ns", "reset_wall_ns", "resource_setup_ns",
                "chunk_replay_ns", "readback_drain_ns", "cleanup_ns", "replay_wall_ns")):
            prepared[field] = 100 + index if prepared_observed else None
        runtime = {"observed": runtime_observed, "monotonic": monotonic,
            "scope": "before-fresh-factory-create-through-after-device-factory-teardown",
            "zero_semantics": "no-recorded-interval;gated-counter-zero-does-not-prove-no-work",
            "prepared_serial_build_gate": "DXMT9_PREPARED_BUILD_TIMING=1-required",
            "resource_mark_scope": "nested-in-prepare-slot",
            "prepared_serial_build_scope": "prepared-serial-read-planning-plus-resource-marking;not-framegraph-planning",
            "encode_chunk_scope": "actual-encode-thread-inclusive-render-pass-setup-pso-work-and-metal-encoding;not-encoder-only"}
        for index, field in enumerate(("prepare_slot_wall_ns", "resource_mark_wall_ns",
                "prepared_serial_build_wall_ns", "encode_chunk_wall_ns")):
            runtime[field] = 200 + index if runtime_observed and monotonic else None
        return {"schema": f"dxmt9.render_tape.v4_diagnostic_timing.v{version}",
                "authority": "diagnostic-service-cost-only",
                "clock": "std::chrono::steady_clock", "units": "nanoseconds",
                "thread_cpu_time": False, "additive": False, "overlapping_scopes": True,
                "prepared_phases": prepared, "runtime_snapshot_delta": runtime,
                "unavailable": {"framegraph_planning_ns": None, "render_pass_preparation_ns": None,
                    "encoder_only_ns": None, "gpu_execution_ns": None,
                    "gpu_completion_wait_ns": None, "readback_only_ns": None}}

    def native_with_timing(self, replay=False, timing_version=1, **kwargs):
        result = self.native(replay, **kwargs)
        result.update(failure="none", stages={"actual_metal": replay,
                                            "fresh_device_teardown_complete": replay})
        result["timing_measurements"] = self.timing_measurements(
            prepared_observed=replay, runtime_observed=replay, monotonic=replay, version=timing_version)
        return result

    def query_timing_measurements(self, *, observed=True):
        timing = {"schema": "dxmt9.render_tape.v4_query_timing.v1",
            "authority": "diagnostic-service-cost-only",
            "clock": "std::chrono::steady_clock", "units": "nanoseconds",
            "thread_cpu_time": False, "additive": False, "observed": observed,
            "nesting_scope": "within-prepared-replay-wall;not-additive-with-other-service-scopes",
            "zero_semantics": "no-recorded-interval-or-below-clock-resolution;not-proof-of-no-query-work",
            "null_semantics": "unobserved-or-unqualified",
            "checkpoint_setup_scope": "checkpoint-event-create-end-snapshot-flush-poll-yield-and-receipt-bookkeeping;not-gpu-only",
            "transcript_scope": "captured-query-getdata-validation-dispatch-including-flush-snapshot-result-comparison-and-row-bookkeeping;excludes-query-issue-records",
            "oracle_scope": "post-transcript-deferred-query-obligation-snapshot-validation-promotion-flush-poll-yield-and-result-comparison;not-gpu-only",
            "checkpoint_setup_count_scope": "setup-end-calls-queue-publication-deltas-poll-calls-and-pending-poll-yield-retries;not-blocking-wait-time"}
        for index, field in enumerate(("checkpoint_setup_wall_ns", "transcript_wall_ns", "oracle_wall_ns",
                                      "checkpoint_setup_end_count", "checkpoint_setup_queue_publication_count",
                                      "checkpoint_setup_poll_count", "checkpoint_setup_pending_poll_yield_count")):
            timing[field] = 10 + index if observed else None
        return timing

    def native_with_query_timing(self, replay=False, **kwargs):
        native = self.native_with_timing(replay, **kwargs)
        native["query_actual"] = {field: replay for field in
                                  ("present", "actual_metal", "transcript_complete", "oracle_complete")}
        native["query_timing_measurements"] = self.query_timing_measurements(observed=replay)
        return native

    def v4_phase_timing_measurements(self, *, observed=True, version=1):
        timing = {"schema": f"dxmt9.render_tape.v4_phase_timing.v{version}",
            "authority": "diagnostic-service-cost-only",
            "clock": "std::chrono::steady_clock", "units": "nanoseconds",
            "thread_cpu_time": False, "additive": False, "observed": observed,
            "outer_plan_admission_scope": (
                "fresh-v4-execution-initialization-and-initial-setup-accounting-from-immutable-preparation;outside-prepared-replay-wall"
                if version == 2 else
                "outer-v4-plan-build-digest-check-execution-admit-and-initial-setup-validation;outside-prepared-replay-wall"),
            "replay_admission_scope": "replay-general-plan-v4-execution-admit;within-prepared-replay-wall",
            "pre_arm_checkpoint_scope": "begin-render-tape-checkpoint-pre-arm;within-prepared-replay-wall",
            "finish_scope": "v4-execution-finish-including-actual-qualification;within-prepared-replay-wall"}
        for index, field in enumerate(("outer_plan_admission_ns", "replay_admission_ns", "pre_arm_checkpoint_ns", "finish_ns")):
            timing[field] = 10 + index if observed else None
        return timing

    def native_with_v4_phase_timing(self, replay=False):
        native = self.native_with_timing(replay)
        native["v4_phase_timing_measurements"] = self.v4_phase_timing_measurements(observed=replay)
        return native

    def provider_call_timing_measurements(self, *, observed=True, version=1):
        return {"schema": f"dxmt9.render_tape.v4_provider_call_timing.v{version}",
            "authority": "diagnostic-service-cost-only",
            "clock": "std::chrono::steady_clock", "units": "nanoseconds",
            "thread_cpu_time": False, "additive": False, "observed": observed,
            "provider_call_scope": (
                "caller-full-expression-replayPreparedRenderTapeIdentityV4-including-return-local-destruction-result-assignment-and-existing-result-replacement;excludes-input-preparation-execution-option-setup-factory-creation-device-teardown"
                if version == 2 else
                "caller-full-expression-replayPreparedRenderTapeIdentity-including-return-local-destruction-result-assignment-and-existing-result-replacement;excludes-input-preparation-execution-option-setup-factory-creation-device-teardown"),
            "included_phases": ("fresh-v4-execution-initialization-initial-setup-accounting-and-executor-destruction"
                                if version == 2 else "outer-v4-admission-and-executor-destruction"),
            "nesting_scope": "includes-prepared-replay-wall-and-v4-phases;not-additive-with-other-service-scopes",
            "zero_semantics": "below-clock-resolution;not-proof-of-no-provider-work",
            "null_semantics": "unobserved-or-unqualified",
            "provider_call_wall_ns": 100 if observed else None}

    def native_with_provider_call_timing(self, replay=False):
        native = self.native_with_timing(replay)
        native["provider_call_timing_measurements"] = self.provider_call_timing_measurements(observed=replay)
        return native

    def immutable_v4_preparation(self, *, observed=True):
        return {"schema": "dxmt9.render_tape.v4_immutable_preparation.v1",
            "authority": "diagnostic-immutable-input-validity-only",
            "clock": "std::chrono::steady_clock", "units": "nanoseconds",
            "thread_cpu_time": False, "additive": False, "observed": observed,
            "preparation_scope": "one-off-provider-v4-source-plan-execution-template-and-initial-setup-plan-construction;excludes-owned-tape-blob-copy-general-preflight-independent-diagnostic-inspection-and-device-effects",
            "sharing_scope": "one-off-provider-preparation-reading-repeated-per-execution;not-per-iteration-work-or-an-aggregate",
            "zero_semantics": "below-clock-resolution;not-proof-of-no-preparation-work",
            "null_semantics": "unobserved-or-unqualified",
            "immutable_preparation_owned": True if observed else None,
            "preparation_count": 1 if observed else None,
            "reused_for_execution": True if observed else None,
            "fresh_execution_admission_count": 1 if observed else None,
            "prepared_handoff_consumed": True if observed else None,
            "v4_prepare_wall_ns": 50 if observed else None}

    def native_with_immutable_v4_preparation(self, replay=False):
        native = self.native_with_timing(replay, timing_version=2)
        native["v4_phase_timing_measurements"] = self.v4_phase_timing_measurements(observed=replay, version=2)
        native["provider_call_timing_measurements"] = self.provider_call_timing_measurements(observed=replay, version=2)
        native["immutable_v4_preparation"] = self.immutable_v4_preparation(observed=replay)
        native["admission_execution"] = self.admission_execution(observed=replay)
        return native

    def admission_execution(self, *, observed=True):
        return {"schema": "dxmt9.render_tape.v4_admission_execution.v1", "observed": observed,
                "admission_count": 1 if observed else None,
                "prepared_handoff_consumed": True if observed else None}

    def native_with_admission_execution(self, replay=False):
        native = self.native(replay)
        native.update(failure="none", stages={"actual_metal": replay,
                                             "fresh_device_teardown_complete": replay})
        native["admission_execution"] = self.admission_execution(observed=replay)
        return native

    def native_repeat(self, *, warmup=0, repeat=1, **kwargs):
        policy = self.launcher.make_repetition_policy(warmup, repeat, "fresh-device")
        prototype = self.native_with_timing(True, **kwargs)
        result = RepeatNativeFixture({key: copy.deepcopy(prototype[key]) for key in
            ("diagnostic", "policy_qualified", "timing_eligible", "status", "failure", "qualified",
             "producer_contract", "publication_mode", "plan_sha256", "expected_plan_sha256", "replay_route",
             "replay_policy", "capacity_policy", "effective_stage_environment", "loaded_images")})
        result.update(schema=self.launcher.REPEAT_NATIVE_SCHEMA, repetition_policy=policy,
                      prepare_count=1, execution_count=warmup + repeat, iterations=[])
        result.results = []
        result.readback_files = {}
        for ordinal in range(warmup + repeat):
            identity = self.launcher.iteration_identity(ordinal, policy)
            directory = self.launcher.iteration_directory(identity)
            native = self.native_with_timing(True, **kwargs)
            native["iteration"] = identity
            native["stages"]["fresh_device_created"] = True
            native["prepared"] = {"immutable_input_owned": True, "preflight_completed": True,
                                  "plan_reused": True, "prepare_count": 1}
            native["v4"] = {field: True for field in
                ("validated", "armed", "complete", "canonical_traversal_complete", "raw_ack_coverage_complete",
                 "actual_attempts_observed", "actual_lifecycle_complete", "actual_topology_complete")}
            native["readback_artifacts"]["directory"] = str(self.root / f"result-{self.count + 1}" / directory)
            result.results.append(native)
            result["iterations"].append({**identity, "result_path": directory + "/native.json",
                "readback_directory": directory, "status": "complete", "result_bytes": 0, "result_sha256": ""})
            for name in ("source", "output"):
                result.readback_files[directory + f"/replay-{name}.rgba"] = (self.bundle / f"{name}.rgba").read_bytes()
        result.refresh()
        return result

    def native_repeat_with_immutable_v4_preparation(self, *, warmup=0, repeat=1):
        result = self.native_repeat(warmup=warmup, repeat=repeat)
        for row in result.results:
            row["timing_measurements"] = self.timing_measurements(version=2)
            row["v4_phase_timing_measurements"] = self.v4_phase_timing_measurements(version=2)
            row["provider_call_timing_measurements"] = self.provider_call_timing_measurements(version=2)
            row["immutable_v4_preparation"] = self.immutable_v4_preparation()
            row["admission_execution"] = self.admission_execution()
        result.refresh()
        return result

    def run_case(self, responses, producer_contract=None, publication_mode=None, observe_encode_draws=False,
                 route="production-slot", expected_plan_digest=None, after_child=None,
                 slot_headroom_bytes=None, aggregate_headroom_bytes=None, draw_command_limit=None,
                 warmup=None, repeat=None, reset_policy=None):
        self.count += 1
        output = self.root / f"result-{self.count}"
        calls, answers = [], iter(responses)

        def child(argv, **kwargs):
            calls.append(argv)
            self.assertNotIn("env", kwargs)  # No rendering-policy override.
            response, code = next(answers)
            kwargs["stderr"].write(f"native-{len(calls)}-stderr\n".encode())
            if isinstance(response, subprocess.TimeoutExpired):
                kwargs["stdout"].write(b"partial native output")
                raise response
            if isinstance(response, RepeatNativeFixture):
                for relative, raw in response.native_files.items():
                    (output / relative).write_bytes(raw)
                for relative, raw in response.readback_files.items():
                    (output / relative).write_bytes(raw)
            if isinstance(response, dict) and "--readback-dir" in argv:
                artifacts = response.get("readback_artifacts")
                if isinstance(artifacts, dict):
                    directory = artifacts.get("directory")
                    # Reused native fixtures are attached to this mocked child
                    # output; deliberate foreign/missing directory evidence is
                    # not repaired. All other native fields remain untouched.
                    if (isinstance(directory, str) and pathlib.Path(directory).parent == self.root
                            and pathlib.Path(directory).name.startswith("result-")):
                        artifacts["directory"] = str(output)
                    for name in ("source", "output"):
                        artifact = artifacts.get(name)
                        if isinstance(artifact, dict) and artifact.get("written") is True:
                            oracle = self.bundle / self.manifest["components"][f"{name}_oracle"]["path"]
                            (output / f"replay-{name}.rgba").write_bytes(oracle.read_bytes())
            raw = response if isinstance(response, bytes) else json.dumps(response).encode()
            kwargs["stdout"].write(raw)
            if after_child is not None:
                after_child("inspect" if "--inspect-plan" in argv else "replay", response, output)
            return subprocess.CompletedProcess(argv, code)

        argv = ["diagnostic", "--bundle", str(self.bundle), "--diagnostic", str(self.binary),
                "--output-dir", str(output), "--route", route, "--timeout-seconds", "10"]
        if producer_contract is not None:
            argv += ["--producer-contract", producer_contract]
        if publication_mode is not None:
            argv += ["--publication-mode", publication_mode]
        if observe_encode_draws:
            argv += ["--observe-encode-draws"]
        if expected_plan_digest is not None:
            argv += ["--expected-plan-digest", expected_plan_digest]
        if slot_headroom_bytes is not None:
            argv += ["--slot-headroom-bytes", str(slot_headroom_bytes)]
        if aggregate_headroom_bytes is not None:
            argv += ["--aggregate-headroom-bytes", str(aggregate_headroom_bytes)]
        if draw_command_limit is not None:
            argv += ["--draw-command-limit", str(draw_command_limit)]
        for option, value in (("--warmup", warmup), ("--repeat", repeat), ("--reset-policy", reset_policy)):
            if value is not None:
                argv += [option, str(value)]
        with mock.patch.object(sys, "argv", argv), contextlib.redirect_stdout(io.StringIO()), \
             mock.patch.object(self.launcher, "load_bundle_tool", return_value=self.shared), \
             mock.patch.object(self.shared, "load_bundle", wraps=self.shared.load_bundle) as loaded, \
             mock.patch.object(self.launcher.subprocess, "run", side_effect=child):
            code = self.launcher.main()
        receipt = json.loads((output / "result.json").read_text(encoding="utf-8"))
        for name in ("inspect.stdout", "inspect.stderr", "replay.stdout", "replay.stderr"):
            self.assertTrue((output / name).is_file())
        return code, receipt, calls, output, loaded

    def semantic_observed_replay(self, route="production-slot"):
        result = self.native(True, publication_mode="semantic-mapping", route=route)
        result["path_counters"] = {"observed": True, "monotonic": True,
                                   "direct_leases": 2, "direct_lease_draws": 4,
                                   "compatibility_draws": 6, "compatibility_batches": 2,
                                   "compatibility_unbatched_draws": 0, "encode_chunks": 3, "gpu_errors": 0}
        result["encode_draw_witness"] = {
            "requested": True, "installed": True, "complete": True, "failure": "none",
            "original_buffer_observer": {"requested": True, "admitted": True,
                                         "complete": True, "closed": True, "failure": "none"},
            "original_context": {"complete": True, "failure": "none"},
            "original_binding_proof": {"observation_complete": True, "issued_receipts": 10,
                                       "qualified_draws": 10, "full_semantic_qualified": False,
                                       "full_shader_fetch_coverage_qualified": False,
                                       "future_preservation_qualified": False,
                                       "policy_qualified": False, "timing_eligible": False}}
        return result

    def test_semantic_observed_production_requires_actual_direct_work_and_allows_mixed_fallback(self):
        self.add_action_profile()
        mode = "semantic-mapping"
        replay = self.semantic_observed_replay()
        code, receipt, calls, _, _ = self.run_case(
            [(self.native(publication_mode=mode), 0), (replay, 0)],
            publication_mode=mode, observe_encode_draws=True)
        self.assertEqual(code, 0)
        self.assertTrue(receipt["accepted"])
        self.assertEqual(receipt["path_counters"], replay["path_counters"])
        self.assertGreater(receipt["path_counters"]["compatibility_draws"], 0)
        self.assertEqual(len(calls), 2)
        self.assertFalse(receipt["policy_qualified"])
        self.assertFalse(receipt["timing_eligible"])
        proof = receipt["encode_draw_witness"]["original_binding_proof"]
        for field in ("full_semantic_qualified", "full_shader_fetch_coverage_qualified",
                      "future_preservation_qualified", "policy_qualified", "timing_eligible"):
            self.assertIs(proof[field], False)
        for leases, draws in ((0, 0), (0, 4), (2, 0)):
            with self.subTest(leases=leases, draws=draws):
                failed = copy.deepcopy(replay)
                failed["path_counters"].update(direct_leases=leases, direct_lease_draws=draws,
                                              compatibility_draws=10 - draws)
                code, receipt, _, output, _ = self.run_case(
                    [(self.native(publication_mode=mode), 0), (failed, 0)],
                    publication_mode=mode, observe_encode_draws=True)
                self.assertEqual(code, 1)
                self.assertFalse(receipt["accepted"])
                self.assertEqual(receipt["failure"], "requested-direct-route-not-exercised")
                self.assertEqual(receipt["steps"]["replay"]["native_result"], failed)
                self.assertTrue(receipt["steps"]["replay"]["native_result"]["qualified"])
                self.assertEqual(json.loads((output / "replay.stdout").read_text()), failed)
                self.assertEqual(receipt["encode_draw_witness"], failed["encode_draw_witness"])

    def test_direct_candidate_counters_fail_closed_if_absent_unobserved_or_malformed(self):
        self.add_action_profile()
        mode = "semantic-mapping"
        failures = [None, {}, {"observed": False, "monotonic": True},
                    {"observed": True, "monotonic": False}]
        for field in ("direct_leases", "direct_lease_draws"):
            for value in (None, True, False, -1, 1.0, "4", 1 << 64):
                counters = self.semantic_observed_replay()["path_counters"]
                counters[field] = value
                failures.append(counters)
            counters = self.semantic_observed_replay()["path_counters"]
            del counters[field]
            failures.append(counters)
        for counters in failures:
            replay = self.semantic_observed_replay()
            replay["path_counters"] = counters
            code, receipt, _, _, _ = self.run_case(
                [(self.native(publication_mode=mode), 0), (replay, 0)],
                publication_mode=mode, observe_encode_draws=True)
            self.assertEqual(code, 1)
            self.assertFalse(receipt["accepted"])

    def test_direct_candidate_gate_does_not_change_other_modes_or_compatibility_acceptance(self):
        self.add_action_profile()
        for mode, observed, route in (("semantic-mapping", True, "compatibility"),
                                      ("semantic-mapping", False, "compatibility"),
                                      ("exact-source-diagnostic", True, "production-slot")):
            with self.subTest(mode=mode, observed=observed, route=route):
                replay = self.semantic_observed_replay(route)
                replay["publication_mode"] = mode
                replay["semantic_mapping_qualified"] = mode == "semantic-mapping"
                replay["exact_source_diagnostic_complete"] = mode != "semantic-mapping"
                replay["path_counters"].update(direct_leases=0, direct_lease_draws=0, compatibility_draws=10)
                code, receipt, _, _, _ = self.run_case(
                    [(self.native(publication_mode=mode), 0), (replay, 0)],
                    publication_mode=mode, observe_encode_draws=observed, route=route)
                self.assertEqual(code, 0)
                self.assertTrue(receipt["accepted"])
                self.assertFalse(receipt["public_v4_execution_enabled"])
                self.assertFalse(receipt["policy_qualified"])
                self.assertFalse(receipt["timing_eligible"])

    def test_semantic_observer_off_production_still_requires_actual_direct_draws_and_leases(self):
        self.add_action_profile()
        mode = "semantic-mapping"
        for counters in (None, {}, {"observed": False, "monotonic": True},
                         {"observed": True, "monotonic": False},
                         {"observed": True, "monotonic": True, "direct_leases": 0, "direct_lease_draws": 0},
                         {"observed": True, "monotonic": True, "direct_leases": 0, "direct_lease_draws": 4},
                         {"observed": True, "monotonic": True, "direct_leases": 2, "direct_lease_draws": 0},
                         {"observed": True, "monotonic": True, "direct_leases": True, "direct_lease_draws": 4}):
            with self.subTest(counters=counters):
                replay = self.native(True, publication_mode=mode)
                replay["path_counters"] = counters
                code, receipt, calls, output, _ = self.run_case(
                    [(self.native(publication_mode=mode), 0), (replay, 0)], publication_mode=mode)
                self.assertEqual(code, 1)
                self.assertFalse(receipt["accepted"])
                self.assertEqual(receipt["steps"]["replay"]["native_result"], replay)
                self.assertEqual(json.loads((output / "replay.stdout").read_bytes()), replay)
                self.assertFalse(receipt["observe_encode_draws"])
                self.assertTrue(all("--observe-encode-draws" not in call for call in calls))

        replay = self.native(True, publication_mode=mode)
        code, receipt, _, _, _ = self.run_case(
            [(self.native(publication_mode=mode), 0), (replay, 0)], publication_mode=mode)
        self.assertEqual(code, 0)
        self.assertTrue(receipt["accepted"])
        self.assertEqual(receipt["path_counters"]["direct_leases"], 2)
        self.assertEqual(receipt["path_counters"]["direct_lease_draws"], 4)

    def test_expected_original_plan_digest_matches_or_rejects_before_replay(self):
        code, receipt, calls, _, _ = self.run_case(
            [(self.native(), 0), (self.native(True), 0)], expected_plan_digest=self.plan)
        self.assertEqual(code, 0)
        self.assertTrue(receipt["accepted"])
        self.assertEqual(receipt["expected_plan_digest"], self.plan)
        self.assertEqual(len(calls), 2)
        code, receipt, calls, output, _ = self.run_case(
            [(self.native(), 0)], expected_plan_digest="2" * 64)
        self.assertEqual(code, 1)
        self.assertFalse(receipt["accepted"])
        self.assertIn("--expected-plan-digest", receipt["failure"])
        self.assertEqual(len(calls), 1)
        self.assertEqual(receipt["steps"]["replay"]["status"], "skipped")
        self.assertEqual((output / "replay.stdout").read_bytes(), b"")

    def test_capacity_exact_default_and_explicit_zero_preserve_every_existing_mode_and_route(self):
        self.add_action_profile()
        expected = self.launcher.make_capacity_policy(0, 0)
        for mode in self.launcher.PUBLICATION_MODES:
            for route in ("compatibility", "production-slot"):
                for explicit in (False, True):
                    with self.subTest(mode=mode, route=route, explicit=explicit):
                        replay = self.native(True, publication_mode=mode, route=route)
                        options = {"slot_headroom_bytes": 0, "aggregate_headroom_bytes": 0,
                                   "draw_command_limit": 0} if explicit else {}
                        code, receipt, calls, _, _ = self.run_case(
                            [(self.native(publication_mode=mode), 0), (replay, 0)],
                            publication_mode=mode, route=route, **options)
                        self.assertEqual(code, 0)
                        self.assertTrue(receipt["accepted"])
                        self.assertEqual(receipt["capacity_policy"], expected)
                        self.assertTrue(receipt["native_capacity_policy_valid"])
                        self.assertTrue(receipt["native_replay_policy_valid"])
                        self.assertEqual(receipt["replay_policy"], self.launcher.make_replay_policy(route, expected))
                        self.assertEqual(receipt["expected_stage_environment"], self.launcher.REPLAY_STAGE_ENVIRONMENT)
                        self.assertEqual(receipt["effective_stage_environment"], self.launcher.REPLAY_STAGE_ENVIRONMENT)
                        for call in calls:
                            self.assertEqual(call[call.index("--slot-headroom-bytes") + 1], "0")
                            self.assertEqual(call[call.index("--aggregate-headroom-bytes") + 1], "0")
                            self.assertEqual(call[call.index("--draw-command-limit") + 1], "0")
                        for field in ("policy_qualified", "timing_eligible", "public_v4_execution_enabled"):
                            self.assertIs(receipt[field], False)

    def test_bounded_semantic_production_capacity_is_forwarded_and_bound_without_changing_original_plan(self):
        self.add_action_profile()
        mode = "semantic-mapping"
        for slot, aggregate in ((self.launcher.MIN_SLOT_HEADROOM_BYTES, self.launcher.MIN_SLOT_HEADROOM_BYTES),
                                (5 * 1024 * 1024, 64 * 1024 * 1024),
                                (self.launcher.MAX_SLOT_HEADROOM_BYTES, self.launcher.MAX_AGGREGATE_HEADROOM_BYTES)):
            with self.subTest(slot=slot, aggregate=aggregate):
                policy = self.launcher.make_capacity_policy(slot, aggregate)
                inspect = self.native(publication_mode=mode, capacity_policy=policy)
                replay = self.native(True, publication_mode=mode, capacity_policy=policy)
                code, receipt, calls, output, _ = self.run_case([(inspect, 0), (replay, 0)],
                    publication_mode=mode, slot_headroom_bytes=slot, aggregate_headroom_bytes=aggregate,
                    expected_plan_digest=self.plan)
                self.assertEqual(code, 0)
                self.assertTrue(receipt["accepted"])
                self.assertTrue(receipt["native_capacity_policy_valid"])
                self.assertTrue(receipt["native_replay_policy_valid"])
                self.assertEqual(receipt["capacity_policy"], policy)
                self.assertEqual(receipt["replay_policy"], self.launcher.make_replay_policy("production-slot", policy))
                self.assertEqual(receipt["plan_sha256"], self.plan)
                self.assertEqual(inspect["effective_stage_environment"], {})
                self.assertEqual(receipt["effective_stage_environment"], self.launcher.expected_stage_environment(policy))
                self.assertEqual(receipt["expected_stage_environment"], receipt["effective_stage_environment"])
                for call in calls:
                    self.assertEqual(call[call.index("--slot-headroom-bytes") + 1], str(slot))
                    self.assertEqual(call[call.index("--aggregate-headroom-bytes") + 1], str(aggregate))
                    self.assertEqual(call[call.index("--draw-command-limit") + 1], "0")
                self.assertNotIn("--replay-route", calls[0])
                self.assertNotIn("--expected-plan-digest", calls[0])
                self.assertEqual(calls[1][calls[1].index("--expected-plan-digest") + 1], self.plan)
                self.assertEqual((output / "replay-source.rgba").read_bytes(), (self.bundle / "source.rgba").read_bytes())
                self.assertEqual((output / "replay-output.rgba").read_bytes(), (self.bundle / "output.rgba").read_bytes())
                for field in ("source_exact", "output_exact", "query_qualified", "pixels_qualified"):
                    self.assertIs(receipt["steps"]["replay"]["native_result"][field], True)
                self.assertFalse(receipt["policy_qualified"])
                self.assertFalse(receipt["timing_eligible"])

    def test_grouped_semantic_compatibility_endpoints_bind_inspection_replay_and_original_plan(self):
        self.add_action_profile()
        mode, route = "semantic-mapping", "compatibility"
        capacity = self.launcher.make_capacity_policy(0, 0)
        for limit in (3, 64):
            with self.subTest(limit=limit):
                inspect = self.native(publication_mode=mode, route=route, draw_command_limit=limit)
                replay = self.native(True, publication_mode=mode, route=route, draw_command_limit=limit)
                code, receipt, calls, output, _ = self.run_case([(inspect, 0), (replay, 0)],
                    publication_mode=mode, route=route, draw_command_limit=limit,
                    expected_plan_digest=self.plan)
                self.assertEqual(code, 0)
                self.assertTrue(receipt["accepted"])
                self.assertTrue(receipt["native_replay_policy_valid"])
                self.assertTrue(receipt["native_capacity_policy_valid"])
                self.assertEqual(receipt["replay_policy"], {
                    "schema": "dxmt9.render_tape.v4_replay_policy.v1", "route": route,
                    "draw_command_limit": limit, "slot_headroom_bytes": 0, "aggregate_headroom_bytes": 0})
                self.assertEqual(inspect["replay_policy"], dict(receipt["replay_policy"], route="inspection"))
                self.assertEqual(inspect["effective_stage_environment"], {})
                self.assertEqual(inspect["execution_count"], 0)
                self.assertIs(inspect["device_created"], False)
                self.assertEqual(receipt["capacity_policy"], capacity)
                self.assertEqual(receipt["plan_sha256"], self.plan)
                expected_environment = dict(self.launcher.REPLAY_STAGE_ENVIRONMENT,
                                            DXMT9_DRAW_CHUNK_COMMAND_LIMIT=str(limit))
                self.assertEqual(len(expected_environment), 7)
                self.assertEqual(receipt["expected_stage_environment"], expected_environment)
                self.assertEqual(receipt["effective_stage_environment"], expected_environment)
                for call in calls:
                    self.assertEqual(call[call.index("--draw-command-limit") + 1], str(limit))
                    self.assertEqual(call[call.index("--slot-headroom-bytes") + 1], "0")
                    self.assertEqual(call[call.index("--aggregate-headroom-bytes") + 1], "0")
                self.assertNotIn("--replay-route", calls[0])
                self.assertNotIn("--expected-plan-digest", calls[0])
                self.assertEqual(calls[1][calls[1].index("--replay-route") + 1], route)
                self.assertEqual(calls[1][calls[1].index("--expected-plan-digest") + 1], self.plan)
                for name in ("source", "output"):
                    self.assertEqual((output / f"replay-{name}.rgba").read_bytes(),
                                     (self.bundle / f"{name}.rgba").read_bytes())
                for field in self.launcher.GATES:
                    self.assertIs(receipt["steps"]["replay"]["native_result"][field], True)
                for field in ("policy_qualified", "timing_eligible", "public_v4_execution_enabled"):
                    self.assertIs(receipt[field], False)

    def test_grouping_invalid_values_routes_modes_and_mixed_axes_reject_before_effects(self):
        draw = "--draw-command-limit"
        flags = [[draw, value] for value in
                 ("-1", "+3", "1", "2", "65", "1.0", "0x3", " 3", "3 ", "", "٣", str(1 << 64))]
        flags.append([draw, "0", draw, "0"])
        selections = [("semantic-mapping", "compatibility", option) for option in flags]
        selections.extend((mode, route, [draw, "3"]) for mode, route in
                          (("semantic-mapping", "production-slot"),
                           ("exact-source-diagnostic", "compatibility"),
                           ("exact-source-diagnostic", "production-slot")))
        for route in ("compatibility", "production-slot"):
            selections.append(("semantic-mapping", route,
                [draw, "3", "--slot-headroom-bytes", str(1024 * 1024),
                 "--aggregate-headroom-bytes", str(64 * 1024 * 1024)]))
        for mode, route, options in selections:
            with self.subTest(mode=mode, route=route, options=options):
                output = self.root / "invalid-grouping-output"
                argv = ["diagnostic", "--bundle", str(self.root / "missing-bundle"),
                        "--diagnostic", str(self.root / "missing-native"), "--output-dir", str(output),
                        "--route", route, "--publication-mode", mode] + options
                with mock.patch.object(sys, "argv", argv), contextlib.redirect_stderr(io.StringIO()), \
                     mock.patch.object(self.launcher, "load_bundle_tool") as load, \
                     mock.patch.object(self.launcher.subprocess, "run") as child, \
                     mock.patch.object(pathlib.Path, "mkdir") as mkdir:
                    with self.assertRaises(SystemExit) as rejected:
                        self.launcher.main()
                self.assertEqual(rejected.exception.code, 2)
                load.assert_not_called()
                child.assert_not_called()
                mkdir.assert_not_called()
                self.assertFalse(output.exists())

    def test_replay_policy_helpers_keep_inspection_effect_free_and_reject_noncanonical_axes(self):
        exact = self.launcher.make_capacity_policy(0, 0)
        headroom = self.launcher.make_capacity_policy(5 * 1024 * 1024, 64 * 1024 * 1024)
        for capacity, limit in ((exact, 0), (exact, 3), (exact, 64), (headroom, 0)):
            policy = self.launcher.make_replay_policy("inspection", capacity, limit)
            self.assertEqual(policy["route"], "inspection")
            self.assertEqual(policy["draw_command_limit"], limit)
            self.launcher.validate_replay_policy({"replay_policy": policy}, policy)
            self.launcher.validate_stage_environment({"effective_stage_environment": {}},
                                                    replay=False, capacity_policy=capacity,
                                                    draw_command_limit=limit)
        for value in (None, False, True, "0", 0.0, -1, 1, 2, 65, 1 << 64):
            with self.subTest(value=value):
                with self.assertRaises(self.launcher.Rejected):
                    self.launcher.make_replay_policy("compatibility", exact, value)
                with self.assertRaises(self.launcher.Rejected):
                    self.launcher.expected_stage_environment(exact, value)
        for route, capacity, limit in (("unknown", exact, 0), (None, exact, 0),
                                       ("production-slot", exact, 3), ("compatibility", headroom, 0),
                                       ("inspection", headroom, 3)):
            with self.subTest(route=route, capacity=capacity, limit=limit):
                with self.assertRaises(self.launcher.Rejected):
                    self.launcher.make_replay_policy(route, capacity, limit)

    def test_native_full_replay_policy_identity_is_required_for_both_axes_and_stages(self):
        self.add_action_profile()
        mode = "semantic-mapping"
        candidates = (("compatibility", self.launcher.make_capacity_policy(0, 0), 3),
                      ("production-slot", self.launcher.make_capacity_policy(5 * 1024 * 1024,
                                                                            64 * 1024 * 1024), 0))
        for route, capacity, limit in candidates:
            for step in ("inspect", "replay"):
                valid = self.native(step == "replay", publication_mode=mode, route=route,
                                    capacity_policy=capacity, draw_command_limit=limit)
                faults = [None, {}, [], dict(valid["replay_policy"], unrequested=0),
                          dict(valid["replay_policy"], schema="other-schema"),
                          dict(valid["replay_policy"], route=route if step == "inspect" else "inspection"),
                          dict(valid["replay_policy"], draw_command_limit=4 if limit else 3)]
                for field in ("draw_command_limit", "slot_headroom_bytes", "aggregate_headroom_bytes"):
                    absent = dict(valid["replay_policy"])
                    del absent[field]
                    faults.append(absent)
                    for value in (True, False, str(valid["replay_policy"][field]),
                                  float(valid["replay_policy"][field]), -1):
                        faults.append(dict(valid["replay_policy"], **{field: value}))
                changed_capacity = dict(valid["replay_policy"],
                    slot_headroom_bytes=0 if capacity["slot_headroom_bytes"] else 1024 * 1024,
                    aggregate_headroom_bytes=0 if capacity["aggregate_headroom_bytes"] else 64 * 1024 * 1024)
                faults.append(changed_capacity)
                for policy in faults:
                    with self.subTest(route=route, step=step, policy=policy):
                        native = copy.deepcopy(valid)
                        if policy is None:
                            del native["replay_policy"]
                        else:
                            native["replay_policy"] = policy
                        prefix = [] if step == "inspect" else [(self.native(publication_mode=mode,
                            route=route, capacity_policy=capacity, draw_command_limit=limit), 0)]
                        code, receipt, calls, output, _ = self.run_case(prefix + [(native, 0)],
                            publication_mode=mode, route=route, draw_command_limit=limit,
                            slot_headroom_bytes=capacity["slot_headroom_bytes"],
                            aggregate_headroom_bytes=capacity["aggregate_headroom_bytes"])
                        self.assertEqual(code, 1)
                        self.assertFalse(receipt["accepted"])
                        self.assertFalse(receipt["native_replay_policy_valid"])
                        self.assertEqual(receipt["replay_policy"],
                                         self.launcher.make_replay_policy(route, capacity, limit))
                        self.assertEqual(len(calls), len(prefix) + 1)
                        self.assertEqual(receipt["steps"][step]["native_result"], native)
                        self.assertEqual(json.loads((output / f"{step}.stdout").read_bytes()), native)

    def test_grouped_compatibility_requires_actual_draw_and_zero_arena_environment_pins(self):
        self.add_action_profile()
        mode, route, limit = "semantic-mapping", "compatibility", 3
        for key in ("DXMT9_DRAW_CHUNK_COMMAND_LIMIT", "DXMT9_CHUNK_DRAW_PAYLOAD_ARENA_LIMIT_BYTES"):
            expected = "3" if key == "DXMT9_DRAW_CHUNK_COMMAND_LIMIT" else "0"
            for value in (None, "unexpected", int(expected), True, "0" if expected == "3" else "1"):
                with self.subTest(key=key, value=value):
                    replay = self.native(True, publication_mode=mode, route=route, draw_command_limit=limit)
                    if value is None:
                        del replay["effective_stage_environment"][key]
                    else:
                        replay["effective_stage_environment"][key] = value
                    code, receipt, _, output, _ = self.run_case(
                        [(self.native(publication_mode=mode, route=route, draw_command_limit=limit), 0), (replay, 0)],
                        publication_mode=mode, route=route, draw_command_limit=limit)
                    self.assertEqual(code, 1)
                    self.assertFalse(receipt["accepted"])
                    self.assertFalse(receipt["stage_environment_valid"])
                    self.assertIn("effective stage environment", receipt["failure"])
                    self.assertEqual(json.loads((output / "replay.stdout").read_bytes()), replay)

    def test_grouped_compatibility_keeps_original_plan_pixel_and_query_gates(self):
        self.add_action_profile()
        mode, route, limit = "semantic-mapping", "compatibility", 3
        inspect = self.native(publication_mode=mode, route=route, draw_command_limit=limit)
        code, receipt, calls, _, _ = self.run_case([(inspect, 0)], publication_mode=mode,
            route=route, draw_command_limit=limit, expected_plan_digest="2" * 64)
        self.assertEqual(code, 1)
        self.assertFalse(receipt["accepted"])
        self.assertEqual(len(calls), 1)
        self.assertEqual(receipt["steps"]["replay"]["status"], "skipped")
        for gate in self.launcher.GATES:
            with self.subTest(gate=gate):
                replay = self.native(True, publication_mode=mode, route=route, draw_command_limit=limit)
                replay[gate] = False
                code, receipt, _, _, _ = self.run_case(
                    [(self.native(publication_mode=mode, route=route, draw_command_limit=limit), 0), (replay, 0)],
                    publication_mode=mode, route=route, draw_command_limit=limit)
                self.assertEqual(code, 1)
                self.assertFalse(receipt["accepted"])
                self.assertEqual(receipt["steps"]["replay"]["native_result"], replay)

    def test_capacity_rejects_half_presence_invalid_bounds_spelling_and_unsupported_routes_before_effects(self):
        slot, aggregate = "--slot-headroom-bytes", "--aggregate-headroom-bytes"
        minimum = self.launcher.MIN_SLOT_HEADROOM_BYTES
        options = [[slot, "0"], [aggregate, "0"], [slot, str(minimum)], [aggregate, str(minimum)],
                   [slot, "0", aggregate, str(minimum)], [slot, str(minimum), aggregate, "0"],
                   [slot, str(minimum - 1), aggregate, str(minimum)],
                   [slot, str(self.launcher.MAX_SLOT_HEADROOM_BYTES + 1), aggregate, str(self.launcher.MAX_AGGREGATE_HEADROOM_BYTES)],
                   [slot, str(minimum), aggregate, str(self.launcher.MAX_AGGREGATE_HEADROOM_BYTES + 1)],
                   [slot, str(2 * minimum), aggregate, str(minimum)],
                   [slot, "0", slot, "0", aggregate, "0"], [slot, "0", aggregate, "0", aggregate, "0"]]
        for value in ("-1", "+1048576", " 1048576", "1.0", "1e6", "NaN", "True", "0x100000", ""):
            options.extend(([slot, value, aggregate, str(minimum)], [slot, str(minimum), aggregate, value]))
        selections = [("semantic-mapping", "production-slot", flags) for flags in options]
        selections.extend((mode, route, [slot, str(minimum), aggregate, str(minimum)])
                          for mode, route in (("semantic-mapping", "compatibility"),
                                              ("exact-source-diagnostic", "compatibility"),
                                              ("exact-source-diagnostic", "production-slot")))
        output = self.root / "invalid-capacity-output"
        for mode, route, flags in selections:
            with self.subTest(mode=mode, route=route, flags=flags):
                argv = ["diagnostic", "--bundle", str(self.bundle), "--diagnostic", str(self.binary),
                        "--output-dir", str(output), "--route", route, "--publication-mode", mode, *flags]
                with mock.patch.object(sys, "argv", argv), contextlib.redirect_stderr(io.StringIO()), \
                     mock.patch.object(self.launcher, "load_bundle_tool") as loaded, \
                     mock.patch.object(self.launcher.subprocess, "run") as child:
                    with self.assertRaises(SystemExit) as rejected:
                        self.launcher.main()
                self.assertEqual(rejected.exception.code, 2)
                loaded.assert_not_called()
                child.assert_not_called()
                self.assertFalse(output.exists())

    def test_capacity_helpers_reject_noninteger_and_noncanonical_native_policy_identity(self):
        default = self.launcher.make_capacity_policy(0, 0)
        for field in default:
            for value in (False, True, 0.0, "0", None, -1):
                with self.subTest(field=field, value=value):
                    invalid = dict(default, **{field: value})
                    with self.assertRaises(self.launcher.Rejected):
                        self.launcher.validate_capacity_policy({"capacity_policy": invalid}, default)
                    with self.assertRaises(self.launcher.Rejected):
                        self.launcher.expected_stage_environment(invalid)
        for invalid in (None, {}, [], dict(default, unrequested=0)):
            with self.subTest(invalid=invalid):
                with self.assertRaises(self.launcher.Rejected):
                    self.launcher.validate_capacity_policy({"capacity_policy": invalid}, default)

    def test_native_capacity_policy_identity_is_required_in_both_inspect_and_replay(self):
        self.add_action_profile()
        mode = "semantic-mapping"
        requested = self.launcher.make_capacity_policy(5 * 1024 * 1024, 64 * 1024 * 1024)
        different = self.launcher.make_capacity_policy(6 * 1024 * 1024, 64 * 1024 * 1024)
        for step in ("inspect", "replay"):
            for fault in ("absent", "empty", "extra", "bool", "string", "zero", "different", "half-zero"):
                with self.subTest(step=step, fault=fault):
                    native = self.native(step == "replay", publication_mode=mode, capacity_policy=requested)
                    if fault == "absent":
                        del native["capacity_policy"]
                    elif fault == "empty":
                        native["capacity_policy"] = {}
                    elif fault == "extra":
                        native["capacity_policy"]["unrequested"] = 1
                    elif fault == "bool":
                        native["capacity_policy"]["slot_headroom_bytes"] = True
                    elif fault == "string":
                        native["capacity_policy"]["slot_headroom_bytes"] = str(requested["slot_headroom_bytes"])
                    elif fault == "zero":
                        native["capacity_policy"] = self.launcher.make_capacity_policy(0, 0)
                    elif fault == "different":
                        native["capacity_policy"] = different
                    else:
                        native["capacity_policy"]["aggregate_headroom_bytes"] = 0
                    prefix = [] if step == "inspect" else [
                        (self.native(publication_mode=mode, capacity_policy=requested), 0)]
                    code, receipt, calls, output, _ = self.run_case(prefix + [(native, 0)], publication_mode=mode,
                        slot_headroom_bytes=requested["slot_headroom_bytes"],
                        aggregate_headroom_bytes=requested["aggregate_headroom_bytes"])
                    self.assertEqual(code, 1)
                    self.assertFalse(receipt["accepted"])
                    self.assertFalse(receipt["native_capacity_policy_valid"])
                    self.assertEqual(receipt["capacity_policy"], requested)
                    self.assertEqual(len(calls), len(prefix) + 1)
                    self.assertEqual(receipt["steps"][step]["native_result"], native)
                    self.assertEqual(json.loads((output / f"{step}.stdout").read_bytes()), native)

    def test_positive_capacity_requires_actual_decimal_environment_pins_and_original_plan(self):
        self.add_action_profile()
        mode = "semantic-mapping"
        policy = self.launcher.make_capacity_policy(5 * 1024 * 1024, 64 * 1024 * 1024)
        for key in ("DXMT9_DIRECT_SLOT_HEADROOM_BYTES", "DXMT9_DIRECT_SLOT_AGGREGATE_HEADROOM_BYTES"):
            for value in (None, "0", "unexpected", 5 * 1024 * 1024, True):
                with self.subTest(key=key, value=value):
                    replay = self.native(True, publication_mode=mode, capacity_policy=policy)
                    if value is None:
                        del replay["effective_stage_environment"][key]
                    else:
                        replay["effective_stage_environment"][key] = value
                    code, receipt, _, output, _ = self.run_case(
                        [(self.native(publication_mode=mode, capacity_policy=policy), 0), (replay, 0)],
                        publication_mode=mode, slot_headroom_bytes=policy["slot_headroom_bytes"],
                        aggregate_headroom_bytes=policy["aggregate_headroom_bytes"])
                    self.assertEqual(code, 1)
                    self.assertFalse(receipt["accepted"])
                    self.assertFalse(receipt["stage_environment_valid"])
                    self.assertIn("effective stage environment", receipt["failure"])
                    self.assertEqual(json.loads((output / "replay.stdout").read_bytes()), replay)
        code, receipt, calls, output, _ = self.run_case(
            [(self.native(publication_mode=mode, capacity_policy=policy), 0)],
            publication_mode=mode, slot_headroom_bytes=policy["slot_headroom_bytes"],
            aggregate_headroom_bytes=policy["aggregate_headroom_bytes"], expected_plan_digest="2" * 64)
        self.assertEqual(code, 1)
        self.assertFalse(receipt["accepted"])
        self.assertEqual(len(calls), 1)
        self.assertEqual((output / "replay.stdout").read_bytes(), b"")
        self.assertIn("--expected-plan-digest", receipt["failure"])

    def test_malformed_expected_plan_digest_is_rejected_before_artifacts_or_children(self):
        output = self.root / "invalid-plan-output"
        for digest in ("0" * 64, "A" * 64, "1" * 63, "1" * 65, "not-a-digest"):
            with self.subTest(digest=digest):
                argv = ["diagnostic", "--bundle", str(self.bundle), "--diagnostic", str(self.binary),
                        "--output-dir", str(output), "--route", "production-slot",
                        "--expected-plan-digest", digest]
                with mock.patch.object(sys, "argv", argv), contextlib.redirect_stderr(io.StringIO()), \
                     mock.patch.object(self.launcher.subprocess, "run") as child:
                    with self.assertRaises(SystemExit) as rejected:
                        self.launcher.main()
                self.assertEqual(rejected.exception.code, 2)
                child.assert_not_called()
                self.assertFalse(output.exists())

    def test_actual_native_stage_environment_pins_are_required_without_overriding_inheritance(self):
        for step in ("inspect", "replay"):
            expected = {} if step == "inspect" else self.launcher.REPLAY_STAGE_ENVIRONMENT
            values = [None, [], dict(expected, DXMT9_UNREQUESTED="1")]
            if step == "replay":
                for key in expected:
                    absent = dict(expected)
                    del absent[key]
                    values.extend((absent, dict(expected, **{key: "unexpected"})))
            else:
                values.append(dict(self.launcher.REPLAY_STAGE_ENVIRONMENT))
            for value in values:
                with self.subTest(step=step, value=value):
                    result = self.native(step == "replay")
                    result["effective_stage_environment"] = value
                    prefix = [] if step == "inspect" else [(self.native(), 0)]
                    code, receipt, calls, output, _ = self.run_case(prefix + [(result, 0)])
                    self.assertEqual(code, 1)
                    self.assertFalse(receipt["accepted"])
                    self.assertFalse(receipt["stage_environment_valid"])
                    self.assertIn("effective stage environment", receipt["failure"])
                    self.assertEqual(len(calls), len(prefix) + 1)
                    self.assertEqual(json.loads((output / f"{step}.stdout").read_bytes()), result)

    def test_every_native_loaded_image_inventory_is_independently_required_and_bounded(self):
        link = self.root / "image-symlink"
        link.symlink_to(self.binary)
        for step in ("inspect", "replay"):
            for fault in ("absent", "empty", "object", "entry", "count", "duplicate", "relative", "noncanonical",
                          "symlink", "size", "bool-size", "negative-size", "hash", "zero-hash", "uppercase-hash",
                          "missing-executable", "missing-provider", "missing-unix-module"):
                with self.subTest(step=step, fault=fault):
                    result = self.native(step == "replay")
                    images = result["loaded_images"]
                    if fault == "absent":
                        del result["loaded_images"]
                    elif fault == "empty":
                        result["loaded_images"] = []
                    elif fault == "object":
                        result["loaded_images"] = {}
                    elif fault == "entry":
                        images[0] = None
                    elif fault == "count":
                        result["loaded_images"] = images * (self.launcher.MAX_LOADED_IMAGES + 1)
                    elif fault == "duplicate":
                        images.append(copy.deepcopy(images[0]))
                    elif fault == "relative":
                        images[0]["path"] = self.binary.name
                    elif fault == "noncanonical":
                        images[0]["path"] = str(self.root) + "/./" + self.binary.name
                    elif fault == "symlink":
                        images[0]["path"] = str(link)
                    elif fault in ("size", "bool-size", "negative-size"):
                        images[0]["bytes"] = {"size": len(self.binary_bytes) + 1,
                                              "bool-size": True, "negative-size": -1}[fault]
                    elif fault in ("hash", "zero-hash", "uppercase-hash"):
                        images[0]["sha256"] = {"hash": "2" * 64, "zero-hash": "0" * 64,
                                               "uppercase-hash": "A" * 64}[fault]
                    else:
                        name = {"missing-executable": self.binary.name,
                                "missing-provider": "libdxmt9_native.dylib",
                                "missing-unix-module": "winemetal_dxmt9.so"}[fault]
                        result["loaded_images"] = [row for row in images if pathlib.Path(row["path"]).name != name]
                    prefix = [] if step == "inspect" else [(self.native(), 0)]
                    code, receipt, calls, output, _ = self.run_case(prefix + [(result, 0)])
                    self.assertEqual(code, 1)
                    self.assertFalse(receipt["accepted"])
                    self.assertFalse(receipt["loaded_images_stable"])
                    self.assertEqual(len(calls), len(prefix) + 1)
                    self.assertEqual(receipt["steps"][step]["native_result"], result)
                    self.assertEqual(json.loads((output / f"{step}.stdout").read_bytes()), result)

    def test_post_replay_executable_and_loaded_module_tampering_rejects_without_healing_evidence(self):
        for path in (self.binary, self.root / "libdxmt9_native.dylib", self.root / "winemetal_dxmt9.so"):
            original = path.read_bytes()
            with self.subTest(path=path.name):
                inspect, replay = self.native(), self.native(True)

                def tamper(step, _native, _output):
                    if step == "replay":
                        raw = bytearray(original)
                        raw[0] ^= 1
                        path.write_bytes(raw)

                try:
                    code, receipt, _, output, _ = self.run_case([(inspect, 0), (replay, 0)], after_child=tamper)
                    self.assertEqual(code, 1)
                    self.assertFalse(receipt["accepted"])
                    self.assertFalse(receipt["loaded_images_stable"])
                    self.assertIs(receipt["binary_reauthenticated"], path != self.binary)
                    self.assertEqual(receipt["steps"]["replay"]["native_result"], replay)
                    self.assertEqual(json.loads((output / "replay.stdout").read_bytes()), replay)
                    self.assertTrue(receipt["reauthentication_failures"])
                finally:
                    path.write_bytes(original)

    def test_post_replay_component_manifest_and_action_witness_tampering_reauthenticates_originals(self):
        self.add_action_profile()
        originals = {path: path.read_bytes() for path in self.bundle.iterdir() if path.is_file()}
        for filename in ("events.bin", "identity.bin", "execution_topology.bin", "source.rgba", "output.rgba",
                         "manifest.json", self.shared.ACTION_PROFILE_NAME):
            with self.subTest(filename=filename):
                replay = self.native(True)

                def tamper(step, _native, _output):
                    if step == "replay":
                        path = self.bundle / filename
                        if filename == "manifest.json":
                            path.write_bytes(originals[path] + b"\n")
                        else:
                            raw = bytearray(originals[path])
                            raw[-1] ^= 1
                            path.write_bytes(raw)

                try:
                    code, receipt, _, output, loaded = self.run_case(
                        [(self.native(), 0), (replay, 0)], after_child=tamper)
                    self.assertEqual(code, 1)
                    self.assertFalse(receipt["accepted"])
                    self.assertFalse(receipt["bundle_reauthenticated"])
                    self.assertEqual(loaded.call_count, 2)
                    if filename == self.shared.ACTION_PROFILE_NAME:
                        self.assertFalse(receipt["action_profile_reauthenticated"])
                    if filename in ("source.rgba", "output.rgba"):
                        self.assertFalse(receipt["readback_artifacts_valid"])
                    self.assertTrue(receipt["reauthentication_failures"])
                    self.assertEqual(receipt["steps"]["replay"]["native_result"], replay)
                    self.assertEqual(json.loads((output / "replay.stdout").read_bytes()), replay)
                finally:
                    for path, raw in originals.items():
                        path.write_bytes(raw)

    def test_readbacks_require_original_exact_bytes_size_regular_ownership_and_exclusive_links(self):
        for name in ("source", "output"):
            for fault in ("different", "short", "long", "missing", "symlink", "hardlink", "directory", "fifo"):
                with self.subTest(name=name, fault=fault):
                    replay = self.native(True)

                    def tamper(step, _native, output):
                        if step != "replay":
                            return
                        path, oracle = output / f"replay-{name}.rgba", self.bundle / f"{name}.rgba"
                        if fault in ("different", "short", "long"):
                            raw = oracle.read_bytes()
                            if fault == "different":
                                raw = raw[:-1] + bytes((raw[-1] ^ 1,))
                            else:
                                raw = raw[:-1] if fault == "short" else raw + b"!"
                            path.write_bytes(raw)
                        else:
                            path.unlink()
                            if fault == "symlink":
                                path.symlink_to(oracle)
                            elif fault == "hardlink":
                                os.link(oracle, path)
                            elif fault == "directory":
                                path.mkdir()
                            elif fault == "fifo":
                                os.mkfifo(path)

                    code, receipt, _, output, _ = self.run_case(
                        [(self.native(), 0), (replay, 0)], after_child=tamper)
                    self.assertEqual(code, 1)
                    self.assertFalse(receipt["accepted"])
                    self.assertTrue(receipt["bundle_reauthenticated"])
                    self.assertFalse(receipt["readback_artifacts_valid"])
                    self.assertIn("readback artifacts:", receipt["failure"])
                    self.assertEqual(receipt["steps"]["replay"]["native_result"], replay)
                    self.assertEqual(json.loads((output / "replay.stdout").read_bytes()), replay)

    def test_native_readback_metadata_cannot_replace_independent_artifact_admission(self):
        faults = [(None, "requested", False), (None, "attempted", False),
                  (None, "failure", "write-rejected"), (None, "directory", str(self.root / "foreign-output")),
                  (None, "directory", None)]
        for name in ("source", "output"):
            faults.extend((name, field, value) for field, value in (
                ("attempted", False), ("written", False), ("failure", "not-written"),
                ("bytes", 1), ("bytes", True), ("written_bytes", 1), ("written_bytes", True),
                ("sha256", "2" * 64), ("sha256", None)))
        for section, field, value in faults:
            with self.subTest(section=section, field=field, value=value):
                replay = self.native(True)
                target = replay["readback_artifacts"] if section is None else replay["readback_artifacts"][section]
                if value is None:
                    del target[field]
                else:
                    target[field] = value
                code, receipt, _, output, _ = self.run_case([(self.native(), 0), (replay, 0)])
                self.assertEqual(code, 1)
                self.assertFalse(receipt["accepted"])
                self.assertFalse(receipt["readback_artifacts_valid"])
                self.assertEqual(receipt["steps"]["replay"]["native_result"], replay)
                self.assertEqual(json.loads((output / "replay.stdout").read_bytes()), replay)

    def test_readback_artifact_uid_and_output_directory_identity_are_checked_independently(self):
        code, receipt, _, output, _ = self.run_case([(self.native(), 0), (self.native(True), 0)])
        self.assertEqual(code, 0)
        native = receipt["steps"]["replay"]["native_result"]
        directory = output.stat()
        identity = (directory.st_dev, directory.st_ino)
        wrong_identity = (directory.st_dev, directory.st_ino + 1)
        with self.assertRaisesRegex(self.launcher.Rejected, "owned output directory"):
            self.launcher.validate_readbacks(native, output, self.bundle, self.manifest,
                                            wrong_identity, time.monotonic() + 10)
        uid = os.geteuid()
        with mock.patch.object(self.launcher.os, "geteuid", return_value=uid + 1):
            with self.assertRaisesRegex(self.launcher.Rejected, "owned output directory"):
                self.launcher.validate_readbacks(native, output, self.bundle, self.manifest,
                                                identity, time.monotonic() + 10)

        original_fstat = os.fstat
        target = (output / "replay-source.rgba").stat()

        def wrong_owner(descriptor):
            info = original_fstat(descriptor)
            if (info.st_dev, info.st_ino) != (target.st_dev, target.st_ino):
                return info
            fields = ("st_dev", "st_ino", "st_size", "st_mtime_ns", "st_ctime_ns", "st_mode", "st_nlink")
            return types.SimpleNamespace(**{field: getattr(info, field) for field in fields}, st_uid=uid + 1)

        with mock.patch.object(self.launcher.os, "fstat", side_effect=wrong_owner):
            with self.assertRaisesRegex(self.launcher.Rejected, "regular owned"):
                self.launcher.validate_readbacks(native, output, self.bundle, self.manifest,
                                                identity, time.monotonic() + 10)

    def test_explicit_recorded_readback_directory_only_supports_authenticated_output_relocation(self):
        code, receipt, _, output, _ = self.run_case([(self.native(), 0), (self.native(True), 0)])
        self.assertEqual(code, 0)
        native = receipt["steps"]["replay"]["native_result"]
        original_native = copy.deepcopy(native)
        directory = output.stat()
        identity = (directory.st_dev, directory.st_ino)
        relocated = self.root / "relocated-result"
        output.rename(relocated)
        with self.assertRaisesRegex(self.launcher.Rejected, "ownership/status"):
            self.launcher.validate_readbacks(native, relocated, self.bundle, self.manifest,
                                            identity, time.monotonic() + 10)
        verified = self.launcher.validate_readbacks(native, relocated, self.bundle, self.manifest,
            identity, time.monotonic() + 10, recorded_directory=str(output))
        self.assertTrue(all(row["exact_original_bytes"] for row in verified.values()))
        with self.assertRaisesRegex(self.launcher.Rejected, "ownership/status"):
            self.launcher.validate_readbacks(native, relocated, self.bundle, self.manifest,
                identity, time.monotonic() + 10, recorded_directory=str(self.root / "not-the-recorded-directory"))
        self.assertEqual(native, original_native)

    def test_symlinked_manifest_or_components_are_rejected_before_native_execution(self):
        for filename in ("manifest.json", "events.bin", "source.rgba", "output.rgba"):
            with self.subTest(filename=filename):
                path = self.bundle / filename
                raw = path.read_bytes()
                target = self.root / f"outside-{filename}"
                target.write_bytes(raw)
                path.unlink()
                path.symlink_to(target)
                try:
                    code, receipt, calls, _, _ = self.run_case([])
                    self.assertEqual(code, 1)
                    self.assertFalse(receipt["accepted"])
                    self.assertEqual(calls, [])
                    self.assertIn("regular non-symlink", receipt["failure"])
                finally:
                    path.unlink()
                    path.write_bytes(raw)

    def test_first_native_failure_survives_later_input_binary_and_artifact_rejections(self):
        replay = dict(self.native(True), status="rejected", qualified=False, failure="native-original-failure")
        events = self.bundle / "events.bin"
        original_events = events.read_bytes()

        def tamper(step, _native, output):
            if step == "replay":
                self.binary.write_bytes(b"tampered executable\n")
                events.write_bytes(b"damaged")
                (output / "replay-output.rgba").write_bytes(b"corrupt")

        try:
            code, receipt, _, output, _ = self.run_case([(self.native(), 0), (replay, 17)], after_child=tamper)
            self.assertEqual(code, 1)
            self.assertEqual(receipt["failure"], "replay rejected with exit code 17")
            self.assertFalse(receipt["accepted"])
            self.assertFalse(receipt["bundle_reauthenticated"])
            self.assertFalse(receipt["binary_reauthenticated"])
            self.assertFalse(receipt["loaded_images_stable"])
            self.assertFalse(receipt["readback_artifacts_valid"])
            self.assertGreaterEqual(len(receipt["reauthentication_failures"]), 4)
            self.assertEqual(receipt["steps"]["replay"]["native_result"], replay)
            self.assertEqual(json.loads((output / "replay.stdout").read_bytes()), replay)
            self.assertEqual((output / "replay.stderr").read_bytes(), b"native-2-stderr\n")
        finally:
            self.binary.write_bytes(self.binary_bytes)
            events.write_bytes(original_events)

    def test_one_replay_is_diagnostic_only_and_public_gate_stays_closed(self):
        code, receipt, calls, output, loaded = self.run_case([(self.native(), 0), (self.native(True), 0)])
        self.assertEqual(code, 0)
        self.assertTrue(receipt["accepted"])
        self.assertNotIn("timing_measurements", receipt)
        self.assertNotIn("query_timing_measurements", receipt)
        self.assertNotIn("v4_phase_timing_measurements", receipt)
        self.assertNotIn("provider_call_timing_measurements", receipt)
        self.assertNotIn("immutable_v4_preparation", receipt)
        self.assertNotIn("admission_execution", receipt)
        for step in receipt["steps"].values():
            self.assertNotIn("timing_measurements", step["native_result"])
            self.assertNotIn("query_timing_measurements", step["native_result"])
            self.assertNotIn("v4_phase_timing_measurements", step["native_result"])
            self.assertNotIn("provider_call_timing_measurements", step["native_result"])
            self.assertNotIn("immutable_v4_preparation", step["native_result"])
            self.assertNotIn("admission_execution", step["native_result"])
        self.assertEqual(loaded.call_args_list, [mock.call(self.bundle, allow_identity_v4=True)] * 2)
        for field in ("bundle_reauthenticated", "binary_reauthenticated", "action_profile_reauthenticated",
                      "loaded_images_stable", "readback_artifacts_valid", "stage_environment_valid",
                      "native_capacity_policy_valid", "native_replay_policy_valid"):
            self.assertIs(receipt[field], True)
        for name in ("source", "output"):
            self.assertEqual((output / f"replay-{name}.rgba").read_bytes(),
                             (self.bundle / f"{name}.rgba").read_bytes())
            evidence = receipt["verified_readback_artifacts"][name]
            self.assertIs(evidence["regular_owned_artifact"], True)
            self.assertIs(evidence["exact_original_bytes"], True)
        for field, value in (("diagnostic", True), ("policy_qualified", False),
                             ("timing_eligible", False), ("public_v4_execution_enabled", False)):
            self.assertIs(receipt[field], value)
        self.assertEqual(len(calls), 2)
        self.assertFalse(receipt["observe_encode_draws"])
        self.assertTrue(all("--observe-encode-draws" not in call for call in calls))
        self.assertEqual(receipt["producer_contract"], "none")
        self.assertEqual(receipt["publication_mode"], "exact-source-diagnostic")
        for call in calls:
            self.assertEqual(call[call.index("--producer-contract") + 1], "none")
            self.assertEqual(call[call.index("--publication-mode") + 1], "exact-source-diagnostic")
        self.assertIn("--inspect-plan", calls[0])
        self.assertNotIn("--replay-route", calls[0])
        self.assertNotIn("--readback-dir", calls[0])
        self.assertNotIn("--inspect-plan", calls[1])
        self.assertEqual(calls[1][calls[1].index("--readback-dir") + 1], str(output))
        self.assertEqual(calls[1][-4:], ["--expected-plan-digest", self.plan, "--replay-route", "production-slot"])
        self.assertEqual(receipt["expected_readback_artifacts"],
                         {"source": "replay-source.rgba", "output": "replay-output.rgba"})
        self.assertEqual(output.stat().st_mode & 0o077, 0)
        self.assertEqual(receipt["diagnostic_executable"]["sha256"], self.shared.digest(self.binary))
        self.assertEqual(receipt["diagnostic_executable"]["bytes"], self.binary.stat().st_size)
        self.assertEqual(receipt["input_components"], self.manifest["components"])
        self.assertEqual((output / "inspect.stderr").read_bytes(), b"native-1-stderr\n")
        self.assertEqual((output / "replay.stderr").read_bytes(), b"native-2-stderr\n")
        with self.assertRaisesRegex(SystemExit, "inspection-only"):
            self.shared.load_bundle(self.bundle)

    def test_optional_timing_is_exported_without_qualifying_performance(self):
        self.add_action_profile()
        for mode in self.launcher.PUBLICATION_MODES:
            with self.subTest(mode=mode):
                inspect = self.native_with_timing(publication_mode=mode)
                replay = self.native_with_timing(True, publication_mode=mode)
                replay["timing_measurements"]["prepared_phases"]["reset_wall_ns"] = 0
                replay["timing_measurements"]["runtime_snapshot_delta"]["encode_chunk_wall_ns"] = (1 << 64) - 1
                code, receipt, calls, _, _ = self.run_case(
                    [(inspect, 0), (replay, 0)], publication_mode=mode)
                self.assertEqual(code, 0)
                self.assertTrue(receipt["accepted"])
                self.assertEqual(len(calls), 2)
                self.assertEqual(receipt["timing_measurements"], replay["timing_measurements"])
                self.assertEqual(receipt["steps"]["inspect"]["native_result"]["timing_measurements"],
                                 inspect["timing_measurements"])
                for field in ("policy_qualified", "timing_eligible", "public_v4_execution_enabled"):
                    self.assertIs(receipt[field], False)
                self.assertTrue(all(value is None for value in receipt["timing_measurements"]["unavailable"].values()))

    def test_explicit_repetition_keeps_warmups_distinct_and_each_original_readback(self):
        for warmup, repeat, selectors in ((0, 1, {"repeat": 1}), (1, 2, {"warmup": 1, "repeat": 2}),
                                         (0, 1, {"reset_policy": "fresh-device"})):
            with self.subTest(warmup=warmup, repeat=repeat, selectors=selectors):
                native = self.native_repeat(warmup=warmup, repeat=repeat)
                code, receipt, calls, output, _ = self.run_case([(self.native(), 0), (native, 0)], **selectors)
                self.assertEqual(code, 0)
                self.assertTrue(receipt["accepted"])
                self.assertTrue(receipt["iterations_valid"])
                self.assertEqual(len(receipt["iterations"]), warmup + repeat)
                self.assertEqual(len(calls), 2)
                self.assertTrue(all(option not in calls[0] for option in ("--warmup", "--repeat", "--reset-policy")))
                for option, value in (("--warmup", warmup), ("--repeat", repeat), ("--reset-policy", "fresh-device")):
                    self.assertEqual(calls[1][calls[1].index(option) + 1], str(value))
                self.assertNotIn("timing_measurements", receipt)
                self.assertNotIn("aggregate", receipt)
                self.assertEqual(native["prepare_count"], 1)
                self.assertEqual(receipt["repetition_policy"]["shared_preparation"],
                                 "once;per-iteration-prepare-wall-ns-is-the-same-one-off-reading")
                for row in receipt["iterations"]:
                    self.assertEqual(row["kind"], "warmup" if row["ordinal"] < warmup else "measured")
                    self.assertTrue(row["native_result_valid"])
                    self.assertTrue(row["readback_artifacts_valid"])
                    self.assertNotIn("native_result", row)
                    for name in ("source", "output"):
                        self.assertEqual((output / row["readback_directory"] / f"replay-{name}.rgba").read_bytes(),
                                         (self.bundle / f"{name}.rgba").read_bytes())
                for field in ("policy_qualified", "timing_eligible", "public_v4_execution_enabled"):
                    self.assertIs(receipt[field], False)

    def test_repeat_one_fixed_grouping_policy_is_checked_on_every_iteration(self):
        self.add_action_profile()
        mode = "semantic-mapping"
        native = self.native_repeat(warmup=1, repeat=2, publication_mode=mode,
                                    route="compatibility", draw_command_limit=3)
        code, receipt, calls, _, _ = self.run_case(
            [(self.native(publication_mode=mode, draw_command_limit=3), 0), (native, 0)],
            warmup=1, repeat=2, publication_mode=mode, route="compatibility", draw_command_limit=3)
        self.assertEqual(code, 0)
        self.assertTrue(receipt["accepted"])
        for call in calls:
            self.assertEqual(call[call.index("--draw-command-limit") + 1], "3")
        self.assertEqual(receipt["effective_stage_environment"]["DXMT9_DRAW_CHUNK_COMMAND_LIMIT"], "3")
        for group, field, value in (("replay_policy", "draw_command_limit", 0),
                                    ("effective_stage_environment", "DXMT9_DRAW_CHUNK_COMMAND_LIMIT", "0")):
            with self.subTest(group=group):
                native = self.native_repeat(warmup=1, repeat=1, publication_mode=mode,
                                            route="compatibility", draw_command_limit=3)
                native.results[1][group][field] = value
                native.refresh()
                code, receipt, _, _, _ = self.run_case(
                    [(self.native(publication_mode=mode, draw_command_limit=3), 0), (native, 0)],
                    warmup=1, repeat=1, publication_mode=mode, route="compatibility", draw_command_limit=3)
                self.assertEqual(code, 1)
                self.assertFalse(receipt["accepted"])

    def test_repeat_controls_reject_before_files_or_native_effects(self):
        cases = [["--warmup", value] for value in ("-1", "+1", " 1", "1x", "1.0", "65")]
        cases += [["--repeat", value] for value in ("0", "-1", "True", "65", str(1 << 64))]
        cases += [["--warmup", "64", "--repeat", "1"], ["--reset-policy", "warm-device"],
                  ["--repeat", "1", "--observe-encode-draws"],
                  ["--repeat", "1", "--repeat", "1"], ["--warmup", "0", "--warmup", "0"],
                  ["--reset-policy", "fresh-device", "--reset-policy", "fresh-device"]]
        for index, extra in enumerate(cases):
            with self.subTest(extra=extra):
                output = self.root / f"invalid-repeat-{index}"
                argv = ["diagnostic", "--bundle", str(self.bundle), "--diagnostic", str(self.binary),
                        "--output-dir", str(output), "--route", "production-slot"] + extra
                with mock.patch.object(sys, "argv", argv), contextlib.redirect_stderr(io.StringIO()), \
                     mock.patch.object(self.launcher.subprocess, "run") as child, \
                     self.assertRaises(SystemExit):
                    self.launcher.main()
                child.assert_not_called()
                self.assertFalse(output.exists())
        self.assertEqual(self.launcher.make_repetition_policy(0, 64, "fresh-device")["repeat"], 64)
        self.assertEqual(self.launcher.make_repetition_policy(63, 1, "fresh-device")["warmup"], 63)

    def test_repeat_envelope_rejects_truncation_prefix_and_unsafe_root_authority(self):
        for field, value in (("execution_count", True), ("execution_count", 1), ("prepare_count", True),
                             ("prepare_count", 2), ("qualified", False), ("timing_eligible", True),
                             ("aggregate", {"mean_ns": 1}), ("plan_sha256", "2" * 64)):
            with self.subTest(field=field):
                native = self.native_repeat(warmup=1, repeat=1)
                native[field] = value
                code, receipt, _, _, _ = self.run_case([(self.native(), 0), (native, 0)], warmup=1, repeat=1)
                self.assertEqual(code, 1)
                self.assertFalse(receipt["accepted"])
                self.assertFalse(receipt["iterations_valid"])
                self.assertEqual(receipt["iterations"], [])
        for policy_field, value in (("warmup", True), ("repeat", False), ("prepared_input_shared", 1),
                                    ("reset_policy", "warm-device"), ("cache_qualification", "warm-qualified"),
                                    ("input_storage", "zero-copy")):
            native = self.native_repeat(warmup=1, repeat=1)
            native["repetition_policy"][policy_field] = value
            with self.subTest(policy_field=policy_field):
                code, receipt, _, _, _ = self.run_case([(self.native(), 0), (native, 0)], warmup=1, repeat=1)
                self.assertEqual(code, 1)
                self.assertFalse(receipt["accepted"])
        native = self.native_repeat(warmup=1, repeat=1)
        native["iterations"] = native["iterations"][:1]
        code, receipt, _, _, _ = self.run_case([(self.native(), 0), (native, 0)], warmup=1, repeat=1)
        self.assertEqual(code, 1)
        self.assertEqual(receipt["iterations"], [])
        code, receipt, _, output, _ = self.run_case([(self.native(), 0), (b'{"schema":', 0)], repeat=1)
        self.assertEqual(code, 1)
        self.assertFalse(receipt["accepted"])
        self.assertEqual((output / "replay.stdout").read_bytes(), b'{"schema":')

    def test_repeat_rows_reject_duplicate_order_role_paths_hashes_and_bool_integers(self):
        faults = (("ordinal", 0), ("ordinal", True), ("ordinal", 2), ("kind", "warmup"),
                  ("kind_index", True), ("kind_index", 2), ("result_path", "../native.json"),
                  ("result_path", "/tmp/native.json"), ("readback_directory", "iterations/../warmup-0000"),
                  ("result_bytes", True), ("result_bytes", -1), ("result_bytes", (16 << 20) + 1),
                  ("result_sha256", "2" * 64), ("status", "rejected"))
        for field, value in faults:
            with self.subTest(field=field, value=value):
                native = self.native_repeat(warmup=1, repeat=1)
                native["iterations"][1][field] = value
                code, receipt, _, _, _ = self.run_case([(self.native(), 0), (native, 0)], warmup=1, repeat=1)
                self.assertEqual(code, 1)
                self.assertFalse(receipt["accepted"])
                self.assertEqual(receipt["iterations"], [])
        native = self.native_repeat(warmup=1, repeat=1)
        native["iterations"].reverse()
        code, receipt, _, _, _ = self.run_case([(self.native(), 0), (native, 0)], warmup=1, repeat=1)
        self.assertEqual(code, 1)
        self.assertFalse(receipt["accepted"])

    def test_repeat_every_warmup_and_measured_row_preserves_original_gates(self):
        faults = [(None, gate, False) for gate in self.launcher.GATES]
        faults += [(None, "plan_sha256", "2" * 64), (None, "expected_plan_sha256", "2" * 64),
                   (None, "execution_count", True), ("stages", "fresh_device_teardown_complete", False),
                   ("stages", "actual_metal", False), ("prepared", "plan_reused", False),
                   ("prepared", "prepare_count", True), ("v4", "actual_lifecycle_complete", False),
                   ("v4", "actual_topology_complete", False), ("v4", "raw_ack_coverage_complete", False),
                   ("iteration", "ordinal", True), ("iteration", "kind", "foreign")]
        for ordinal in (0, 1):
            for group, field, value in faults:
                with self.subTest(ordinal=ordinal, group=group, field=field):
                    native = self.native_repeat(warmup=1, repeat=1)
                    target = native.results[ordinal]
                    (target if group is None else target[group])[field] = value
                    native.refresh()
                    code, receipt, _, output, _ = self.run_case(
                        [(self.native(), 0), (native, 0)], warmup=1, repeat=1)
                    self.assertEqual(code, 1)
                    self.assertFalse(receipt["accepted"])
                    self.assertEqual(receipt["iterations"], [])
                    self.assertTrue((output / native["iterations"][ordinal]["result_path"]).is_file())

    def test_repeat_failed_warmup_or_midloop_never_accepts_prefix(self):
        for failed_ordinal in (0, 2):
            with self.subTest(failed_ordinal=failed_ordinal):
                native = self.native_repeat(warmup=1, repeat=3)
                native["iterations"] = native["iterations"][:failed_ordinal + 1]
                native.results = native.results[:failed_ordinal + 1]
                native.results[-1].update(status="rejected", qualified=False, failure="injected-iteration-failure")
                native["iterations"][-1]["status"] = "rejected"
                native.update(status="rejected", qualified=False, failure="injected-iteration-failure",
                              execution_count=failed_ordinal + 1)
                retained = {row["readback_directory"] for row in native["iterations"]}
                native.readback_files = {path: raw for path, raw in native.readback_files.items()
                                        if str(pathlib.PurePosixPath(path).parent) in retained}
                native.refresh()
                code, receipt, calls, output, _ = self.run_case(
                    [(self.native(), 0), (native, 17)], warmup=1, repeat=3)
                self.assertEqual(code, 1)
                self.assertFalse(receipt["accepted"])
                self.assertFalse(receipt["iterations_valid"])
                self.assertEqual(receipt["iterations"], [])
                self.assertEqual(receipt["failure"], "replay rejected with exit code 17")
                self.assertEqual(len(calls), 2)
                self.assertTrue((output / native["iterations"][-1]["result_path"]).is_file())
                self.assertNotIn("aggregate", receipt)

    def test_repeat_readback_and_json_aggregate_caps_precede_effects_or_row_loading(self):
        with mock.patch.object(self.launcher, "MAX_REPEAT_READBACK_BYTES", 1):
            code, receipt, calls, _, _ = self.run_case([], warmup=1, repeat=1)
        self.assertEqual(code, 1)
        self.assertEqual(calls, [])
        self.assertIn("readback byte cap", receipt["failure"])
        native = self.native_repeat(warmup=1, repeat=1)
        with mock.patch.object(self.launcher, "MAX_REPEAT_JSON_BYTES", 1), \
             mock.patch.object(self.launcher, "load_iteration_result", wraps=self.launcher.load_iteration_result) as loaded:
            code, receipt, _, _, _ = self.run_case([(self.native(), 0), (native, 0)], warmup=1, repeat=1)
        self.assertEqual(code, 1)
        self.assertFalse(receipt["accepted"])
        self.assertIn("aggregate JSON byte cap", receipt["failure"])
        loaded.assert_not_called()

    def test_repeat_readbacks_and_json_reject_tampering_and_symlinks(self):
        for ordinal, artifact, symlink in ((0, "replay-source.rgba", False), (1, "replay-output.rgba", False),
                                           (0, "native.json", True), (1, "replay-output.rgba", True)):
            with self.subTest(ordinal=ordinal, artifact=artifact, symlink=symlink):
                native = self.native_repeat(warmup=1, repeat=1)

                def tamper(step, _response, output):
                    if step != "replay":
                        return
                    path = output / native["iterations"][ordinal]["readback_directory"] / artifact
                    if symlink:
                        path.unlink()
                        path.symlink_to(self.binary)
                    else:
                        path.write_bytes(b"broken!")

                code, receipt, _, _, _ = self.run_case(
                    [(self.native(), 0), (native, 0)], warmup=1, repeat=1, after_child=tamper)
                self.assertEqual(code, 1)
                self.assertFalse(receipt["accepted"])
                self.assertFalse(receipt["iterations_valid"])

    def test_repeat_final_pass_reauthenticates_files_inputs_and_images(self):
        for artifact in ("native-json", "readback", "input", "binary", "runtime-image"):
            with self.subTest(artifact=artifact):
                native = self.native_repeat(warmup=1, repeat=1)
                verify = self.launcher.verify_repeat_iterations
                calls = []
                restored = {}

                def verify_then_tamper(*args, **kwargs):
                    verified = verify(*args, **kwargs)
                    calls.append(args[-1])
                    if len(calls) == 1:
                        output = args[1]
                        row = native["iterations"][0]
                        paths = {"native-json": output / row["result_path"],
                                 "readback": output / row["readback_directory"] / "replay-source.rgba",
                                 "input": self.bundle / "events.bin", "binary": self.binary,
                                 "runtime-image": self.root / "libdxmt9_native.dylib"}
                        path = paths[artifact]
                        restored[path] = path.read_bytes()
                        path.write_bytes(b"tampered-final-evidence")
                    return verified

                try:
                    with mock.patch.object(self.launcher, "verify_repeat_iterations", side_effect=verify_then_tamper):
                        code, receipt, _, _, _ = self.run_case(
                            [(self.native(), 0), (native, 0)], warmup=1, repeat=1)
                    self.assertEqual(code, 1)
                    self.assertFalse(receipt["accepted"])
                    self.assertTrue(receipt["reauthentication_failures"])
                finally:
                    for path, raw in restored.items():
                        path.write_bytes(raw)

    def test_repeat_final_iteration_verification_uses_same_deadline(self):
        native = self.native_repeat(warmup=1, repeat=1)
        verify = self.launcher.verify_repeat_iterations
        deadlines = []

        def final_deadline(*args, **kwargs):
            deadlines.append(args[-1])
            if len(deadlines) == 2:
                raise self.launcher.Rejected("shared diagnostic deadline exhausted")
            return verify(*args, **kwargs)

        with mock.patch.object(self.launcher, "verify_repeat_iterations", side_effect=final_deadline):
            code, receipt, _, _, _ = self.run_case([(self.native(), 0), (native, 0)], warmup=1, repeat=1)
        self.assertEqual(code, 1)
        self.assertFalse(receipt["accepted"])
        self.assertFalse(receipt["iterations_valid"])
        self.assertEqual(len(deadlines), 2)
        self.assertEqual(deadlines[0], deadlines[1])

    def test_optional_query_timing_is_exported_and_remains_diagnostic_only(self):
        self.add_action_profile()
        for mode in self.launcher.PUBLICATION_MODES:
            with self.subTest(mode=mode):
                inspect = self.native_with_query_timing(publication_mode=mode)
                replay = self.native_with_query_timing(True, publication_mode=mode)
                replay["query_timing_measurements"]["checkpoint_setup_wall_ns"] = 0
                replay["query_timing_measurements"]["oracle_wall_ns"] = (1 << 64) - 1
                replay["query_timing_measurements"]["checkpoint_setup_poll_count"] = (1 << 32) - 1
                code, receipt, calls, _, _ = self.run_case([(inspect, 0), (replay, 0)], publication_mode=mode)
                self.assertEqual(code, 0)
                self.assertTrue(receipt["accepted"])
                self.assertEqual(len(calls), 2)
                self.assertEqual(receipt["query_timing_measurements"], replay["query_timing_measurements"])
                for field in ("policy_qualified", "timing_eligible", "public_v4_execution_enabled"):
                    self.assertIs(receipt[field], False)

    def test_query_timing_strict_types_missing_fields_and_scopes_fail_closed(self):
        valid = self.native_with_query_timing(True)
        timing = valid["query_timing_measurements"]
        for field, value in timing.items():
            with self.subTest(missing=field):
                failed = copy.deepcopy(valid)
                del failed["query_timing_measurements"][field]
                with self.assertRaisesRegex(self.launcher.Rejected, "query timing"):
                    self.launcher.validate_common(failed, "none", "exact-source-diagnostic")
            if type(value) is str:
                invalid_values = ("wrong-query-authority-or-scope", None, 1)
            elif type(value) is bool:
                invalid_values = (None, 0, 1, "true")
                if field != "observed":
                    invalid_values += (True,)
            else:
                overflow = 1 << (64 if field.endswith("_ns") else 32)
                invalid_values = (None, -1, True, False, overflow, 1.0, "1")
            for invalid in invalid_values:
                with self.subTest(field=field, invalid=invalid):
                    failed = copy.deepcopy(valid)
                    failed["query_timing_measurements"][field] = invalid
                    with self.assertRaisesRegex(self.launcher.Rejected, "query timing"):
                        self.launcher.validate_common(failed, "none", "exact-source-diagnostic")
        for block in (None, [], True, "query timing"):
            failed = copy.deepcopy(valid)
            failed["query_timing_measurements"] = block
            with self.subTest(block=block), self.assertRaisesRegex(self.launcher.Rejected, "query timing"):
                self.launcher.validate_query_timing_measurements(failed)
        failed = copy.deepcopy(valid)
        failed["query_timing_measurements"]["exclusive_gpu_time"] = 0
        with self.assertRaisesRegex(self.launcher.Rejected, "query timing"):
            self.launcher.validate_query_timing_measurements(failed)

    def test_query_timing_unobserved_inspect_failed_and_query_absent_values_are_null(self):
        inspect = self.native_with_query_timing()
        self.launcher.validate_common(inspect, "none", "exact-source-diagnostic")
        for kind in ("failed", "query-absent"):
            native = self.native_with_query_timing(True)
            native["query_timing_measurements"] = self.query_timing_measurements(observed=False)
            if kind == "failed":
                native.update(status="rejected", failure="query-failed", qualified=False)
            else:
                native["query_actual"]["present"] = False
            self.launcher.validate_query_timing_measurements(native)
            for field in (key for key in native["query_timing_measurements"]
                          if key.endswith(("_ns", "_count"))):
                for value in (0, False, -1):
                    with self.subTest(kind=kind, field=field, value=value):
                        failed = copy.deepcopy(native)
                        failed["query_timing_measurements"][field] = value
                        with self.assertRaisesRegex(self.launcher.Rejected, "must be null"):
                            self.launcher.validate_query_timing_measurements(failed)

    def test_query_timing_observed_requires_success_and_actual_query_qualification(self):
        valid = self.native_with_query_timing(True)
        faults = [(None, "status", "inspection-complete"), (None, "failure", "failed"),
                  (None, "qualified", False), (None, "query_qualified", False),
                  (None, "device_created", False), (None, "execution_count", True),
                  ("stages", "actual_metal", False), ("stages", "fresh_device_teardown_complete", False)]
        faults += [("query_actual", field, False) for field in
                   ("present", "actual_metal", "transcript_complete", "oracle_complete")]
        for group, field, value in faults:
            with self.subTest(group=group, field=field):
                failed = copy.deepcopy(valid)
                target = failed if group is None else failed[group]
                target[field] = value
                with self.assertRaisesRegex(self.launcher.Rejected, "qualified actual-Metal query replay"):
                    self.launcher.validate_query_timing_measurements(failed)
                del target[field]
                with self.assertRaisesRegex(self.launcher.Rejected, "qualified actual-Metal query replay"):
                    self.launcher.validate_query_timing_measurements(failed)
        for query in (None, [], {"present": True}):
            failed = copy.deepcopy(valid)
            failed["query_actual"] = query
            with self.subTest(query=query), \
                 self.assertRaisesRegex(self.launcher.Rejected, "qualified actual-Metal query replay"):
                self.launcher.validate_query_timing_measurements(failed)

    def test_optional_query_timing_is_checked_on_both_legacy_steps(self):
        for step in ("inspect", "replay"):
            failed = self.native_with_query_timing(step == "replay")
            failed["query_timing_measurements"]["transcript_scope"] = "exclusive-gpu-execution"
            responses = [] if step == "inspect" else [(self.native_with_query_timing(), 0)]
            with self.subTest(step=step):
                code, receipt, calls, _, _ = self.run_case(responses + [(failed, 0)])
                self.assertEqual(code, 1)
                self.assertFalse(receipt["accepted"])
                self.assertIn("query timing", receipt["failure"])
                self.assertNotIn("query_timing_measurements", receipt)
                self.assertEqual(len(calls), len(responses) + 1)

    def test_query_timing_repeat_summary_copies_every_warmup_and_measured_block(self):
        native = self.native_repeat(warmup=1, repeat=2)
        for row in native.results:
            row["query_actual"] = {field: True for field in
                                   ("present", "actual_metal", "transcript_complete", "oracle_complete")}
            row["query_timing_measurements"] = self.query_timing_measurements()
        native.refresh()
        code, receipt, _, _, _ = self.run_case([(self.native(), 0), (native, 0)], warmup=1, repeat=2)
        self.assertEqual(code, 0)
        self.assertTrue(receipt["accepted"])
        self.assertNotIn("query_timing_measurements", receipt)
        for summary, actual in zip(receipt["iterations"], native.results):
            self.assertEqual(summary["query_timing_measurements"], actual["query_timing_measurements"])
        for field in ("policy_qualified", "timing_eligible", "public_v4_execution_enabled"):
            self.assertIs(receipt[field], False)

    def test_query_timing_repeat_row_malformed_or_query_unqualified_rejects_no_prefix(self):
        for ordinal in (0, 1):
            for fault in ("bool-count", "negative-ns", "missing-scope", "unqualified-query"):
                with self.subTest(ordinal=ordinal, fault=fault):
                    native = self.native_repeat(warmup=1, repeat=1)
                    for row in native.results:
                        row["query_actual"] = {field: True for field in
                                               ("present", "actual_metal", "transcript_complete", "oracle_complete")}
                        row["query_timing_measurements"] = self.query_timing_measurements()
                    row = native.results[ordinal]
                    timing = row["query_timing_measurements"]
                    if fault == "bool-count":
                        timing["checkpoint_setup_poll_count"] = True
                    elif fault == "negative-ns":
                        timing["oracle_wall_ns"] = -1
                    elif fault == "missing-scope":
                        del timing["nesting_scope"]
                    else:
                        row["query_actual"]["oracle_complete"] = False
                    native.refresh()
                    code, receipt, _, _, _ = self.run_case([(self.native(), 0), (native, 0)], warmup=1, repeat=1)
                    self.assertEqual(code, 1)
                    self.assertFalse(receipt["accepted"])
                    self.assertFalse(receipt["iterations_valid"])
                    self.assertEqual(receipt["iterations"], [])
                    self.assertIn("query timing", receipt["failure"])

    def test_v4_phase_timing_success_exports_legacy_and_each_repeat_row_without_aggregation(self):
        inspect = self.native_with_v4_phase_timing()
        replay = self.native_with_v4_phase_timing(True)
        replay["v4_phase_timing_measurements"]["outer_plan_admission_ns"] = 0
        replay["v4_phase_timing_measurements"]["finish_ns"] = (1 << 64) - 1
        code, receipt, _, _, _ = self.run_case([(inspect, 0), (replay, 0)])
        self.assertEqual(code, 0)
        self.assertTrue(receipt["accepted"])
        self.assertEqual(receipt["v4_phase_timing_measurements"], replay["v4_phase_timing_measurements"])
        repeated = self.native_repeat(warmup=1, repeat=1)
        for row in repeated.results:
            row["v4_phase_timing_measurements"] = self.v4_phase_timing_measurements()
        repeated.refresh()
        code, receipt, _, _, _ = self.run_case([(self.native(), 0), (repeated, 0)], warmup=1, repeat=1)
        self.assertEqual(code, 0)
        self.assertTrue(receipt["accepted"])
        self.assertNotIn("v4_phase_timing_measurements", receipt)
        for summary, row in zip(receipt["iterations"], repeated.results):
            self.assertEqual(summary["v4_phase_timing_measurements"], row["v4_phase_timing_measurements"])
        self.assertIs(receipt["timing_eligible"], False)
        self.assertIs(receipt["policy_qualified"], False)

    def test_v4_phase_timing_missing_types_scopes_and_unsigned_values_fail_closed(self):
        valid = self.native_with_v4_phase_timing(True)
        for field, value in valid["v4_phase_timing_measurements"].items():
            failed = copy.deepcopy(valid)
            del failed["v4_phase_timing_measurements"][field]
            with self.subTest(missing=field), self.assertRaisesRegex(self.launcher.Rejected, "V4 phase timing"):
                self.launcher.validate_common(failed, "none", "exact-source-diagnostic")
            if type(value) is str:
                invalid_values = ("wrong-schema-or-elapsed-scope", None)
            elif type(value) is bool:
                invalid_values = (0, 1, "true") + (() if field == "observed" else (True,))
            else:
                invalid_values = (None, -1, True, False, 1 << 64, 1.0, "1")
            for invalid in invalid_values:
                failed = copy.deepcopy(valid)
                failed["v4_phase_timing_measurements"][field] = invalid
                with self.subTest(field=field, invalid=invalid), \
                     self.assertRaisesRegex(self.launcher.Rejected, "V4 phase timing"):
                    self.launcher.validate_common(failed, "none", "exact-source-diagnostic")
        for block in (None, [], True):
            failed = copy.deepcopy(valid)
            failed["v4_phase_timing_measurements"] = block
            with self.subTest(block=block), self.assertRaisesRegex(self.launcher.Rejected, "V4 phase timing"):
                self.launcher.validate_v4_phase_timing_measurements(failed)

    def test_v4_phase_timing_unobserved_nulls_and_observed_success_gate(self):
        inspected = self.native_with_v4_phase_timing()
        self.launcher.validate_common(inspected, "none", "exact-source-diagnostic")
        failed_replay = self.native_with_v4_phase_timing(True)
        failed_replay.update(status="rejected", failure="replay-failed", qualified=False)
        failed_replay["v4_phase_timing_measurements"] = self.v4_phase_timing_measurements(observed=False)
        self.launcher.validate_v4_phase_timing_measurements(failed_replay)
        for field in (key for key in failed_replay["v4_phase_timing_measurements"] if key.endswith("_ns")):
            failed = copy.deepcopy(failed_replay)
            failed["v4_phase_timing_measurements"][field] = 0
            with self.subTest(unobserved=field), self.assertRaisesRegex(self.launcher.Rejected, "must be null"):
                self.launcher.validate_v4_phase_timing_measurements(failed)
        for group, field, value in ((None, "qualified", False), (None, "execution_count", True),
                                    ("stages", "actual_metal", False),
                                    ("stages", "fresh_device_teardown_complete", False)):
            failed = self.native_with_v4_phase_timing(True)
            (failed if group is None else failed[group])[field] = value
            with self.subTest(group=group, field=field), \
                 self.assertRaisesRegex(self.launcher.Rejected, "qualified actual-Metal replay"):
                self.launcher.validate_v4_phase_timing_measurements(failed)

    def test_v4_phase_timing_malformed_repeat_rows_and_root_aggregate_are_rejected(self):
        for ordinal in (0, 1):
            repeated = self.native_repeat(warmup=1, repeat=1)
            for row in repeated.results:
                row["v4_phase_timing_measurements"] = self.v4_phase_timing_measurements()
            repeated.results[ordinal]["v4_phase_timing_measurements"]["pre_arm_checkpoint_ns"] = True
            repeated.refresh()
            with self.subTest(ordinal=ordinal):
                code, receipt, _, _, _ = self.run_case([(self.native(), 0), (repeated, 0)], warmup=1, repeat=1)
                self.assertEqual(code, 1)
                self.assertFalse(receipt["accepted"])
                self.assertEqual(receipt["iterations"], [])
                self.assertIn("V4 phase timing", receipt["failure"])
        repeated = self.native_repeat(repeat=1)
        repeated["v4_phase_timing_measurements"] = self.v4_phase_timing_measurements()
        code, receipt, _, _, _ = self.run_case([(self.native(), 0), (repeated, 0)], repeat=1)
        self.assertEqual(code, 1)
        self.assertFalse(receipt["accepted"])
        self.assertIn("envelope", receipt["failure"])

    def test_provider_call_timing_success_exports_single_and_each_repeat_row_without_aggregation(self):
        inspected = self.native_with_provider_call_timing()
        replay = self.native_with_provider_call_timing(True)
        replay["provider_call_timing_measurements"]["provider_call_wall_ns"] = (1 << 64) - 1
        code, receipt, _, _, _ = self.run_case([(inspected, 0), (replay, 0)])
        self.assertEqual(code, 0)
        self.assertTrue(receipt["accepted"])
        self.assertEqual(receipt["provider_call_timing_measurements"], replay["provider_call_timing_measurements"])
        self.assertEqual(receipt["steps"]["inspect"]["native_result"]["provider_call_timing_measurements"],
                         inspected["provider_call_timing_measurements"])
        repeated = self.native_repeat(warmup=1, repeat=1)
        for ordinal, row in enumerate(repeated.results):
            row["provider_call_timing_measurements"] = self.provider_call_timing_measurements()
            row["provider_call_timing_measurements"]["provider_call_wall_ns"] = 0 if ordinal == 0 else (1 << 64) - 1
        repeated.refresh()
        code, receipt, _, _, _ = self.run_case([(self.native(), 0), (repeated, 0)], warmup=1, repeat=1)
        self.assertEqual(code, 0)
        self.assertTrue(receipt["accepted"])
        self.assertNotIn("provider_call_timing_measurements", receipt)
        for summary, row in zip(receipt["iterations"], repeated.results):
            self.assertEqual(summary["provider_call_timing_measurements"], row["provider_call_timing_measurements"])
        self.assertIs(receipt["timing_eligible"], False)
        self.assertIs(receipt["policy_qualified"], False)

    def test_provider_call_timing_missing_types_scopes_and_finite_unsigned_values_fail_closed(self):
        valid = self.native_with_provider_call_timing(True)
        for field, value in valid["provider_call_timing_measurements"].items():
            failed = copy.deepcopy(valid)
            del failed["provider_call_timing_measurements"][field]
            with self.subTest(missing=field), self.assertRaisesRegex(self.launcher.Rejected, "provider-call timing"):
                self.launcher.validate_common(failed, "none", "exact-source-diagnostic")
            if type(value) is str:
                invalid_values = ("wrong-schema-or-elapsed-scope", None, 1)
            elif type(value) is bool:
                invalid_values = (0, 1, "true") + (() if field == "observed" else (True,))
            else:
                invalid_values = (None, -1, True, False, 1 << 64, 1.0, "1",
                                  float("nan"), float("inf"), float("-inf"))
            for invalid in invalid_values:
                failed = copy.deepcopy(valid)
                failed["provider_call_timing_measurements"][field] = invalid
                with self.subTest(field=field, invalid=invalid), \
                     self.assertRaisesRegex(self.launcher.Rejected, "provider-call timing"):
                    self.launcher.validate_common(failed, "none", "exact-source-diagnostic")
        for block in (None, [], True):
            failed = copy.deepcopy(valid)
            failed["provider_call_timing_measurements"] = block
            with self.subTest(block=block), self.assertRaisesRegex(self.launcher.Rejected, "provider-call timing"):
                self.launcher.validate_provider_call_timing_measurements(failed)
        failed = copy.deepcopy(valid)
        failed["provider_call_timing_measurements"]["exclusive_cpu_time_ns"] = 0
        with self.assertRaisesRegex(self.launcher.Rejected, "provider-call timing"):
            self.launcher.validate_provider_call_timing_measurements(failed)

    def test_provider_call_timing_unobserved_nulls_and_observed_success_gate(self):
        inspected = self.native_with_provider_call_timing()
        self.launcher.validate_common(inspected, "none", "exact-source-diagnostic")
        invalid_inspection = copy.deepcopy(inspected)
        invalid_inspection["provider_call_timing_measurements"] = self.provider_call_timing_measurements()
        with self.assertRaisesRegex(self.launcher.Rejected, "qualified actual-Metal replay"):
            self.launcher.validate_provider_call_timing_measurements(invalid_inspection)
        failed_replay = self.native_with_provider_call_timing(True)
        failed_replay.update(status="rejected", failure="replay-failed", qualified=False)
        failed_replay["provider_call_timing_measurements"] = self.provider_call_timing_measurements(observed=False)
        self.launcher.validate_provider_call_timing_measurements(failed_replay)
        for value in (0, True, "0"):
            failed = copy.deepcopy(failed_replay)
            failed["provider_call_timing_measurements"]["provider_call_wall_ns"] = value
            with self.subTest(unobserved=value), self.assertRaisesRegex(self.launcher.Rejected, "must be null"):
                self.launcher.validate_provider_call_timing_measurements(failed)
        for group, field, value in ((None, "status", "rejected"), (None, "failure", "replay-failed"),
                                    (None, "qualified", False), (None, "device_created", False),
                                    (None, "execution_count", True), (None, "execution_count", 1.0),
                                    (None, "execution_count", 0), (None, "execution_count", 2),
                                    (None, "stages", None), ("stages", "actual_metal", False),
                                    ("stages", "fresh_device_teardown_complete", False)):
            failed = self.native_with_provider_call_timing(True)
            (failed if group is None else failed[group])[field] = value
            with self.subTest(group=group, field=field, value=value), \
                 self.assertRaisesRegex(self.launcher.Rejected, "qualified actual-Metal replay"):
                self.launcher.validate_provider_call_timing_measurements(failed)

    def test_provider_call_timing_malformed_repeat_rows_and_root_aggregate_are_rejected(self):
        for ordinal in (0, 1):
            repeated = self.native_repeat(warmup=1, repeat=1)
            for row in repeated.results:
                row["provider_call_timing_measurements"] = self.provider_call_timing_measurements()
            repeated.results[ordinal]["provider_call_timing_measurements"]["provider_call_wall_ns"] = True
            repeated.refresh()
            with self.subTest(ordinal=ordinal):
                code, receipt, _, _, _ = self.run_case([(self.native(), 0), (repeated, 0)], warmup=1, repeat=1)
                self.assertEqual(code, 1)
                self.assertFalse(receipt["accepted"])
                self.assertFalse(receipt["iterations_valid"])
                self.assertEqual(receipt["iterations"], [])
                self.assertIn("provider-call timing", receipt["failure"])
        for block in (None, self.provider_call_timing_measurements()):
            repeated = self.native_repeat(repeat=1)
            repeated["provider_call_timing_measurements"] = block
            with self.subTest(root_block=block):
                code, receipt, _, _, _ = self.run_case([(self.native(), 0), (repeated, 0)], repeat=1)
                self.assertEqual(code, 1)
                self.assertFalse(receipt["accepted"])
                self.assertIn("envelope", receipt["failure"])

    def test_provider_call_timing_legacy_repeat_rows_remain_absent(self):
        repeated = self.native_repeat(warmup=1, repeat=1)
        code, receipt, _, _, _ = self.run_case([(self.native(), 0), (repeated, 0)], warmup=1, repeat=1)
        self.assertEqual(code, 0)
        self.assertTrue(receipt["accepted"])
        self.assertNotIn("provider_call_timing_measurements", receipt)
        self.assertNotIn("immutable_v4_preparation", receipt)
        for summary in receipt["iterations"]:
            self.assertNotIn("provider_call_timing_measurements", summary)
            self.assertNotIn("immutable_v4_preparation", summary)

    def test_immutable_v4_preparation_success_exports_one_off_reading_and_fresh_reuse_proof(self):
        inspected = self.native_with_immutable_v4_preparation()
        replay = self.native_with_immutable_v4_preparation(True)
        replay["immutable_v4_preparation"]["v4_prepare_wall_ns"] = 0
        code, receipt, _, _, _ = self.run_case([(inspected, 0), (replay, 0)])
        self.assertEqual(code, 0)
        self.assertTrue(receipt["accepted"])
        self.assertEqual(receipt["immutable_v4_preparation"], replay["immutable_v4_preparation"])
        for block in ("timing_measurements", "v4_phase_timing_measurements", "provider_call_timing_measurements"):
            self.assertEqual(receipt[block], replay[block])
            self.assertTrue(receipt[block]["schema"].endswith(".v2"))
        repeated = self.native_repeat_with_immutable_v4_preparation(warmup=1, repeat=1)
        for row in repeated.results:
            row["immutable_v4_preparation"]["v4_prepare_wall_ns"] = (1 << 64) - 1
        repeated.refresh()
        code, receipt, _, _, _ = self.run_case([(self.native(), 0), (repeated, 0)], warmup=1, repeat=1)
        self.assertEqual(code, 0)
        self.assertTrue(receipt["accepted"])
        self.assertNotIn("immutable_v4_preparation", receipt)
        for summary, row in zip(receipt["iterations"], repeated.results):
            self.assertEqual(summary["immutable_v4_preparation"], row["immutable_v4_preparation"])
            self.assertEqual(summary["admission_execution"]["admission_count"], 1)
            self.assertEqual(summary["immutable_v4_preparation"]["preparation_count"], 1)
        self.assertIs(receipt["timing_eligible"], False)
        self.assertIs(receipt["policy_qualified"], False)

    def test_versioned_v4_timing_scopes_cannot_be_relabelled(self):
        factories = {"timing_measurements": self.timing_measurements,
                     "v4_phase_timing_measurements": self.v4_phase_timing_measurements,
                     "provider_call_timing_measurements": self.provider_call_timing_measurements}
        for block, factory in factories.items():
            for version in (1, 2):
                valid = (self.native_with_immutable_v4_preparation(True) if version == 2
                         else self.native_with_timing(True))
                valid[block] = factory(version=version)
                self.launcher.validate_common(valid, "none", "exact-source-diagnostic")
                relabelled = copy.deepcopy(valid)
                relabelled[block]["schema"] = factory(version=3 - version)["schema"]
                with self.subTest(block=block, version=version), \
                     self.assertRaisesRegex(self.launcher.Rejected, "scope"):
                    self.launcher.validate_common(relabelled, "none", "exact-source-diagnostic")

    def test_immutable_v4_preparation_missing_types_counts_and_finite_values_fail_closed(self):
        valid = self.native_with_immutable_v4_preparation(True)
        counts = {"preparation_count", "fresh_execution_admission_count"}
        for field, value in valid["immutable_v4_preparation"].items():
            failed = copy.deepcopy(valid)
            del failed["immutable_v4_preparation"][field]
            with self.subTest(missing=field), self.assertRaisesRegex(self.launcher.Rejected, "immutable V4 preparation"):
                self.launcher.validate_common(failed, "none", "exact-source-diagnostic")
            if type(value) is str:
                invalid_values = (None, 1, "wrong-schema-or-scope")
            elif type(value) is bool:
                invalid_values = (0, 1, "true") + (() if field == "observed" else (not value,))
            elif field in counts:
                invalid_values = (None, True, False, 0, 2, -1, 1.0, "1", 1 << 64)
            else:
                invalid_values = (None, True, False, -1, 1 << 64, 1.0, "1",
                                  float("nan"), float("inf"), float("-inf"))
            for invalid in invalid_values:
                failed = copy.deepcopy(valid)
                failed["immutable_v4_preparation"][field] = invalid
                with self.subTest(field=field, invalid=invalid), \
                     self.assertRaisesRegex(self.launcher.Rejected, "immutable V4 preparation"):
                    self.launcher.validate_common(failed, "none", "exact-source-diagnostic")
        for block in (None, [], True):
            failed = copy.deepcopy(valid)
            failed["immutable_v4_preparation"] = block
            with self.subTest(block=block), self.assertRaisesRegex(self.launcher.Rejected, "immutable V4 preparation"):
                self.launcher.validate_immutable_v4_preparation(failed)
        failed = copy.deepcopy(valid)
        failed["immutable_v4_preparation"]["input_validation_bypassed"] = True
        with self.assertRaisesRegex(self.launcher.Rejected, "immutable V4 preparation"):
            self.launcher.validate_immutable_v4_preparation(failed)

    def test_immutable_v4_preparation_unobserved_nulls_and_success_gate(self):
        inspected = self.native_with_immutable_v4_preparation()
        self.launcher.validate_common(inspected, "none", "exact-source-diagnostic")
        invalid_inspection = copy.deepcopy(inspected)
        invalid_inspection["immutable_v4_preparation"] = self.immutable_v4_preparation()
        with self.assertRaisesRegex(self.launcher.Rejected, "qualified actual-Metal replay"):
            self.launcher.validate_immutable_v4_preparation(invalid_inspection)
        failed_replay = self.native_with_immutable_v4_preparation(True)
        failed_replay.update(status="rejected", failure="replay-failed", qualified=False)
        failed_replay["immutable_v4_preparation"] = self.immutable_v4_preparation(observed=False)
        self.launcher.validate_immutable_v4_preparation(failed_replay)
        for field, value in failed_replay["immutable_v4_preparation"].items():
            if value is not None:
                continue
            failed = copy.deepcopy(failed_replay)
            failed["immutable_v4_preparation"][field] = 0
            with self.subTest(unobserved=field), self.assertRaisesRegex(self.launcher.Rejected, "must be null"):
                self.launcher.validate_immutable_v4_preparation(failed)
        for group, field, value in ((None, "status", "rejected"), (None, "failure", "replay-failed"),
                                    (None, "qualified", False), (None, "device_created", False),
                                    (None, "execution_count", True), (None, "execution_count", 0),
                                    (None, "execution_count", 2), (None, "stages", None),
                                    ("stages", "actual_metal", False),
                                    ("stages", "fresh_device_teardown_complete", False)):
            failed = self.native_with_immutable_v4_preparation(True)
            (failed if group is None else failed[group])[field] = value
            with self.subTest(group=group, field=field, value=value), \
                 self.assertRaisesRegex(self.launcher.Rejected, "qualified actual-Metal replay"):
                self.launcher.validate_immutable_v4_preparation(failed)

    def test_observed_v2_timing_requires_preparation_and_matching_admission_receipts(self):
        valid = self.native_with_immutable_v4_preparation(True)
        for block in ("immutable_v4_preparation", "admission_execution"):
            for missing in (False, True):
                failed = copy.deepcopy(valid)
                if missing:
                    del failed[block]
                elif block == "immutable_v4_preparation":
                    failed[block] = self.immutable_v4_preparation(observed=False)
                else:
                    failed[block] = self.admission_execution(observed=False)
                with self.subTest(block=block, missing=missing), \
                     self.assertRaisesRegex(self.launcher.Rejected, "evidence"):
                    self.launcher.validate_common(failed, "none", "exact-source-diagnostic")
        for timing_block in ("timing_measurements", "v4_phase_timing_measurements",
                             "provider_call_timing_measurements"):
            failed = copy.deepcopy(valid)
            for other in ("timing_measurements", "v4_phase_timing_measurements",
                          "provider_call_timing_measurements", "immutable_v4_preparation", "admission_execution"):
                if other != timing_block:
                    del failed[other]
            with self.subTest(timing_block=timing_block), \
                 self.assertRaisesRegex(self.launcher.Rejected, "requires immutable V4 preparation"):
                self.launcher.validate_common(failed, "none", "exact-source-diagnostic")
        inspected = self.native_with_immutable_v4_preparation()
        del inspected["immutable_v4_preparation"]
        del inspected["admission_execution"]
        self.launcher.validate_common(inspected, "none", "exact-source-diagnostic")

    def test_immutable_v4_preparation_malformed_or_changed_repeat_rows_and_root_are_rejected(self):
        for ordinal in (0, 1):
            for field, invalid in (("reused_for_execution", False), ("preparation_count", 0),
                                   ("fresh_execution_admission_count", 2), ("prepared_handoff_consumed", False)):
                repeated = self.native_repeat_with_immutable_v4_preparation(warmup=1, repeat=1)
                repeated.results[ordinal]["immutable_v4_preparation"][field] = invalid
                repeated.refresh()
                with self.subTest(ordinal=ordinal, field=field):
                    code, receipt, _, _, _ = self.run_case([(self.native(), 0), (repeated, 0)], warmup=1, repeat=1)
                    self.assertEqual(code, 1)
                    self.assertFalse(receipt["accepted"])
                    self.assertEqual(receipt["iterations"], [])
                    self.assertIn("immutable V4 preparation", receipt["failure"])
        for missing in (False, True):
            repeated = self.native_repeat_with_immutable_v4_preparation(warmup=1, repeat=1)
            if missing:
                del repeated.results[1]["immutable_v4_preparation"]
            else:
                repeated.results[1]["immutable_v4_preparation"]["v4_prepare_wall_ns"] += 1
            repeated.refresh()
            with self.subTest(missing=missing):
                code, receipt, _, _, _ = self.run_case([(self.native(), 0), (repeated, 0)], warmup=1, repeat=1)
                self.assertEqual(code, 1)
                self.assertFalse(receipt["accepted"])
                self.assertEqual(receipt["iterations"], [])
                expected_failure = ("observed V2 timing requires immutable V4 preparation evidence"
                                    if missing else "changed between repetitions")
                self.assertIn(expected_failure, receipt["failure"])
        for block in (None, self.immutable_v4_preparation()):
            repeated = self.native_repeat_with_immutable_v4_preparation(repeat=1)
            repeated["immutable_v4_preparation"] = block
            with self.subTest(root_block=block):
                code, receipt, _, _, _ = self.run_case([(self.native(), 0), (repeated, 0)], repeat=1)
                self.assertEqual(code, 1)
                self.assertFalse(receipt["accepted"])
                self.assertIn("envelope", receipt["failure"])

    def test_admission_execution_success_exports_legacy_and_each_repeat_row(self):
        inspected = self.native_with_admission_execution()
        replay = self.native_with_admission_execution(True)
        code, receipt, _, _, _ = self.run_case([(inspected, 0), (replay, 0)])
        self.assertEqual(code, 0)
        self.assertTrue(receipt["accepted"])
        self.assertEqual(receipt["admission_execution"], replay["admission_execution"])
        repeated = self.native_repeat(warmup=1, repeat=1)
        for row in repeated.results:
            row["admission_execution"] = self.admission_execution()
        repeated.refresh()
        code, receipt, _, _, _ = self.run_case([(self.native(), 0), (repeated, 0)], warmup=1, repeat=1)
        self.assertEqual(code, 0)
        self.assertTrue(receipt["accepted"])
        self.assertNotIn("admission_execution", receipt)
        for summary, row in zip(receipt["iterations"], repeated.results):
            self.assertEqual(summary["admission_execution"], row["admission_execution"])
        self.assertIs(receipt["timing_eligible"], False)
        self.assertIs(receipt["policy_qualified"], False)

    def test_admission_execution_exact_schema_and_one_consumed_uint32_admission(self):
        valid = self.native_with_admission_execution(True)
        for field in valid["admission_execution"]:
            failed = copy.deepcopy(valid)
            del failed["admission_execution"][field]
            with self.subTest(missing=field), self.assertRaisesRegex(self.launcher.Rejected, "admission execution"):
                self.launcher.validate_common(failed, "none", "exact-source-diagnostic")
        invalid_fields = {
            "schema": (None, False, 1, "wrong-schema"),
            "observed": (None, 0, 1, "true"),
            "admission_count": (None, -1, 0, 2, True, False, 1 << 32, 1.0, "1"),
            "prepared_handoff_consumed": (None, False, 0, 1, "true", [])}
        for field, invalid_values in invalid_fields.items():
            for invalid in invalid_values:
                failed = copy.deepcopy(valid)
                failed["admission_execution"][field] = invalid
                with self.subTest(field=field, invalid=invalid), \
                     self.assertRaisesRegex(self.launcher.Rejected, "admission execution"):
                    self.launcher.validate_common(failed, "none", "exact-source-diagnostic")
        for block in (None, [], True, {**valid["admission_execution"], "extra": True}):
            failed = copy.deepcopy(valid)
            failed["admission_execution"] = block
            with self.subTest(block=block), self.assertRaisesRegex(self.launcher.Rejected, "admission execution"):
                self.launcher.validate_common(failed, "none", "exact-source-diagnostic")

    def test_admission_execution_unobserved_nulls_and_success_only_observation(self):
        self.launcher.validate_common(self.native(), "none", "exact-source-diagnostic")
        self.launcher.validate_common(self.native(True), "none", "exact-source-diagnostic")
        self.launcher.validate_common(self.native_with_admission_execution(), "none", "exact-source-diagnostic")
        failed_replay = self.native_with_admission_execution(True)
        failed_replay.update(status="rejected", failure="replay-failed", qualified=False)
        failed_replay["admission_execution"] = self.admission_execution(observed=False)
        self.launcher.validate_common(failed_replay, "none", "exact-source-diagnostic")
        for field in ("admission_count", "prepared_handoff_consumed"):
            for value in (0, False, True):
                failed = copy.deepcopy(failed_replay)
                failed["admission_execution"][field] = value
                with self.subTest(unobserved=field, value=value), \
                     self.assertRaisesRegex(self.launcher.Rejected, "must be null"):
                    self.launcher.validate_common(failed, "none", "exact-source-diagnostic")
        for group, field, value in (
                (None, "status", "inspection-complete"), (None, "status", "rejected"),
                (None, "failure", "replay-failed"), (None, "qualified", False),
                (None, "device_created", False), (None, "execution_count", True),
                (None, "execution_count", 0), (None, "execution_count", 2), (None, "stages", None),
                ("stages", "actual_metal", False), ("stages", "actual_metal", 1),
                ("stages", "fresh_device_teardown_complete", False)):
            failed = self.native_with_admission_execution(True)
            (failed if group is None else failed[group])[field] = value
            with self.subTest(group=group, field=field, value=value), \
                 self.assertRaisesRegex(self.launcher.Rejected, "successful qualified actual-Metal replay"):
                self.launcher.validate_common(failed, "none", "exact-source-diagnostic")

    def test_admission_execution_malformed_repeat_rows_and_compact_root_are_rejected(self):
        for ordinal in (0, 1):
            for field, value in (("admission_count", 0), ("admission_count", 2),
                                 ("admission_count", True), ("prepared_handoff_consumed", False)):
                repeated = self.native_repeat(warmup=1, repeat=1)
                for row in repeated.results:
                    row["admission_execution"] = self.admission_execution()
                repeated.results[ordinal]["admission_execution"][field] = value
                repeated.refresh()
                with self.subTest(ordinal=ordinal, field=field, value=value):
                    code, receipt, _, _, _ = self.run_case([(self.native(), 0), (repeated, 0)], warmup=1, repeat=1)
                    self.assertEqual(code, 1)
                    self.assertFalse(receipt["accepted"])
                    self.assertFalse(receipt["iterations_valid"])
                    self.assertEqual(receipt["iterations"], [])
                    self.assertIn("admission execution", receipt["failure"])
        for block in (None, self.admission_execution()):
            repeated = self.native_repeat(repeat=1)
            repeated["admission_execution"] = block
            with self.subTest(root_block=block):
                code, receipt, _, _, _ = self.run_case([(self.native(), 0), (repeated, 0)], repeat=1)
                self.assertEqual(code, 1)
                self.assertFalse(receipt["accepted"])
                self.assertIn("envelope", receipt["failure"])

    def test_timing_nanoseconds_reject_negative_bool_overflow_and_wrong_types(self):
        valid = self.native_with_timing(True)
        timing = valid["timing_measurements"]
        for name in ("prepared_phases", "runtime_snapshot_delta"):
            for field in (key for key in timing[name] if key.endswith("_ns")):
                for value in (-1, True, False, 1 << 64, 1.0, "1", None):
                    with self.subTest(group=name, field=field, value=value):
                        failed = copy.deepcopy(valid)
                        failed["timing_measurements"][name][field] = value
                        with self.assertRaisesRegex(self.launcher.Rejected, "diagnostic timing"):
                            self.launcher.validate_common(failed, "none", "exact-source-diagnostic")

    def test_timing_present_contract_requires_every_field_and_explicit_scopes(self):
        valid = self.native_with_timing(True)
        for group in (None, "prepared_phases", "runtime_snapshot_delta", "unavailable"):
            timing = valid["timing_measurements"]
            fields = timing if group is None else timing[group]
            for field in fields:
                with self.subTest(group=group, missing=field):
                    failed = copy.deepcopy(valid)
                    target = failed["timing_measurements"]
                    del (target if group is None else target[group])[field]
                    with self.assertRaisesRegex(self.launcher.Rejected, "diagnostic timing"):
                        self.launcher.validate_timing_measurements(failed)
            for field, value in fields.items():
                if type(value) is not str:
                    continue
                with self.subTest(group=group, scope=field):
                    failed = copy.deepcopy(valid)
                    target = failed["timing_measurements"]
                    (target if group is None else target[group])[field] = "performance-authority"
                    with self.assertRaisesRegex(self.launcher.Rejected, "diagnostic timing"):
                        self.launcher.validate_timing_measurements(failed)
        for value in (None, [], "timing", True):
            failed = copy.deepcopy(valid)
            failed["timing_measurements"] = value
            with self.subTest(block=value), self.assertRaisesRegex(self.launcher.Rejected, "diagnostic timing"):
                self.launcher.validate_timing_measurements(failed)
        for field, values in (("thread_cpu_time", (True, 0)), ("additive", (True, 0)),
                              ("overlapping_scopes", (False, 1))):
            for value in values:
                failed = copy.deepcopy(valid)
                failed["timing_measurements"][field] = value
                with self.subTest(field=field, value=value), \
                     self.assertRaisesRegex(self.launcher.Rejected, "diagnostic timing"):
                    self.launcher.validate_timing_measurements(failed)

    def test_timing_unavailable_or_unobserved_dimensions_remain_null(self):
        valid = self.native_with_timing(True)
        for name in ("prepared_phases", "runtime_snapshot_delta", "unavailable"):
            fields = [key for key in valid["timing_measurements"][name] if key.endswith("_ns")]
            unavailable = copy.deepcopy(valid)
            if name == "prepared_phases":
                unavailable["timing_measurements"] = self.timing_measurements(prepared_observed=False)
            elif name == "runtime_snapshot_delta":
                unavailable["timing_measurements"] = self.timing_measurements(runtime_observed=False, monotonic=False)
            self.launcher.validate_timing_measurements(unavailable)
            for field in fields:
                for value in (0, False, -1, "unavailable"):
                    with self.subTest(group=name, field=field, value=value):
                        failed = copy.deepcopy(unavailable)
                        failed["timing_measurements"][name][field] = value
                        with self.assertRaisesRegex(self.launcher.Rejected, "diagnostic timing"):
                            self.launcher.validate_timing_measurements(failed)
        nonmonotonic = copy.deepcopy(valid)
        nonmonotonic["timing_measurements"] = self.timing_measurements(monotonic=False)
        self.launcher.validate_timing_measurements(nonmonotonic)
        for field in (key for key in nonmonotonic["timing_measurements"]["runtime_snapshot_delta"]
                      if key.endswith("_ns")):
            failed = copy.deepcopy(nonmonotonic)
            failed["timing_measurements"]["runtime_snapshot_delta"][field] = 0
            with self.subTest(nonmonotonic_field=field), \
                 self.assertRaisesRegex(self.launcher.Rejected, "diagnostic timing"):
                self.launcher.validate_timing_measurements(failed)
        failed = copy.deepcopy(valid)
        failed["timing_measurements"] = self.timing_measurements(runtime_observed=False, monotonic=True)
        with self.assertRaisesRegex(self.launcher.Rejected, "diagnostic timing"):
            self.launcher.validate_timing_measurements(failed)

    def test_timing_observation_flags_require_exact_bools(self):
        valid = self.native_with_timing(True)
        for name, field in (("prepared_phases", "observed"), ("runtime_snapshot_delta", "observed"),
                            ("runtime_snapshot_delta", "monotonic")):
            for value in (None, 0, 1, "true"):
                with self.subTest(group=name, flag=field, value=value):
                    failed = copy.deepcopy(valid)
                    failed["timing_measurements"][name][field] = value
                    with self.assertRaisesRegex(self.launcher.Rejected, "diagnostic timing"):
                        self.launcher.validate_timing_measurements(failed)

    def test_observed_timing_requires_successful_actual_metal_replay(self):
        valid = self.native_with_timing(True)
        faults = ((None, "status", "inspection-complete"), (None, "failure", "rejected"),
                  (None, "qualified", False), (None, "device_created", False),
                  (None, "execution_count", True), (None, "execution_count", 0),
                  ("stages", "actual_metal", False), ("stages", "actual_metal", 1),
                  ("stages", "fresh_device_teardown_complete", False))
        for group, field, value in faults:
            with self.subTest(group=group, field=field, value=value):
                failed = copy.deepcopy(valid)
                (failed if group is None else failed[group])[field] = value
                with self.assertRaisesRegex(self.launcher.Rejected, "successful actual-Metal replay"):
                    self.launcher.validate_timing_measurements(failed)
                del (failed if group is None else failed[group])[field]
                with self.assertRaisesRegex(self.launcher.Rejected, "successful actual-Metal replay"):
                    self.launcher.validate_timing_measurements(failed)
        failed = copy.deepcopy(valid)
        failed["stages"] = None
        with self.assertRaisesRegex(self.launcher.Rejected, "successful actual-Metal replay"):
            self.launcher.validate_timing_measurements(failed)

    def test_invalid_optional_timing_rejects_inspection_and_replay_before_acceptance(self):
        for step in ("inspect", "replay"):
            with self.subTest(step=step):
                failed = self.native_with_timing(step == "replay")
                failed["timing_measurements"]["authority"] = "performance-qualified"
                responses = [] if step == "inspect" else [(self.native_with_timing(), 0)]
                code, receipt, calls, _, _ = self.run_case(responses + [(failed, 0)])
                self.assertEqual(code, 1)
                self.assertFalse(receipt["accepted"])
                self.assertEqual(len(calls), len(responses) + 1)
                self.assertIn("diagnostic timing", receipt["failure"])
                self.assertNotIn("timing_measurements", receipt)
                self.assertEqual(receipt["steps"][step]["native_result"], failed)

    def test_timing_does_not_waive_pixel_query_or_plan_identity_gates(self):
        faults = [(gate, False) for gate in self.launcher.GATES]
        faults += [("plan_sha256", "2" * 64), ("expected_plan_sha256", "2" * 64),
                   ("replay_route", "compatibility"), ("timing_eligible", True), ("policy_qualified", True)]
        for field, value in faults:
            with self.subTest(field=field):
                failed = self.native_with_timing(True)
                failed[field] = value
                code, receipt, calls, _, _ = self.run_case([(self.native_with_timing(), 0), (failed, 0)])
                self.assertEqual(code, 1)
                self.assertFalse(receipt["accepted"])
                self.assertEqual(len(calls), 2)
                self.assertIs(receipt["timing_eligible"], False)
                self.assertIs(receipt["policy_qualified"], False)

    def test_requested_encode_observation_is_forwarded_and_separately_required(self):
        replay = self.native(True)
        replay["encode_draw_witness"] = {"requested": True, "installed": True,
            "complete": True, "failure": "none",
            "original_buffer_observer": {"requested": True, "admitted": True,
                "complete": True, "closed": True, "failure": "none"},
            "original_context": {"complete": True, "failure": "none"},
            "original_binding_proof": {"observation_complete": True,
                "issued_draws": 10, "qualified_draws": 0}}
        code, receipt, calls, _, _ = self.run_case(
            [(self.native(), 0), (replay, 0)], observe_encode_draws=True)
        self.assertEqual(code, 0)
        self.assertTrue(receipt["accepted"])
        self.assertNotIn("--observe-encode-draws", calls[0])
        self.assertIn("--observe-encode-draws", calls[1])
        self.assertEqual(receipt["encode_draw_witness"], replay["encode_draw_witness"])
        self.assertFalse(receipt["policy_qualified"])
        self.assertFalse(receipt["timing_eligible"])
        for witness in (None, {}, {"requested": True, "installed": True,
                                   "complete": False, "failure": "capacity-exceeded"}):
            failed = self.native(True)
            failed["encode_draw_witness"] = witness
            code, receipt, _, _, _ = self.run_case(
                [(self.native(), 0), (failed, 0)], observe_encode_draws=True)
            self.assertEqual(code, 1)
            self.assertFalse(receipt["accepted"])
            self.assertIn("Encode draw observation", receipt["failure"])

        for section, field, value in (
                ("original_buffer_observer", "requested", False),
                ("original_buffer_observer", "admitted", False),
                ("original_buffer_observer", "closed", False),
                ("original_buffer_observer", "complete", False),
                ("original_buffer_observer", "failure", "callback-rejected"),
                ("original_context", "complete", False),
                ("original_context", "failure", "budget-exceeded"),
                ("original_binding_proof", "observation_complete", False)):
            with self.subTest(section=section, field=field):
                failed = json.loads(json.dumps(replay))
                failed["encode_draw_witness"][section][field] = value
                code, receipt, _, _, _ = self.run_case(
                    [(self.native(), 0), (failed, 0)], observe_encode_draws=True)
                self.assertEqual(code, 1)
                self.assertFalse(receipt["accepted"])
                self.assertIn("original BUFFER", receipt["failure"])
        for section in ("original_buffer_observer", "original_context", "original_binding_proof"):
            failed = json.loads(json.dumps(replay))
            del failed["encode_draw_witness"][section]
            code, receipt, _, _, _ = self.run_case(
                [(self.native(), 0), (failed, 0)], observe_encode_draws=True)
            self.assertEqual(code, 1)
            self.assertFalse(receipt["accepted"])

    def test_semantic_mode_requires_and_propagates_authenticated_stable_exact_witness(self):
        self.add_action_profile()
        mode = "semantic-mapping"
        code, receipt, calls, _output, _loaded = self.run_case(
            [(self.native(publication_mode=mode), 0), (self.native(True, publication_mode=mode), 0)],
            publication_mode=mode)
        self.assertEqual(code, 0)
        self.assertTrue(receipt["accepted"])
        self.assertEqual(receipt["publication_mode"], mode)
        self.assertTrue(receipt["action_profile_validation"]["stable_exact"])
        for call in calls:
            self.assertEqual(call[call.index("--publication-mode") + 1], mode)
            self.assertEqual(call[call.index("--action-profile-sha256") + 1],
                             self.manifest["components"]["action_profile"]["sha256"])
        for field in ("policy_qualified", "timing_eligible", "public_v4_execution_enabled"):
            self.assertIs(receipt[field], False)

    def test_semantic_legacy_unknown_or_changed_action_profile_never_spawns(self):
        original = copy.deepcopy(self.manifest)
        for kind in ("absent", "normal", "aba", "manifest-only"):
            with self.subTest(kind=kind):
                self.manifest = copy.deepcopy(original)
                if kind == "normal":
                    self.add_action_profile(profile=0)
                elif kind == "aba":
                    self.add_action_profile(revision=2)
                elif kind == "manifest-only":
                    self.manifest["scope"] = {"capture_action_profile": "queue-observed",
                                              "action_profile_stable_exact": True}
                    self.save_manifest()
                else:
                    self.save_manifest()
                code, receipt, calls, _output, _loaded = self.run_case([], publication_mode="semantic-mapping")
                self.assertEqual(code, 1)
                self.assertFalse(receipt["accepted"])
                self.assertEqual(calls, [])

    def test_semantic_action_profile_digest_and_component_binding_fail_closed(self):
        for fault in ("digest", "events-binding", "capture-binding"):
            with self.subTest(fault=fault):
                self.add_action_profile()
                path = self.bundle / self.shared.ACTION_PROFILE_NAME
                raw = bytearray(path.read_bytes())
                raw[{"digest": 104, "events-binding": 104, "capture-binding": 40}[fault]] ^= 1
                path.write_bytes(raw)
                if fault != "digest":
                    self.manifest["components"]["action_profile"]["sha256"] = hashlib.sha256(raw).hexdigest()
                self.save_manifest()
                code, receipt, calls, _output, _loaded = self.run_case([], publication_mode="semantic-mapping")
                self.assertEqual(code, 1)
                self.assertFalse(receipt["accepted"])
                self.assertEqual(calls, [])

    def test_publication_mode_echo_and_mode_specific_qualification_fail_closed(self):
        self.add_action_profile()
        for mode in self.launcher.PUBLICATION_MODES:
            for step in ("inspect", "replay"):
                for field in ("publication_mode", "semantic_mapping_qualified", "exact_source_diagnostic_complete"):
                    with self.subTest(mode=mode, step=step, field=field):
                        result = self.native(step == "replay", publication_mode=mode)
                        del result[field]
                        responses = [] if step == "inspect" else [(self.native(publication_mode=mode), 0)]
                        code, receipt, calls, _output, _loaded = self.run_case(
                            responses + [(result, 0)], publication_mode=mode)
                        self.assertEqual(code, 1)
                        self.assertFalse(receipt["accepted"])
                        self.assertEqual(len(calls), len(responses) + 1)

    def test_semantic_native_action_witness_cannot_be_waived_by_launcher_authentication(self):
        self.add_action_profile()
        mode = "semantic-mapping"
        for step in ("inspect", "replay"):
            for changes in (None, {"authenticated": False, "stable_exact": True},
                            {"authenticated": True, "stable_exact": False}):
                with self.subTest(step=step, changes=changes):
                    result = self.native(step == "replay", publication_mode=mode)
                    result["action_profile"] = changes
                    responses = [] if step == "inspect" else [(self.native(publication_mode=mode), 0)]
                    code, receipt, calls, _output, _loaded = self.run_case(
                        responses + [(result, 0)], publication_mode=mode)
                    self.assertEqual(code, 1)
                    self.assertFalse(receipt["accepted"])
                    self.assertEqual(len(calls), len(responses) + 1)

    def test_semantic_original_pixel_and_query_gates_remain_required(self):
        self.add_action_profile()
        mode = "semantic-mapping"
        for gate in self.launcher.GATES:
            with self.subTest(gate=gate):
                replay = self.native(True, publication_mode=mode)
                replay[gate] = False
                code, receipt, calls, _output, _loaded = self.run_case(
                    [(self.native(publication_mode=mode), 0), (replay, 0)], publication_mode=mode)
                self.assertEqual(code, 1)
                self.assertFalse(receipt["accepted"])
                self.assertEqual(len(calls), 2)

    def test_explicit_producer_contract_is_authenticated_propagated_and_diagnostic_only(self):
        contract = "pe-present-reservation-v1"
        self.add_production_pe_provenance()
        code, receipt, calls, _output, loaded = self.run_case(
            [(self.native(producer_contract=contract), 0), (self.native(True, contract), 0)], contract)
        self.assertEqual(code, 0)
        self.assertEqual(loaded.call_args_list, [mock.call(self.bundle, allow_identity_v4=True)] * 2)
        self.assertTrue(receipt["manifest_authenticated"])
        self.assertEqual(receipt["producer_contract"], contract)
        self.assertTrue(receipt["accepted"])
        self.assertEqual(len(calls), 2)
        for step, call in zip(("inspect", "replay"), calls):
            self.assertEqual(call[call.index("--producer-contract") + 1], contract)
            self.assertEqual(receipt["steps"][step]["native_result"]["producer_contract"], contract)
        for field in ("policy_qualified", "timing_eligible", "public_v4_execution_enabled"):
            self.assertIs(receipt[field], False)

    def test_missing_or_wrong_selected_producer_provenance_never_spawns(self):
        self.add_production_pe_provenance()
        original = copy.deepcopy(self.manifest)
        mutations = [("producer", None, None), ("scope", None, None),
                     ("producer", None, []), ("scope", None, []),
                     ("producer", "path", None), ("producer", "path", "offline-import"),
                     (None, "stage", None), (None, "stage", "replay"),
                     (None, "profile", "sequence-tape"), (None, "schema", "unsupported")]
        for field in ("production_capture", "output_oracle", "source_oracle"):
            mutations += [("scope", field, value) for value in (None, False, 1, "true")]
        for section, field, value in mutations:
            with self.subTest(section=section, field=field, value=value):
                self.manifest = copy.deepcopy(original)
                target = self.manifest if section is None or field is None else self.manifest[section]
                key = section if field is None else field
                if value is None:
                    del target[key]
                else:
                    target[key] = value
                self.save_manifest()
                code, receipt, calls, _output, loaded = self.run_case([], "pe-present-reservation-v1")
                self.assertEqual(code, 1)
                self.assertFalse(receipt["accepted"])
                self.assertEqual(calls, [])
                loaded.assert_called_once_with(self.bundle, allow_identity_v4=True)
                self.assertEqual(receipt["producer_contract"], "pe-present-reservation-v1")
                self.assertEqual(receipt["steps"]["inspect"]["status"], "skipped")
                self.assertEqual(receipt["steps"]["replay"]["status"], "skipped")
                loader_failure = (
                    "unsupported render tape bundle schema" if key == "schema" else
                    "action profile scope must be an object" if key == "scope" and value == [] else None)
                self.assertIs(receipt["manifest_authenticated"], loader_failure is None)
                self.assertIn(loader_failure or "selected producer contract requires", receipt["failure"])

    def test_nested_only_producer_stage_cannot_authorize_selected_contract(self):
        self.add_production_pe_provenance()
        self.manifest["producer"]["stage"] = self.manifest.pop("stage")
        self.save_manifest()
        code, receipt, calls, _output, loaded = self.run_case([], "pe-present-reservation-v1")
        self.assertEqual(code, 1)
        self.assertFalse(receipt["accepted"])
        self.assertTrue(receipt["manifest_authenticated"])
        self.assertEqual(calls, [])
        loaded.assert_called_once_with(self.bundle, allow_identity_v4=True)
        self.assertEqual(receipt["steps"]["inspect"]["status"], "skipped")

    def test_none_does_not_require_or_infer_producer_provenance(self):
        original = copy.deepcopy(self.manifest)
        for selected in (None, "none"):
            for production_provenance in (False, True):
                with self.subTest(selected=selected, production_provenance=production_provenance):
                    self.manifest = copy.deepcopy(original)
                    if production_provenance:
                        self.add_production_pe_provenance()
                    else:
                        self.save_manifest()
                    code, receipt, calls, _output, _loaded = self.run_case(
                        [(self.native(), 0), (self.native(True), 0)], selected)
                    self.assertEqual(code, 0)
                    self.assertEqual(receipt["producer_contract"], "none")
                    for call in calls:
                        self.assertEqual(call[call.index("--producer-contract") + 1], "none")

    def test_selected_contract_cannot_bypass_original_component_digest_authentication(self):
        self.add_production_pe_provenance()
        original = copy.deepcopy(self.manifest)
        for name in self.launcher.COMPONENTS:
            with self.subTest(component=name):
                self.manifest = copy.deepcopy(original)
                self.manifest["components"][name]["sha256"] = "2" * 64
                self.save_manifest()
                code, receipt, calls, _output, loaded = self.run_case([], "pe-present-reservation-v1")
                self.assertEqual(code, 1)
                self.assertFalse(receipt["accepted"])
                self.assertFalse(receipt["manifest_authenticated"])
                self.assertEqual(calls, [])
                loaded.assert_called_once_with(self.bundle, allow_identity_v4=True)

    def test_native_contract_echo_must_match_inspection_and_replay(self):
        self.add_production_pe_provenance()
        for contract in self.launcher.PRODUCER_CONTRACTS:
            other = next(value for value in self.launcher.PRODUCER_CONTRACTS if value != contract)
            for step in ("inspect", "replay"):
                for echo in (None, other):
                    with self.subTest(contract=contract, step=step, echo=echo):
                        result = self.native(step == "replay", contract)
                        if echo is None:
                            del result["producer_contract"]
                        else:
                            result["producer_contract"] = echo
                        responses = [] if step == "inspect" else [(self.native(producer_contract=contract), 0)]
                        code, receipt, calls, _output, _loaded = self.run_case(responses + [(result, 0)], contract)
                        self.assertEqual(code, 1)
                        self.assertFalse(receipt["accepted"])
                        self.assertEqual(len(calls), len(responses) + 1)
                        self.assertIn("producer contract echo", receipt["failure"])
                        if step == "inspect":
                            self.assertEqual(receipt["steps"]["replay"]["status"], "skipped")

    def test_bad_inspection_never_replays(self):
        for changes in (None, {"plan_sha256": "0" * 64}, {"device_created": True}):
            with self.subTest(changes=changes):
                response = b"{malformed" if changes is None else dict(self.native(), **changes)
                code, receipt, calls, output, _loaded = self.run_case([(response, 0)])
                self.assertEqual(code, 1)
                self.assertFalse(receipt["accepted"])
                self.assertEqual(len(calls), 1)
                self.assertEqual(receipt["steps"]["replay"]["status"], "skipped")
                self.assertEqual((output / "replay.stdout").read_bytes(), b"")

    def test_nonzero_native_exit_retains_structured_failure_and_logs(self):
        for step in ("inspect", "replay"):
            with self.subTest(step=step):
                rejection = dict(self.native(step == "replay"), status="rejected", qualified=False,
                                 failure="v4-local-arm-failed", failed_event=11)
                if step == "replay":
                    rejection["readback_artifacts"] = {
                        "requested": True, "attempted": True,
                        "directory": str(self.root / f"result-{self.count + 1}"),
                        "failure": "exclusive write failed",
                        "source": {"attempted": True, "written": True, "bytes": 4,
                                   "written_bytes": 4, "sha256": "2" * 64, "failure": "none"},
                        "output": {"attempted": True, "written": False, "bytes": 4,
                                   "written_bytes": 0, "sha256": "3" * 64, "failure": "exclusive write failed"},
                    }
                responses = [] if step == "inspect" else [(self.native(), 0)]
                code, receipt, calls, output, _loaded = self.run_case(responses + [(rejection, 17)])
                self.assertEqual(code, 1)
                self.assertEqual(len(calls), len(responses) + 1)
                evidence = receipt["steps"][step]
                self.assertEqual(evidence["returncode"], 17)
                self.assertEqual(evidence["native_result"], rejection)
                self.assertEqual(json.loads((output / f"{step}.stdout").read_bytes()), rejection)
                self.assertFalse(receipt["accepted"])
                if step == "replay":
                    self.assertEqual(evidence["native_result"]["readback_artifacts"], rejection["readback_artifacts"])
                    self.assertEqual(calls[-1][calls[-1].index("--readback-dir") + 1], str(output))

    def test_child_timeout_preserves_partial_logs_and_never_accepts(self):
        for step in ("inspect", "replay"):
            with self.subTest(step=step):
                responses = [] if step == "inspect" else [(self.native(), 0)]
                code, receipt, calls, output, _loaded = self.run_case(
                    responses + [(subprocess.TimeoutExpired("mock-native", 10), 0)])
                self.assertEqual(code, 1)
                self.assertEqual(len(calls), len(responses) + 1)
                self.assertEqual(receipt["steps"][step]["status"], "child-timeout")
                self.assertTrue(receipt["steps"][step]["timed_out"])
                self.assertEqual((output / f"{step}.stdout").read_bytes(), b"partial native output")
                self.assertFalse(receipt["accepted"])

    def test_absent_missing_or_unauthenticated_oracles_never_spawn(self):
        original = copy.deepcopy(self.manifest)
        for name in ("source_oracle", "output_oracle"):
            path = self.bundle / original["components"][name]["path"]
            raw = path.read_bytes()
            for mutation in ("absent", "missing", "digest"):
                with self.subTest(oracle=name, mutation=mutation):
                    self.manifest = copy.deepcopy(original)
                    if mutation == "absent":
                        del self.manifest["components"][name]
                    elif mutation == "missing":
                        path.unlink()
                    else:
                        self.manifest["components"][name]["sha256"] = "2" * 64
                    self.save_manifest()
                    code, receipt, calls, _output, loaded = self.run_case([])
                    self.assertEqual(code, 1)
                    self.assertEqual(calls, [])
                    self.assertFalse(receipt["manifest_authenticated"])
                    self.assertEqual(receipt["steps"]["inspect"]["status"], "skipped")
                    if mutation == "digest":
                        loaded.assert_called_once_with(self.bundle, allow_identity_v4=True)
                    path.write_bytes(raw)

    def test_capture27_blob_count_fits_without_large_fixture_files(self):
        manifest = copy.deepcopy(self.manifest)
        for entry in manifest["components"].values():
            if isinstance(entry, dict):
                entry["bytes"] = 1
        manifest["components"]["blobs"] = [
            {"path": f"blobs/{index:064x}.bin", "bytes": 1} for index in range(1, 4753)]
        raw = json.dumps(manifest).encode()
        self.assertLessEqual(len(raw), self.launcher.MAX_MANIFEST_BYTES)
        self.assertEqual(self.launcher.MAX_BLOBS, 65536)
        with mock.patch.object(self.launcher, "read_capped", return_value=raw), \
             mock.patch.object(pathlib.Path, "stat", return_value=types.SimpleNamespace(st_size=1)), \
             mock.patch.object(pathlib.Path, "exists", return_value=False):
            admitted, _raw, total = self.launcher.bound_inputs(self.bundle, time.monotonic() + 10)
        self.assertEqual(len(admitted["components"]["blobs"]), 4752)
        self.assertEqual(total, len(raw) + 4752 + 5)


if __name__ == "__main__":
    unittest.main()
