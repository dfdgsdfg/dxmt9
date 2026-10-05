#!/usr/bin/env python3
"""Compare two independently qualified controlled-route replay policies.

Run through scripts/run_python.sh. The ordered qualification directories select
the arms, including A/A. Native body wall/process-CPU observations do not become
FPS, Metal timing, unconditional preservation, or general policy authority.
"""

from __future__ import annotations

import argparse
import copy
import datetime
import importlib.util
import json
import math
import os
import pathlib
import time
from typing import Any


SCHEMA = "dxmt9.render_tape.v4_capacity_experiment.v1"
ROUTE = "production-slot"
MIN_WARMUP = 2


class Rejected(RuntimeError):
    pass


def load_dependencies():
    path = pathlib.Path(__file__).with_name("render_tape_v4_experiment.py")
    spec = importlib.util.spec_from_file_location("v4_capacity_experiment_helpers", path)
    if spec is None or spec.loader is None:
        raise Rejected("cannot load the frozen V4 experiment helpers")
    experiment = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(experiment)
    paired, execute = experiment.load_dependencies()
    return experiment, paired, execute


def _object(owner: dict, key: str) -> dict:
    value = owner.get(key)
    if not isinstance(value, dict):
        raise Rejected(f"missing cross-qualification object: {key}")
    return value


def _snapshot(value):
    return json.loads(json.dumps(value, sort_keys=True, allow_nan=False))


def _common_binding(context: dict, paired, execute) -> dict:
    policies = paired.validate_route_capacity_policies(context.get("capacity_policies"))
    replay_policies = paired.validate_route_replay_policies(context.get("replay_policies"), policies)
    stage_environments = paired.stage_environments(policies, replay_policies)
    if (context.get("cpu_stage_environment") != execute.STAGE_ENVIRONMENT
            or context.get("cpu_stage_environments") != stage_environments["cpu"]
            or context["receipt"].get("capacity_policies") != policies
            or context["receipt"].get("replay_policies") != replay_policies
            or context["receipt"].get("expected_stage_environment") != stage_environments
            or any(stage_environments["cpu"][route] != execute.expected_stage_environment(
                   policies[route], replay_policies[route]["draw_command_limit"])
                   for route in paired.ROUTES)):
        raise Rejected("qualification route capacity/environment identities are inconsistent")
    # Owned-directory and receipt identities remain independently bound below.
    # Only declared replay-policy axes and their derived stage pins may differ;
    # all original data, work, endpoint/module images and base inputs must match.
    return _snapshot({
        "input_manifest": _object(context["receipt"], "input_manifest"),
        "manifest": _object(context, "manifest"), "action": _object(context, "action"),
        "plan_sha256": context["plan_sha256"],
        "canonical_execution_counts": _object(context["receipt"], "canonical_execution_counts"),
        "cpu_binary": str(context["cpu_binary"]), "gpu_binary": str(context["gpu_binary"]),
        "stage_executables": _object(context, "stage_executables"),
        "shared_runtime_images": _object(context, "shared_runtime_images"),
        "environment": _object(context, "environment"),
        "cpu_stage_environment": context["cpu_stage_environment"],
        "producer_contract": context["producer_contract"],
        "buffer_preservation_contract": context["buffer_preservation_contract"],
    })


def _candidate_semantics(context: dict, directory: pathlib.Path, route, paired, execute, runner, deadline: float) -> dict:
    path = directory / f"cpu-{route}" / "result.json"
    if path.parent.is_symlink() or path.resolve() != path:
        raise Rejected("candidate semantic receipt must be an owned canonical paired artifact")
    image = {}
    child = paired._read_json(execute.auth, path, evidence=image)
    expected = _object(context, "qualification_images").get(str(path))
    if image != expected or execute.auth.fingerprint_file(path, runner, deadline) != image:
        raise Rejected("candidate semantic receipt bytes differ from revalidated qualification")
    native = paired._completed_step(_object(_object(child, "steps"), "semantics"))
    return _object(_object(native, "execution"), "logical_semantics")


def _qualify_arms(directories, args, experiment, paired, execute, runner, deadline, environment):
    contexts = [paired.revalidate_paired_qualification(path, args, runner, deadline=deadline)
                for path in directories]
    common = [_common_binding(context, paired, execute) for context in contexts]
    if common[0] != common[1]:
        raise Rejected("qualifications differ in original input/action/plan/work/images/base environment/contracts")
    if (common[0]["environment"] != environment or common[0]["producer_contract"] != args.producer_contract
            or common[0]["buffer_preservation_contract"] != args.buffer_preservation_contract):
        raise Rejected("measurement base environment/contracts differ from both qualifications")
    requested = getattr(args, "diagnostic", None)
    if requested is not None and pathlib.Path(requested).resolve() != contexts[0]["cpu_binary"]:
        raise Rejected("measurement endpoint must be the exact CPU binary shared by both qualifications")
    work = [experiment.qualified_original_work(context, path, execute, runner, deadline)
            for context, path in zip(contexts, directories)]
    if work[0] != work[1]:
        raise Rejected("qualifications disagree on authenticated original plan/provenance work")
    route = getattr(args, "route", ROUTE)
    semantics = [_candidate_semantics(context, path, route, paired, execute, runner, deadline)
                 for context, path in zip(contexts, directories)]
    comparison = paired.compare_logical_streams(semantics[0], semantics[1])
    return contexts, common[0], work[0], comparison


def capacity_experiment(args: argparse.Namespace, runner=None) -> int:
    """Historical API name; compare two policies on one explicitly selected route."""
    experiment, paired, execute = load_dependencies()
    route = getattr(args, "route", ROUTE)
    if route not in paired.ROUTES:
        raise SystemExit("--route must select compatibility or production-slot")
    timeout = args.timeout_seconds
    if (type(timeout) not in (int, float) or not math.isfinite(timeout)
            or not 1 <= timeout <= experiment.MAX_TIMEOUT_SECONDS):
        raise SystemExit("--timeout-seconds must be finite and within 1..7200")
    if args.producer_contract != experiment.PRODUCER_CONTRACT or args.buffer_preservation_contract != experiment.PRESERVATION_CONTRACT:
        raise SystemExit("capacity comparison requires both explicit producer and legal-NOOVERWRITE contracts")
    if type(args.warmup) is not int or args.warmup < MIN_WARMUP:
        raise SystemExit("capacity comparison requires at least two warmups per arm")
    if type(args.repeat) is not int or not experiment.MIN_COMPARISON_REPEAT <= args.repeat <= experiment.MAX_REPEAT:
        raise SystemExit("capacity comparison requires 16..32 measured samples per arm")
    try:
        rows = experiment.iteration_schedule((route, route), args.warmup, args.repeat)
    except experiment.Rejected as error:
        raise SystemExit(str(error)) from error
    source_paths = (pathlib.Path(args.qualification_a), pathlib.Path(args.qualification_b))
    directories = tuple(path.resolve() for path in source_paths)
    output = pathlib.Path(args.output_dir).absolute()
    if any(path.is_symlink() or not directory.is_dir() for path, directory in zip(source_paths, directories)):
        raise SystemExit("both qualifications must be owned directories")
    if output.exists() or output.is_symlink() or any(output.resolve().is_relative_to(path) for path in directories):
        raise SystemExit("capacity output must be a new directory outside both qualifications")
    runner = runner if runner is not None else execute.load_bundle_tool()
    auth = execute.auth
    errors = (Rejected, experiment.Rejected, paired.Rejected, execute.Rejected,
              OSError, ValueError, SystemExit, KeyError, TypeError)
    started = time.monotonic()
    deadline = started + timeout
    reserve = max(2.0, min(60.0, timeout / 10.0))
    repository = pathlib.Path(__file__).resolve().parents[2]
    scripts = {
        "capacity_experiment": pathlib.Path(__file__).resolve(),
        "experiment_helpers": pathlib.Path(experiment.__file__).resolve(),
        "paired_revalidator": pathlib.Path(paired.__file__).resolve(),
        "execution_launcher": pathlib.Path(execute.__file__).resolve(),
        "semantics_helpers": repository / "scripts/check/run_render_tape_cpu_v4_semantics.py",
        "authentication_helpers": repository / "scripts/check/run_render_tape_v4_capture_diagnostic.py",
        "bundle_tool": repository / "scripts/tools/run_dxmt9_render_tape.py",
        "python_launcher": repository / "scripts/run_python.sh",
    }
    receipt: dict[str, Any] = {
        "schema": SCHEMA, "status": "authenticating", "accepted": False,
        **{name: False for name in experiment.FALSE_CLAIMS},
        "capacity_matrix_complete": False, "candidate_logical_streams_equal": False,
        "replay_policy_matrix_complete": False,
        "paired_qualifications_revalidated": False, "final_qualifications_revalidated": False,
        "executions_reauthenticated": False, "launcher_images_reauthenticated": False,
        "measurement_started": False, "measurement_complete": False,
        "producer_contract": args.producer_contract, "buffer_preservation_contract": args.buffer_preservation_contract,
        "conditional_on_legal_nooverwrite": True, "preservation_contract_is_caller_assumption": True,
        "route": route,
        "arms": [{"arm_id": f"arm-{arm}", "route": route, "qualification_dir": str(path)}
                 for arm, path in enumerate(directories)],
        "same_qualification_control": directories[0] == directories[1],
        "warmup_per_arm": args.warmup, "repeat_per_arm": args.repeat,
        "fresh_process_per_iteration": True, "input_copy_policy": "reuse-arm-0-paired-owned-identical-immutable-input",
        "native_arm_identity_scope": "process-local; aggregate arm_id and run_id identify fresh processes",
        "replay_comparison_scope": f"two-independently-qualified-fixed-input-{route}-replay-policy-arms",
        "acceptance_scope": "bound-original-logical-and-gpu-qualified-observer-light-cpu-cost-comparison",
        "output_dir": str(output), "environment": None, "steps": {}, "iterations": rows,
        "timeout_seconds": timeout, "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "limits": {"rounds_per_arm": experiment.MAX_ROUNDS_PER_ARM,
                   "fresh_children": experiment.MAX_FRESH_CHILDREN,
                   "child_timeout_seconds": experiment.MAX_CHILD_TIMEOUT_SECONDS,
                   "child_receipt_bytes": experiment.MAX_CHILD_RECEIPT_BYTES,
                   "native_result_json_bytes": auth.MAX_RESULT_JSON_BYTES,
                   "child_log_bytes": experiment.MAX_CHILD_LOG_BYTES},
    }
    exit_code = 1
    with runner.bundle_transaction(output, preserve_rejected=True,
            failure_metadata={"schema": SCHEMA, "lane": "v4-capacity-experiment", "accepted": False}) as staging:
        receipt_path = staging / "result.json"
        baseline = contexts = environment = original_work = common = logical_comparison = None
        script_images, child_images = {}, {}
        auth.write_receipt(receipt_path, receipt)
        try:
            environment = experiment.measurement_environment(paired, dict(os.environ))
            receipt["environment"] = environment
            contexts, common, original_work, logical_comparison = _qualify_arms(
                directories, args, experiment, paired, execute, runner, deadline, environment)
            baseline = [experiment._context_binding(context) for context in contexts]
            execution_input = contexts[0]["owned_input"]
            # Freeze initial authority metadata without copying any input file.
            # A later validator/test seam may reuse mutable context objects;
            # it must not rebind a pending sample's already-selected policy.
            execution_contexts = [{**copy.deepcopy(context), "owned_input": execution_input} for context in contexts]
            script_images = {name: auth.fingerprint_file(path, runner, deadline) for name, path in scripts.items()}
            for arm, context in zip(receipt["arms"], contexts):
                arm.update(capacity_policy=_snapshot(context["capacity_policies"][route]),
                    replay_policy=_snapshot(context["replay_policies"][route]),
                    qualified_input=str(context["owned_input"]), qualification_images=_snapshot(context["qualification_images"]),
                    effective_stage_environment=_snapshot(context["cpu_stage_environments"][route]))
            for row in rows:
                arm = receipt["arms"][row["arm_index"]]
                row.update(capacity_policy=arm["capacity_policy"], replay_policy=arm["replay_policy"],
                           qualification_dir=arm["qualification_dir"])
            receipt.update(status="executing", paired_qualifications_revalidated=True,
                candidate_logical_streams_equal=True, logical_comparison=logical_comparison,
                shared_execution_input=str(execution_input), common_binding=common,
                qualified_original_work=original_work, launcher_images=script_images,
                same_capacity_control=receipt["arms"][0]["capacity_policy"] == receipt["arms"][1]["capacity_policy"],
                same_replay_policy_control=receipt["arms"][0]["replay_policy"] == receipt["arms"][1]["replay_policy"])
            auth.write_receipt(receipt_path, receipt)
            for row in rows:
                name, arm = row["run_id"], row["arm_index"]
                child_dir = staging / name
                context = execution_contexts[arm]
                policy = context["capacity_policies"][route]
                replay = context["replay_policies"][route]
                remaining = auth.require_time(deadline) - reserve
                if remaining < 1:
                    raise Rejected("shared capacity experiment deadline exhausted before next fresh process")
                child_timeout = min(experiment.MAX_CHILD_TIMEOUT_SECONDS, remaining)
                child_deadline = min(deadline - reserve, time.monotonic() + child_timeout)
                argv = [str(scripts["python_launcher"]), str(scripts["execution_launcher"]),
                    "--bundle", str(execution_input), "--diagnostic", str(context["cpu_binary"]),
                    "--output-dir", str(child_dir), "--expected-plan-digest", context["plan_sha256"],
                    "--route", route, "--producer-contract", args.producer_contract,
                    "--buffer-preservation-contract", args.buffer_preservation_contract,
                    "--slot-headroom-bytes", str(policy["slot_headroom_bytes"]),
                    "--aggregate-headroom-bytes", str(policy["aggregate_headroom_bytes"]),
                    "--draw-command-limit", str(replay["draw_command_limit"]),
                    "--timeout-seconds", str(child_timeout)]
                receipt["steps"][name] = {"status": "not-started", "argv": None,
                    "stdout": f"{name}.stdout", "stderr": f"{name}.stderr"}
                row["status"] = "running"
                receipt["measurement_started"] |= row["phase"] == "measurement"
                try:
                    try:
                        auth.run_child(name, argv, staging, child_deadline, receipt, receipt_path)
                        paired._completed_step(_object(receipt["steps"], name))
                    finally:
                        paired._validate_child_logs(staging, name)
                    if child_dir.is_symlink():
                        raise Rejected("execution child directory is not an owned artifact")
                    path = child_dir / "result.json"
                    child = paired._read_json(auth, path)
                    evidence = experiment.validate_execution_child(child, context, route, execute, runner,
                        deadline, environment, child_dir, original_work)
                    child_images[str(path)] = auth.fingerprint_file(path, runner, deadline)
                    row.update(evidence, status="complete", accepted=True, receipt_fingerprint=child_images[str(path)])
                except errors as error:
                    row.update(status="rejected", failure=str(error))
                    raise
                finally:
                    auth.write_receipt(receipt_path, receipt)
            exit_code = 0
        except KeyboardInterrupt:
            receipt.update(status="interrupted", failure="capacity experiment interrupted")
            exit_code = 130
        except errors as error:
            receipt.update(status="rejected", failure=str(error))
        finally:
            if baseline is not None:
                try:
                    if experiment.measurement_environment(paired, dict(os.environ)) != environment:
                        raise Rejected("measurement environment changed during capacity experiment")
                    current, current_common, current_work, current_logical = _qualify_arms(
                        directories, args, experiment, paired, execute, runner, deadline, environment)
                    if ([experiment._context_binding(context) for context in current] != baseline
                            or current_common != common or current_work != original_work
                            or current_logical != logical_comparison):
                        raise Rejected("qualification/input/work/policy evidence changed across capacity measurement")
                    receipt["final_qualifications_revalidated"] = True
                    for name, expected in script_images.items():
                        if auth.fingerprint_file(scripts[name], runner, deadline) != expected:
                            raise Rejected("capacity execution/authentication helper image changed")
                    receipt["launcher_images_reauthenticated"] = True
                    for path, expected in child_images.items():
                        if auth.fingerprint_file(pathlib.Path(path), runner, deadline) != expected:
                            raise Rejected("capacity execution child receipt changed across measurement")
                    receipt["executions_reauthenticated"] = len(child_images) == len(rows)
                except errors as error:
                    receipt["reauthentication_failure"] = str(error)
                    receipt.setdefault("failure", str(error))
                    exit_code = 130 if exit_code == 130 else 1
                    receipt["status"] = "interrupted" if exit_code == 130 else "rejected"
            for row in rows:
                if row["status"] == "not-started":
                    row.update(status="skipped", failure="prior qualification/deadline/child failure")
                elif row["status"] == "running":
                    row.update(status="interrupted" if exit_code == 130 else "rejected", failure=receipt.get("failure"))
            accepted = exit_code == 0 and all(receipt[name] is True for name in (
                "paired_qualifications_revalidated", "candidate_logical_streams_equal", "final_qualifications_revalidated",
                "executions_reauthenticated", "launcher_images_reauthenticated")) and all(row["accepted"] for row in rows)
            receipt["accepted"] = receipt["measurement_complete"] = accepted
            if accepted:
                receipt["status"] = "complete"
            elif exit_code == 0:
                exit_code = 1
                receipt.update(status="rejected", failure="required capacity aggregate authentication/execution incomplete")
            receipt["summary"] = experiment.summarize(rows, (route, route), args.repeat, accepted)
            for summary_arm, arm in zip(receipt["summary"]["arms"], receipt["arms"]):
                summary_arm.update(qualification_dir=arm["qualification_dir"], capacity_policy=arm.get("capacity_policy"),
                                   replay_policy=arm.get("replay_policy"))
            receipt["elapsed_seconds"] = time.monotonic() - started
            auth.write_receipt(receipt_path, receipt)
    print(json.dumps({"schema": SCHEMA, "accepted": receipt["accepted"], "status": receipt["status"],
        "receipt": str(output / "result.json"), "comparison_ready": receipt["summary"]["comparison_ready"],
        "failure": receipt.get("failure")}))
    return exit_code


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    for name in ("qualification-a", "qualification-b", "output-dir"):
        result.add_argument(f"--{name}", required=True, type=pathlib.Path)
    result.add_argument("--diagnostic", type=pathlib.Path)
    result.add_argument("--route", choices=("compatibility", "production-slot"), default=ROUTE)
    result.add_argument("--warmup", type=int, default=2)
    result.add_argument("--repeat", type=int, default=16)
    result.add_argument("--timeout-seconds", type=float, default=3600)
    result.add_argument("--producer-contract", required=True, choices=("pe-present-reservation-v1",))
    result.add_argument("--buffer-preservation-contract", required=True,
                        choices=("candidate-legal-nooverwrite-closed-writers-v1",))
    return result


def main() -> int:
    return capacity_experiment(parser().parse_args())


if __name__ == "__main__":
    raise SystemExit(main())
