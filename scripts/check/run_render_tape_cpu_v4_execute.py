#!/usr/bin/env python3
"""Authenticate one fresh observer-light controlled CPU V4 execution child.

Run through scripts/run_python.sh. Completion proves only the explicit CPU
execution/lifecycle/omission contract and requested route exercise. Native
executor clocks are retained as observations, never as timing qualification.
Semantic/GPU/policy qualification must be bound by a separate aggregate.
The preservation contract remains a caller assumption, not capture evidence.
"""

from __future__ import annotations

import datetime
import hashlib
import importlib.util
import json
import math
import os
import pathlib
import sys
import time
from typing import Any


def load_semantics_launcher():
    path = pathlib.Path(__file__).with_name("run_render_tape_cpu_v4_semantics.py")
    spec = importlib.util.spec_from_file_location("controlled_cpu_v4_execute_shared", path)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load the controlled CPU V4 authentication helpers")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


semantics = load_semantics_launcher()
auth = semantics.auth
Rejected = semantics.Rejected
load_bundle_tool = semantics.load_bundle_tool
authenticate = semantics.authenticate
fingerprint = semantics.fingerprint
count = semantics.count
flags = semantics.flags
object_field = semantics.object_field
validate_route_coverage = semantics.validate_route_coverage
capacity_policy = semantics.capacity_policy
replay_policy = semantics.replay_policy
expected_stage_environment = semantics.expected_stage_environment
validate_capacity = semantics.validate_capacity
validate_replay_policy = semantics.validate_replay_policy
REPLAY_POLICY_SCHEMA = semantics.REPLAY_POLICY_SCHEMA
PRESERVATION_CONTRACT = semantics.PRESERVATION_CONTRACT
MAX_WALL_MS = semantics.MAX_WALL_MS
MAX_IMAGES = semantics.MAX_IMAGES
MAX_ITEMS = semantics.MAX_ITEMS
U64_MAX = semantics.U64_MAX
MIN_SLOT_HEADROOM_BYTES = semantics.MIN_SLOT_HEADROOM_BYTES
MAX_SLOT_HEADROOM_BYTES = semantics.MAX_SLOT_HEADROOM_BYTES
MAX_AGGREGATE_HEADROOM_BYTES = semantics.MAX_AGGREGATE_HEADROOM_BYTES
MIN_DRAW_COMMAND_LIMIT = semantics.MIN_DRAW_COMMAND_LIMIT
MAX_DRAW_COMMAND_LIMIT = semantics.MAX_DRAW_COMMAND_LIMIT
NATIVE_SCHEMA = "dxmt9.controlled-cpu-v4-execute.v1"
RECEIPT_SCHEMA = "dxmt9.controlled-cpu-v4-execute-run.v1"
BROAD_FALSE = semantics.BROAD_FALSE
SEMANTIC_FALSE = semantics.SEMANTIC_FALSE
FALSE_CLAIMS = tuple(dict.fromkeys(BROAD_FALSE + SEMANTIC_FALSE + ("logical_stream_complete",)))
OMISSIONS = (
    "resource_versions_omitted", "record_contexts_omitted", "expected_images_omitted",
    "canonical_draw_copies_omitted", "payload_observer_omitted", "source_row_reads_omitted",
    "logical_hashing_omitted", "diagnostic_mutation_receipts_omitted",
    "captured_binding_images_omitted", "upload_byte_comparisons_omitted",
)
STAGE_ENVIRONMENT = dict(semantics.STAGE_ENVIRONMENT)
IDENTITY_FIELDS = (
    "queue", "capture_token", "capture_epoch", "sequence", "source_index",
    "source_generation", "storage_first_page", "storage_page_count", "storage_generation",
)


def empty_table(value: dict, name: str) -> None:
    if not isinstance(value.get(name), list) or value[name]:
        raise Rejected(f"execution-only diagnostic table {name} must be present and empty")


def validate_images(native: dict, binary: pathlib.Path, binary_evidence: dict,
                    runner, deadline: float) -> None:
    semantics.validate_images(native, binary, binary_evidence, runner, deadline)
    basenames = {pathlib.Path(image["path"]).name for image in native["loaded_images"]}
    if not {"libdxmt9_native.dylib", "winemetal_dxmt9.so"} <= basenames:
        raise Rejected("native loaded image evidence omits a required actual runtime module")


def validate_clocks(execution: dict) -> None:
    clocks = object_field(execution, "clocks")
    for name, expected in (
        ("clock_domain", "steady-wall-and-process-cpu"),
        ("process_cpu_clock", "CLOCK_PROCESS_CPUTIME_ID"),
        ("scope", "executor-run-only"),
        ("body_scope", "after-setup-and-arm-through-consumer-join"),
    ):
        if clocks.get(name) != expected:
            raise Rejected("native execution clock scope/domain rejected")
    flags(clocks, ("body_excludes_setup_and_teardown",), True)
    for name in ("setup", "body", "teardown"):
        scope = object_field(clocks, name)
        flags(scope, ("started", "valid", "complete"), True)
        # Integers deliberately preserve native nanoseconds exactly. Process
        # CPU is summed across threads and must not be forced <= wall time.
        count(scope, "wall_ns", 0, U64_MAX)
        count(scope, "process_cpu_ns", 0, U64_MAX)


def validate_omissions(execution: dict) -> dict:
    if execution.get("execution_mode") != "execution-only":
        raise Rejected("native executor did not select execution-only mode")
    flags(execution, ("metadata_inventory_complete",), False)
    omitted = object_field(execution, "execution_only")
    flags(omitted, ("requested", "complete", "lifecycle_coverage_complete") + OMISSIONS, True)
    empty_table(execution, "sources")
    versions = object_field(execution, "buffer_version_projection")
    empty_table(versions, "rows")
    flags(versions, ("logical_epoch_authenticated_by_revision", "backing_retained",
                     "old_ring_epoch_proven", "physical_byte_proof"), False)
    budget = object_field(execution, "expected_buffer_budget")
    count(budget, "charged_bytes", 0, 0)
    count(budget, "high_water_bytes", 0, 0)
    flags(budget, ("physical_byte_proof",), False)
    logical = object_field(execution, "logical_semantics")
    flags(logical, ("requested", "admitted", "accepted", "complete",
                    "legal_candidate_nooverwrite_contract", "conditional_on_legal_nooverwrite",
                    "logical_stream_complete") + SEMANTIC_FALSE, False)
    if logical.get("failure") != "":
        raise Rejected("execution-only logical observer failure evidence rejected")
    for name in ("sources", "commands", "draws", "original_items", "source_rows_required",
                 "source_rows_qualified", "logical_buffer_hash_bytes"):
        count(logical, name, 0, 0)
    empty_table(logical, "receipts")
    rows = object_field(execution, "source_row_observation")
    count(rows, "qualified_draws", 0, 0)
    count(rows, "not_qualified_draws", 0, 0)
    flags(rows, ("shader_demand_complete", "future_preservation", "physical_fetch_complete"), False)
    counters = object_field(execution, "counters")
    for name in ("record_contexts", "buffer_versions_observed", "buffer_versions_unavailable"):
        count(counters, name, 0, 0)
    return omitted


def validate_lifecycle(execution: dict, omitted: dict, captured_sources: int) -> None:
    flags(execution, ("complete", "runtime_budgeted_attempt", "source_progress_complete", "reset_ok"), True)
    if (execution.get("failure") != "" or "failed_event" not in execution
            or execution["failed_event"] is not None or "resource_version_failure" not in execution
            or execution["resource_version_failure"] is not None):
        raise Rejected("native execution-only executor reports a failure")
    consumer = object_field(execution, "consumer")
    flags(consumer, ("worker_exited", "producer_closed", "cleanup_complete"), True)
    flags(consumer, ("cleanup_failed",), False)
    if (count(consumer, "failure", 0, 0) != 0 or count(consumer, "phase", 3, 3) != 3
            or consumer.get("completion_policy") != "immediate-fifo-cpu-acknowledgement"):
        raise Rejected("native CPU consumer did not stop with controlled FIFO acknowledgement")
    active = object_field(consumer, "active_identity")
    for name in IDENTITY_FIELDS:
        count(active, name, 0, 0)
    sources = count(omitted, "sources", 1, MAX_ITEMS)
    commands = count(omitted, "commands", 1, MAX_ITEMS)
    draws = count(omitted, "draws", 1, MAX_ITEMS)
    for name in ("observed", "submitted", "acknowledged", "retired", "receipts"):
        if count(consumer, name, 1, MAX_ITEMS) != sources:
            raise Rejected("native source submission/ACK/retirement coverage does not conserve sources")
    for name in ("submitted", "acknowledged", "retired"):
        if count(omitted, name, 1, MAX_ITEMS) != sources:
            raise Rejected("native execution-only source coverage disagrees with consumer")
    if (count(consumer, "commands", 1, MAX_ITEMS) != commands
            or count(consumer, "lifecycle_actions", 4, MAX_ITEMS * 4) != sources * 4):
        raise Rejected("native consumer command/lifecycle-action conservation rejected")
    receipts = execution.get("consumer_receipts")
    if not isinstance(receipts, list) or len(receipts) != sources:
        raise Rejected("native consumer lifecycle receipt count mismatch")
    prior_sequence, binding = 0, None
    for receipt in receipts:
        if not isinstance(receipt, dict):
            raise Rejected("native consumer lifecycle receipt is malformed")
        flags(receipt, ("submitted", "acknowledged", "retired", "cleanup_complete"), True)
        identity = object_field(receipt, "identity")
        values = {name: count(identity, name) for name in IDENTITY_FIELDS}
        for name in ("queue", "capture_token", "capture_epoch", "sequence",
                     "source_generation", "storage_generation"):
            if values[name] == 0:
                raise Rejected("native consumer lifecycle receipt lacks a genuine positive identity")
        current_binding = tuple(values[name] for name in ("queue", "capture_token", "capture_epoch"))
        if (binding is not None and binding != current_binding) or values["sequence"] <= prior_sequence:
            raise Rejected("native consumer source identities are duplicated, unordered or differently bound")
        binding, prior_sequence = current_binding, values["sequence"]
    counters = object_field(execution, "counters")
    if (count(counters, "draws", 1, MAX_ITEMS) != draws
            or count(counters, "resources_created") != count(counters, "resources_released")
            or count(counters, "setup_submissions") != count(counters, "setup_acknowledgements")):
        raise Rejected("native execution draw/resource/setup-ACK conservation rejected")
    setup = object_field(execution, "initial_setup")
    flags(setup, ("host_checkpoint_joined",), True)
    owners = count(setup, "owners", 0, MAX_ITEMS)
    seeds = count(setup, "seeds", 0, MAX_ITEMS)
    for name in ("created", "prepared", "activated"):
        if count(setup, name, 0, MAX_ITEMS) != owners:
            raise Rejected("native initial owner setup/activation coverage rejected")
    for name in ("staged_seeds", "consumed_seeds"):
        if count(setup, name, 0, MAX_ITEMS) != seeds:
            raise Rejected("native initial seed staging/consumption coverage rejected")
    count(setup, "aliases", 0, MAX_ITEMS)
    count(setup, "released_hidden", 0, owners)
    count(setup, "seed_bytes")
    count(setup, "allocation_bytes")
    provenance = object_field(execution, "provenance")
    flags(provenance, ("canonical_traversal_complete", "raw_ack_coverage_complete",
                       "canonical_origin_mapping_complete", "controlled_cpu_complete",
                       "present_preparation_attempted", "present_preparation_completed"), True)
    # Obligations are indexed by original captured sources. Genuine candidate
    # batching can change its physical source count without changing these.
    if (count(provenance, "source_obligations_registered", 1, MAX_ITEMS) != captured_sources
            or count(provenance, "source_obligations_qualified", 1, MAX_ITEMS) != captured_sources):
        raise Rejected("native canonical source obligations lack complete qualification")
    count(provenance, "replayed_events", 1, MAX_ITEMS)
    count(provenance, "dispatched_records", 1, MAX_ITEMS)
    count(provenance, "acknowledged_raws", 1, MAX_ITEMS)
    query = object_field(execution, "query_projection")
    flags(query, ("gpu_result_oracle",), False)
    if type(query.get("present")) is not bool:
        raise Rejected("native query presence evidence is missing")
    polls = count(query, "recorded_polls", 0, MAX_ITEMS)
    if count(query, "replayed_polls", 0, MAX_ITEMS) != polls or count(query, "rows", 0, MAX_ITEMS) != polls:
        raise Rejected("native recorded query projection was not completely replayed")
    for name in ("definitions", "issues", "checkpoints"):
        value = count(query, name, 0, MAX_ITEMS)
        if not query["present"] and value:
            raise Rejected("absent native query projection contains query work")
    if not query["present"] and polls:
        raise Rejected("absent native query projection contains polls")


def validate_native(native: dict, args, manifest: dict, action: dict, wall_ms: int) -> None:
    # This is an independent execution-only validator. The semantic validator
    # is intentionally not invoked with its evidence requirements removed.
    if native.get("schema") != NATIVE_SCHEMA or native.get("mode") != "controlled-cpu-v4-execute":
        raise Rejected("native schema/mode is not the controlled CPU execution-only endpoint")
    flags(native, ("diagnostic", "complete", "fixture_teardown_complete", "authority_scope_valid",
                   "metal_resource_device_created"), True)
    flags(native, BROAD_FALSE + ("native_producer_attestation_authenticated",), False)
    if (native.get("phase") != "execution-only-complete" or native.get("failure") != ""
            or any(name not in native or native[name] is not None for name in
                   ("failed_input_component", "failed_event", "failed_event_ordinal"))
            or count(native, "iterations", 1, 1) != 1
            or count(native, "max_wall_ms", 1, MAX_WALL_MS) != wall_ms
            or native.get("producer_contract") != args.producer_contract
            or native.get("requested_route") != args.route or native.get("effective_route") != args.route
            or native.get("buffer_preservation_contract") != args.buffer_preservation_contract
            or native.get("requires_launcher_pe_attestation") is not
               (args.producer_contract == "pe-present-reservation-v1")
            or native.get("input_admission_profile") != "controlled-v4-raw256"):
        raise Rejected("native execution-only completion/contract/failure evidence rejected")
    validate_replay_policy(native, args)
    authentication = object_field(native, "authentication")
    flags(authentication, ("owned_inputs_authenticated", "wire_valid", "general_valid", "pair_valid",
                           "action_profile_authenticated", "stable_exact_action_witness",
                           "original_oracle_bytes_authenticated_only", "plan_digest_matched"), True)
    blobs = manifest["components"].get("blobs", [])
    if (count(authentication, "blob_count", 0, auth.MAX_BLOBS) != len(blobs)
            or count(authentication, "blob_bytes", 0, auth.MAX_INPUT_BYTES) != sum(blob["bytes"] for blob in blobs)):
        raise Rejected("native authenticated blob count/bytes differ from immutable bundle")
    hashes = object_field(native, "sha256")
    for name, component in (("events", "events"), ("identity", "identity"),
                            ("execution_topology", "execution_topology"),
                            ("source", "source_oracle"), ("output", "output_oracle"),
                            ("action_profile", "action_profile")):
        if hashes.get(name) != manifest["components"][component]["sha256"]:
            raise Rejected(f"native authenticated {name} digest mismatch")
    if hashes.get("expected_plan") != args.expected_plan_digest or hashes.get("actual_plan") != args.expected_plan_digest:
        raise Rejected("native expected/actual plan digest mismatch")
    witness = object_field(native, "action_witness")
    for name in ("capture_token", "capture_epoch", "queue", "construction_profile", "arm_profile",
                 "freeze_profile", "arm_revision", "freeze_revision"):
        if count(witness, name) != int(action["witness"][name]):
            raise Rejected("native action witness differs from the authenticated original binding")
    fixture = object_field(native, "fixed_fixture_setup")
    flags(fixture, ("render_workers_disabled_verified", "replay_offload_absent_verified",
                    "exact_queue_action_profile_verified"), True)
    flags(fixture, ("cpu_ready_arena_replay_supported",), args.route == "compatibility")
    flags(fixture, ("direct_chunk_slot_replay_supported",), args.route == "production-slot")
    for name, expected in (("queue_construction_profile", 1), ("queue_current_profile", 1),
                           ("queue_profile_revision", 0)):
        if count(fixture, name) != expected:
            raise Rejected("native fixture queue action profile rejected")
    flags(object_field(native, "input_admission"), ("accepted",), True)
    plan_counts = object_field(native, "plan_counts")
    count(plan_counts, "events", 1, MAX_ITEMS)
    count(plan_counts, "records", 1, MAX_ITEMS)
    captured_sources = count(plan_counts, "sources", 1, MAX_ITEMS)
    execution = object_field(native, "execution")
    omitted = validate_omissions(execution)
    validate_lifecycle(execution, omitted, captured_sources)
    validate_clocks(execution)
    validate_route_coverage(execution, args.route, count(omitted, "draws", 1, MAX_ITEMS))


def parser():
    result = semantics.parser()
    result.description = __doc__
    return result


def main() -> int:
    arguments = parser()
    args = arguments.parse_args()
    if not math.isfinite(args.timeout_seconds) or not 1 <= args.timeout_seconds <= 600:
        arguments.error("--timeout-seconds must be finite and within 1..600")
    if not auth.sha256_nonzero(args.expected_plan_digest):
        arguments.error("--expected-plan-digest must be a nonzero lowercase SHA-256")
    try:
        policy = capacity_policy(args)
        full_policy = replay_policy(args)
        stage_environment = expected_stage_environment(policy, full_policy["draw_command_limit"])
    except Rejected as error:
        arguments.error(str(error))
    bundle, binary, output = args.bundle.resolve(), args.diagnostic.resolve(), args.output_dir.resolve()
    if args.output_dir.exists() or args.output_dir.is_symlink() or output.exists():
        arguments.error("--output-dir must name a new path")
    if output.is_relative_to(bundle):
        arguments.error("--output-dir must not be inside the immutable input bundle")
    if not bundle.is_dir() or not binary.is_file() or not os.access(binary, os.X_OK):
        arguments.error("--bundle must be a directory and --diagnostic an executable file")
    output.mkdir(mode=0o700, parents=True, exist_ok=False)
    for name in ("execute.stdout", "execute.stderr"):
        with (output / name).open("xb"):
            pass
    started = time.monotonic()
    deadline = started + args.timeout_seconds
    receipt_path = output / "result.json"
    receipt: dict[str, Any] = {
        "schema": RECEIPT_SCHEMA, "status": "authenticating", "accepted": False,
        "diagnostic": True, **{name: False for name in FALSE_CLAIMS},
        "execution_complete": False,
        "acceptance_scope": "observer-light-controlled-cpu-execution-only-and-requested-route-exercised",
        "bundle": str(bundle), "diagnostic_binary": str(binary), "output_dir": str(output),
        "expected_plan_digest": args.expected_plan_digest, "producer_contract": args.producer_contract,
        "buffer_preservation_contract": args.buffer_preservation_contract,
        "preservation_contract_is_caller_assumption": True, "conditional_on_legal_nooverwrite": True,
        "route": args.route, "requested_route": args.route, "effective_route": None,
        "capacity_policy": policy, "expected_stage_environment": stage_environment,
        "replay_policy": full_policy, "replay_policy_valid": False,
        "effective_stage_environment": None, "capacity_policy_valid": False, "stage_environment_valid": False,
        "iterations": 1, "manifest_authenticated": False,
        "bundle_reauthenticated": False, "binary_reauthenticated": False, "loaded_images_stable": False,
        "launcher_argv": sys.argv, "cwd": os.getcwd(), "environment": auth.environment_provenance(),
        "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "timeout_seconds": args.timeout_seconds,
        "limits": {"manifest_bytes": auth.MAX_MANIFEST_BYTES, "input_bytes": auth.MAX_INPUT_BYTES,
                   "blobs": auth.MAX_BLOBS, "native_result_json_bytes": auth.MAX_RESULT_JSON_BYTES,
                   "loaded_images": MAX_IMAGES, "native_max_wall_ms": MAX_WALL_MS,
                   "minimum_positive_slot_headroom_bytes": MIN_SLOT_HEADROOM_BYTES,
                   "slot_headroom_bytes": MAX_SLOT_HEADROOM_BYTES,
                   "aggregate_headroom_bytes": MAX_AGGREGATE_HEADROOM_BYTES,
                   "minimum_positive_draw_command_limit": MIN_DRAW_COMMAND_LIMIT,
                   "draw_command_limit": MAX_DRAW_COMMAND_LIMIT},
        "authentication_deadline_enforcement": "byte caps and synchronous-loader before/after checks; no asynchronous interrupt",
        "steps": {"execute": {"status": "not-started", "argv": None,
                               "stdout": "execute.stdout", "stderr": "execute.stderr"}},
    }
    runner, manifest, manifest_raw, action = None, None, None, None
    exit_code = 1
    try:
        auth.write_receipt(receipt_path, receipt)
        runner = load_bundle_tool()
        receipt["diagnostic_executable"] = fingerprint(binary, runner, deadline)
        manifest, manifest_raw, total, action = authenticate(bundle, runner, deadline, args.producer_contract)
        auth.require_regular_inputs(bundle, manifest)
        receipt.update(manifest_authenticated=True, bounded_input_bytes=total,
                       input_components=manifest["components"], action_profile_validation=action,
                       input_manifest={"path": "manifest.json", "bytes": len(manifest_raw),
                                       "sha256": hashlib.sha256(manifest_raw).hexdigest()})
        wall_ms = min(MAX_WALL_MS, int(auth.require_time(deadline) * 1000))
        if wall_ms <= 0:
            raise Rejected("shared execution deadline exhausted before native admission")
        argv = [str(binary), "--controlled-cpu-v4-execute", "--bundle", str(bundle),
                "--route", args.route, "--producer-contract", args.producer_contract,
                "--buffer-preservation-contract", args.buffer_preservation_contract,
                "--expected-plan-digest", args.expected_plan_digest, "--max-wall-ms", str(wall_ms),
                "--slot-headroom-bytes", str(policy["slot_headroom_bytes"]),
                "--aggregate-headroom-bytes", str(policy["aggregate_headroom_bytes"]),
                "--draw-command-limit", str(full_policy["draw_command_limit"])]
        for component, (_path, option) in auth.COMPONENTS.items():
            digest = manifest["components"][component]["sha256"]
            if not auth.sha256_nonzero(digest):
                raise Rejected(f"invalid authenticated component digest: {component}")
            argv += [option, digest]
        argv += [auth.ACTION_PROFILE_COMPONENT[1], manifest["components"]["action_profile"]["sha256"]]
        receipt.update(native_max_wall_ms=wall_ms, status="executing")
        native = auth.run_child("execute", argv, output, deadline, receipt, receipt_path)
        validate_native(native, args, manifest, action, wall_ms)
        receipt.update(effective_route=native["effective_route"],
                       effective_stage_environment=native["effective_stage_environment"],
                       capacity_policy_valid=True, stage_environment_valid=True, replay_policy_valid=True,
                       route_coverage=native["execution"]["route_coverage"],
                       execution=native["execution"], clocks=native["execution"]["clocks"])
        exit_code = 0
    except KeyboardInterrupt:
        receipt.update(status="interrupted", failure="launcher interrupted")
        exit_code = 130
    except (Rejected, OSError, ValueError, SystemExit) as error:
        receipt.update(status="rejected", failure=str(error))
    finally:
        # Preserve parseable rejected native evidence and the first failure.
        # Reauthentication cannot upgrade an execution rejection to success.
        if runner is not None and receipt["manifest_authenticated"]:
            try:
                current, current_raw, _total, current_action = authenticate(bundle, runner, deadline, args.producer_contract)
                auth.require_regular_inputs(bundle, current)
                if current != manifest or current_raw != manifest_raw or current_action != action:
                    raise Rejected("immutable bundle changed across controlled CPU execution")
                receipt["bundle_reauthenticated"] = True
                if fingerprint(binary, runner, deadline) != receipt["diagnostic_executable"]:
                    raise Rejected("native executable changed across controlled CPU execution")
                receipt["binary_reauthenticated"] = True
                native = receipt["steps"]["execute"].get("native_result")
                if native is not None:
                    validate_replay_policy(native, args)
                    receipt["loaded_images"] = native.get("loaded_images")
                    validate_images(native, binary, receipt["diagnostic_executable"], runner, deadline)
                    receipt["loaded_images_stable"] = True
            except (Rejected, OSError, ValueError, SystemExit) as error:
                receipt["reauthentication_failure"] = str(error)
                if "failure" not in receipt:
                    receipt["failure"] = str(error)
                if exit_code != 130:
                    exit_code = 1
                receipt["status"] = "interrupted" if exit_code == 130 else "rejected"
        step = receipt["steps"]["execute"]
        if step["status"] == "not-started":
            step.update(status="skipped", failure="prior authentication/deadline failure")
        receipt["accepted"] = (exit_code == 0 and receipt["bundle_reauthenticated"]
                               and receipt["binary_reauthenticated"] and receipt["loaded_images_stable"]
                               and receipt["capacity_policy_valid"] and receipt["stage_environment_valid"]
                               and receipt["replay_policy_valid"])
        if receipt["accepted"]:
            receipt.update(status="complete", execution_complete=True)
        elif exit_code == 0:
            receipt.update(status="rejected", failure="required post-run authentication incomplete")
            exit_code = 1
        receipt["elapsed_seconds"] = time.monotonic() - started
        auth.write_receipt(receipt_path, receipt)
    print(json.dumps({"schema": RECEIPT_SCHEMA, "status": receipt["status"], "accepted": receipt["accepted"],
                      "receipt": str(receipt_path), "failure": receipt.get("failure")}))
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
