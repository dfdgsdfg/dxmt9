#!/usr/bin/env python3
"""One private controlled CPU V4 observed logical-stream diagnostic.

Run through scripts/run_python.sh. The preservation contract is a caller
assumption, not an authenticated property of the capture. This launcher does
not enable public V4 execution, GPU/pixel qualification, policy or timing.
Accepted means collection of an original-keyed field stream with scoped byte
checks, not independently verified original scalar/uniform equivalence.
ProductionSlot acceptance additionally requires observed direct work; natural
mixed compatibility fallback remains reported and allowed.
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import importlib.util
import json
import math
import os
import pathlib
import re
import sys
import time
from typing import Any


def load_authenticator():
    path = pathlib.Path(__file__).with_name("run_render_tape_v4_capture_diagnostic.py")
    spec = importlib.util.spec_from_file_location("controlled_cpu_v4_authenticator", path)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load the private V4 capture authenticator")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


auth = load_authenticator()
Rejected = auth.Rejected
load_bundle_tool = auth.load_bundle_tool
RECEIPT_SCHEMA = "dxmt9.controlled-cpu-v4-semantics-run.v1"
NATIVE_SCHEMA = "dxmt9.controlled-cpu-v4-semantics.v1"
PRESERVATION_CONTRACT = "candidate-legal-nooverwrite-closed-writers-v1"
ROUTES = ("compatibility", "production-slot")
MAX_WALL_MS = 300000
MAX_IMAGES = 256
MAX_ITEMS = 1048576
U64_MAX = (1 << 64) - 1
MIN_SLOT_HEADROOM_BYTES = 1024 * 1024
MAX_SLOT_HEADROOM_BYTES = 64 * 1024 * 1024
MAX_AGGREGATE_HEADROOM_BYTES = 512 * 1024 * 1024
CAPACITY_FIELDS = ("slot_headroom_bytes", "aggregate_headroom_bytes")
REPLAY_POLICY_SCHEMA = "dxmt9.render_tape.v4_replay_policy.v1"
MIN_DRAW_COMMAND_LIMIT = 3
MAX_DRAW_COMMAND_LIMIT = 64
REPLAY_POLICY_FIELDS = ("schema", "route", "draw_command_limit", *CAPACITY_FIELDS)
STAGE_ENVIRONMENT = {
    "DXMT9_DISABLE_PRESENT_BOUNDARY": "1", "DXMT9_SYNC_PRESENT_FLUSH": "0",
    "DXMT9_OFFLOAD_COMMIT_REPLAY": "0", "DXMT9_DRAW_CHUNK_COMMAND_LIMIT": "0",
    "DXMT9_CHUNK_DRAW_PAYLOAD_ARENA_LIMIT_BYTES": "0", "DXMT9_CPU_READY_TAPE": "0",
    "DXMT9_CPU_READY_EARLY_PREFIX": "0", "DXMT9_SPLIT_STRETCH_CHUNK": "0",
    "DXMT_PERF_COUNTERS": "1", "DXMT9_DIRECT_SLOT_HEADROOM_BYTES": "0",
    "DXMT9_DIRECT_SLOT_AGGREGATE_HEADROOM_BYTES": "0",
}
BROAD_FALSE = (
    "full_semantic_oracle_complete", "actual_metal", "gpu_replay",
    "gpu_result_oracle", "pixel_readback", "policy_qualified", "timing_eligible",
    "physical_fetch_oracle_complete", "public_v4_execution_enabled",
    "future_backing_preservation_complete",
)
SEMANTIC_FALSE = ("gpu_result_oracle", "policy_qualified", "timing_eligible",
                  "future_preservation_qualified", "full_shader_fetch_coverage_qualified",
                  "full_logical_semantic_oracle_complete", "original_semantic_equivalence_qualified",
                  "cross_policy_equivalence_qualified")
# core::MetalCommandKind -> RenderTapeIdentityV3CommandKind. The inventory
# uses the latter wire enum; receipt.command_kind retains the former enum.
CORE_TO_IDENTITY_COMMAND_KIND = {
    0: 1,  # DrawRun
    1: 2,  # Clear
    2: 3,  # SurfaceCopy
    3: 4,  # StretchRect
    4: 5,  # Readback
    5: 6,  # ColorFill
    6: 7,  # DepthResolve
    7: 8,  # GenerateMipmaps
    8: 9,  # Present
}


def object_field(value: dict, name: str) -> dict:
    result = value.get(name)
    if not isinstance(result, dict):
        raise Rejected(f"native {name} must be an object")
    return result


def count(value: dict, name: str, minimum: int = 0, maximum: int = U64_MAX) -> int:
    result = value.get(name)
    if type(result) is not int or not minimum <= result <= maximum:
        raise Rejected(f"native {name} is not a bounded integer")
    return result


def flags(value: dict, names: tuple[str, ...], expected: bool) -> None:
    if any(value.get(name) is not expected for name in names):
        raise Rejected(f"native required {'true' if expected else 'false'} flags rejected: {', '.join(names)}")


def _checked_capacity_values(slot, aggregate) -> dict:
    if (type(slot) is not int or not 0 <= slot <= MAX_SLOT_HEADROOM_BYTES
            or type(aggregate) is not int or not 0 <= aggregate <= MAX_AGGREGATE_HEADROOM_BYTES):
        raise Rejected("headroom capacity values must be bounded nonnegative integers")
    if ((slot == 0) != (aggregate == 0) or aggregate < slot
            or (slot != 0 and slot < MIN_SLOT_HEADROOM_BYTES)):
        raise Rejected("headroom capacity requires exact 0/0 or a positive ordered pair with slot >=1 MiB")
    return dict(zip(CAPACITY_FIELDS, (slot, aggregate)))


def capacity_policy(args) -> dict:
    """Pure route-bound admission; omitted legacy API fields mean exact 0/0."""
    if hasattr(args, CAPACITY_FIELDS[0]) != hasattr(args, CAPACITY_FIELDS[1]):
        raise Rejected("--slot-headroom-bytes and --aggregate-headroom-bytes must be supplied together")
    policy = _checked_capacity_values(*(getattr(args, name, 0) for name in CAPACITY_FIELDS))
    if getattr(args, "route", None) not in ROUTES:
        raise Rejected("headroom capacity requires an explicit supported route")
    if args.route == "compatibility" and any(policy.values()):
        raise Rejected("Compatibility capacity policy must remain exact 0/0")
    return policy


def _checked_draw_command_limit(value, capacity: dict, route=None) -> int:
    if type(value) is not int or (value != 0 and not MIN_DRAW_COMMAND_LIMIT <= value <= MAX_DRAW_COMMAND_LIMIT):
        raise Rejected("draw-command limit must be integer 0 or within 3..64")
    if value and any(capacity.values()):
        raise Rejected("positive draw-command limit conflicts with positive headroom")
    if value and route is not None and route != "compatibility":
        raise Rejected("positive draw-command limit requires Compatibility")
    return value


def replay_policy(args) -> dict:
    capacity = capacity_policy(args)
    draw_limit = _checked_draw_command_limit(getattr(args, "draw_command_limit", 0), capacity, args.route)
    return {"schema": REPLAY_POLICY_SCHEMA, "route": args.route,
            "draw_command_limit": draw_limit, **capacity}


def expected_stage_environment(policy: dict, draw_command_limit=0) -> dict:
    if not isinstance(policy, dict) or set(policy) != set(CAPACITY_FIELDS):
        raise Rejected("capacity policy requires exactly the paired headroom fields")
    checked = _checked_capacity_values(*(policy[name] for name in CAPACITY_FIELDS))
    draw_limit = _checked_draw_command_limit(draw_command_limit, checked)
    return {**STAGE_ENVIRONMENT,
            "DXMT9_DRAW_CHUNK_COMMAND_LIMIT": str(draw_limit),
            "DXMT9_DIRECT_SLOT_HEADROOM_BYTES": str(checked["slot_headroom_bytes"]),
            "DXMT9_DIRECT_SLOT_AGGREGATE_HEADROOM_BYTES": str(checked["aggregate_headroom_bytes"])}


def validate_capacity(native: dict, args) -> dict:
    policy = capacity_policy(args)
    observed = object_field(native, "capacity_policy")
    # The derivation also rejects booleans and extraneous fields rather than
    # allowing Python's bool/int dictionary equality to establish identity.
    expected_stage_environment(observed)
    if observed != policy:
        raise Rejected("native CPU capacity policy does not match the explicit bounded request")
    draw_limit = _checked_draw_command_limit(getattr(args, "draw_command_limit", 0), policy, args.route)
    if native.get("effective_stage_environment") != expected_stage_environment(policy, draw_limit):
        raise Rejected("native effective CPU stage environment does not match actual capacity/isolation pins")
    return policy


def validate_replay_policy(native: dict, args) -> dict:
    expected = replay_policy(args)
    observed = object_field(native, "replay_policy")
    if set(observed) != set(REPLAY_POLICY_FIELDS) or observed.get("schema") != REPLAY_POLICY_SCHEMA:
        raise Rejected("native replay policy schema or fields do not match the versioned contract")
    observed_args = argparse.Namespace(**{name: observed[name] for name in REPLAY_POLICY_FIELDS if name != "schema"})
    actual = replay_policy(observed_args)
    if observed != actual or actual != expected:
        raise Rejected("native full replay policy does not match the explicit bounded request")
    validate_capacity(native, args)
    return expected


def headroom_decimal(value: str) -> int:
    if re.fullmatch(r"[0-9]{1,10}", value) is None:
        raise argparse.ArgumentTypeError("headroom bytes must be an unsigned bounded decimal")
    return int(value)


def draw_command_decimal(value: str) -> int:
    if re.fullmatch(r"[0-9]{1,10}", value) is None:
        raise argparse.ArgumentTypeError("draw-command limit must be an unsigned bounded decimal")
    return int(value)


class UniquePolicyArgument(argparse.Action):
    def __call__(self, parser, namespace, values, option_string=None):
        if hasattr(namespace, self.dest):
            parser.error(f"duplicate policy option: {option_string}")
        setattr(namespace, self.dest, values)


def fingerprint(path: pathlib.Path, runner, deadline: float) -> dict:
    auth.require_time(deadline)
    if path.is_symlink() or not path.is_file():
        raise Rejected("fingerprinted executable/image is not a regular file")
    size = path.stat().st_size
    if not 0 < size <= auth.MAX_INPUT_BYTES:
        raise Rejected("fingerprinted executable/image exceeds the diagnostic byte cap")
    digest = runner.digest(path)
    auth.require_time(deadline)
    if path.stat().st_size != size or not auth.sha256_nonzero(digest):
        raise Rejected("fingerprinted executable/image changed during hashing")
    return {"bytes": size, "sha256": digest}


def authenticate(bundle: pathlib.Path, runner, deadline: float, contract: str) -> tuple[dict, bytes, int, dict]:
    bounded, raw, total = auth.bound_inputs(bundle, deadline)
    manifest, _events, _refs, _paths, identity, source = runner.load_bundle(
        bundle, allow_identity_v4=True)
    auth.require_time(deadline)
    if (manifest != bounded or auth.read_capped(bundle / "manifest.json", auth.MAX_MANIFEST_BYTES) != raw
            or identity is None or source is None
            or manifest["components"]["identity"].get("schema") != runner.IDENTITY_V4_SCHEMA
            or manifest.get("profile") != "frame-tape"):
        raise Rejected("controlled CPU diagnostic requires an unchanged authenticated V4 frame pair and both oracles")
    auth.validate_producer_contract(manifest, contract, runner.SCHEMA)
    action = runner.load_action_profile(bundle, manifest)
    auth.require_time(deadline)
    if action.get("present") is not True or action.get("stable_exact") is not True:
        raise Rejected("controlled CPU semantics requires an authenticated stable exact action witness")
    return manifest, raw, total, action


def validate_images(native: dict, binary: pathlib.Path, binary_evidence: dict,
                    runner, deadline: float) -> None:
    images = native.get("loaded_images")
    if not isinstance(images, list) or not 1 <= len(images) <= MAX_IMAGES:
        raise Rejected("native loaded image evidence is absent or unbounded")
    seen = set()
    total = 0
    for image in images:
        if not isinstance(image, dict) or not isinstance(image.get("path"), str):
            raise Rejected("native loaded image entry is malformed")
        path = pathlib.Path(image["path"])
        if not path.is_absolute() or path.is_symlink() or path.resolve() != path or path in seen:
            raise Rejected("native loaded image path must be unique, absolute and canonical")
        seen.add(path)
        size = count(image, "bytes", 1, auth.MAX_INPUT_BYTES)
        total += size
        if total > auth.MAX_INPUT_BYTES or not auth.sha256_nonzero(image.get("sha256")):
            raise Rejected("native loaded image digest/total byte cap rejected")
        if fingerprint(path, runner, deadline) != {"bytes": size, "sha256": image["sha256"]}:
            raise Rejected("native loaded image fingerprint changed")
        if path == binary and {"bytes": size, "sha256": image["sha256"]} != binary_evidence:
            raise Rejected("native loaded executable does not match launcher authentication")
    if binary not in seen or not runner.experiment_images_stable(native):
        raise Rejected("native loaded executable/image reauthentication failed")
    auth.require_time(deadline)


def validate_route_coverage(execution: dict, route: str, draws: int) -> None:
    coverage = object_field(execution, "route_coverage")
    if coverage.get("requested_route") != route or coverage.get("effective_route") != route:
        raise Rejected("native body route coverage does not match the explicit selected route")
    flags(coverage, ("policy_selected", "observed", "monotonic", "complete"), True)
    values = {name: count(coverage, name, 0, MAX_ITEMS) for name in (
        "direct_leases", "direct_draws", "compatibility_draws", "compatibility_batches",
        "compatibility_unbatched_draws", "fallback_draws")}
    direct, compatibility = values["direct_draws"], values["compatibility_draws"]
    batched_draws = compatibility - values["compatibility_unbatched_draws"]
    if (direct + compatibility != draws or batched_draws < 0
            or values["compatibility_batches"] > batched_draws
            or (batched_draws > 0 and values["compatibility_batches"] == 0)
            or values["fallback_draws"] > compatibility):
        raise Rejected("native body route counters do not conserve original draws")
    if route == "compatibility":
        if direct or values["direct_leases"] or values["fallback_draws"]:
            raise Rejected("native Compatibility body unexpectedly reports a direct/fallback route")
    elif not direct or not values["direct_leases"]:
        # A completed all-fallback native stream is retained, but does not
        # establish that this route-verification run exercised direct work.
        raise Rejected("requested-direct-route-not-exercised")


def validate_native(native: dict, args, manifest: dict, action: dict, wall_ms: int) -> None:
    if native.get("schema") != NATIVE_SCHEMA or native.get("mode") != "controlled-cpu-v4-semantics":
        raise Rejected("native schema/mode is not the private controlled CPU semantics endpoint")
    flags(native, ("diagnostic", "complete", "fixture_teardown_complete", "authority_scope_valid",
                   "metal_resource_device_created"), True)
    flags(native, BROAD_FALSE + ("native_producer_attestation_authenticated",), False)
    if (native.get("phase") != "logical-semantics-complete" or native.get("failure") != ""
            or any(name not in native or native[name] is not None for name in
                   ("failed_input_component", "failed_event", "failed_event_ordinal"))
            or count(native, "iterations", 1, 1) != 1
            or count(native, "max_wall_ms", 1, MAX_WALL_MS) != wall_ms
            or native.get("producer_contract") != args.producer_contract
            or native.get("requested_route") != args.route
            or native.get("effective_route") != args.route
            or native.get("buffer_preservation_contract") != args.buffer_preservation_contract
            or native.get("requires_launcher_pe_attestation") is not
               (args.producer_contract == "pe-present-reservation-v1")
            or native.get("input_admission_profile") != "controlled-v4-raw256"):
        raise Rejected("native completion/contract/failure evidence rejected")
    validate_replay_policy(native, args)
    authentication = object_field(native, "authentication")
    flags(authentication, ("owned_inputs_authenticated", "wire_valid", "general_valid", "pair_valid",
                           "action_profile_authenticated", "stable_exact_action_witness",
                           "original_oracle_bytes_authenticated_only", "plan_digest_matched"), True)
    count(authentication, "blob_count", 0, auth.MAX_BLOBS)
    count(authentication, "blob_bytes", 0, auth.MAX_INPUT_BYTES)
    hashes = object_field(native, "sha256")
    for native_name, component in (("events", "events"), ("identity", "identity"),
                                   ("execution_topology", "execution_topology"),
                                   ("source", "source_oracle"), ("output", "output_oracle"),
                                   ("action_profile", "action_profile")):
        if hashes.get(native_name) != manifest["components"][component]["sha256"]:
            raise Rejected(f"native authenticated {native_name} digest mismatch")
    if hashes.get("expected_plan") != args.expected_plan_digest or hashes.get("actual_plan") != args.expected_plan_digest:
        raise Rejected("native expected/actual plan digest mismatch")
    witness = object_field(native, "action_witness")
    for name in ("capture_token", "capture_epoch", "queue", "construction_profile", "arm_profile",
                 "freeze_profile", "arm_revision", "freeze_revision"):
        actual = count(witness, name)
        if actual != int(action["witness"][name]):
            raise Rejected("native action witness does not match authenticated original binding")
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
    execution = object_field(native, "execution")
    flags(execution, ("complete", "metadata_inventory_complete", "runtime_budgeted_attempt",
                      "source_progress_complete", "reset_ok"), True)
    if (execution.get("failure") != "" or "failed_event" not in execution
            or execution["failed_event"] is not None or "resource_version_failure" not in execution
            or execution["resource_version_failure"] is not None):
        raise Rejected("native controlled execution reports a failure")
    provenance = object_field(execution, "provenance")
    flags(provenance, ("canonical_traversal_complete", "raw_ack_coverage_complete",
                       "canonical_origin_mapping_complete", "controlled_cpu_complete",
                       "present_preparation_attempted", "present_preparation_completed"), True)
    consumer = object_field(execution, "consumer")
    flags(consumer, ("worker_exited", "producer_closed", "cleanup_complete"), True)
    flags(consumer, ("cleanup_failed",), False)
    if count(consumer, "failure", 0, 0) != 0 or consumer.get("completion_policy") != "immediate-fifo-cpu-acknowledgement":
        raise Rejected("native consumer completion policy/failure rejected")
    sources = count(consumer, "submitted", 1, MAX_ITEMS)
    for name in ("observed", "acknowledged", "retired", "receipts"):
        if count(consumer, name, 1, MAX_ITEMS) != sources:
            raise Rejected("native source submission/acknowledgement/retirement conservation rejected")
    semantic = object_field(execution, "logical_semantics")
    flags(semantic, ("requested", "admitted", "accepted", "complete",
                     "legal_candidate_nooverwrite_contract", "conditional_on_legal_nooverwrite",
                     "logical_stream_complete"), True)
    flags(semantic, SEMANTIC_FALSE, False)
    if (semantic.get("failure") != "" or semantic.get("digest_algorithm") != "fnv1a64-logical-v4"
            or not isinstance(semantic.get("digest"), str)
            or re.fullmatch(r"[0-9a-f]{16}", semantic["digest"]) is None):
        raise Rejected("native logical semantics failure/fingerprint rejected")
    if count(semantic, "sources", 1, MAX_ITEMS) != sources:
        raise Rejected("native logical source count mismatch")
    commands = count(semantic, "commands", 1, MAX_ITEMS)
    if commands != count(consumer, "commands", 1, MAX_ITEMS):
        raise Rejected("native logical/consumer physical command count mismatch")
    draws = count(semantic, "draws", 1, MAX_ITEMS)
    if count(semantic, "source_rows_required", 1, MAX_ITEMS) != draws or count(semantic, "source_rows_qualified", 1, MAX_ITEMS) != draws:
        raise Rejected("native logical draws lack complete scoped source-row qualification")
    count(semantic, "logical_buffer_hash_bytes")
    receipts = semantic.get("receipts")
    items = count(semantic, "original_items", draws, MAX_ITEMS)
    if not isinstance(receipts, list) or len(receipts) != items:
        raise Rejected("native original-keyed logical receipt count mismatch")
    inventory = execution.get("sources")
    if not isinstance(inventory, list) or len(inventory) != sources:
        raise Rejected("native logical source/inventory count mismatch")
    source_sequences, physical_commands, expected_items = set(), {}, set()
    for source in inventory:
        if not isinstance(source, dict):
            raise Rejected("native source inventory entry malformed")
        sequence = count(source, "sequence", 1)
        if sequence in source_sequences:
            raise Rejected("duplicate native source sequence")
        source_sequences.add(sequence)
        source_commands = source.get("commands")
        if not isinstance(source_commands, list) or len(source_commands) > commands:
            raise Rejected("native physical command inventory malformed")
        for position, command in enumerate(source_commands):
            if not isinstance(command, dict) or count(command, "physical_command", 0, MAX_ITEMS) != position:
                raise Rejected("native physical command inventory position rejected")
            kind = count(command, "kind", 1, 9)
            draw_count = count(command, "draw_count", 0, draws)
            physical_commands[(sequence, position)] = (kind, draw_count)
            if draw_count:
                expected_items.update((sequence, position, index) for index in range(draw_count))
            else:
                expected_items.add((sequence, position, None))
            if len(expected_items) > items:
                raise Rejected("native physical item inventory exceeds original receipt count")
    if len(physical_commands) != commands or len(expected_items) != items:
        raise Rejected("native physical command/item conservation rejected")
    original_keys = set()
    physical_items = set()
    prior_original = None
    row_receipts = 0
    for receipt in receipts:
        if not isinstance(receipt, dict) or receipt.get("serialized") is not True:
            raise Rejected("native logical receipt was not serialized")
        record = object_field(receipt, "record")
        key = (count(record, "event_ordinal", 1), count(record, "record_index", 0, MAX_ITEMS))
        if key in original_keys or (prior_original is not None and key <= prior_original):
            raise Rejected("duplicate or unordered original logical receipt locator")
        original_keys.add(key)
        prior_original = key
        if receipt.get("failure") != "":
            raise Rejected("native logical receipt reports a failure")
        sequence = count(receipt, "sequence", 1)
        position = count(receipt, "physical_command_index", 0, MAX_ITEMS)
        command = physical_commands.get((sequence, position))
        core_kind = count(receipt, "command_kind", 0, 8)
        identity_kind = count(receipt, "identity_command_kind", 1, 9)
        if (CORE_TO_IDENTITY_COMMAND_KIND[core_kind] != identity_kind
                or command is None or identity_kind != command[0]):
            raise Rejected("native logical receipt lacks its genuine physical command association")
        if not isinstance(receipt.get("digest"), str) or re.fullmatch(r"[0-9a-f]{16}", receipt["digest"]) is None:
            raise Rejected("native logical receipt fingerprint malformed")
        required = receipt.get("source_rows_required")
        if type(required) is not bool:
            raise Rejected("native logical receipt source-row requirement missing")
        if required is not (command[1] > 0) or "subdraw_index" not in receipt:
            raise Rejected("native logical receipt draw/command association rejected")
        subdraw = count(receipt, "subdraw_index", 0, command[1] - 1) if required else None
        if not required and receipt["subdraw_index"] is not None:
            raise Rejected("native non-draw logical receipt has a fabricated subdraw")
        physical_key = (sequence, position, subdraw)
        if physical_key not in expected_items or physical_key in physical_items:
            raise Rejected("duplicate or unknown native physical item association")
        physical_items.add(physical_key)
        if required:
            row_receipts += 1
            rows = object_field(receipt, "source_rows")
            if (rows.get("status") != "qualified" or rows.get("qualified") is not True
                    or rows.get("proved") is not True
                    or count(rows, "source_sequence", 1) != sequence
                    or count(rows, "completed_sequence") >= sequence
                    or count(rows, "planned_read_bytes", 1) != count(rows, "proved_bytes", 1)):
                raise Rejected("native original draw source-row/byte qualification rejected")
    if row_receipts != draws or physical_items != expected_items:
        raise Rejected("native logical source-row receipt count mismatch")
    counters = object_field(execution, "counters")
    if count(counters, "draws", 1, MAX_ITEMS) != draws or count(counters, "resources_created") != count(counters, "resources_released"):
        raise Rejected("native draw/resource conservation rejected")
    validate_route_coverage(execution, args.route, draws)
    rows = object_field(execution, "source_row_observation")
    if count(rows, "qualified_draws", 1, MAX_ITEMS) != draws or count(rows, "not_qualified_draws", 0, 0) != 0:
        raise Rejected("native source-row observation count mismatch")
    flags(rows, ("shader_demand_complete", "future_preservation", "physical_fetch_complete"), False)
    query = object_field(execution, "query_projection")
    flags(query, ("gpu_result_oracle",), False)
    if count(query, "recorded_polls") != count(query, "replayed_polls"):
        raise Rejected("native recorded query projection count mismatch")


def parser() -> argparse.ArgumentParser:
    value = argparse.ArgumentParser(description=__doc__)
    value.add_argument("--bundle", type=pathlib.Path, required=True)
    value.add_argument("--diagnostic", type=pathlib.Path, required=True,
                       help="dxmt9-cpu-ready-production-routing-spec executable")
    value.add_argument("--output-dir", type=pathlib.Path, required=True, help="new owned artifact directory")
    value.add_argument("--expected-plan-digest", required=True, help="authenticated V4 plan SHA-256 to require")
    value.add_argument("--route", choices=ROUTES, required=True,
                       action=UniquePolicyArgument, default=argparse.SUPPRESS,
                       help="explicit private CPU publication route; no policy/timing authority")
    value.add_argument("--producer-contract", choices=auth.PRODUCER_CONTRACTS, default="none")
    value.add_argument("--buffer-preservation-contract", choices=(PRESERVATION_CONTRACT,), required=True,
                       help="explicit conditional assumption of legal NoOverwrite and closed writers")
    value.add_argument("--timeout-seconds", type=float, default=180.0,
                       help="shared authentication/one-child/reauthentication deadline, 1..600 seconds")
    value.add_argument("--slot-headroom-bytes", type=headroom_decimal, default=argparse.SUPPRESS,
                       action=UniquePolicyArgument,
                       help="paired ProductionSlot retained-byte ceiling, 0 or 1..64 MiB; omitted means exact0")
    value.add_argument("--aggregate-headroom-bytes", type=headroom_decimal, default=argparse.SUPPRESS,
                       action=UniquePolicyArgument,
                       help="paired aggregate ceiling, >=slot and <=512 MiB; Compatibility requires exact0")
    value.add_argument("--draw-command-limit", type=draw_command_decimal, default=argparse.SUPPRESS,
                       action=UniquePolicyArgument,
                       help="Compatibility-only bounded grouping, 0 or 3..64; omitted means 0")
    return value


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
    for name in ("semantics.stdout", "semantics.stderr"):
        with (output / name).open("xb"):
            pass
    started = time.monotonic()
    deadline = started + args.timeout_seconds
    receipt_path = output / "result.json"
    receipt: dict[str, Any] = {
        "schema": RECEIPT_SCHEMA, "status": "authenticating", "accepted": False,
        "diagnostic": True, **{name: False for name in BROAD_FALSE},
        "full_logical_semantic_oracle_complete": False, "original_semantic_equivalence_qualified": False,
        "cross_policy_equivalence_qualified": False, "logical_stream_complete": False,
        "acceptance_scope": "observed-original-keyed-logical-stream-scoped-buffer-bytes-and-requested-route-exercised",
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
        "steps": {"semantics": {"status": "not-started", "argv": None,
                                 "stdout": "semantics.stdout", "stderr": "semantics.stderr"}},
    }
    runner, manifest, manifest_raw, action, wall_ms = None, None, None, None, None
    exit_code = 1
    try:
        auth.write_receipt(receipt_path, receipt)
        runner = load_bundle_tool()
        receipt["diagnostic_executable"] = fingerprint(binary, runner, deadline)
        manifest, manifest_raw, total, action = authenticate(bundle, runner, deadline, args.producer_contract)
        receipt.update(manifest_authenticated=True, bounded_input_bytes=total,
                       input_components=manifest["components"], action_profile_validation=action,
                       input_manifest={"path": "manifest.json", "bytes": len(manifest_raw),
                                       "sha256": hashlib.sha256(manifest_raw).hexdigest()})
        wall_ms = min(MAX_WALL_MS, int(auth.require_time(deadline) * 1000))
        if wall_ms <= 0:
            raise Rejected("shared diagnostic deadline exhausted before native admission")
        argv = [str(binary), "--controlled-cpu-v4-semantics", "--bundle", str(bundle),
                "--route", args.route,
                "--producer-contract", args.producer_contract,
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
        receipt["native_max_wall_ms"] = wall_ms
        receipt["status"] = "executing"
        native = auth.run_child("semantics", argv, output, deadline, receipt, receipt_path)
        validate_native(native, args, manifest, action, wall_ms)
        receipt["effective_route"] = native["effective_route"]
        receipt.update(effective_stage_environment=native["effective_stage_environment"],
                       capacity_policy_valid=True, stage_environment_valid=True, replay_policy_valid=True)
        receipt["route_coverage"] = native["execution"]["route_coverage"]
        receipt["logical_semantics"] = native["execution"]["logical_semantics"]
        exit_code = 0
    except KeyboardInterrupt:
        receipt.update(status="interrupted", failure="launcher interrupted")
        exit_code = 130
    except (Rejected, OSError, ValueError, SystemExit) as error:
        receipt.update(status="rejected", failure=str(error))
    finally:
        # Reauthenticate even a parseable rejected child. Its raw evidence and
        # the first failure survive; no failure path becomes an accepted run.
        if runner is not None and receipt["manifest_authenticated"]:
            try:
                current, current_raw, _total, current_action = authenticate(bundle, runner, deadline, args.producer_contract)
                if current != manifest or current_raw != manifest_raw or current_action != action:
                    raise Rejected("immutable bundle changed across the controlled CPU diagnostic")
                receipt["bundle_reauthenticated"] = True
                if fingerprint(binary, runner, deadline) != receipt["diagnostic_executable"]:
                    raise Rejected("native executable changed across the controlled CPU diagnostic")
                receipt["binary_reauthenticated"] = True
                native = receipt["steps"]["semantics"].get("native_result")
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
                receipt["status"] = "rejected" if exit_code != 130 else "interrupted"
        step = receipt["steps"]["semantics"]
        if step["status"] == "not-started":
            step.update(status="skipped", failure="prior authentication/deadline failure")
        receipt["accepted"] = (exit_code == 0 and receipt["bundle_reauthenticated"]
                               and receipt["binary_reauthenticated"] and receipt["loaded_images_stable"]
                               and receipt["capacity_policy_valid"] and receipt["stage_environment_valid"]
                               and receipt["replay_policy_valid"])
        if receipt["accepted"]:
            receipt["status"] = "complete"
            receipt["logical_stream_complete"] = True
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
