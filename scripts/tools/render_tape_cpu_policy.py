#!/usr/bin/env python3
"""Policy-bound CPU Render Tape qualification and low-observer measurement.

This module deliberately keeps policy parsing and measurement acceptance
separate from the historical ``cpu_experiment_accepts`` predicate.  A
measurement has no semantic oracle by design; its qualification is only the
receipt binding established by a preceding full-oracle verification.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import statistics
import subprocess
import sys
from typing import Any, Iterable


SCHEMA = "dxmt9.render_tape.cpu_policy.v1"
COMPARE_SCHEMA = "dxmt9.render_tape.cpu_policy_compare.v1"
POLICY_KEYS = frozenset({
    "schema", "replay_route", "draw_command_limit", "slot_headroom_bytes",
    "aggregate_headroom_bytes",
})
ROUTES = frozenset(("compatibility", "production-slot"))
MAX_DRAW_COMMAND_LIMIT = 64
MIN_HEADROOM = 1 << 20
MAX_SLOT_HEADROOM = 64 << 20
MAX_AGGREGATE_HEADROOM = 512 << 20


class PolicyError(ValueError):
    """A malformed or semantically impossible versioned policy."""


def _strict_object(source: bytes | str) -> dict[str, Any]:
    def pairs(items: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, value in items:
            if key in result:
                raise PolicyError(f"duplicate policy key: {key}")
            result[key] = value
        return result

    try:
        value = json.loads(source, object_pairs_hook=pairs)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise PolicyError(f"invalid policy JSON: {error}") from error
    if not isinstance(value, dict):
        raise PolicyError("policy must be a JSON object")
    return value


def validate_policy(value: Any) -> dict[str, Any]:
    """Validate and return a fresh policy object.

    Exact Python types are intentional: JSON booleans are integers in Python,
    but accepting them here would make a forged policy indistinguishable from
    a real capacity value.
    """
    if not isinstance(value, dict):
        raise PolicyError("policy must be a JSON object")
    if set(value) != POLICY_KEYS:
        missing = sorted(POLICY_KEYS - set(value))
        unknown = sorted(set(value) - POLICY_KEYS)
        detail = []
        if missing:
            detail.append(f"missing={','.join(missing)}")
        if unknown:
            detail.append(f"unknown={','.join(unknown)}")
        raise PolicyError("policy keys are not exact: " + " ".join(detail))
    if value["schema"] != SCHEMA or type(value["schema"]) is not str:
        raise PolicyError("unsupported policy schema")
    route = value["replay_route"]
    if type(route) is not str or route not in ROUTES:
        raise PolicyError("replay_route must be compatibility or production-slot")
    draw_limit = value["draw_command_limit"]
    slot = value["slot_headroom_bytes"]
    aggregate = value["aggregate_headroom_bytes"]
    if type(draw_limit) is not int or draw_limit < 0 or (
            draw_limit != 0 and not 3 <= draw_limit <= MAX_DRAW_COMMAND_LIMIT):
        raise PolicyError("draw_command_limit must be 0 or 3..64")
    for name, amount, maximum in (
        ("slot_headroom_bytes", slot, MAX_SLOT_HEADROOM),
        ("aggregate_headroom_bytes", aggregate, MAX_AGGREGATE_HEADROOM),
    ):
        if type(amount) is not int or amount < 0:
            raise PolicyError(f"{name} must be a non-negative integer")
        if amount and not MIN_HEADROOM <= amount <= maximum:
            raise PolicyError(f"{name} must be 0 or within its bounded range")
    if route == "compatibility" and (slot or aggregate):
        raise PolicyError("compatibility policy cannot request production headroom")
    if route == "production-slot" and draw_limit:
        raise PolicyError("production-slot policy cannot request a draw limit")
    if (slot == 0) != (aggregate == 0) or (aggregate and aggregate < slot):
        raise PolicyError("headroom values must both be zero or aggregate >= slot")
    return {key: value[key] for key in (
        "schema", "replay_route", "draw_command_limit", "slot_headroom_bytes",
        "aggregate_headroom_bytes")}


def load_policy(path: pathlib.Path) -> dict[str, Any]:
    try:
        with path.open("rb") as stream:
            source = stream.read(65537)
    except OSError as error:
        raise PolicyError(f"cannot read policy {path}: {error}") from error
    if len(source) > 65536:
        raise PolicyError("policy exceeds 64 KiB")
    return validate_policy(_strict_object(source))


def default_policy(route: str) -> dict[str, Any]:
    if route not in ROUTES:
        raise PolicyError("unknown replay route")
    return validate_policy({
        "schema": SCHEMA, "replay_route": route, "draw_command_limit": 0,
        "slot_headroom_bytes": 0, "aggregate_headroom_bytes": 0,
    })


def canonical_policy(policy: dict[str, Any]) -> bytes:
    return (json.dumps(validate_policy(policy), sort_keys=True,
                        separators=(",", ":")) + "\n").encode()


def policy_digest(policy: dict[str, Any]) -> str:
    return hashlib.sha256(canonical_policy(policy)).hexdigest()


def _evidence(receipt: dict[str, Any]) -> dict[str, Any] | None:
    if not isinstance(receipt, dict):
        return None
    result = receipt.get("result")
    return result if isinstance(result, dict) else receipt


def _integer(value: Any) -> bool:
    return type(value) is int and value >= 0


def _policy_in_receipt(receipt: dict[str, Any], policy: dict[str, Any]) -> bool:
    evidence = _evidence(receipt)
    return bool(evidence and isinstance(evidence.get("cpu_policy"), dict)
                and evidence.get("cpu_policy") == policy)


ARTIFACT_SCHEMA = "dxmt9.render_tape.cpu_experiment_artifact.v1"
NATIVE_SCHEMA = "dxmt9.render_tape.cpu_experiment.v1"


def _is_artifact(receipt: Any) -> bool:
    return (isinstance(receipt, dict) and "result" in receipt
            and receipt.get("schema") == ARTIFACT_SCHEMA)


def _artifact_accepted(receipt: dict[str, Any], *, require_accept: bool = True) -> bool:
    """Require the public child artifact to have passed its image/execution gate."""
    if not _is_artifact(receipt):
        return True
    return ((receipt.get("accepted") is require_accept)
            and receipt.get("semantic_qualified") is require_accept
            and receipt.get("diagnostic") is False
            and receipt.get("metal_validation_diagnostic") is False
            and receipt.get("execution_valid") is True
            and receipt.get("provider_exit_code") == 0
            and receipt.get("loaded_images_stable") is True
            and receipt.get("gpu_evidence") is False
            and receipt.get("compare_wrapper_exit_code", 0) == 0)


def _artifact_identity_present(receipt: dict[str, Any]) -> bool:
    """Check the fields that make an artifact attributable to one execution."""
    if not _is_artifact(receipt):
        return False
    provider = receipt.get("provider")
    validator = receipt.get("validator")
    components = receipt.get("input_components")
    renderer_environment = receipt.get("renderer_environment")
    forced_environment = receipt.get("forced_environment")
    if (not _components_identity(components)
            or not isinstance(provider, dict)
            or not isinstance(validator, dict)
            or not isinstance(renderer_environment, dict)
            or not isinstance(forced_environment, dict)
            or not isinstance(receipt.get("input_inspection"), dict)):
        return False
    for identity in (provider, validator):
        path, digest = identity.get("path"), identity.get("sha256")
        if (not isinstance(path, str) or not path
                or not _sha256(digest)):
            return False
    evidence = _evidence(receipt)
    images = evidence.get("loaded_images") if evidence else None
    if (not isinstance(images, list) or not images
            or any(not _image_identity(image) for image in images)):
        return False
    try:
        if str(pathlib.Path(__file__).resolve().parent) not in sys.path:
            sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
        import run_dxmt9_render_tape as runner
        if not runner.experiment_images_stable(evidence):
            return False
    except (ImportError, AttributeError, OSError, TypeError):
        return False
    return True


def _components_identity(value: Any) -> bool:
    if not isinstance(value, dict) or not value:
        return False
    for name, component in value.items():
        if name == "blobs":
            if not isinstance(component, list):
                return False
            entries = component
        else:
            entries = [component]
        for entry in entries:
            if not isinstance(entry, dict) or not isinstance(entry.get("path"), str):
                return False
            if not _sha256(entry.get("sha256")):
                return False
            if type(entry.get("bytes")) is not int or entry["bytes"] < 0:
                return False
    return True


def _sha256(value: Any) -> bool:
    return (isinstance(value, str) and len(value) == 64
            and all(character in "0123456789abcdef" for character in value))


def _image_identity(value: Any) -> bool:
    if not isinstance(value, dict):
        return False
    return (isinstance(value.get("path"), str) and bool(value["path"])
            and _sha256(value.get("sha256"))
            and type(value.get("bytes")) is int and value["bytes"] >= 0)


def _draw_digests(iteration: dict[str, Any]) -> list[Any] | None:
    observations = iteration.get("draw_observations")
    if not isinstance(observations, list):
        return None
    values: list[Any] = []
    for row in observations:
        if not isinstance(row, dict) or "cumulative_semantic_digest" not in row:
            return None
        values.append(row["cumulative_semantic_digest"])
    return values


def _iterations(receipt: dict[str, Any]) -> list[dict[str, Any]] | None:
    evidence = _evidence(receipt)
    rows = evidence.get("iterations") if evidence else None
    return rows if isinstance(rows, list) and all(isinstance(row, dict) for row in rows) else None


def _conservation(row: dict[str, Any]) -> tuple[Any, ...]:
    return tuple(row.get(name) for name in (
        "draw_count", "resources_created", "resources_released", "reset_ok"))


def _runner_general_accepts(
    result: dict[str, Any], policy: dict[str, Any], *, repeat: int,
    input_inspection: dict[str, Any] | None, measurement: bool,
) -> bool:
    """Delegate full structural validation to the runner when inspection exists.

    The import is intentionally lazy: this module is also used by the runner
    while it is being imported and by native-free checks which have no runner
    dependencies.  A missing/old helper is a rejection, never a reason to
    accept an unqualified execution.
    """
    if input_inspection is None:
        return False
    try:
        if str(pathlib.Path(__file__).resolve().parent) not in sys.path:
            sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
        import run_dxmt9_render_tape as runner
        return runner._cpu_general_frame_accepts(
            result, repeat=repeat, route=policy["replay_route"],
            require_path=False, completion=result.get("completion"),
            resource_backend=result.get("backend"),
            publication=result.get("publication"), fault=result.get("fault"),
            completion_plan=None, input_inspection=input_inspection,
            max_pending=result.get("max_pending"), source_binding=None,
            cpu_policy=policy, measurement=measurement)
    except (ImportError, AttributeError, TypeError, ValueError):
        return False


def _basic_result_shape(
    evidence: dict[str, Any], policy: dict[str, Any], *, repeat: int,
    measurement: bool,
) -> tuple[bool, str, list[dict[str, Any]]]:
    if (evidence.get("schema") != NATIVE_SCHEMA
            or evidence.get("grammar") != "general-frame"
            or evidence.get("backend") != "cpu-metal-resources"
            or evidence.get("resource_execution") != "cpu-visible-metal-no-gpu-submit"
            or evidence.get("status") != "complete"
            or evidence.get("run_mode") != ("measure" if measurement else "verify")
            or evidence.get("replay_route") != policy["replay_route"]
            or evidence.get("completion") != "after-command"
            or evidence.get("publication") != "per-command"
            or evidence.get("fault") != "none"
            or evidence.get("deterministic") is not True
            or evidence.get("immutable_input_owned") is not True
            or evidence.get("shared_native_resource_device") is not True
            or type(evidence.get("repeat")) is not int
            or evidence.get("repeat") != repeat):
        return False, "execution-result-shape", []
    rows = _iterations(evidence)
    if rows is None or len(rows) != repeat:
        return False, "execution-iteration-count", []
    return True, "accepted", rows


def _check_verification_rows(rows: list[dict[str, Any]]) -> tuple[bool, str]:
    for row in rows:
        if (row.get("complete") is not True or row.get("reset_ok") is not True
                or row.get("failure") != "" or row.get("fault_triggered") is not False
                or type(row.get("full_oracle_calls")) is not int
                or row["full_oracle_calls"] <= 0
                or row.get("semantic_digest") in (None, "0")
                or _draw_digests(row) is None
                or row.get("logical_resource_oracle_complete") is not True
                or row.get("physical_texture_oracle") is not False):
            return False, "verification-semantic-observations-missing"
        if (type(row.get("draw_count")) is not int or row["draw_count"] < 0
                or len(row["draw_observations"]) != row["draw_count"]):
            return False, "verification-draw-observations"
        if any(not _integer(row.get(key)) for key in (
                "resources_created", "resources_released", "submitted",
                "completed", "reclaimed")):
            return False, "verification-conservation-fields"
        if (row["resources_created"] != row["resources_released"]
                or not (row["submitted"] == row["completed"] == row["reclaimed"])):
            return False, "verification-conservation"
    return True, "accepted"


def verify_receipt(
    receipt: dict[str, Any], policy: dict[str, Any], *, repeat: int,
    input_inspection: dict[str, Any] | None = None,
) -> tuple[bool, str]:
    """Accept a raw/native or public artifact full-oracle receipt."""
    try:
        policy = validate_policy(policy)
    except PolicyError:
        return False, "invalid-policy"
    if not isinstance(receipt, dict) or not _artifact_accepted(receipt):
        return False, "artifact-rejected"
    evidence = _evidence(receipt)
    if (not isinstance(evidence, dict) or evidence.get("run_mode") != "verify"
            or not _policy_in_receipt(receipt, policy)):
        return False, "verification-mode-or-policy-mismatch"
    if input_inspection is None and _is_artifact(receipt):
        candidate = receipt.get("input_inspection")
        input_inspection = candidate if isinstance(candidate, dict) else None
    valid, reason, rows = _basic_result_shape(
        evidence, policy, repeat=repeat, measurement=False)
    if not valid:
        return False, reason
    if not _runner_general_accepts(
            evidence, policy, repeat=repeat, input_inspection=input_inspection,
            measurement=False):
        return False, "runner-verification-rejected"
    return _check_verification_rows(rows)


def compare_verification(reference: dict[str, Any], candidate: dict[str, Any], *, repeat: int) -> dict[str, Any]:
    failures: list[dict[str, Any]] = []
    refs, cands = _iterations(reference), _iterations(candidate)
    if refs is None or cands is None or len(refs) != len(cands) or len(refs) != repeat:
        failures.append({"name": "verification-iteration-count"})
    else:
        for index, (ref, cand) in enumerate(zip(refs, cands)):
            if ref.get("semantic_digest") != cand.get("semantic_digest"):
                failures.append({"name": "semantic-digest-mismatch", "iteration": index})
            if _draw_digests(ref) != _draw_digests(cand):
                failures.append({"name": "draw-digest-mismatch", "iteration": index})
            if _conservation(ref) != _conservation(cand):
                failures.append({"name": "logical-conservation-mismatch", "iteration": index})
    return {"accepted": not failures, "failures": failures}


def execution_receipt(
    receipt: dict[str, Any], policy: dict[str, Any], *, repeat: int,
    input_inspection: dict[str, Any] | None = None,
) -> tuple[bool, str, list[dict[str, Any]]]:
    """Validate a low-observer execution, without treating it as semantic proof."""
    try:
        policy = validate_policy(policy)
    except PolicyError:
        return False, "invalid-policy", []
    if not isinstance(receipt, dict) or not _artifact_accepted(
            receipt, require_accept=False):
        return False, "artifact-rejected", []
    evidence = _evidence(receipt)
    if (not isinstance(evidence, dict) or evidence.get("run_mode") != "measure"
            or not _policy_in_receipt(receipt, policy)):
        return False, "measurement-mode-or-policy-mismatch", []
    if input_inspection is None and _is_artifact(receipt):
        candidate = receipt.get("input_inspection")
        input_inspection = candidate if isinstance(candidate, dict) else None
    valid, reason, rows = _basic_result_shape(
        evidence, policy, repeat=repeat, measurement=True)
    if not valid:
        return False, reason, []
    # These are deliberately allowed to be absent in the native root object;
    # the per-iteration fields below are the authenticated measurement contract.
    if (evidence.get("full_oracle_calls") not in (None, 0)
            or evidence.get("semantic_digest") not in (None, "0")
            or evidence.get("logical_resource_oracle_complete") not in (None, False)
            or evidence.get("physical_texture_oracle") not in (None, False)
            or evidence.get("semantic_proof") is True
            or evidence.get("oracle_complete") is True):
        return False, "measurement-semantic-proof", []
    if not _runner_general_accepts(
            evidence, policy, repeat=repeat, input_inspection=input_inspection,
            measurement=True):
        return False, "runner-measurement-rejected", []
    topology_fields = (
        "source_summary_commands", "source_summary_draws", "direct_draws",
        "compatibility_draws", "slot_provisioned", "slot_provision_reused",
        "slot_provision_failed", "capacity_lease_denied", "draw_limit_publishes")
    topology = tuple(rows[0].get(field) for field in topology_fields)
    for row in rows:
        if (row.get("complete") is not True or row.get("reset_ok") is not True
                or row.get("failure") != "" or row.get("fault_triggered") is not False
                or row.get("full_oracle_calls") != 0
                or row.get("semantic_digest") != "0"
                or row.get("draw_observations") != []
                or row.get("logical_resource_oracle_complete") is not False
                or row.get("physical_texture_oracle") is not False):
            return False, "measurement-semantic-observation", []
        if tuple(row.get(field) for field in topology_fields) != topology:
            return False, "measurement-topology-nondeterministic", []
        if any(not _integer(row.get(key)) for key in (
                "draw_count", "resources_created", "resources_released",
                "submitted", "completed", "reclaimed")):
            return False, "measurement-conservation-fields", []
        if (row["resources_created"] != row["resources_released"]
                or not (row["submitted"] == row["completed"] == row["reclaimed"])):
            return False, "measurement-conservation", []
    return True, "accepted", rows


def measure_receipt(
    receipt: dict[str, Any], policy: dict[str, Any], *, total: int, warmup: int,
    verification: dict[str, Any],
    input_inspection: dict[str, Any] | None = None,
) -> tuple[bool, str, list[dict[str, Any]]]:
    """Bind and slice a measurement after independently qualifying verification."""
    if warmup < 0 or total < 1 or warmup >= total:
        return False, "measurement-warmup-range", []
    verified, reason = verify_receipt(
        verification, policy, repeat=len(_iterations(verification) or []),
        input_inspection=input_inspection)
    if not verified:
        return False, f"verification-not-qualified:{reason}", []
    accepted, reason, rows = execution_receipt(
        receipt, policy, repeat=total, input_inspection=input_inspection)
    if not accepted:
        return False, reason, []
    # Warmups are wrapper-owned.  Native rows intentionally have no warmup
    # marker, so this slice is the only source of measured-row qualification.
    measured = rows[warmup:]
    if not measured:
        return False, "measurement-warmup-range", []
    verify_rows = _iterations(verification) or []
    topology_fields = (
        "source_summary_commands", "source_summary_draws", "direct_draws",
        "compatibility_draws", "slot_provisioned", "slot_provision_reused",
        "slot_provision_failed", "capacity_lease_denied", "draw_limit_publishes",
        "submitted", "completed", "reclaimed")
    for index, row in enumerate(rows):
        source = verify_rows[index % len(verify_rows)]
        for key in ("draw_count", "resources_created", "resources_released",
                    *topology_fields):
            if row.get(key) != source.get(key):
                return False, f"measurement-qualified-count-mismatch:{key}", []
    if _is_artifact(receipt) or _is_artifact(verification):
        if not bindings_match(verification, receipt, policy):
            return False, "measurement-qualification-binding-mismatch", []
    return True, "accepted", measured


def median(values: Iterable[int]) -> int:
    values = list(values)
    if not values:
        raise ValueError("median requires at least one value")
    return int(statistics.median(values))


def measurement_summary(receipt: dict[str, Any], rows: list[dict[str, Any]]) -> dict[str, Any]:
    walls = [row["elapsed_ns"] for row in rows if _integer(row.get("elapsed_ns"))]
    cpus = [row["native_cpu_ns"] for row in rows if _integer(row.get("native_cpu_ns"))]
    return {
        "iterations": len(rows),
        "median_wall_ns": median(walls) if len(walls) == len(rows) else None,
        "median_native_cpu_ns": median(cpus) if len(cpus) == len(rows) else None,
        "fps": None,
    }


def receipt_file_hash(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _binding(receipt: dict[str, Any], policy: dict[str, Any]) -> dict[str, Any]:
    evidence = _evidence(receipt) or {}
    return {
        "input_components": receipt.get("input_components"),
        "provider": receipt.get("provider"), "validator": receipt.get("validator"),
        "loaded_images": evidence.get("loaded_images"),
        "renderer_environment": receipt.get("renderer_environment"),
        "forced_environment": receipt.get("forced_environment"),
        "backend": evidence.get("backend"), "reset_policy": evidence.get("reset_policy"),
        "cpu_policy": policy,
    }


def bindings_match(reference: dict[str, Any], measurement: dict[str, Any], policy: dict[str, Any]) -> bool:
    try:
        policy = validate_policy(policy)
    except PolicyError:
        return False
    if (not _artifact_accepted(reference)
            or not _artifact_accepted(measurement, require_accept=False)
            or not _artifact_identity_present(reference)
            or not _artifact_identity_present(measurement)):
        return False
    if not (_policy_in_receipt(reference, policy) and _policy_in_receipt(measurement, policy)):
        return False
    reference_binding = _binding(reference, policy)
    measurement_binding = _binding(measurement, policy)
    required = (
        "input_components", "provider", "validator", "loaded_images",
        "renderer_environment", "forced_environment", "backend",
        "reset_policy", "cpu_policy")
    if any(reference_binding.get(key) is None
           or measurement_binding.get(key) is None for key in required):
        return False
    return reference_binding == measurement_binding


def _policy_pair_bindings_match(
    reference: dict[str, Any], candidate: dict[str, Any]
) -> bool:
    """Require one input/provider/validator/environment for both policies."""
    if (not _artifact_identity_present(reference)
            or not _artifact_identity_present(candidate)):
        return False
    left, right = _binding(reference, _evidence(reference).get("cpu_policy")), _binding(
        candidate, _evidence(candidate).get("cpu_policy"))
    for binding in (left, right):
        policy = binding["cpu_policy"]
        expected_policy_environment = {
            "DXMT9_DRAW_CHUNK_COMMAND_LIMIT": str(policy["draw_command_limit"]),
            "DXMT9_DIRECT_SLOT_HEADROOM_BYTES": str(policy["slot_headroom_bytes"]),
            "DXMT9_DIRECT_SLOT_AGGREGATE_HEADROOM_BYTES": str(
                policy["aggregate_headroom_bytes"]),
        }
        for key, value in expected_policy_environment.items():
            if (key not in binding["forced_environment"]
                    or binding["forced_environment"][key] != value):
                return False
    for key in ("input_components", "provider", "validator", "loaded_images",
                "renderer_environment", "backend", "reset_policy"):
        if left.get(key) != right.get(key):
            return False
    allowed = {"DXMT9_DRAW_CHUNK_COMMAND_LIMIT",
               "DXMT9_DIRECT_SLOT_HEADROOM_BYTES",
               "DXMT9_DIRECT_SLOT_AGGREGATE_HEADROOM_BYTES"}
    left_env = {key: value for key, value in left["forced_environment"].items()
                if key not in allowed}
    right_env = {key: value for key, value in right["forced_environment"].items()
                 if key not in allowed}
    return left_env == right_env


def _final_identity_check(
    runner: Any, staged: pathlib.Path, provider: pathlib.Path,
    validator: pathlib.Path, receipts: Iterable[dict[str, Any]],
) -> bool:
    """Re-authenticate immutable inputs and executable/image hashes at commit."""
    try:
        manifest, _, _, _, _, _ = runner.load_bundle(staged)
        provider_digest = runner.digest(provider.resolve())
        validator_digest = runner.digest(validator.resolve())
    except (OSError, ValueError, TypeError):
        return False
    for receipt in receipts:
        if (receipt.get("input_components") != manifest.get("components")
                or receipt.get("provider", {}).get("sha256") != provider_digest
                or receipt.get("validator", {}).get("sha256") != validator_digest
                or not _artifact_identity_present(receipt)):
            return False
    return True


def _runner_path() -> pathlib.Path:
    return pathlib.Path(__file__).resolve().with_name("run_dxmt9_render_tape.py")


def _child(command: list[str], timeout: int) -> dict[str, Any]:
    output_dir = pathlib.Path(command[command.index("--output-dir") + 1])
    output_dir.parent.mkdir(parents=True, exist_ok=True)
    stdout: str | bytes = ""
    stderr: str | bytes = ""
    exit_code: int | None = None
    timed_out = False
    try:
        completed = subprocess.run(
            command, check=False, capture_output=True, text=True, timeout=timeout)
        stdout, stderr, exit_code = completed.stdout, completed.stderr, completed.returncode
    except subprocess.TimeoutExpired as error:
        timed_out = True
        stdout, stderr = error.stdout or b"", error.stderr or b""
    output_dir.mkdir(parents=True, exist_ok=True)
    for name, contents in (("command.stdout", stdout), ("command.stderr", stderr)):
        (output_dir / name).write_bytes(
            contents if isinstance(contents, bytes) else contents.encode())
    output = output_dir / "result.json"
    try:
        value = json.loads(output.read_text(encoding="utf-8"))
        if not isinstance(value, dict):
            raise ValueError("child receipt is not an object")
    except (OSError, json.JSONDecodeError, ValueError):
        value = {
            "schema": ARTIFACT_SCHEMA,
            "accepted": False,
            "execution_valid": False,
            "provider_exit_code": exit_code,
            "failure_receipt": True,
            "timeout": timed_out,
            "result": {"status": "timeout" if timed_out else "missing-child-receipt"},
        }
        output.write_text(json.dumps(value, sort_keys=True, separators=(",", ":"))
                           + "\n", encoding="utf-8")
    value["compare_wrapper_exit_code"] = exit_code
    value["compare_wrapper_timeout"] = timed_out
    output.write_text(json.dumps(value, sort_keys=True, separators=(",", ":"))
                       + "\n", encoding="utf-8")
    return value


def cpu_policy_compare(args: argparse.Namespace) -> int:
    # Policies are parsed before touching the requested output directory.
    reference_policy = load_policy(args.reference_policy)
    candidate_policy = load_policy(args.candidate_policy)
    if args.repeat < 1 or args.warmup < 0 or args.repeat + args.warmup > 32 or args.timeout_seconds <= 0:
        raise SystemExit("repeat/warmup must be non-negative and total at most 32")
    root = pathlib.Path(__file__).resolve().parents[2]
    launcher = root / "scripts" / "run_python.sh"
    runner = _runner_path()
    output = args.output_dir.resolve()
    if str(runner.parent) not in sys.path:
        sys.path.insert(0, str(runner.parent))
    try:
        import run_dxmt9_render_tape as runner_module
        with runner_module.bundle_transaction(
                output, preserve_rejected=True,
                failure_metadata={"operation": "cpu-policy-compare"}) as staging:
            staged = staging / "input"
            runner_module.copy_cpu_input_owned(args.bundle.resolve(), staged)
            verify: dict[str, dict[str, Any]] = {}
            policies = (("reference", reference_policy), ("candidate", candidate_policy))
            for name, policy in policies:
                runner_module.write_flushed(staging / (name + "-policy.json"),
                                            canonical_policy(policy))
            for name, policy in policies:
                child = staging / "verify" / name
                command = [str(launcher), str(runner), "cpu-experiment", str(staged),
                           "--provider", str(args.provider), "--validator", str(args.validator),
                           "--grammar", "general-frame", "--resource-backend",
                           "cpu-metal-resources", "--completion", "after-command",
                           "--max-pending", "1", "--publication", "per-command",
                           "--run-mode", "verify", "--policy-lane", "--repeat", "2",
                           "--replay-route", policy["replay_route"], "--draw-command-limit",
                           str(policy["draw_command_limit"]), "--slot-headroom-bytes",
                           str(policy["slot_headroom_bytes"]), "--aggregate-headroom-bytes",
                           str(policy["aggregate_headroom_bytes"]), "--output-dir", str(child),
                           "--timeout-seconds", str(args.timeout_seconds)]
                verify[name] = _child(command, args.timeout_seconds + 5)
            verify_checks = {name: verify_receipt(verify[name], policy, repeat=2)
                             for name, policy in policies}
            valid = all(check[0] for check in verify_checks.values())
            comparison = (compare_verification(verify["reference"], verify["candidate"], repeat=2)
                          if valid else {"accepted": False,
                                        "failures": [{"name": "verification-rejected"}]})
            pair_bound = _policy_pair_bindings_match(verify["reference"], verify["candidate"])
            measurements: dict[str, Any] = {}
            if valid and comparison["accepted"] and pair_bound:
                for name, policy in policies:
                    child = staging / "measure" / name
                    command = [str(launcher), str(runner), "cpu-experiment", str(staged),
                               "--provider", str(args.provider), "--validator", str(args.validator),
                               "--grammar", "general-frame", "--resource-backend",
                               "cpu-metal-resources", "--completion", "after-command",
                               "--max-pending", "1", "--publication", "per-command",
                               "--run-mode", "measure", "--policy-lane", "--repeat",
                               str(args.repeat + args.warmup), "--replay-route",
                               policy["replay_route"], "--draw-command-limit",
                               str(policy["draw_command_limit"]), "--slot-headroom-bytes",
                               str(policy["slot_headroom_bytes"]), "--aggregate-headroom-bytes",
                               str(policy["aggregate_headroom_bytes"]), "--output-dir", str(child),
                               "--timeout-seconds", str(args.timeout_seconds)]
                    measurements[name] = _child(command, args.timeout_seconds + 5)
            qualifications: dict[str, tuple[bool, str, list[dict[str, Any]]]] = {}
            for name, policy in policies:
                if name in measurements:
                    qualifications[name] = measure_receipt(
                        measurements[name], policy,
                        total=args.repeat + args.warmup, warmup=args.warmup,
                        verification=verify[name])
            identity_bound = (pair_bound and _final_identity_check(
                    runner_module, staged, args.provider, args.validator,
                    [*verify.values(), *measurements.values()]))
            accepted = bool(valid and comparison["accepted"] and identity_bound and all(
                name in qualifications and qualifications[name][0]
                and bindings_match(verify[name], measurements[name], policy)
                for name, policy in policies))
            failures = [dict(name=f"{name}-verification", reason=check[1])
                        for name, check in verify_checks.items() if not check[0]]
            failures.extend(comparison["failures"])
            if not identity_bound:
                failures.append({"name": "input-provider-image-binding"})
            for name, check in qualifications.items():
                if not check[0]:
                    failures.append({"name": f"{name}-measurement", "reason": check[1]})
            report = {
                "schema": COMPARE_SCHEMA,
                "accepted": accepted,
                "failures": failures,
                "identity_bound": identity_bound,
                "input_components": verify["reference"].get("input_components"),
                "warmup": args.warmup,
                "repeat": args.repeat,
                "semantic_proof": False,
                "gpu_evidence": False,
                "reference_policy": reference_policy,
                "candidate_policy": candidate_policy,
                "verification": {
                    "reference": {"sha256": receipt_file_hash(
                        staging / "verify" / "reference" / "result.json"),
                                   "accepted": verify_checks["reference"][0]},
                    "candidate": {"sha256": receipt_file_hash(
                        staging / "verify" / "candidate" / "result.json"),
                                   "accepted": verify_checks["candidate"][0]},
                    "comparison": comparison},
                "measurement": {
                    name: {"accepted": accepted and qualifications[name][0],
                           "reason": qualifications[name][1],
                           "qualification_receipt": f"verify/{name}/result.json",
                           "qualification_sha256": receipt_file_hash(
                               staging / "verify" / name / "result.json"),
                           "receipt": f"measure/{name}/result.json",
                           "sha256": receipt_file_hash(staging / "measure" / name / "result.json"),
                           "measured_indices": list(range(args.warmup, args.warmup + args.repeat)),
                           **measurement_summary(measurements[name], qualifications[name][2]
                                                 if accepted else [])}
                    for name in qualifications},
                "input": str(output / "input"),
            }
            (staging / "result.json").write_text(
                json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
            print(json.dumps({"accepted": accepted, "output": str(output)}, sort_keys=True))
            return 0 if accepted else 1
    except BaseException:
        raise


def parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=pathlib.Path)
    parser.add_argument("--reference-policy", type=pathlib.Path, required=True)
    parser.add_argument("--candidate-policy", type=pathlib.Path, required=True)
    parser.add_argument("--repeat", type=int, default=16)
    parser.add_argument("--warmup", type=int, default=2)
    parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    parser.add_argument("--provider", type=pathlib.Path, default=pathlib.Path(
        "build/tests/native/backend/dxmt9-cpu-ready-production-routing-spec"))
    parser.add_argument("--validator", type=pathlib.Path, default=pathlib.Path(
        "build/tools/dxmt9-render-tape"))
    parser.add_argument("--timeout-seconds", type=int, default=30)
    return parser


if __name__ == "__main__":
    raise SystemExit(cpu_policy_compare(parser().parse_args()))
