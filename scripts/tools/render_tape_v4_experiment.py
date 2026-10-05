#!/usr/bin/env python3
"""Qualified fixed-input V4 observer-light CPU experiment.

Every warmup/sample is a fresh execution-only process. One existing paired-owned
input is shared without another full copy per iteration. Native body wall and
process CPU costs remain separate; neither is FPS, GPU time, or new semantic,
pixel, policy, or standalone timing authority.
"""

from __future__ import annotations

import argparse
import copy
import datetime
import hashlib
import importlib.util
import json
import math
import os
import pathlib
import time
from typing import Any


SCHEMA = "dxmt9.render_tape.v4_cpu_experiment.v1"
ROUTES = ("compatibility", "production-slot")
PRODUCER_CONTRACT = "pe-present-reservation-v1"
PRESERVATION_CONTRACT = "candidate-legal-nooverwrite-closed-writers-v1"
MAX_REPEAT = 32
MIN_COMPARISON_REPEAT = 16
MAX_ROUNDS_PER_ARM = 64
MAX_FRESH_CHILDREN = MAX_ROUNDS_PER_ARM * 2
MAX_TIMEOUT_SECONDS = 7200
MAX_CHILD_TIMEOUT_SECONDS = 600
MAX_CHILD_LOG_BYTES = 32 * 1024 * 1024
MAX_CHILD_RECEIPT_BYTES = 64 * 1024 * 1024
U64_MAX = (1 << 64) - 1
VALIDATION_SETTINGS = ("MTL_DEBUG_LAYER", "MTL_SHADER_VALIDATION", "MTL_HUD_ENABLED")
FALSE_CLAIMS = (
    "full_semantic_oracle_complete", "logical_stream_complete", "actual_metal",
    "gpu_replay", "gpu_result_oracle", "pixel_readback", "pixels_qualified",
    "policy_qualified", "timing_eligible", "full_logical_semantic_oracle_complete",
    "original_semantic_equivalence_qualified", "cross_policy_equivalence_qualified",
    "future_backing_preservation_complete", "full_shader_fetch_coverage_qualified",
    "physical_fetch_oracle_complete", "public_v4_execution_enabled",
    "future_preservation_qualified", "original_scalar_uniform_equivalence_qualified",
)
ORIGINAL_COUNTER_FIELDS = (
    "events", "commands", "draws", "resources_created", "resources_released",
    "observed_mutations", "observed_texture_uploads",
)
ORIGINAL_QUERY_FIELDS = ("definitions", "issues", "recorded_polls", "replayed_polls", "checkpoints", "rows")
ORIGINAL_PROVENANCE_FIELDS = ("replayed_events", "dispatched_records", "acknowledged_raws")
ORIGINAL_PLAN_FIELDS = ("events", "records", "sources")


class Rejected(RuntimeError):
    pass


def _load(path: pathlib.Path, name: str):
    specification = importlib.util.spec_from_file_location(name, path)
    if specification is None or specification.loader is None:
        raise Rejected(f"cannot load V4 experiment dependency: {path.name}")
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


def load_dependencies():
    repository = pathlib.Path(__file__).resolve().parents[2]
    paired = _load(repository / "scripts/tools/render_tape_v4_paired.py", "v4_experiment_paired")
    execute = _load(repository / "scripts/check/run_render_tape_cpu_v4_execute.py", "v4_experiment_execute")
    return paired, execute


def order_routes(value: str) -> tuple[str, str]:
    if not isinstance(value, str):
        raise Rejected("--order must select two explicit V4 routes")
    parts = value.split(",")
    if len(parts) != 2 or any(route not in ROUTES for route in parts):
        raise Rejected("--order must contain two compatibility/production-slot routes; A/A is allowed")
    return parts[0], parts[1]


def _object(owner: dict, name: str) -> dict:
    value = owner.get(name)
    if not isinstance(value, dict):
        raise Rejected(f"missing authoritative object: {name}")
    return value


def _uint(owner: dict, name: str, maximum: int = U64_MAX) -> int:
    value = owner.get(name)
    if type(value) is not int or not 0 <= value <= maximum:
        raise Rejected(f"invalid native count/clock: {name}")
    return value


def measurement_environment(paired, environment: dict[str, str]) -> dict[str, str]:
    selected = paired.inherited_environment(environment)
    if any(selected.get(name, "") not in ("", "0") for name in VALIDATION_SETTINGS):
        raise Rejected("CPU experiment rejects enabled Metal validation or HUD before measurement")
    return selected


def iteration_schedule(routes: tuple[str, str], warmup: int, repeat: int) -> list[dict]:
    if len(routes) != 2 or any(route not in ROUTES for route in routes):
        raise Rejected("each experiment must have two explicitly selected route arms")
    if (type(warmup) is not int or type(repeat) is not int or not 0 <= warmup
            or not 1 <= repeat <= MAX_REPEAT or warmup + repeat > MAX_ROUNDS_PER_ARM):
        raise Rejected("V4 experiment requires repeat 1..32 and warmup+repeat <=64 per arm")
    rows = []
    for phase, count in (("warmup", warmup), ("measurement", repeat)):
        for index in range(count):
            for arm, route in enumerate(routes):
                run_id = f"{phase}-{index:02d}-arm-{arm}"
                rows.append({"run_id": run_id, "arm_id": f"arm-{arm}", "arm_index": arm,
                             "route": route, "phase": phase, "round": index,
                             "status": "not-started", "accepted": False,
                             "receipt": f"{run_id}/result.json"})
    if len(rows) > MAX_FRESH_CHILDREN:
        raise Rejected("fresh execution child count exceeds the experiment cap")
    return rows


def _context_binding(context: dict) -> dict:
    # This is a comparison snapshot of the revalidator's authoritative result,
    # not a replacement validator or an interpretation of an acceptance bit.
    required = ("receipt", "manifest", "action", "stage_executables", "shared_runtime_images",
                "environment", "cpu_stage_environment", "qualification_images", "capacity_policies",
                "cpu_stage_environments", "replay_policies")
    for field in required:
        _object(context, field)
    binding = {
        "owned_input": str(context["owned_input"]),
        "cpu_binary": str(context["cpu_binary"]), "gpu_binary": str(context["gpu_binary"]),
        "plan_sha256": context["plan_sha256"], "manifest": context["manifest"], "action": context["action"],
        "stage_executables": context["stage_executables"], "shared_runtime_images": context["shared_runtime_images"],
        "environment": context["environment"], "cpu_stage_environment": context["cpu_stage_environment"],
        "producer_contract": context["producer_contract"],
        "buffer_preservation_contract": context["buffer_preservation_contract"],
        "qualification_images": context["qualification_images"],
        "canonical_execution_counts": _object(context["receipt"], "canonical_execution_counts"),
        "capacity_policies": context["capacity_policies"], "cpu_stage_environments": context["cpu_stage_environments"],
        "replay_policies": context["replay_policies"],
    }
    # A later validator invocation or test seam must not mutate the baseline
    # through shared nested dictionaries. This copies evidence only, not input.
    return json.loads(json.dumps(binding, sort_keys=True, allow_nan=False))


def _runtime_images(native: dict, binary: pathlib.Path) -> dict:
    images = native.get("loaded_images")
    if not isinstance(images, list) or not 1 <= len(images) <= 256:
        raise Rejected("missing actual bounded runtime module evidence")
    result, seen = {}, set()
    for item in images:
        if not isinstance(item, dict) or set(item) != {"path", "bytes", "sha256"}:
            raise Rejected("actual runtime module row contract rejected")
        path = item["path"]
        if not isinstance(path, str) or path in seen:
            raise Rejected("duplicate or malformed actual runtime module path")
        seen.add(path)
        if path != str(binary):
            result[path] = {"bytes": item["bytes"], "sha256": item["sha256"]}
    if str(binary) not in seen:
        raise Rejected("actual loaded execution endpoint is absent")
    return result


def qualified_original_work(context: dict, qualification: pathlib.Path, execute, runner, deadline: float) -> dict:
    """Join original-work counts to bytes already authenticated by the pair.

    Provenance is not projected into the public pair's canonical counters, so
    read its two actual CPU native receipts by their authenticated fingerprints.
    This never treats an acceptance flag or an unbound copied summary as proof.
    """
    canonical = _object(context["receipt"], "canonical_execution_counts")
    reference = {
        "counters": {name: _uint(_object(canonical, "counters"), name, execute.MAX_ITEMS)
                     for name in ORIGINAL_COUNTER_FIELDS},
        "query": {name: _uint(_object(canonical, "query"), name, execute.MAX_ITEMS)
                  for name in ORIGINAL_QUERY_FIELDS},
    }
    originals = []
    images = _object(context, "qualification_images")
    for route in ROUTES:
        directory = qualification / f"cpu-{route}"
        path = directory / "result.json"
        expected = images.get(str(path))
        if (directory.is_symlink() or path.is_symlink() or path.resolve() != path
                or not isinstance(expected, dict)):
            raise Rejected("paired original CPU receipt lacks owned authenticated image evidence")
        raw = execute.auth.read_capped(path, MAX_CHILD_RECEIPT_BYTES)
        parsed_image = {"bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}
        if (parsed_image != expected or execute.auth.fingerprint_file(path, runner, deadline) != expected):
            raise Rejected("paired original CPU receipt changed before original-work binding")
        child = execute.auth.parse_json(raw)
        if not isinstance(child, dict):
            raise Rejected("paired original CPU receipt is not an object")
        steps = _object(child, "steps")
        step = _object(steps, "semantics")
        if (set(steps) != {"semantics"} or step.get("status") != "exited"
                or type(step.get("returncode")) is not int or step["returncode"] != 0
                or step.get("timed_out") is not False):
            raise Rejected("paired original CPU receipt lacks one completed semantic native process")
        native = _object(step, "native_result")
        policy = context["capacity_policies"][route]
        if (child.get("capacity_policy") != policy or native.get("capacity_policy") != policy
                or child.get("replay_policy") != context["replay_policies"][route]
                or native.get("replay_policy") != context["replay_policies"][route]
                or child.get("expected_stage_environment") != context["cpu_stage_environments"][route]
                or native.get("effective_stage_environment") != context["cpu_stage_environments"][route]):
            raise Rejected("paired original CPU capacity identity differs from its route binding")
        execution = _object(native, "execution")
        counters, query = _object(execution, "counters"), _object(execution, "query_projection")
        if (any(_uint(counters, name, execute.MAX_ITEMS) != value for name, value in reference["counters"].items())
                or any(_uint(query, name, execute.MAX_ITEMS) != value for name, value in reference["query"].items())):
            raise Rejected("paired original CPU counts differ from authenticated canonical counts")
        originals.append({
            "plan_counts": {name: _uint(_object(native, "plan_counts"), name, execute.MAX_ITEMS)
                            for name in ORIGINAL_PLAN_FIELDS},
            "provenance": {name: _uint(_object(execution, "provenance"), name, execute.MAX_ITEMS)
                           for name in ORIGINAL_PROVENANCE_FIELDS},
        })
    if originals[0] != originals[1]:
        raise Rejected("paired routes disagree on captured original plan/provenance counts")
    return {**reference, **originals[0]}


def validate_execution_child(child: dict, context: dict, route: str, execute, runner,
                             deadline: float, environment: dict, child_directory: pathlib.Path,
                             original_work: dict) -> dict:
    if (child.get("schema") != execute.RECEIPT_SCHEMA or child.get("status") != "complete"
            or child.get("diagnostic") is not True
            or any(child.get(name) is not True for name in (
                "accepted", "execution_complete", "manifest_authenticated", "bundle_reauthenticated",
                "binary_reauthenticated", "loaded_images_stable", "conditional_on_legal_nooverwrite",
                "preservation_contract_is_caller_assumption", "capacity_policy_valid", "stage_environment_valid",
                "replay_policy_valid"))
            or any(child.get(name) is not False for name in execute.FALSE_CLAIMS)):
        raise Rejected("execution child wrapper completion or authority contract rejected")
    expected = {
        "route": route, "requested_route": route, "effective_route": route,
        "bundle": str(context["owned_input"]), "diagnostic_binary": str(context["cpu_binary"]),
        "output_dir": str(child_directory),
        "expected_plan_digest": context["plan_sha256"], "producer_contract": context["producer_contract"],
        "buffer_preservation_contract": context["buffer_preservation_contract"],
        "input_manifest": context["receipt"]["input_manifest"],
        "input_components": context["manifest"]["components"], "action_profile_validation": context["action"],
        "diagnostic_executable": context["stage_executables"]["cpu"],
        "environment": environment, "effective_stage_environment": context["cpu_stage_environments"][route],
        "expected_stage_environment": context["cpu_stage_environments"][route],
        "capacity_policy": context["capacity_policies"][route],
        "replay_policy": context["replay_policies"][route],
        "iterations": 1,
    }
    if any(child.get(name) != value for name, value in expected.items()) or type(child.get("iterations")) is not int:
        raise Rejected("execution child input/binary/contract/configuration binding mismatch")
    if execute.expected_stage_environment(_object(child, "capacity_policy"),
            _object(child, "replay_policy").get("draw_command_limit")) != context["cpu_stage_environments"][route]:
        raise Rejected("execution child capacity policy must retain exact integer route identity")
    steps = _object(child, "steps")
    step = _object(steps, "execute")
    if (set(steps) != {"execute"} or step.get("status") != "exited"
            or type(step.get("returncode")) is not int or step["returncode"] != 0
            or step.get("timed_out") is not False):
        raise Rejected("execution child did not complete its one actual native process")
    native = _object(step, "native_result")
    native_args = argparse.Namespace(route=route, expected_plan_digest=context["plan_sha256"],
        producer_contract=context["producer_contract"],
        buffer_preservation_contract=context["buffer_preservation_contract"],
        draw_command_limit=context["replay_policies"][route]["draw_command_limit"], **context["capacity_policies"][route])
    # Re-run the independent native/omission/query/clock/lifecycle validator and
    # hash actual loaded images ourselves; accepted:true is insufficient.
    execute.validate_native(native, native_args, context["manifest"], context["action"],
                            _uint(child, "native_max_wall_ms", execute.MAX_WALL_MS))
    execute.validate_images(native, context["cpu_binary"], context["stage_executables"]["cpu"], runner, deadline)
    if _runtime_images(native, context["cpu_binary"]) != context["shared_runtime_images"]:
        raise Rejected("execution child runtime module set/fingerprints differ from paired qualification")
    execution = _object(native, "execution")
    for group, values in original_work.items():
        actual = _object(native, group) if group == "plan_counts" else _object(
            execution, "query_projection" if group == "query" else group)
        if any(_uint(actual, name, execute.MAX_ITEMS) != value for name, value in values.items()):
            raise Rejected(f"execution-only original-work count differs from paired qualification: {group}")
    clocks = _object(execution, "clocks")
    if (child.get("execution") != execution or child.get("clocks") != clocks
            or child.get("route_coverage") != execution.get("route_coverage")
            or child.get("loaded_images") != native.get("loaded_images")):
        raise Rejected("execution child copied projections differ from authoritative native evidence")
    native_binding = _object(execution["consumer_receipts"][0], "identity")
    return {
        "clocks": clocks, "route_coverage": execution["route_coverage"],
        "execution_only": execution["execution_only"],
        "native_arm": {name: _uint(native_binding, name) for name in ("queue", "capture_token", "capture_epoch")},
        "provenance": execution["provenance"], "consumer": execution["consumer"],
        "query_projection": execution["query_projection"], "counters": execution["counters"],
        "capacity_policy": context["capacity_policies"][route],
        "replay_policy": context["replay_policies"][route],
        "effective_stage_environment": context["cpu_stage_environments"][route],
    }


def clock_summary(values: list[int]) -> dict:
    if not values:
        return {"samples": 0, "min_ns": None, "median_ns": None, "p95_ns": None, "max_ns": None}
    ordered = sorted(values)
    count = len(ordered)
    median = (ordered[(count - 1) // 2] + ordered[count // 2]) // 2
    return {"samples": count, "min_ns": ordered[0], "median_ns": median,
            "p95_ns": ordered[(95 * count + 99) // 100 - 1], "max_ns": ordered[-1]}


def summarize(rows: list[dict], routes: tuple[str, str], repeat: int, accepted: bool) -> dict:
    arms = []
    for arm, route in enumerate(routes):
        samples = [row for row in rows if row["arm_index"] == arm and row["phase"] == "measurement"
                   and row.get("accepted") is True]
        warmups = [row for row in rows if row["arm_index"] == arm and row["phase"] == "warmup"]
        arms.append({"arm_id": f"arm-{arm}", "route": route,
                     "warmups_attempted": sum(row["status"] not in ("not-started", "skipped") for row in warmups),
                     "warmups_accepted": sum(row.get("accepted") is True for row in warmups),
                     "body_wall": clock_summary([row["clocks"]["body"]["wall_ns"] for row in samples]),
                     "body_process_cpu": clock_summary([row["clocks"]["body"]["process_cpu_ns"] for row in samples])})
    enough = accepted and repeat >= MIN_COMPARISON_REPEAT and all(
        arm[clock]["samples"] == repeat and arm[clock]["median_ns"] > 0
        for arm in arms for clock in ("body_wall", "body_process_cpu"))
    comparison = None
    if enough:
        comparison = {}
        for clock in ("body_wall", "body_process_cpu"):
            first, second = (arm[clock]["median_ns"] for arm in arms)
            comparison[clock] = {"arm_1_over_arm_0_median_ratio": second / first,
                                 "arm_1_minus_arm_0_median_ns": second - first}
    return {"arms": arms, "comparison_ready": bool(enough), "comparison": comparison,
            "minimum_comparison_repeat": MIN_COMPARISON_REPEAT,
            "smoke_sample_count": repeat < MIN_COMPARISON_REPEAT,
            "clock_scope": "native-body-after-setup-and-arm-through-controlled-consumer-join",
            "fps": None, "gpu_time": None, "policy_ranking": None}


def experiment(args: argparse.Namespace, runner=None) -> int:
    """Public adapter; authoritative qualification is revalidated before runs."""
    timeout = args.timeout_seconds
    if type(timeout) not in (int, float) or not math.isfinite(timeout) or not 1 <= timeout <= MAX_TIMEOUT_SECONDS:
        raise SystemExit("--timeout-seconds must be finite and within 1..7200")
    if args.producer_contract != PRODUCER_CONTRACT or args.buffer_preservation_contract != PRESERVATION_CONTRACT:
        raise SystemExit("V4 experiment requires explicit producer and legal-candidate-NOOVERWRITE contracts")
    try:
        routes = order_routes(args.order)
        rows = iteration_schedule(routes, args.warmup, args.repeat)
    except Rejected as error:
        raise SystemExit(str(error)) from error
    qualification_path = pathlib.Path(args.qualification_dir)
    qualification = qualification_path.resolve()
    output = pathlib.Path(args.output_dir).absolute()
    if (qualification_path.is_symlink() or not qualification.is_dir()
            or output.resolve().is_relative_to(qualification)):
        raise SystemExit("experiment requires a paired qualification directory and new output outside it")
    if output.exists() or output.is_symlink():
        raise SystemExit("experiment output directory must be new")
    paired, execute = load_dependencies()
    auth = execute.auth
    runner = runner if runner is not None else execute.load_bundle_tool()
    errors = (Rejected, paired.Rejected, execute.Rejected, OSError, ValueError, SystemExit, KeyError, TypeError)
    started = time.monotonic()
    deadline = started + timeout
    reserve = max(1.0, min(30.0, timeout / 10.0))
    repository = pathlib.Path(__file__).resolve().parents[2]
    scripts = {
        "execute": repository / "scripts/check/run_render_tape_cpu_v4_execute.py",
        "semantics_helpers": repository / "scripts/check/run_render_tape_cpu_v4_semantics.py",
        "authentication_helpers": repository / "scripts/check/run_render_tape_v4_capture_diagnostic.py",
        "paired_revalidator": repository / "scripts/tools/render_tape_v4_paired.py",
        "experiment": pathlib.Path(__file__).resolve(),
        "python_launcher": repository / "scripts/run_python.sh",
    }
    receipt: dict[str, Any] = {
        "schema": SCHEMA, "status": "authenticating", "accepted": False,
        **{claim: False for claim in FALSE_CLAIMS}, "measurement_started": False,
        "measurement_complete": False, "qualification_revalidated": False,
        "final_qualification_revalidated": False, "executions_reauthenticated": False,
        "launcher_images_reauthenticated": False,
        "qualification_dir": str(qualification), "output_dir": str(output),
        "producer_contract": args.producer_contract, "buffer_preservation_contract": args.buffer_preservation_contract,
        "conditional_on_legal_nooverwrite": True, "preservation_contract_is_caller_assumption": True,
        "capacity_matrix_complete": False,
        "replay_policy_matrix_complete": False,
        "order": list(routes), "arms": [{"arm_id": f"arm-{index}", "route": route} for index, route in enumerate(routes)],
        "same_route_control": routes[0] == routes[1], "warmup_per_arm": args.warmup, "repeat_per_arm": args.repeat,
        "fresh_process_per_iteration": True, "input_copy_policy": "reuse-one-paired-owned-immutable-input",
        "native_arm_identity_scope": "process-local; distinct aggregate arm_id and run_id disambiguate fresh processes",
        "environment": None, "steps": {}, "iterations": rows,
        "limits": {"rounds_per_arm": MAX_ROUNDS_PER_ARM, "fresh_children": MAX_FRESH_CHILDREN,
                   "measurement_repeat_per_arm": MAX_REPEAT, "child_timeout_seconds": MAX_CHILD_TIMEOUT_SECONDS,
                   "child_receipt_bytes": MAX_CHILD_RECEIPT_BYTES,
                   "native_result_json_bytes": auth.MAX_RESULT_JSON_BYTES, "child_log_bytes": MAX_CHILD_LOG_BYTES},
        "timeout_seconds": timeout, "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "acceptance_scope": "revalidated-fixed-input-pair-bound-observer-light-cpu-lifecycle-and-clock-observations",
    }
    exit_code = 1
    with runner.bundle_transaction(output, preserve_rejected=True,
            failure_metadata={"schema": SCHEMA, "lane": "v4-cpu-experiment", "accepted": False}) as staging:
        receipt_path = staging / "result.json"
        context = baseline = environment = original_work = None
        script_bindings, child_bindings = {}, {}
        auth.write_receipt(receipt_path, receipt)
        try:
            environment = measurement_environment(paired, dict(os.environ))
            receipt["environment"] = environment
            context = paired.revalidate_paired_qualification(qualification, args, runner, deadline=deadline)
            baseline = _context_binding(context)
            context = copy.deepcopy(context)
            policies = paired.validate_route_capacity_policies(context["capacity_policies"])
            replay_policies = paired.validate_route_replay_policies(context["replay_policies"], policies)
            if (context["cpu_stage_environments"] != paired.stage_environments(policies, replay_policies)["cpu"]
                    or any(context["cpu_stage_environments"][route] !=
                           execute.expected_stage_environment(policies[route], replay_policies[route]["draw_command_limit"])
                           for route in ROUTES)):
                raise Rejected("measurement route capacity policy or CPU environment differs from qualification")
            if (context["environment"] != environment or context["producer_contract"] != args.producer_contract
                    or context["buffer_preservation_contract"] != args.buffer_preservation_contract
                    or context["cpu_stage_environment"] != execute.STAGE_ENVIRONMENT):
                raise Rejected("measurement environment/contracts/configuration differ from revalidated qualification")
            requested_binary = getattr(args, "diagnostic", None)
            if requested_binary is not None and pathlib.Path(requested_binary).resolve() != context["cpu_binary"]:
                raise Rejected("measurement CPU endpoint must be the exact paired-qualified executable")
            original_work = qualified_original_work(context, qualification, execute, runner, deadline)
            script_bindings = {name: auth.fingerprint_file(path, runner, deadline) for name, path in scripts.items()}
            receipt.update(qualification_revalidated=True, owned_input=str(context["owned_input"]),
                plan_sha256=context["plan_sha256"], input_manifest=context["receipt"]["input_manifest"],
                input_components=context["manifest"]["components"], stage_executables=context["stage_executables"],
                shared_runtime_images=context["shared_runtime_images"],
                qualification_images=context["qualification_images"], launcher_images=script_bindings,
                qualified_original_work=original_work,
                capacity_policies=policies, replay_policies=replay_policies, capacity_matrix_complete=False,
                effective_stage_environments=context["cpu_stage_environments"], status="executing")
            for arm in receipt["arms"]:
                arm["capacity_policy"] = dict(policies[arm["route"]])
                arm["replay_policy"] = dict(replay_policies[arm["route"]])
            auth.write_receipt(receipt_path, receipt)
            for row in rows:
                child_dir = staging / row["run_id"]
                name = row["run_id"]
                remaining = auth.require_time(deadline) - reserve
                if remaining < 1.0:
                    raise Rejected("shared experiment deadline exhausted before the next fresh process")
                child_timeout = min(MAX_CHILD_TIMEOUT_SECONDS, remaining)
                child_deadline = min(deadline - reserve, time.monotonic() + child_timeout)
                argv = [str(scripts["python_launcher"]), str(scripts["execute"]),
                    "--bundle", str(context["owned_input"]), "--diagnostic", str(context["cpu_binary"]),
                    "--output-dir", str(child_dir), "--expected-plan-digest", context["plan_sha256"],
                    "--route", row["route"], "--producer-contract", args.producer_contract,
                    "--buffer-preservation-contract", args.buffer_preservation_contract,
                    "--slot-headroom-bytes", str(policies[row["route"]]["slot_headroom_bytes"]),
                    "--aggregate-headroom-bytes", str(policies[row["route"]]["aggregate_headroom_bytes"]),
                    "--draw-command-limit", str(replay_policies[row["route"]]["draw_command_limit"]),
                    "--timeout-seconds", str(child_timeout)]
                receipt["steps"][name] = {"status": "not-started", "argv": None,
                    "stdout": f"{name}.stdout", "stderr": f"{name}.stderr"}
                row["status"] = "running"
                receipt["measurement_started"] |= row["phase"] == "measurement"
                try:
                    try:
                        auth.run_child(name, argv, staging, child_deadline, receipt, receipt_path)
                        outer_step = _object(receipt["steps"], name)
                        if (outer_step.get("status") != "exited" or type(outer_step.get("returncode")) is not int
                                or outer_step["returncode"] != 0 or outer_step.get("timed_out") is not False):
                            raise Rejected("execution launcher process completion/timeout evidence rejected")
                    finally:
                        for log in (staging / f"{name}.stdout", staging / f"{name}.stderr"):
                            if log.exists() and log.stat().st_size > MAX_CHILD_LOG_BYTES:
                                raise Rejected("execution launcher log exceeds the experiment byte cap")
                    child_path = child_dir / "result.json"
                    if child_dir.is_symlink() or child_path.is_symlink() or not child_path.is_file():
                        raise Rejected("execution child receipt is not a regular owned artifact")
                    child = auth.parse_json(auth.read_capped(child_path, MAX_CHILD_RECEIPT_BYTES))
                    if not isinstance(child, dict):
                        raise Rejected("execution child receipt is not a regular bounded JSON object")
                    evidence = validate_execution_child(child, context, row["route"], execute, runner,
                                                        deadline, environment, child_dir, original_work)
                    child_bindings[str(child_path)] = auth.fingerprint_file(child_path, runner, deadline)
                    row.update(evidence, status="complete", accepted=True,
                               receipt_fingerprint=child_bindings[str(child_path)])
                except errors as error:
                    row.update(status="rejected", failure=str(error))
                    raise
                finally:
                    auth.write_receipt(receipt_path, receipt)
            exit_code = 0
        except KeyboardInterrupt:
            receipt.update(status="interrupted", failure="V4 experiment interrupted")
            exit_code = 130
        except errors as error:
            receipt.update(status="rejected", failure=str(error))
        finally:
            if baseline is not None:
                try:
                    if measurement_environment(paired, dict(os.environ)) != environment:
                        raise Rejected("measurement environment changed during the V4 experiment")
                    current = paired.revalidate_paired_qualification(qualification, args, runner, deadline=deadline)
                    if _context_binding(current) != baseline:
                        raise Rejected("input/binary/module/configuration/qualification evidence changed across measurement")
                    if (original_work is not None
                            and qualified_original_work(current, qualification, execute, runner, deadline) != original_work):
                        raise Rejected("paired original-work references changed across measurement")
                    receipt["final_qualification_revalidated"] = True
                    for name, binding in script_bindings.items():
                        if auth.fingerprint_file(scripts[name], runner, deadline) != binding:
                            raise Rejected("execution/authentication/aggregate launcher image changed")
                    receipt["launcher_images_reauthenticated"] = True
                    for path, binding in child_bindings.items():
                        if auth.fingerprint_file(pathlib.Path(path), runner, deadline) != binding:
                            raise Rejected("authoritative execution child receipt changed across measurement")
                    receipt["executions_reauthenticated"] = len(child_bindings) == len(rows)
                except errors as error:
                    receipt["reauthentication_failure"] = str(error)
                    receipt.setdefault("failure", str(error))
                    exit_code = 130 if exit_code == 130 else 1
                    receipt["status"] = "interrupted" if exit_code == 130 else "rejected"
            for row in rows:
                if row["status"] == "not-started":
                    row.update(status="skipped", failure="prior qualification, deadline or child failure")
                elif row["status"] == "running":
                    row.update(status="interrupted" if exit_code == 130 else "rejected", failure=receipt.get("failure"))
            accepted = exit_code == 0 and all(receipt[name] is True for name in (
                "qualification_revalidated", "final_qualification_revalidated", "executions_reauthenticated",
                "launcher_images_reauthenticated")) and all(row["accepted"] is True for row in rows)
            receipt["accepted"] = receipt["measurement_complete"] = accepted
            if accepted:
                receipt["status"] = "complete"
            elif exit_code == 0:
                exit_code = 1
                receipt.update(status="rejected", failure="required aggregate authentication/execution coverage incomplete")
            receipt["summary"] = summarize(rows, routes, args.repeat, accepted)
            receipt["elapsed_seconds"] = time.monotonic() - started
            auth.write_receipt(receipt_path, receipt)
    print(json.dumps({"schema": SCHEMA, "accepted": receipt["accepted"], "status": receipt["status"],
                      "receipt": str(output / "result.json"), "comparison_ready": receipt["summary"]["comparison_ready"],
                      "failure": receipt.get("failure")}))
    return exit_code
