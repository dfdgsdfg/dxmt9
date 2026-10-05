#!/usr/bin/env python3
"""Bounded public V4 Compatibility/ProductionSlot paired qualification.

The aggregate compares observed original-keyed CPU field streams and separately
requires each route's original GPU query/source/output oracle. It does not mint
legacy receipts, qualify arbitrary policies, measure cost, or prove independent
original scalar/uniform equivalence or unconditional future buffer preservation.
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


SCHEMA = "dxmt9.render_tape.v4_paired_qualification.v1"
PRODUCER_CONTRACT = "pe-present-reservation-v1"
PRESERVATION_CONTRACT = "candidate-legal-nooverwrite-closed-writers-v1"
ROUTES = ("compatibility", "production-slot")
MAX_TIMEOUT_SECONDS = 1800
MAX_CHILD_TIMEOUT_SECONDS = 600
FINAL_AUTH_RESERVE_SECONDS = 1.0
MAX_CHILD_LOG_BYTES = 32 * 1024 * 1024
# Private wrapper receipts retain pretty-expanded native evidence and copied
# logical streams. Their bound is distinct from the unchanged 16 MiB native
# stdout/result bound enforced by each private launcher and plan inspection.
MAX_CHILD_RECEIPT_BYTES = 64 * 1024 * 1024
MAX_ITEMS = 1048576
MIN_SLOT_HEADROOM_BYTES = 1024 * 1024
MAX_SLOT_HEADROOM_BYTES = 64 * 1024 * 1024
MAX_AGGREGATE_HEADROOM_BYTES = 512 * 1024 * 1024
CAPACITY_FIELDS = ("slot_headroom_bytes", "aggregate_headroom_bytes")
CAPACITY_SELECTORS = ("DXMT9_DIRECT_SLOT_HEADROOM_BYTES", "DXMT9_DIRECT_SLOT_AGGREGATE_HEADROOM_BYTES")
REPLAY_POLICY_SCHEMA = "dxmt9.render_tape.v4_replay_policy.v1"
REPLAY_POLICY_FIELDS = ("schema", "route", "draw_command_limit", *CAPACITY_FIELDS)
CPU_STAGE_PINS = {
    "DXMT9_DISABLE_PRESENT_BOUNDARY": "1", "DXMT9_SYNC_PRESENT_FLUSH": "0",
    "DXMT9_OFFLOAD_COMMIT_REPLAY": "0", "DXMT9_DRAW_CHUNK_COMMAND_LIMIT": "0",
    "DXMT9_CHUNK_DRAW_PAYLOAD_ARENA_LIMIT_BYTES": "0", "DXMT9_CPU_READY_TAPE": "0",
    "DXMT9_CPU_READY_EARLY_PREFIX": "0", "DXMT9_SPLIT_STRETCH_CHUNK": "0",
    "DXMT_PERF_COUNTERS": "1",
    "DXMT9_DIRECT_SLOT_HEADROOM_BYTES": "0", "DXMT9_DIRECT_SLOT_AGGREGATE_HEADROOM_BYTES": "0",
}
GPU_STAGE_PINS = {
    "DXMT_DEBUG_DISABLE_SHADER_ARCHIVE": "1", "DXMT9_PREWARM": "disabled", "DXMT_PERF_COUNTERS": "1",
    "DXMT9_DIRECT_SLOT_HEADROOM_BYTES": "0", "DXMT9_DIRECT_SLOT_AGGREGATE_HEADROOM_BYTES": "0",
    "DXMT9_DRAW_CHUNK_COMMAND_LIMIT": "0", "DXMT9_CHUNK_DRAW_PAYLOAD_ARENA_LIMIT_BYTES": "0",
}
INHERITED_VALUES = {
    "DXMT_DEBUG_DISABLE_SHADER_ARCHIVE": {"", "1"}, "DXMT9_PREWARM": {"", "disabled"},
    "DXMT_PERF_COUNTERS": {"", "1"}, "DXMT9_UPDATE_PE_PRODUCER_GOLDEN": {"0"},
    "MTL_DEBUG_LAYER": {"", "0", "1"}, "MTL_SHADER_VALIDATION": {"", "0", "1"},
    "MTL_HUD_ENABLED": {"", "0", "1"},
}
DYLD_DIRECTORY_LISTS = ("DYLD_LIBRARY_PATH", "DYLD_FALLBACK_LIBRARY_PATH")
REQUIRED_RUNTIME_IMAGES = {"libdxmt9_native.dylib", "winemetal_dxmt9.so"}
AGGREGATE_GATES = (
    "logical_streams_equal", "original_gpu_queries_qualified", "original_pixels_qualified",
    "paired_qualification_complete",
)


class Rejected(RuntimeError):
    pass


def capacity_policy(slot=None, aggregate=None) -> dict:
    if slot is None and aggregate is None:
        slot = aggregate = 0
    if (type(slot) is not int or type(aggregate) is not int
            or not ((slot == 0 and aggregate == 0)
                    or (MIN_SLOT_HEADROOM_BYTES <= slot <= MAX_SLOT_HEADROOM_BYTES
                        and slot <= aggregate <= MAX_AGGREGATE_HEADROOM_BYTES))):
        raise Rejected("capacity policy requires exact0/0 or slot1..64MiB with slot<=aggregate<=512MiB; both options required")
    return {"slot_headroom_bytes": slot, "aggregate_headroom_bytes": aggregate}


def _policy(value) -> dict:
    if (not isinstance(value, dict) or set(value) != set(CAPACITY_FIELDS)
            or any(type(value[field]) is not int for field in CAPACITY_FIELDS)):
        raise Rejected("missing exact bounded capacity policy identity")
    return capacity_policy(value["slot_headroom_bytes"], value["aggregate_headroom_bytes"])


def route_capacity_policies(args) -> dict:
    candidate = capacity_policy(getattr(args, "slot_headroom_bytes", None),
                                getattr(args, "aggregate_headroom_bytes", None))
    return {"compatibility": capacity_policy(), "production-slot": candidate}


def validate_route_capacity_policies(value) -> dict:
    if not isinstance(value, dict) or set(value) != set(ROUTES):
        raise Rejected("paired capacity policies must bind exactly the two closed routes")
    result = {route: _policy(value[route]) for route in ROUTES}
    if result["compatibility"] != capacity_policy():
        raise Rejected("Compatibility reference requires exact0 capacity policy")
    return result


def draw_command_limit(value=0) -> int:
    if type(value) is not int or not (value == 0 or 3 <= value <= 64):
        raise Rejected("draw-command-limit requires exact0 or integer3..64")
    return value


def replay_policy(route: str, policy: dict, limit=0) -> dict:
    policy, limit = _policy(policy), draw_command_limit(limit)
    if route not in ROUTES or (route == "production-slot" and limit):
        raise Rejected("positive draw-command-limit requires the Compatibility route")
    if route == "compatibility" and any(policy.values()):
        raise Rejected("positive headroom requires the ProductionSlot route")
    return {"schema": REPLAY_POLICY_SCHEMA, "route": route, "draw_command_limit": limit, **policy}


def route_replay_policies(args, policies=None) -> dict:
    policies = route_capacity_policies(args) if policies is None else validate_route_capacity_policies(policies)
    limit = draw_command_limit(getattr(args, "draw_command_limit", 0))
    if limit and any(policies["production-slot"].values()):
        raise Rejected("paired qualification permits one positive policy axis, not grouping plus headroom")
    return {route: replay_policy(route, policies[route], limit if route == "compatibility" else 0) for route in ROUTES}


def validate_replay_policy(value, route, policy) -> dict:
    if not isinstance(value, dict) or set(value) != set(REPLAY_POLICY_FIELDS):
        raise Rejected("missing exact full replay policy identity")
    checked = replay_policy(route, _policy({key: value[key] for key in CAPACITY_FIELDS}), value["draw_command_limit"])
    if checked != value or any(value[field] != policy[field] for field in CAPACITY_FIELDS):
        raise Rejected("full replay policy does not match its route/capacity identity")
    return checked


def validate_route_replay_policies(value, policies) -> dict:
    if not isinstance(value, dict) or set(value) != set(ROUTES):
        raise Rejected("replay policies must bind exactly both closed routes")
    result = {}
    for route in ROUTES:
        result[route] = validate_replay_policy(value[route], route, policies[route])
    if result["compatibility"]["draw_command_limit"] and any(policies["production-slot"].values()):
        raise Rejected("paired replay policies mix positive grouping and headroom axes")
    return result


def stage_environment(stage: str, policy: dict, limit=0) -> dict:
    if stage not in ("cpu", "gpu"):
        raise Rejected("unsupported capacity stage")
    policy, limit = _policy(policy), draw_command_limit(limit)
    return {**(CPU_STAGE_PINS if stage == "cpu" else GPU_STAGE_PINS),
            **{selector: str(policy[field]) for selector, field in zip(CAPACITY_SELECTORS, CAPACITY_FIELDS)},
            "DXMT9_DRAW_CHUNK_COMMAND_LIMIT": str(limit)}


def stage_environments(policies: dict, replay_policies=None) -> dict:
    replay_policies = ({route: replay_policy(route, policies[route]) for route in ROUTES}
                       if replay_policies is None else validate_route_replay_policies(replay_policies, policies))
    return {stage: {route: stage_environment(stage, policies[route], replay_policies[route]["draw_command_limit"]) for route in ROUTES}
            for stage in ("cpu", "gpu")}


def _load_cpu_launcher():
    path = pathlib.Path(__file__).resolve().parents[1] / "check/run_render_tape_cpu_v4_semantics.py"
    spec = importlib.util.spec_from_file_location("v4_paired_cpu_launcher", path)
    if spec is None or spec.loader is None:
        raise Rejected("cannot load the existing controlled V4 launcher")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def inherited_environment(environment: dict[str, str]) -> dict[str, str]:
    """Finite declared renderer/validation/search inputs, never unrelated env."""
    result = {}
    for key, value in sorted(environment.items()):
        if not key.startswith(("DXMT", "MTL", "DYLD")):
            continue
        if key in INHERITED_VALUES:
            if value not in INHERITED_VALUES[key]:
                raise Rejected(f"unsupported inherited renderer setting: {key}")
        elif key in DYLD_DIRECTORY_LISTS:
            if not value or len(value) > 16384:
                raise Rejected(f"invalid inherited library search directories: {key}")
            for entry in value.split(":"):
                path = pathlib.Path(entry)
                if not entry or not path.is_absolute() or not path.is_dir() or str(path.resolve()) != entry:
                    raise Rejected(f"noncanonical inherited library search directory: {key}")
        else:
            raise Rejected(f"undeclared inherited renderer setting: {key}")
        result[key] = value
    return result


def _object(owner: dict, key: str) -> dict:
    value = owner.get(key)
    if not isinstance(value, dict):
        raise Rejected(f"missing object evidence: {key}")
    return value


def _true(owner: dict, *keys: str) -> None:
    if any(owner.get(key) is not True for key in keys):
        raise Rejected(f"missing required qualification gates: {', '.join(keys)}")


def _completed_step(step: dict) -> dict:
    if (step.get("status") != "exited" or type(step.get("returncode")) is not int
            or step["returncode"] != 0 or step.get("timed_out") is not False):
        raise Rejected("required child step did not exit successfully")
    return _object(step, "native_result")


def _validate_inspection(auth, inspect: dict, producer_contract: str, plan_digest=None, replay_policy=None) -> None:
    auth.validate_common(inspect, producer_contract, "semantic-mapping")
    policy = replay_policy or {"draw_command_limit": 0, **capacity_policy()}
    auth.validate_replay_policy(inspect, {"schema": REPLAY_POLICY_SCHEMA, "route": "inspection",
        "draw_command_limit": policy["draw_command_limit"], **{field: policy[field] for field in CAPACITY_FIELDS}})
    if (inspect.get("status") != "inspection-complete" or inspect.get("execution_count") != 0
            or type(inspect.get("execution_count")) is not int or inspect.get("device_created") is not False
            or inspect.get("qualified") is not False or not auth.sha256_nonzero(inspect.get("plan_sha256"))
            or (plan_digest is not None and inspect.get("plan_sha256") != plan_digest)
            or any(inspect.get(gate) is not False for gate in auth.GATES)
            or inspect.get("semantic_mapping_qualified") is not False
            or inspect.get("exact_source_diagnostic_complete") is not False
            or inspect.get("effective_stage_environment") != {}):
        raise Rejected("GPU inspection lacks safe no-effect authenticated plan evidence")


def _validate_child_logs(staging: pathlib.Path, name: str) -> None:
    for log in (staging / f"{name}.stdout", staging / f"{name}.stderr"):
        if log.exists() and log.stat().st_size > MAX_CHILD_LOG_BYTES:
            raise Rejected("paired child log byte cap exceeded")


def _uint(owner: dict, key: str, maximum: int = MAX_ITEMS) -> int:
    value = owner.get(key)
    if type(value) is not int or not 0 <= value <= maximum:
        raise Rejected(f"invalid bounded count: {key}")
    return value


def logical_projection(semantic: dict) -> dict:
    """Compare EVERY ordered original item, not only a final fingerprint."""
    _true(semantic, "complete", "accepted", "logical_stream_complete")
    if semantic.get("digest_algorithm") != "fnv1a64-logical-v4":
        raise Rejected("unsupported logical digest contract")
    receipts = semantic.get("receipts")
    count = _uint(semantic, "original_items")
    if not count or not isinstance(receipts, list) or len(receipts) != count:
        raise Rejected("logical original item conservation rejected")
    items, prior = [], None
    for item in receipts:
        if not isinstance(item, dict):
            raise Rejected("logical item is not an object")
        record = _object(item, "record")
        key = (_uint(record, "event_ordinal", (1 << 64) - 1), _uint(record, "record_index"))
        if not key[0] or (prior is not None and key <= prior):
            raise Rejected("logical original locators are duplicated or unordered")
        prior = key
        core, wire = _uint(item, "command_kind", 8), _uint(item, "identity_command_kind", 9)
        if wire != core + 1 or item.get("serialized") is not True or item.get("failure") != "":
            raise Rejected("logical core/wire command or serialization evidence rejected")
        digest = item.get("digest")
        if not isinstance(digest, str) or re.fullmatch(r"[0-9a-f]{16}", digest) is None:
            raise Rejected("logical original item digest rejected")
        items.append((key[0], key[1], core, wire, digest))
    draws = _uint(semantic, "draws")
    if not draws or sum(item[2] == 0 for item in items) != draws:
        raise Rejected("logical original draw conservation rejected")
    for name in ("source_rows_required", "source_rows_qualified"):
        if _uint(semantic, name) != draws:
            raise Rejected("logical source-row count does not qualify every original draw")
    digest = semantic.get("digest")
    if not isinstance(digest, str) or re.fullmatch(r"[0-9a-f]{16}", digest) is None:
        raise Rejected("logical complete-stream digest rejected")
    return {"items": items, "original_items": count, "draws": draws,
            "source_rows_required": draws, "source_rows_qualified": draws,
            "digest_algorithm": semantic["digest_algorithm"], "digest": digest}


def compare_logical_streams(reference: dict, candidate: dict) -> dict:
    left, right = logical_projection(reference), logical_projection(candidate)
    for field in ("original_items", "draws", "source_rows_required", "source_rows_qualified", "digest_algorithm"):
        if left[field] != right[field]:
            raise Rejected(f"paired logical count/contract mismatch: {field}")
    for index, (expected, actual) in enumerate(zip(left["items"], right["items"])):
        if expected != actual:
            raise Rejected(f"paired logical original item mismatch at index {index}: {expected[:2]} vs {actual[:2]}")
    if left["digest"] != right["digest"]:
        raise Rejected("paired complete logical stream digest mismatch")
    # Physical commands/sources, sequence, subdraw grouping and cache bytes are
    # separately validated child observations, never paired equality criteria.
    return {key: left[key] for key in left if key != "items"}


def canonical_execution_counts(native: dict) -> dict:
    execution = _object(native, "execution")
    counters = _object(execution, "counters")
    query = _object(execution, "query_projection")
    return {"counters": {name: _uint(counters, name) for name in (
        "events", "commands", "draws", "resources_created", "resources_released",
        "observed_mutations", "observed_texture_uploads", "record_contexts",
        "buffer_versions_observed", "buffer_versions_unavailable")},
        "query": {name: _uint(query, name) for name in (
            "definitions", "issues", "recorded_polls", "replayed_polls", "checkpoints", "rows")}}


def _image_map(native: dict, binary: pathlib.Path) -> dict:
    images = native.get("loaded_images")
    if not isinstance(images, list) or not 1 <= len(images) <= 256:
        raise Rejected("missing bounded loaded image evidence")
    result = {}
    executable_present = False
    for image in images:
        if not isinstance(image, dict) or set(image) != {"path", "bytes", "sha256"}:
            raise Rejected("loaded image row contract rejected")
        path = image["path"]
        if not isinstance(path, str) or path in result:
            raise Rejected("duplicate/malformed loaded image path")
        size = _uint(image, "bytes", 1024 * 1024 * 1024)
        if not size or not isinstance(image["sha256"], str) or re.fullmatch(r"[0-9a-f]{64}", image["sha256"]) is None:
            raise Rejected("loaded image fingerprint rejected")
        if path == str(binary):
            if executable_present:
                raise Rejected("duplicated endpoint executable image")
            executable_present = True
        else:
            result[path] = {"bytes": size, "sha256": image["sha256"]}
    if not executable_present or not REQUIRED_RUNTIME_IMAGES.issubset({pathlib.Path(path).name for path in result}):
        raise Rejected("loaded endpoint/native-provider/Unix-module evidence incomplete")
    return result


def _remaining_child_timeout(auth, deadline: float) -> float:
    remaining = auth.require_time(deadline) - FINAL_AUTH_RESERVE_SECONDS
    if remaining < 1.0:
        raise Rejected("shared paired deadline exhausted before the next child")
    return min(MAX_CHILD_TIMEOUT_SECONDS, remaining)


def _read_json(auth, path: pathlib.Path, *, evidence=None) -> dict:
    """Read a bounded wrapper/aggregate receipt, not native child stdout."""
    if path.is_symlink() or not path.is_file():
        raise Rejected(f"missing owned child result: {path.name}")
    raw = auth.read_capped(path, MAX_CHILD_RECEIPT_BYTES)
    value = auth.parse_json(raw)
    if not isinstance(value, dict):
        raise Rejected("child receipt must be an object")
    if evidence is not None:
        evidence.update(bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest())
    return value


def _validate_child_binding(child: dict, args, route: str, input_manifest: dict,
                            environment: dict, stage: str, plan_digest: str, policy: dict, replay: dict) -> None:
    _true(child, "accepted", "manifest_authenticated", "bundle_reauthenticated",
          "binary_reauthenticated", "loaded_images_stable")
    if child.get("status") != "complete" or child.get("producer_contract") != args.producer_contract:
        raise Rejected("child completion/producer contract rejected")
    if child.get("route") != route or child.get("input_manifest") != input_manifest:
        raise Rejected("child route or owned input identity mismatch")
    if child.get("environment") != environment:
        raise Rejected("child inherited renderer/validation environment changed")
    if child.get("expected_plan_digest") != plan_digest:
        raise Rejected("child expected authenticated plan echo mismatch")
    if (_policy(child.get("capacity_policy")) != policy
            or validate_replay_policy(child.get("replay_policy"), route, policy) != replay
            or child.get("expected_stage_environment") != stage_environment(stage, policy, replay["draw_command_limit"])):
        raise Rejected("child requested capacity policy/stage identity mismatch")
    _true(child, "stage_environment_valid", "capacity_policy_valid" if stage == "cpu" else "native_capacity_policy_valid")
    _true(child, "replay_policy_valid" if stage == "cpu" else "native_replay_policy_valid")
    if child.get("policy_qualified") is not False or child.get("timing_eligible") is not False:
        raise Rejected("child attempts policy or timing promotion")
    if stage == "cpu":
        if child.get("buffer_preservation_contract") != args.buffer_preservation_contract:
            raise Rejected("child conditional buffer preservation contract mismatch")
        _true(child, "logical_stream_complete", "conditional_on_legal_nooverwrite")
        if (child.get("requested_route") != route or child.get("effective_route") != route
                or type(child.get("iterations")) is not int or child["iterations"] != 1):
            raise Rejected("CPU child requested/effective route or iteration mismatch")
    else:
        _true(child, "action_profile_reauthenticated", "readback_artifacts_valid", "stage_environment_valid")
        if child.get("publication_mode") != "semantic-mapping":
            raise Rejected("GPU child substituted an exact captured-source partition")


def _child_specs():
    for route in ROUTES:
        yield "cpu", route, f"cpu-{route}"
    for route in ROUTES:
        for iteration in (1, 2):
            yield "gpu", route, f"gpu-{route}-{iteration}"


def _validate_native_child(child, args, stage, route, binary, binary_evidence,
                           manifest, action, plan_digest, child_dir, runner,
                           deadline, cpu, *, policy, replay, recorded_directory=None):
    auth = cpu.auth
    steps = _object(child, "steps")
    expected_steps = {"semantics"} if stage == "cpu" else {"inspect", "replay"}
    if set(steps) != expected_steps:
        raise Rejected("child native step coverage rejected")
    native = _completed_step(_object(steps, "semantics" if stage == "cpu" else "replay"))
    if (_policy(native.get("capacity_policy")) != policy
            or native.get("replay_policy") != replay
            or native.get("effective_stage_environment") != stage_environment(stage, policy, replay["draw_command_limit"])
            or child.get("effective_stage_environment") != native.get("effective_stage_environment")):
        raise Rejected("child actual stage environment pins mismatch")
    if stage == "cpu":
        cpu_args = argparse.Namespace(route=route, producer_contract=args.producer_contract,
            expected_plan_digest=plan_digest, buffer_preservation_contract=args.buffer_preservation_contract,
            draw_command_limit=replay["draw_command_limit"], **policy)
        wall = child.get("native_max_wall_ms")
        if type(wall) is not int or not 1 <= wall <= cpu.MAX_WALL_MS:
            raise Rejected("CPU child native deadline admission rejected")
        cpu.validate_native(native, cpu_args, manifest, action, wall)
        cpu.validate_images(native, binary, binary_evidence, runner, deadline)
    else:
        auth.validate_common(native, args.producer_contract, "semantic-mapping")
        auth.validate_replay_policy(native, replay)
        _true(native, "qualified", "semantic_mapping_qualified", *auth.GATES)
        if (native.get("status") != "complete" or native.get("execution_count") != 1
                or type(native.get("execution_count")) is not int or native.get("replay_route") != route
                or native.get("device_created") is not True
                or native.get("plan_sha256") != plan_digest or native.get("expected_plan_sha256") != plan_digest
                or child.get("plan_sha256") != plan_digest
                or native.get("exact_source_diagnostic_complete") is not False):
            raise Rejected("GPU original query/source/output plan or route mismatch")
        inspect = _completed_step(_object(steps, "inspect"))
        _validate_inspection(auth, inspect, args.producer_contract, plan_digest, replay)
        auth.validate_loaded_images(inspect, binary, binary_evidence, runner, deadline)
        auth.validate_loaded_images(native, binary, binary_evidence, runner, deadline)
        if _image_map(inspect, binary) != _image_map(native, binary):
            raise Rejected("GPU child inspection/replay runtime image set changed")
        if route == "production-slot":
            auth.require_direct_candidate_path(native)
        stat = child_dir.stat()
        keywords = {"recorded_directory": recorded_directory} if recorded_directory is not None else {}
        auth.validate_readbacks(native, child_dir, child_dir.parent / "input", manifest,
                                (stat.st_dev, stat.st_ino), deadline, **keywords)
    _image_map(native, binary)
    return native


def paired_qualification(args: argparse.Namespace, runner=None) -> int:
    """Public dispatch entry; all six replay children are bounded/sequential."""
    timeout = args.timeout_seconds
    if type(timeout) not in (int, float) or not math.isfinite(timeout) or not 1 <= timeout <= MAX_TIMEOUT_SECONDS:
        raise SystemExit("--timeout-seconds must be finite and within 1..1800")
    if args.producer_contract != PRODUCER_CONTRACT or args.buffer_preservation_contract != PRESERVATION_CONTRACT:
        raise SystemExit("V4 paired qualification requires both explicit producer and legal-candidate-NOOVERWRITE contracts")
    try:
        policies = route_capacity_policies(args)
        replay_policies = route_replay_policies(args, policies)
    except Rejected as error:
        raise SystemExit(str(error)) from error
    started = time.monotonic()
    deadline = started + timeout
    cpu = _load_cpu_launcher()
    auth = cpu.auth
    runner = runner if runner is not None else cpu.load_bundle_tool()
    environment = inherited_environment(dict(os.environ))
    source = pathlib.Path(args.bundle).resolve()
    output = pathlib.Path(args.output_dir).absolute()
    binaries = {"cpu": pathlib.Path(args.cpu_diagnostic).resolve(), "gpu": pathlib.Path(args.gpu_diagnostic).resolve()}
    if binaries["cpu"] == binaries["gpu"]:
        raise SystemExit("CPU and GPU diagnostics must identify their separate executable roles")
    if not source.is_dir() or output.resolve().is_relative_to(source):
        raise SystemExit("paired output must be outside the immutable input directory")
    if output.exists() or output.is_symlink():
        raise SystemExit("paired output directory must be new")
    for binary in binaries.values():
        if not binary.is_file() or not os.access(binary, os.X_OK):
            raise SystemExit("paired diagnostics must be executable regular files")
    repository = pathlib.Path(__file__).resolve().parents[2]
    scripts = {"cpu": repository / "scripts/check/run_render_tape_cpu_v4_semantics.py",
               "gpu": repository / "scripts/check/run_render_tape_v4_capture_diagnostic.py",
               "python_launcher": repository / "scripts/run_python.sh",
               "public_adapter": pathlib.Path(__file__).resolve(),
               "bundle_tool": repository / "scripts/tools/run_dxmt9_render_tape.py"}
    receipt: dict[str, Any] = {
        "schema": SCHEMA, "status": "authenticating", "accepted": False,
        **{gate: False for gate in AGGREGATE_GATES},
        "policy_qualified": False, "timing_eligible": False, "measurement_started": False,
        "full_semantic_oracle_complete": False, "original_scalar_uniform_equivalence_qualified": False,
        "future_backing_preservation_complete": False,
        "qualification_scope": "fixed-input-compatibility-production-slot-pair",
        "capacity_policies": policies, "replay_policies": replay_policies,
        "replay_comparison_scope": "bound-compatibility-versus-production-slot-single-axis-pair",
        "capacity_matrix_complete": False,
        "replay_policy_matrix_complete": False,
        "conditional_on_legal_nooverwrite": True, "preservation_contract_is_caller_assumption": True,
        "producer_contract": args.producer_contract, "buffer_preservation_contract": args.buffer_preservation_contract,
        "routes": list(ROUTES), "iterations_per_route_per_backend": {"cpu": 1, "gpu": 2}, "gpu_warmup": 0,
        "bundle": str(source), "owned_input": "input", "output_dir": str(output),
        "artifact_paths": "relative-to-aggregate; child execution argv paths are pre-publication provenance",
        "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "timeout_seconds": timeout, "environment": environment,
        "expected_stage_environment": stage_environments(policies, replay_policies),
        "manifest_authenticated": False, "source_reauthenticated": False, "owned_input_reauthenticated": False,
        "binaries_reauthenticated": False, "launcher_images_reauthenticated": False,
        "loaded_images_stable": False, "steps": {}, "children": {},
        "limits": {"input_bytes": auth.MAX_INPUT_BYTES, "blobs": auth.MAX_BLOBS,
                   "child_receipt_bytes": MAX_CHILD_RECEIPT_BYTES,
                   "native_result_json_bytes": auth.MAX_RESULT_JSON_BYTES,
                   "child_log_bytes": MAX_CHILD_LOG_BYTES,
                   "original_items": MAX_ITEMS, "loaded_images": 256, "child_timeout_seconds": MAX_CHILD_TIMEOUT_SECONDS},
        "authentication_deadline_enforcement": "bounded synchronous input copies/loaders before/after deadline checks; child timeouts share one deadline",
    }
    failure_metadata = {"schema": SCHEMA, "lane": "v4-paired-qualification", "accepted": False}
    exit_code = 1
    with runner.bundle_transaction(output, preserve_rejected=True, failure_metadata=failure_metadata) as staging:
        receipt_path, owned = staging / "result.json", staging / "input"
        receipt["execution_staging_directory"] = str(staging)
        receipt["stage_binary_paths"] = {stage: str(binary) for stage, binary in binaries.items()}
        baseline = source_manifest = source_raw = source_action = None
        bindings, launcher_bindings, natives, child_receipts = {}, {}, {}, {}
        auth.write_receipt(receipt_path, receipt)
        try:
            source_manifest, source_raw, total, source_action = cpu.authenticate(source, runner, deadline, args.producer_contract)
            # Both original scopes are required and contradictory synthetic flags
            # cannot promote a production PE capture into this public lane.
            scope = _object(source_manifest, "scope")
            if scope.get("synthetic") is True or scope.get("test_input") is True:
                raise Rejected("production paired lane rejects synthetic/test provenance")
            bindings = {stage: cpu.fingerprint(path, runner, deadline) for stage, path in binaries.items()}
            launcher_bindings = {stage: cpu.fingerprint(path, runner, deadline) for stage, path in scripts.items()}
            auth.require_time(deadline)
            runner.copy_input_owned(source, owned)
            auth.require_time(deadline)
            baseline = cpu.authenticate(owned, runner, deadline, args.producer_contract)
            if baseline[:2] != (source_manifest, source_raw) or baseline[3] != source_action:
                raise Rejected("owned staging changed the authenticated original input")
            receipt.update(manifest_authenticated=True, bounded_input_bytes=total,
                           input_components=source_manifest["components"], action_profile_validation=source_action,
                           input_manifest={"path": "manifest.json", "bytes": len(source_raw),
                                           "sha256": hashlib.sha256(source_raw).hexdigest()},
                           stage_executables=bindings, launcher_images=launcher_bindings)
            base = [str(binaries["gpu"]), "--bundle", str(owned), "--producer-contract", args.producer_contract,
                    "--publication-mode", "semantic-mapping"]
            for component, (_path, option) in auth.COMPONENTS.items():
                base += [option, source_manifest["components"][component]["sha256"]]
            base += [auth.ACTION_PROFILE_COMPONENT[1], source_manifest["components"]["action_profile"]["sha256"]]
            receipt["steps"]["plan"] = {"status": "not-started", "argv": None,
                                         "stdout": "plan.stdout", "stderr": "plan.stderr"}
            inspection_deadline = min(deadline, time.monotonic() + _remaining_child_timeout(auth, deadline))
            plan = auth.run_child("plan", base + ["--inspect-plan"], staging, inspection_deadline, receipt, receipt_path)
            _validate_inspection(auth, plan, args.producer_contract)
            _validate_child_logs(staging, "plan")
            auth.validate_loaded_images(plan, binaries["gpu"], bindings["gpu"], runner, deadline)
            natives["plan"] = plan
            plan_digest = receipt["plan_sha256"] = plan["plan_sha256"]
            receipt["status"] = "qualifying"
            semantics = {}
            for stage, route, name in _child_specs():
                child_dir = staging / name
                receipt["steps"][name] = {"status": "not-started", "argv": None,
                                          "stdout": f"{name}.stdout", "stderr": f"{name}.stderr"}
                argv = [str(scripts["python_launcher"]), str(scripts[stage]), "--bundle", str(owned),
                        "--diagnostic", str(binaries[stage]), "--output-dir", str(child_dir),
                        "--route", route, "--producer-contract", args.producer_contract,
                        "--expected-plan-digest", plan_digest,
                        "--timeout-seconds", str(_remaining_child_timeout(auth, deadline))]
                policy = policies[route]
                replay = replay_policies[route]
                argv += ["--slot-headroom-bytes", str(policy["slot_headroom_bytes"]),
                         "--aggregate-headroom-bytes", str(policy["aggregate_headroom_bytes"]),
                         "--draw-command-limit", str(replay["draw_command_limit"])]
                if stage == "cpu":
                    argv += ["--buffer-preservation-contract", args.buffer_preservation_contract]
                else:
                    argv += ["--publication-mode", "semantic-mapping"]
                receipt["children"][name] = {"receipt": f"{name}/result.json", "accepted": False}
                try:
                    child_deadline = min(deadline, time.monotonic() + _remaining_child_timeout(auth, deadline))
                    auth.run_child(name, argv, staging, child_deadline, receipt, receipt_path)
                except (KeyboardInterrupt, auth.Rejected, OSError, ValueError, SystemExit):
                    try:
                        _validate_child_logs(staging, name)
                    except (Rejected, OSError) as error:
                        receipt.setdefault("secondary_failures", []).append(str(error))
                    raise
                _validate_child_logs(staging, name)
                child = _read_json(auth, child_dir / "result.json")
                expected_schema = cpu.RECEIPT_SCHEMA if stage == "cpu" else auth.RECEIPT_SCHEMA
                if child.get("schema") != expected_schema:
                    raise Rejected("child launcher schema rejected")
                _validate_child_binding(child, args, route, receipt["input_manifest"], environment, stage, plan_digest, policy, replay)
                if (child.get("diagnostic_executable") != bindings[stage]
                        or child.get("diagnostic_binary") != str(binaries[stage])
                        or child.get("bundle") != str(owned) or child.get("output_dir") != str(child_dir)):
                    raise Rejected("child endpoint executable fingerprint mismatch")
                native = _validate_native_child(child, args, stage, route, binaries[stage], bindings[stage],
                    source_manifest, source_action, plan_digest, child_dir, runner, deadline, cpu, policy=policy, replay=replay)
                natives[name] = native
                child_receipts[name] = child
                if stage == "cpu":
                    semantics[route] = native["execution"]["logical_semantics"]
                _image_map(native, binaries[stage])
                receipt["children"][name]["accepted"] = True
                auth.write_receipt(receipt_path, receipt)
                if stage == "cpu" and route == ROUTES[-1]:
                    receipt["logical_comparison"] = compare_logical_streams(semantics[ROUTES[0]], semantics[ROUTES[1]])
                    counts = canonical_execution_counts(natives[f"cpu-{ROUTES[0]}"])
                    if canonical_execution_counts(natives[f"cpu-{ROUTES[1]}"]) != counts:
                        raise Rejected("paired original execution/query count mismatch")
                    receipt["canonical_execution_counts"] = counts
                    receipt["logical_streams_equal"] = True
            expected_images = _image_map(natives["plan"], binaries["gpu"])
            for name, native in natives.items():
                stage = "gpu" if name == "plan" else name.split("-", 1)[0]
                if _image_map(native, binaries[stage]) != expected_images:
                    raise Rejected("paired CPU/GPU shared runtime image set or fingerprint mismatch")
            receipt.update(shared_runtime_images=expected_images,
                           original_gpu_queries_qualified=True, original_pixels_qualified=True)
            exit_code = 0
        except KeyboardInterrupt:
            receipt.update(status="interrupted", failure="paired launcher interrupted")
            exit_code = 130
        except (Rejected, auth.Rejected, OSError, ValueError, SystemExit) as error:
            receipt.update(status="rejected", failure=str(error))
        finally:
            if baseline is not None:
                try:
                    if inherited_environment(dict(os.environ)) != environment:
                        raise Rejected("inherited renderer environment changed during paired qualification")
                    if route_capacity_policies(args) != policies or receipt["capacity_policies"] != policies:
                        raise Rejected("requested paired route capacity policy changed during qualification")
                    if route_replay_policies(args, policies) != replay_policies or receipt["replay_policies"] != replay_policies:
                        raise Rejected("requested paired full replay policy changed during qualification")
                    current = cpu.authenticate(source, runner, deadline, args.producer_contract)
                    if current[:2] != (source_manifest, source_raw) or current[3] != source_action:
                        raise Rejected("original source input changed during paired qualification")
                    receipt["source_reauthenticated"] = True
                    current = cpu.authenticate(owned, runner, deadline, args.producer_contract)
                    if current[:2] != baseline[:2] or current[3] != baseline[3]:
                        raise Rejected("owned input changed during paired qualification")
                    receipt["owned_input_reauthenticated"] = True
                    for stage, evidence in bindings.items():
                        if cpu.fingerprint(binaries[stage], runner, deadline) != evidence:
                            raise Rejected("paired native endpoint executable changed")
                    receipt["binaries_reauthenticated"] = True
                    for stage, evidence in launcher_bindings.items():
                        if cpu.fingerprint(scripts[stage], runner, deadline) != evidence:
                            raise Rejected("paired launcher image changed")
                    receipt["launcher_images_reauthenticated"] = True
                    for name, native in natives.items():
                        stage = "gpu" if name == "plan" else name.split("-", 1)[0]
                        cpu.validate_images(native, binaries[stage], bindings[stage], runner, deadline)
                        if name != "plan" and _read_json(auth, staging / name / "result.json") != child_receipts[name]:
                            raise Rejected("accepted child receipt changed during paired qualification")
                        if name.startswith("gpu-"):
                            child_dir = staging / name
                            stat = child_dir.stat()
                            auth.validate_readbacks(native, child_dir, owned, source_manifest,
                                                    (stat.st_dev, stat.st_ino), deadline)
                    receipt["loaded_images_stable"] = bool(natives)
                except (Rejected, auth.Rejected, OSError, ValueError, SystemExit) as error:
                    receipt["reauthentication_failure"] = str(error)
                    receipt.setdefault("failure", str(error))
                    exit_code = 130 if exit_code == 130 else 1
                    receipt["status"] = "interrupted" if exit_code == 130 else "rejected"
            complete = exit_code == 0 and all(receipt.get(gate) is True for gate in (
                "logical_streams_equal", "original_gpu_queries_qualified", "original_pixels_qualified",
                "source_reauthenticated", "owned_input_reauthenticated", "binaries_reauthenticated",
                "launcher_images_reauthenticated", "loaded_images_stable"))
            receipt["accepted"] = receipt["paired_qualification_complete"] = complete
            if complete:
                receipt["status"] = "complete"
            elif exit_code == 0:
                receipt.update(status="rejected", failure="required paired qualification/authentication incomplete")
                exit_code = 1
            receipt["elapsed_seconds"] = time.monotonic() - started
            auth.write_receipt(receipt_path, receipt)
    print(json.dumps({"schema": SCHEMA, "status": receipt["status"], "accepted": receipt["accepted"],
                      "receipt": str(output / "result.json"), "failure": receipt.get("failure")}))
    return exit_code


def revalidate_paired_qualification(directory, args=None, runner=None, *, deadline=None) -> dict:
    """Read-only authority check for a later execution-only consumer.

    Relative artifacts survive the transaction's publication rename. Historical
    child directory strings remain provenance: the readback validator receives
    their separately checked recorded location, never a rewritten native receipt.
    No accepted bit, inherited flag or comparison summary alone grants authority.
    """
    try:
        cpu = _load_cpu_launcher()
        return _revalidate_paired_qualification(directory, args, runner, deadline, cpu)
    except (RuntimeError, OSError, ValueError, SystemExit) as error:
        # Each lazy import owns a distinct private Rejected class. A later
        # consumer needs one stable exported exception for preserved failures.
        if isinstance(error, Rejected):
            raise
        raise Rejected(str(error)) from error


def _revalidate_paired_qualification(directory, args, runner, deadline, cpu) -> dict:
    auth = cpu.auth
    runner = runner if runner is not None else cpu.load_bundle_tool()
    deadline = deadline if deadline is not None else time.monotonic() + MAX_CHILD_TIMEOUT_SECONDS
    if type(deadline) not in (int, float) or not math.isfinite(deadline):
        raise Rejected("paired revalidation deadline must be finite")
    root = pathlib.Path(directory).resolve()
    if not root.is_dir() or pathlib.Path(directory).is_symlink():
        raise Rejected("paired qualification must be an owned directory")
    receipt_path = root / "result.json"
    receipt_image = {}
    receipt = _read_json(auth, receipt_path, evidence=receipt_image)
    if auth.fingerprint_file(receipt_path, runner, deadline) != receipt_image:
        raise Rejected("paired aggregate receipt changed during authentication")
    if receipt.get("schema") != SCHEMA or receipt.get("status") != "complete":
        raise Rejected("paired qualification schema/status rejected")
    _true(receipt, "accepted", *AGGREGATE_GATES, "manifest_authenticated", "source_reauthenticated",
          "owned_input_reauthenticated", "binaries_reauthenticated", "launcher_images_reauthenticated",
          "loaded_images_stable", "conditional_on_legal_nooverwrite", "preservation_contract_is_caller_assumption")
    for flag in ("policy_qualified", "timing_eligible", "measurement_started", "full_semantic_oracle_complete",
                 "original_scalar_uniform_equivalence_qualified", "future_backing_preservation_complete"):
        if receipt.get(flag) is not False:
            raise Rejected("paired qualification contains unsupported promoted authority")
    iterations = _object(receipt, "iterations_per_route_per_backend")
    policies = validate_route_capacity_policies(receipt.get("capacity_policies"))
    replay_policies = validate_route_replay_policies(receipt.get("replay_policies"), policies)
    if (receipt.get("producer_contract") != PRODUCER_CONTRACT
            or receipt.get("buffer_preservation_contract") != PRESERVATION_CONTRACT
            or receipt.get("routes") != list(ROUTES) or receipt.get("gpu_warmup") != 0
            or type(receipt.get("gpu_warmup")) is not int
            or iterations != {"cpu": 1, "gpu": 2}
            or any(type(value) is not int for value in iterations.values())
            or receipt.get("expected_stage_environment") != stage_environments(policies, replay_policies)
            or receipt.get("capacity_matrix_complete") is not False
            or receipt.get("replay_policy_matrix_complete") is not False
            or receipt.get("replay_comparison_scope") != "bound-compatibility-versus-production-slot-single-axis-pair"
            or receipt.get("owned_input") != "input"):
        raise Rejected("paired route/iteration/contract/stage configuration rejected")
    environment = inherited_environment(dict(os.environ))
    if receipt.get("environment") != environment:
        raise Rejected("paired qualification inherited environment no longer matches")
    paths = _object(receipt, "stage_binary_paths")
    binaries = {}
    for stage in ("cpu", "gpu"):
        path = paths.get(stage)
        if not isinstance(path, str) or not pathlib.Path(path).is_absolute() or str(pathlib.Path(path).resolve()) != path:
            raise Rejected("paired native executable path is not canonical")
        binaries[stage] = pathlib.Path(path)
    config = argparse.Namespace(producer_contract=PRODUCER_CONTRACT, buffer_preservation_contract=PRESERVATION_CONTRACT)
    if args is not None:
        if (getattr(args, "slot_headroom_bytes", None) is not None
                or getattr(args, "aggregate_headroom_bytes", None) is not None):
            if route_capacity_policies(args) != policies:
                raise Rejected("requested route capacity identity differs from qualification")
        if hasattr(args, "draw_command_limit") and args.draw_command_limit != replay_policies["compatibility"]["draw_command_limit"]:
            raise Rejected("requested Compatibility grouping identity differs from qualification")
        for field in ("producer_contract", "buffer_preservation_contract"):
            if hasattr(args, field) and getattr(args, field) != getattr(config, field):
                raise Rejected("requested execution contract differs from qualification")
        for stage in ("cpu", "gpu"):
            field = f"{stage}_diagnostic"
            if hasattr(args, field) and pathlib.Path(getattr(args, field)).resolve() != binaries[stage]:
                raise Rejected("requested execution binary differs from qualification")
    owned = root / "input"
    manifest, raw, _total, action = cpu.authenticate(owned, runner, deadline, PRODUCER_CONTRACT)
    scope = _object(manifest, "scope")
    if scope.get("synthetic") is True or scope.get("test_input") is True:
        raise Rejected("production paired authority rejects synthetic/test provenance")
    expected_manifest = {"path": "manifest.json", "bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}
    if (receipt.get("input_manifest") != expected_manifest or receipt.get("input_components") != manifest["components"]
            or receipt.get("action_profile_validation") != action):
        raise Rejected("paired owned bundle/authenticated action changed")
    plan = receipt.get("plan_sha256")
    if not auth.sha256_nonzero(plan):
        raise Rejected("paired authenticated plan digest is missing")
    bindings = _object(receipt, "stage_executables")
    for stage, binary in binaries.items():
        if auth.fingerprint_file(binary, runner, deadline) != bindings.get(stage):
            raise Rejected("paired native executable fingerprint changed")
    repository = pathlib.Path(__file__).resolve().parents[2]
    scripts = {"cpu": repository / "scripts/check/run_render_tape_cpu_v4_semantics.py",
               "gpu": repository / "scripts/check/run_render_tape_v4_capture_diagnostic.py",
               "python_launcher": repository / "scripts/run_python.sh",
               "public_adapter": pathlib.Path(__file__).resolve(),
               "bundle_tool": repository / "scripts/tools/run_dxmt9_render_tape.py"}
    launcher_images = _object(receipt, "launcher_images")
    for stage, path in scripts.items():
        if auth.fingerprint_file(path, runner, deadline) != launcher_images.get(stage):
            raise Rejected("paired launcher executable/script changed")
    recorded = receipt.get("execution_staging_directory")
    if (not isinstance(recorded, str) or not pathlib.Path(recorded).is_absolute()
            or str(pathlib.Path(recorded)) != recorded):
        raise Rejected("paired historical execution directory rejected")
    historical = pathlib.Path(recorded)
    steps, children = _object(receipt, "steps"), _object(receipt, "children")
    specs = list(_child_specs())
    if set(children) != {name for _stage, _route, name in specs} or set(steps) != {"plan", *children}:
        raise Rejected("paired replay child/iteration coverage incomplete")
    inspection = _completed_step(_object(steps, "plan"))
    _validate_inspection(auth, inspection, PRODUCER_CONTRACT, plan)
    auth.validate_loaded_images(inspection, binaries["gpu"], bindings["gpu"], runner, deadline)
    runtime_images = _image_map(inspection, binaries["gpu"])
    if receipt.get("shared_runtime_images") != runtime_images:
        raise Rejected("paired shared runtime image declaration changed")
    qualification_images = {str(receipt_path): receipt_image}
    semantics, counts = {}, {}
    for stage, route, name in specs:
        _completed_step(_object(steps, name))
        if children[name] != {"receipt": f"{name}/result.json", "accepted": True}:
            raise Rejected("paired child path/acceptance evidence rejected")
        child_path = root / name / "result.json"
        child_image = {}
        child = _read_json(auth, child_path, evidence=child_image)
        if child.get("schema") != (cpu.RECEIPT_SCHEMA if stage == "cpu" else auth.RECEIPT_SCHEMA):
            raise Rejected("paired child receipt schema changed")
        policy = policies[route]
        replay = replay_policies[route]
        _validate_child_binding(child, config, route, expected_manifest, environment, stage, plan, policy, replay)
        if (child.get("diagnostic_executable") != bindings[stage]
                or child.get("diagnostic_binary") != str(binaries[stage])
                or child.get("bundle") != str(historical / "input")
                or child.get("output_dir") != str(historical / name)):
            raise Rejected("paired child executable/input/execution location mismatch")
        native = _validate_native_child(child, config, stage, route, binaries[stage], bindings[stage],
            manifest, action, plan, root / name, runner, deadline, cpu,
            policy=policy, replay=replay, recorded_directory=str(historical / name) if stage == "gpu" else None)
        if _image_map(native, binaries[stage]) != runtime_images:
            raise Rejected("paired child shared stage runtime images changed")
        if auth.fingerprint_file(child_path, runner, deadline) != child_image:
            raise Rejected("paired child receipt changed during authentication")
        qualification_images[str(child_path)] = child_image
        if stage == "cpu":
            semantics[route] = native["execution"]["logical_semantics"]
            counts[route] = canonical_execution_counts(native)
    comparison = compare_logical_streams(semantics[ROUTES[0]], semantics[ROUTES[1]])
    if (receipt.get("logical_comparison") != comparison or counts[ROUTES[0]] != counts[ROUTES[1]]
            or receipt.get("canonical_execution_counts") != counts[ROUTES[0]]):
        raise Rejected("paired logical item/count comparison changed")
    for path, evidence in qualification_images.items():
        if auth.fingerprint_file(pathlib.Path(path), runner, deadline) != evidence:
            raise Rejected("paired qualification receipt changed across revalidation")
    auth.require_time(deadline)
    return {"receipt": receipt, "owned_input": owned, "manifest": manifest, "action": action,
            "plan_sha256": plan, "cpu_binary": binaries["cpu"], "gpu_binary": binaries["gpu"],
            "stage_executables": bindings, "shared_runtime_images": runtime_images, "environment": environment,
            "producer_contract": PRODUCER_CONTRACT, "buffer_preservation_contract": PRESERVATION_CONTRACT,
            "cpu_stage_environment": dict(CPU_STAGE_PINS), "qualification_images": qualification_images,
            "capacity_policies": policies, "replay_policies": replay_policies,
            "cpu_stage_environments": stage_environments(policies, replay_policies)["cpu"]}
