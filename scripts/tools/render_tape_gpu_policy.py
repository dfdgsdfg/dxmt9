#!/usr/bin/env python3
"""Strict GPU qualification for the canonical Render Tape CPU policy pair.

The GPU lane is deliberately a qualification gate, not a timing oracle.  It
reuses the production ``experiment`` subprocess and readback artifacts, then
adds the stronger binding required for policy promotion: both original RGBA
oracles must be authenticated, every interval must use strict oracle mode,
and every readback byte must match those original bytes exactly.
"""

from __future__ import annotations

import hashlib
import json
import pathlib
import subprocess
from typing import Any, Callable

try:
    import render_tape_cpu_policy
except ModuleNotFoundError:
    import sys
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
    import render_tape_cpu_policy


SCHEMA = "dxmt9.render_tape.gpu_policy_qualification.v1"
ARTIFACT_SCHEMA = "dxmt9.render_tape.experiment_artifact.v1"
POLICY_ENVIRONMENT = {
    "DXMT9_DRAW_CHUNK_COMMAND_LIMIT": "draw_command_limit",
    "DXMT9_DIRECT_SLOT_HEADROOM_BYTES": "slot_headroom_bytes",
    "DXMT9_DIRECT_SLOT_AGGREGATE_HEADROOM_BYTES": "aggregate_headroom_bytes",
}
QUALIFICATION_SCOPE = "production-capture"  # or synthetic-test-input


def texture_input_audit_is_default(owner: dict[str, Any]) -> bool:
    """Reject intervention evidence even when its request flag is forged off."""
    if "texture_input_audit" not in owner:
        return True
    audit = owner["texture_input_audit"]
    if not isinstance(audit, dict):
        return False
    return (all(audit.get(key) is False for key in (
                "requested", "validated", "attempted", "uploaded", "readback_matched"))
            and all(type(audit.get(key)) is int and audit[key] == 0 for key in (
                "kind", "generation", "object_id", "after_captured_seq", "bytes"))
            and all(audit.get(key) == "0" * 64 for key in (
                "input_sha256", "before_sha256", "after_sha256")))


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _digest(path: pathlib.Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def _files_equal(left: pathlib.Path, right: pathlib.Path) -> bool:
    try:
        if left.stat().st_size != right.stat().st_size:
            return False
        with left.open("rb") as left_stream, right.open("rb") as right_stream:
            while True:
                left_block = left_stream.read(1024 * 1024)
                right_block = right_stream.read(1024 * 1024)
                if left_block != right_block:
                    return False
                if not left_block:
                    return True
    except OSError:
        return False


def _valid_digest(value: Any) -> bool:
    return (isinstance(value, str) and len(value) == 64
            and all(char in "0123456789abcdef" for char in value))


def _evidence(receipt: dict[str, Any]) -> dict[str, Any]:
    result = receipt.get("result")
    return result if isinstance(result, dict) else receipt


def _empty_optional_batch_cuts(value: dict[str, Any]) -> bool:
    """Accept only an omitted or explicitly empty batch-cut request.

    Batch cuts are a perturbative source-boundary diagnostic.  An artifact
    cannot become qualifying evidence by hiding a nonempty or malformed
    request behind a false diagnostic flag.
    """
    return ("audit_batch_cuts" not in value or
            (type(value["audit_batch_cuts"]) is list and
             not value["audit_batch_cuts"]))


def policy_environment(policy: dict[str, Any]) -> dict[str, str]:
    policy = render_tape_cpu_policy.validate_policy(policy)
    return {name: str(policy[field]) for name, field in POLICY_ENVIRONMENT.items()}


def _oracle_scope(manifest: dict[str, Any]) -> tuple[str | None, str]:
    scope = manifest.get("scope")
    if not isinstance(scope, dict):
        return None, "missing-scope"
    if scope.get("production_capture") is True:
        return "production-capture", "accepted"
    if scope.get("synthetic") is True or scope.get("test_input") is True:
        return "synthetic-test-input", "accepted"
    return None, "input-is-not-production-capture-or-explicit-synthetic-fixture"


def original_oracles(
    manifest: dict[str, Any], bundle: pathlib.Path,
) -> tuple[dict[str, dict[str, Any]] | None, str | None, str]:
    """Return authenticated original output/source metadata and their scope."""
    scope, reason = _oracle_scope(manifest)
    if scope is None:
        return None, None, reason
    components = manifest.get("components")
    if not isinstance(components, dict):
        return None, None, "missing-components"
    oracles: dict[str, dict[str, Any]] = {}
    for kind, filename in (("output", "output.rgba"), ("source", "source.rgba")):
        component = components.get(f"{kind}_oracle")
        if not isinstance(component, dict) or component.get("path") != filename:
            return None, None, f"missing-original-{kind}-oracle"
        path = bundle / filename
        try:
            root = bundle.resolve(strict=True)
            resolved = path.resolve(strict=True)
            stat = path.stat()
        except OSError:
            return None, None, f"missing-original-{kind}-oracle"
        declared_bytes = component.get("bytes")
        if (path.is_symlink() or not path.is_file() or resolved.parent != root
                or type(declared_bytes) is not int or declared_bytes <= 0
                or stat.st_size != declared_bytes
                or component.get("sha256") != _digest(path)):
            return None, None, f"tampered-original-{kind}-oracle"
        oracles[kind] = {"path": path, "bytes": declared_bytes,
                         "sha256": component["sha256"]}
    return oracles, scope, "accepted"


def _strict_interval_matches(
    interval: dict[str, Any], readbacks: dict[str, Any],
    readback_root: pathlib.Path, originals: dict[str, dict[str, Any]],
) -> tuple[bool, str]:
    validity = interval.get("validity")
    if not isinstance(validity, dict):
        return False, "missing-interval-validity"
    if validity.get("oracle_mode") != "strict":
        return False, "pixel-envelope-or-nonstrict-oracle"
    if validity.get("expected_digest_matched") is not True:
        return False, "expected-digest-not-matched"
    if validity.get("output_readback") is not True or validity.get("source_readback") is not True:
        return False, "readback-not-authenticated"
    for kind in ("output", "source"):
        digest_key = f"{kind}_sha256"
        if validity.get(digest_key) != originals[kind]["sha256"]:
            return False, f"{kind}-original-oracle-mismatch"
        artifact = readbacks.get(f"{kind}_rgba")
        if not isinstance(artifact, dict):
            return False, f"missing-{kind}-readback-artifact"
        name = artifact.get("path")
        claimed_bytes, claimed_digest = artifact.get("bytes"), artifact.get("sha256")
        if (not isinstance(name, str) or pathlib.Path(name).name != name
                or not isinstance(claimed_bytes, int) or claimed_bytes < 0
                or not _valid_digest(claimed_digest)):
            return False, f"invalid-{kind}-readback-artifact"
        path = readback_root / name
        try:
            root = readback_root.resolve(strict=True)
            resolved = path.resolve(strict=True)
            actual_size = path.stat().st_size
        except OSError:
            return False, f"missing-{kind}-readback-artifact"
        original_path = pathlib.Path(originals[kind]["path"])
        if (path.is_symlink() or not path.is_file() or resolved.parent != root
                or actual_size != claimed_bytes or _digest(path) != claimed_digest
                or claimed_bytes != originals[kind]["bytes"]
                or claimed_digest != originals[kind]["sha256"]
                or not _files_equal(path, original_path)):
            return False, f"tampered-{kind}-readback-artifact"
    return True, "accepted"


def gpu_receipt_accepts(
    receipt: dict[str, Any], policy: dict[str, Any], *,
    manifest: dict[str, Any], bundle: pathlib.Path,
    readback_root: pathlib.Path, warmup: int = 0, repeat: int = 2,
    reset: str = "cold-device", require_path: bool = True,
    runner: Any | None = None,
) -> tuple[bool, str]:
    """Shared acceptance predicate for real GPU child receipts."""
    try:
        policy = render_tape_cpu_policy.validate_policy(policy)
    except render_tape_cpu_policy.PolicyError:
        return False, "invalid-policy"
    originals, scope, reason = original_oracles(manifest, bundle)
    if originals is None or scope is None:
        return False, reason
    if not isinstance(receipt, dict) or receipt.get("schema") != ARTIFACT_SCHEMA:
        return False, "invalid-gpu-artifact"
    if (receipt.get("accepted") is not True or receipt.get("provider_exit_code") != 0
            or receipt.get("compare_wrapper_exit_code") != 0
            or receipt.get("compare_wrapper_timeout") is not False
            or receipt.get("diagnostic") is not False
            or receipt.get("audit_source_boundaries", False) is not False
            or receipt.get("audit_texture_input", False) is not False
            or not _empty_optional_batch_cuts(receipt)
            or receipt.get("metal_validation_diagnostic") is not False
            or receipt.get("loaded_images_stable") is not True
            or receipt.get("readback_artifacts_valid") is not True):
        return False, "gpu-artifact-rejected"
    if receipt.get("input_components") != manifest.get("components"):
        return False, "gpu-input-identity-mismatch"
    renderer_environment = receipt.get("renderer_environment")
    if (not isinstance(renderer_environment, dict)
            or any(not isinstance(key, str) or not isinstance(value, str)
                   for key, value in renderer_environment.items())):
        return False, "gpu-renderer-environment-missing-or-invalid"
    forced = receipt.get("forced_environment")
    if not isinstance(forced, dict):
        return False, "gpu-policy-environment-missing"
    expected_environment = policy_environment(policy)
    if any(forced.get(key) != value for key, value in expected_environment.items()):
        return False, "gpu-policy-environment-mismatch"
    requested = receipt.get("requested")
    requested_policy = requested.get("cpu_policy") if isinstance(requested, dict) else None
    requested_digest = requested.get("cpu_policy_sha256") if isinstance(requested, dict) else None
    if (not isinstance(requested, dict)
            or requested.get("replay_route") != policy["replay_route"]
            or requested.get("partition_mode") != "identity"
            or requested.get("warmup") != warmup
            or requested.get("repeat") != repeat
            or requested.get("reset") != reset
            or requested.get("require_path") is not require_path):
        return False, "gpu-request-policy-mismatch"
    if not _empty_optional_batch_cuts(requested):
        return False, "gpu-source-boundary-batch-cuts"
    if requested.get("audit_source_boundaries", False) is not False:
        return False, "gpu-source-boundary-diagnostic"
    if requested.get("audit_texture_input") is not None:
        return False, "gpu-texture-input-intervention"
    if (requested_policy != policy
            or requested_digest != render_tape_cpu_policy.policy_digest(policy)):
        return False, "gpu-canonical-policy-mismatch"
    expected_mode = policy.get("source_boundary_mode", "chunk")
    if (receipt.get("source_boundary_mode", "chunk") != expected_mode
            or requested.get("source_boundary_mode", "chunk") != expected_mode
            or requested.get("source_plan_sha256") != policy.get("source_plan_sha256")):
        return False, "gpu-source-policy-request-mismatch"
    evidence = _evidence(receipt)
    if evidence.get("completion") != "real-gpu":
        return False, "gpu-completion-not-real"
    if runner is None:
        try:
            import run_dxmt9_render_tape as runner
        except ImportError:
            return False, "runner-unavailable"
    if runner.experiment_inline_observer_enabled(renderer_environment):
        return False, "gpu-inline-observer-diagnostic"
    if not runner.experiment_images_stable(evidence):
        return False, "gpu-loaded-image-authentication-failed"
    if not runner.experiment_readbacks_valid(evidence, readback_root):
        return False, "gpu-readback-artifacts-invalid"
    if not runner.experiment_accepts(
            evidence, warmup=warmup, repeat=repeat, reset=reset,
            route=policy["replay_route"], require_path=require_path,
            non_degenerate=True, cpu_policy=policy,
            input_components=manifest.get("components")):
        return False, "gpu-experiment-rejected"
    iterations = evidence.get("iterations")
    if not isinstance(iterations, list) or len(iterations) != warmup + repeat:
        return False, "gpu-iteration-count"
    for iteration in iterations:
        if not isinstance(iteration, dict):
            return False, "gpu-iteration-invalid"
        provider_result = iteration.get("provider_result")
        artifacts = iteration.get("readback_artifacts")
        if not isinstance(provider_result, dict) or not isinstance(artifacts, dict):
            return False, "gpu-interval-evidence-missing"
        if not texture_input_audit_is_default(provider_result):
            return False, "gpu-texture-input-intervention"
        provider_validity = provider_result.get("validity")
        if (provider_result.get("profile") != "frame-tape"):
            return False, "gpu-sequence-profile-unsupported"
        if (not isinstance(provider_validity, dict)
                or provider_validity.get("oracle_mode") != "strict"
                or provider_validity.get("expected_digest_matched") is not True
                or provider_validity.get("source_oracle_exact_matched") is not True
                or provider_validity.get("source_oracle_matched") is not True):
            return False, "gpu-top-level-strict-source-oracle-missing"
        intervals = provider_result.get("intervals")
        if not isinstance(intervals, list) or len(intervals) != 1:
            return False, "gpu-sequence-interval-unsupported"
        accepted, interval_reason = _strict_interval_matches(
            intervals[0], artifacts, readback_root, originals)
        if not accepted:
            return False, interval_reason
        counters = iteration.get("path_counters")
        if not isinstance(counters, dict) or counters.get("gpu_errors") != 0:
            return False, "gpu-command-buffer-error"
    return True, "accepted"


def cpu_gpu_binding_matches(
    cpu_receipt: dict[str, Any], gpu_receipt: dict[str, Any],
    policy: dict[str, Any],
) -> tuple[bool, str]:
    """Bind GPU evidence to the already-qualified CPU child fingerprint."""
    try:
        policy = render_tape_cpu_policy.validate_policy(policy)
    except render_tape_cpu_policy.PolicyError:
        return False, "invalid-policy"
    cpu_result = _evidence(cpu_receipt)
    gpu_result = _evidence(gpu_receipt)
    def module_images(receipt: dict[str, Any], result: dict[str, Any]) -> Any:
        images = result.get("loaded_images")
        provider_path = receipt.get("provider", {}).get("path")
        if not isinstance(images, list):
            return None
        return [image for image in images
                if isinstance(image, dict) and image.get("path") != provider_path]
    policy_keys = set(POLICY_ENVIRONMENT)
    cpu_renderer = cpu_receipt.get("renderer_environment")
    gpu_renderer = gpu_receipt.get("renderer_environment")
    if isinstance(cpu_renderer, dict):
        cpu_renderer = {key: value for key, value in cpu_renderer.items()
                        if key not in policy_keys}
    if isinstance(gpu_renderer, dict):
        gpu_renderer = {key: value for key, value in gpu_renderer.items()
                        if key not in policy_keys}
    if (not isinstance(cpu_result, dict) or not isinstance(gpu_result, dict)
            or cpu_receipt.get("input_components") != gpu_receipt.get("input_components")
            or cpu_receipt.get("validator") != gpu_receipt.get("validator")
            or cpu_renderer != gpu_renderer
            or module_images(cpu_receipt, cpu_result) != module_images(gpu_receipt, gpu_result)):
        return False, "cpu-gpu-fingerprint-mismatch"
    if cpu_result.get("cpu_policy") != policy:
        return False, "cpu-policy-binding-mismatch"
    requested = gpu_receipt.get("requested")
    if (not isinstance(requested, dict) or requested.get("cpu_policy") != policy
            or requested.get("cpu_policy_sha256") != render_tape_cpu_policy.policy_digest(policy)):
        return False, "gpu-policy-binding-mismatch"
    if policy["schema"] == render_tape_cpu_policy.SOURCE_SCHEMA:
        import render_tape_source_policy
        components = cpu_receipt.get("input_components")
        for result, gpu in ((cpu_result, False), (gpu_result, True)):
            iterations = result.get("iterations")
            if not isinstance(iterations, list) or not iterations:
                return False, "cpu-gpu-source-iterations-missing"
            for iteration in iterations:
                owner = iteration.get("provider_result") if gpu else iteration
                if (not isinstance(owner, dict) or
                        not render_tape_source_policy.source_replay_accepts(
                            owner.get("source_replay"), policy, components)):
                    return False, "cpu-gpu-source-settlement-mismatch"
    expected = policy_environment(policy)
    cpu_forced = cpu_receipt.get("forced_environment")
    gpu_forced = gpu_receipt.get("forced_environment")
    if (not isinstance(cpu_forced, dict) or not isinstance(gpu_forced, dict)
            or any(cpu_forced.get(key) != value or gpu_forced.get(key) != value
                   for key, value in expected.items())):
        return False, "cpu-gpu-policy-environment-mismatch"
    return True, "accepted"


def _gpu_pair_fingerprint(receipt: dict[str, Any]) -> tuple[Any, ...]:
    """Fingerprint shared GPU identities while excluding the three policy knobs."""
    renderer = receipt.get("renderer_environment")
    if isinstance(renderer, dict):
        renderer = {key: value for key, value in renderer.items()
                    if key not in POLICY_ENVIRONMENT}
    forced = receipt.get("forced_environment")
    if isinstance(forced, dict):
        forced = {key: value for key, value in forced.items()
                  if key not in POLICY_ENVIRONMENT}
    evidence = _evidence(receipt)
    return (
        receipt.get("input_components"), receipt.get("provider"),
        receipt.get("validator"), renderer, forced, evidence.get("loaded_images"),
    )


def _child(command: list[str], timeout: int) -> dict[str, Any]:
    output_dir = pathlib.Path(command[command.index("--output-dir") + 1])
    output_dir.parent.mkdir(parents=True, exist_ok=True)
    try:
        completed = subprocess.run(command, check=False, capture_output=True,
                                   text=True, timeout=timeout)
        stdout, stderr, exit_code = completed.stdout, completed.stderr, completed.returncode
    except subprocess.TimeoutExpired as error:
        stdout, stderr, exit_code = error.stdout or b"", error.stderr or b"", None
    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "command.stdout").write_bytes(
        stdout if isinstance(stdout, bytes) else stdout.encode())
    (output_dir / "command.stderr").write_bytes(
        stderr if isinstance(stderr, bytes) else stderr.encode())
    try:
        value = json.loads((output_dir / "result.json").read_text(encoding="utf-8"))
        if not isinstance(value, dict):
            raise ValueError("child receipt is not an object")
    except (OSError, json.JSONDecodeError, ValueError):
        value = {"schema": ARTIFACT_SCHEMA, "accepted": False,
                 "provider_exit_code": exit_code, "failure_receipt": True}
        (output_dir / "result.json").write_text(
            json.dumps(value, sort_keys=True) + "\n", encoding="utf-8")
    value["compare_wrapper_exit_code"] = exit_code
    value["compare_wrapper_timeout"] = exit_code is None
    (output_dir / "result.json").write_text(
        json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n",
        encoding="utf-8")
    return value


def qualify_policy_pair(
    *, input_bundle: pathlib.Path, staging: pathlib.Path,
    provider: pathlib.Path, validator: pathlib.Path,
    reference_policy: dict[str, Any], candidate_policy: dict[str, Any],
    timeout_seconds: int, reset: str = "cold-device",
    cpu_verification: dict[str, dict[str, Any]] | None = None,
    expected_provider_sha256: str | None = None,
    expected_validator_sha256: str | None = None,
    child_runner: Callable[[list[str], int], dict[str, Any]] | None = None,
) -> dict[str, Any]:
    """Run one fresh real-Metal child for each policy and return evidence."""
    import sys
    if str(pathlib.Path(__file__).resolve().parent) not in sys.path:
        sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
    import run_dxmt9_render_tape as runner
    manifest, _, _, _, _, _ = runner.load_bundle(input_bundle)
    scope, scope_reason = _oracle_scope(manifest)
    result: dict[str, Any] = {"schema": SCHEMA, "accepted": False,
                              "qualification_scope": scope,
                              "failures": [], "policies": {}}
    if scope is None:
        result["failures"].append({"name": scope_reason})
        return result
    originals, _, _ = original_oracles(manifest, input_bundle)
    if originals is None:
        result["failures"].append({"name": "original-oracles", "reason": "missing-or-tampered"})
        return result
    result["original_oracles"] = {
        kind: {"bytes": data["bytes"], "sha256": data["sha256"]}
        for kind, data in originals.items()
    }
    if reset not in ("cold-device", "warm-device") or timeout_seconds <= 0:
        result["failures"].append({"name": "invalid-reset-or-timeout"})
        return result
    child_runner = child_runner or _child
    try:
        provider_sha256 = _digest(provider.resolve())
        validator_sha256 = _digest(validator.resolve())
    except OSError:
        result["failures"].append({"name": "provider-validator-missing"})
        return result
    if (expected_provider_sha256 is not None and provider_sha256 != expected_provider_sha256
            or expected_validator_sha256 is not None and validator_sha256 != expected_validator_sha256):
        result["failures"].append({"name": "pre-gpu-binary-fingerprint-mismatch"})
        return result
    launcher = pathlib.Path(__file__).resolve().parents[2] / "scripts" / "run_python.sh"
    runner_path = pathlib.Path(__file__).resolve().with_name("run_dxmt9_render_tape.py")
    gpu_fingerprint: tuple[Any, ...] | None = None
    for name, policy in (("reference", reference_policy), ("candidate", candidate_policy)):
        child = staging / "gpu" / name
        command = [str(launcher), str(runner_path), "experiment", str(input_bundle),
                   "--provider", str(provider), "--validator", str(validator),
                   "--warmup", "0", "--repeat", "2", "--reset", reset,
                   "--replay-route", policy["replay_route"], "--partition-mode", "identity",
                   "--require-path", "--draw-command-limit", str(policy["draw_command_limit"]),
                   "--slot-headroom-bytes", str(policy["slot_headroom_bytes"]),
                   "--aggregate-headroom-bytes", str(policy["aggregate_headroom_bytes"]),
                   "--output-dir", str(child), "--timeout-seconds", str(timeout_seconds)]
        command += render_tape_cpu_policy.source_policy_arguments(policy)
        receipt = child_runner(command, timeout_seconds + 5)
        accepted, reason = gpu_receipt_accepts(
            receipt, policy, manifest=manifest, bundle=input_bundle,
            readback_root=child / "readback", warmup=0, repeat=2,
            reset=reset, require_path=True, runner=runner)
        if accepted and (expected_provider_sha256 is not None
                         and receipt.get("provider", {}).get("sha256") != expected_provider_sha256):
            accepted, reason = False, "gpu-provider-fingerprint-mismatch"
        if accepted and (expected_validator_sha256 is not None
                         and receipt.get("validator", {}).get("sha256") != expected_validator_sha256):
            accepted, reason = False, "gpu-validator-fingerprint-mismatch"
        if accepted and cpu_verification is not None:
            accepted, reason = cpu_gpu_binding_matches(
                cpu_verification[name], receipt, policy)
        if accepted:
            fingerprint = _gpu_pair_fingerprint(receipt)
            if gpu_fingerprint is None:
                gpu_fingerprint = fingerprint
            elif gpu_fingerprint != fingerprint:
                accepted, reason = False, "gpu-pair-fingerprint-mismatch"
        result["policies"][name] = {
            "accepted": accepted, "reason": reason,
            "receipt": f"gpu/{name}/result.json",
            "sha256": _digest(child / "result.json") if (child / "result.json").is_file() else None,
        }
        if not accepted:
            result["failures"].append({"name": f"{name}-gpu", "reason": reason})
    result["accepted"] = not result["failures"] and len(result["policies"]) == 2
    result["reset"] = reset
    result["warmup"] = 0
    result["repeat"] = 2
    return result


def final_reauthenticate(
    qualification: dict[str, Any], *, staging: pathlib.Path,
    input_bundle: pathlib.Path, provider: pathlib.Path, validator: pathlib.Path,
    runner: Any, expected_policies: dict[str, dict[str, Any]] | None = None,
) -> bool:
    """Recheck stored GPU witnesses, binaries, images and readback bytes at commit."""
    if qualification.get("accepted") is not True:
        return False
    policies = qualification.get("policies")
    if not isinstance(policies, dict) or set(policies) != {"reference", "candidate"}:
        return False
    if not isinstance(expected_policies, dict) or set(expected_policies) != set(policies):
        return False
    try:
        expected_policies = {name: render_tape_cpu_policy.validate_policy(policy)
                             for name, policy in expected_policies.items()}
    except render_tape_cpu_policy.PolicyError:
        return False
    manifest, _, _, _, _, _ = runner.load_bundle(input_bundle)
    try:
        provider_digest = _digest(provider.resolve())
        validator_digest = _digest(validator.resolve())
    except OSError:
        return False
    for name, evidence in policies.items():
        if not isinstance(evidence, dict):
            return False
        receipt_path = staging / "gpu" / name / "result.json"
        try:
            receipt_bytes = receipt_path.read_bytes()
            receipt = json.loads(receipt_bytes)
        except (OSError, json.JSONDecodeError):
            return False
        if (not isinstance(receipt, dict)
                or _sha256(receipt_bytes) != evidence.get("sha256")
                or receipt.get("provider", {}).get("sha256") != provider_digest
                or receipt.get("validator", {}).get("sha256") != validator_digest):
            return False
        requested = receipt.get("requested")
        policy = expected_policies[name]
        if (not isinstance(requested, dict) or requested.get("cpu_policy") != policy
                or requested.get("cpu_policy_sha256") !=
                   render_tape_cpu_policy.policy_digest(policy)):
            return False
        accepted, _ = gpu_receipt_accepts(
            receipt, policy, manifest=manifest, bundle=input_bundle,
            readback_root=staging / "gpu" / name / "readback", warmup=0,
            repeat=2, reset=receipt.get("requested", {}).get("reset", ""),
            require_path=True, runner=runner)
        if not accepted:
            return False
    return True
