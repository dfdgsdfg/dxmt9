#!/usr/bin/env python3

import argparse
import contextlib
import errno
import hashlib
import json
import os
import platform
import pathlib
import re
import shutil
import struct
import tempfile
import subprocess
import sys
from typing import Any, Optional

# Policy qualification is kept in a small, importable module so the
# measurement lane cannot accidentally reuse the full-oracle acceptance gate.
try:
    import render_tape_cpu_policy
except ModuleNotFoundError:  # importlib-loaded checks do not set script dir
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
    import render_tape_cpu_policy

import render_tape_gpu_policy


SCHEMA = "dxmt9.render_tape.bundle.v2"
EVENTS_NAME = "events.bin"
IDENTITY_NAME = "identity.bin"
IDENTITY_SCHEMA = "dxmt9.render_tape.identity.v2"
MAX_REPLAY_RUNS = 64
PARALLEL_VERIFY_RUNS = 2
PROVIDER_SCHEMA = "dxmt9.render_tape.provider_replay.v2"
REJECTED_MARKER_NAME = "rejected.json"
PARALLEL_COUNTER_FIELDS = (
    "selected",
    "children",
    "draws",
    "pre_effect_fallbacks",
    "gpu_command_buffer_errors",
    "worker_batches",
    "worker_tasks",
    "worker_cpu_ns",
    "worker_wall_ns",
    "worker_active_peak",
)
PARALLEL_DETERMINISTIC_COUNTER_FIELDS = (
    "selected",
    "children",
    "draws",
    "pre_effect_fallbacks",
    "gpu_command_buffer_errors",
    "worker_batches",
    "worker_tasks",
)

# The general-frame lane is intentionally finite even though it admits the
# complete GT2 capture.  These are policy bounds, not claims about all tapes.
CPU_GENERAL_INPUT_LIMITS = {
    "events": 2048,
    "objects": 512,
    "mutations": 1024,
    "chunks": 128,
    # Match GeneralCpuAdmissionCaps, not the executor's defensive limits.
    "records": 4096,
    "draws": 4096,
}
CPU_WINDOW_PLAN_SCHEMA = "dxmt9.render_tape.cpu_window_plan.v1"
CPU_WINDOW_PAGE_SIZE = 4096


def digest(path: pathlib.Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def copy_flushed(source: pathlib.Path, destination: pathlib.Path) -> None:
    with source.open("rb") as input_stream, destination.open("xb") as output_stream:
        shutil.copyfileobj(input_stream, output_stream, 1024 * 1024)
        output_stream.flush()
        os.fsync(output_stream.fileno())


def copy_input_owned(source: pathlib.Path, destination: pathlib.Path) -> None:
    """Stage an independently writable CPU/GPU input bundle.

    APFS clone requests keep large comparison inputs bounded in space while
    preserving copy-on-write ownership.  ``cp -L`` matches ``copytree``'s
    follow-links behavior; other platforms retain the portable deep-copy path.
    The destination is deliberately required to be absent so a failed stage
    cannot remove or overwrite an existing artifact.
    """
    source = pathlib.Path(source)
    destination = pathlib.Path(destination)
    if destination.exists() or destination.is_symlink():
        raise FileExistsError(
            f"Input staging target already exists: {destination}"
        )
    if platform.system() == "Darwin":
        subprocess.run(
            ["/bin/cp", "-c", "-R", "-L", str(source), str(destination)],
            check=True,
        )
    else:
        shutil.copytree(source, destination)


def write_flushed(path: pathlib.Path, data: bytes) -> None:
    with path.open("xb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())


def sync_directory(path: pathlib.Path) -> None:
    descriptor = os.open(path, os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _rejected_staging_path(output: pathlib.Path) -> pathlib.Path:
    """Reserve a private, collision-resistant path beside ``output``."""
    reserved = pathlib.Path(
        tempfile.mkdtemp(prefix=f".{output.name}.rejected-", dir=output.parent)
    )
    # ``mkdtemp`` gives us a private, exclusive name.  Remove only that empty
    # directory so the actual staging tree can be moved there atomically.
    reserved.rmdir()
    return reserved


def _write_rejected_marker(
    staging: pathlib.Path, output: pathlib.Path, error: BaseException,
    failure_metadata: Optional[dict[str, Any]] = None,
) -> None:
    marker = {
        "schema": "dxmt9.render_tape.rejected_staging.v1",
        "accepted": False,
        "exception": str(error),
        "exception_type": type(error).__name__,
        "intended_target": str(output),
    }
    error_number = getattr(error, "errno", None)
    if type(error_number) is int:
        marker["exception_errno"] = error_number
        marker["exception_errno_name"] = errno.errorcode.get(error_number)
    if failure_metadata:
        marker["failure_metadata"] = dict(failure_metadata)
    write_flushed(
        staging / REJECTED_MARKER_NAME,
        (json.dumps(marker, indent=2, sort_keys=True) + "\n").encode(),
    )


def _staging_has_content(staging: pathlib.Path) -> bool:
    try:
        return any(staging.iterdir())
    except OSError:
        # If enumeration itself failed, retaining the directory is safer than
        # destroying evidence that may have been written successfully.
        return True


def _report_retained_staging(staging: pathlib.Path, detail: str) -> None:
    try:
        print(
            f"render tape transaction retained rejected staging {staging}: {detail}",
            file=sys.stderr,
        )
    except BaseException:
        # Diagnostics must not replace the transaction's original exception.
        pass


@contextlib.contextmanager
def bundle_transaction(
    output: pathlib.Path, *, preserve_rejected: bool = False,
    failure_metadata: Optional[dict[str, Any]] = None,
):
    """Build ``output`` privately, optionally retaining failed CPU evidence.

    The normal transaction remains cleanup-on-failure.  CPU experiment lanes
    opt into retention because a provider may have emitted useful child data
    before a later receipt/fsync fails.  The marker is advisory: failures while
    writing it or moving the private directory never replace the original
    exception.
    """
    output = output.absolute()
    ensure_empty_output(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    staging = pathlib.Path(
        tempfile.mkdtemp(prefix=f".{output.name}.staging-", dir=output.parent)
    )
    published = False
    try:
        yield staging
        for directory, _, filenames in os.walk(staging, topdown=False):
            directory_path = pathlib.Path(directory)
            for filename in filenames:
                path = directory_path / filename
                with path.open("rb") as stream:
                    os.fsync(stream.fileno())
            sync_directory(directory_path)
        os.replace(staging, output)
        published = True
        try:
            sync_directory(output.parent)
        except OSError:
            # The destination is already complete and atomically claimable.
            # Do not report a post-rename failure that cannot be rolled back.
            pass
    except BaseException as error:
        if published:
            # Cancellation after the commit point must not quarantine the
            # now-absent private tree or misreport the published receipt.
            raise
        if preserve_rejected and _staging_has_content(staging):
            try:
                rejected = _rejected_staging_path(output)
                marker_error: Optional[BaseException] = None
                try:
                    _write_rejected_marker(
                        staging, output, error, failure_metadata=failure_metadata)
                except BaseException as caught_marker_error:
                    # Marker I/O is intentionally best effort.  Raw evidence
                    # and the transaction's original error are authoritative.
                    marker_error = caught_marker_error
                try:
                    staging.rename(rejected)
                except BaseException as move_error:
                    # Never delete populated CPU staging after opting into
                    # retention.  It remains unpublished under its original
                    # private name if quarantine allocation/move fails.
                    _report_retained_staging(staging, f"quarantine move failed: {move_error}")
                    try:
                        rejected.rmdir()
                    except BaseException:
                        pass
                else:
                    detail = (f"marker failed: {marker_error}" if marker_error is not None
                              else "transaction failed")
                    _report_retained_staging(rejected, detail)
            except BaseException as quarantine_error:
                # Allocation can itself fail (including ENOSPC).  Leave the
                # original staging tree in place and preserve the root error.
                _report_retained_staging(staging, f"quarantine allocation failed: {quarantine_error}")
        else:
            shutil.rmtree(staging, ignore_errors=True)
        raise


def write_cpu_receipt(
    path: pathlib.Path, payload: bytes, failure_metadata: dict[str, Any]
) -> None:
    """Write a CPU JSON receipt while retaining bounded ENOSPC diagnostics."""
    try:
        free_bytes = shutil.disk_usage(path.parent).free
    except OSError:
        free_bytes = None
    failure_metadata["receipt_write_diagnostic"] = {
        "attempted_receipt_payload_bytes": len(payload),
        "free_bytes_before_receipt_write": free_bytes,
    }
    write_flushed(path, payload)


def run_validator(
    validator: pathlib.Path,
    command: str,
    events: pathlib.Path,
    blob_refs: list[str],
) -> dict[str, Any]:
    arguments = [str(validator), command, str(events)]
    for reference in blob_refs:
        arguments.extend(("--verified-blob", reference))
    completed = subprocess.run(
        arguments,
        check=False,
        text=True,
        capture_output=True,
    )
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip()
        raise SystemExit(f"render tape {command} failed: {detail}")
    try:
        return json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise SystemExit(
            f"render tape validator emitted invalid JSON: {error}"
        ) from error


def run_identity_validator(
    validator: pathlib.Path,
    events: pathlib.Path,
    identity: pathlib.Path,
    blob_refs: list[str],
) -> dict[str, Any]:
    arguments = [str(validator), "identity", str(events), str(identity)]
    for reference in blob_refs:
        arguments.extend(("--verified-blob", reference))
    completed = subprocess.run(arguments, check=False, text=True, capture_output=True)
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip()
        raise SystemExit(f"render tape identity validation failed: {detail}")
    try:
        return json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise SystemExit("render tape identity validator emitted invalid JSON") from error


def run_policy_explore(
    validator: pathlib.Path,
    events: pathlib.Path,
    identity: pathlib.Path,
    blob_refs: list[str],
) -> dict[str, Any]:
    arguments = [str(validator), "policy-explore", str(events), str(identity)]
    for reference in blob_refs:
        arguments.extend(("--verified-blob", reference))
    completed = subprocess.run(
        arguments, check=False, text=True, capture_output=True
    )
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip()
        raise SystemExit(f"render tape policy exploration failed: {detail}")
    try:
        return json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise SystemExit(
            "render tape policy explorer emitted invalid JSON"
        ) from error


def policy_explore(args: argparse.Namespace) -> int:
    manifest, events, blob_refs, _, identity, _source = load_bundle(args.bundle)
    if identity is None:
        raise SystemExit(
            "policy exploration requires authenticated components/identity.bin"
        )
    identity_result = run_identity_validator(
        args.validator, events, identity, blob_refs
    )
    result = run_policy_explore(args.validator, events, identity, blob_refs)
    if (
        result.get("status") != "valid"
        or result.get("authenticated_input") is not True
        or result.get("structural_only") is not True
        or result.get("proof_core_validated") is not False
    ):
        raise SystemExit(
            "policy exploration returned an unsafe or unauthenticated status"
        )
    result["bundle"] = str(args.bundle.resolve())
    result["bundle_profile"] = manifest.get("profile")
    result["identity_validation"] = identity_result
    print(json.dumps(result, sort_keys=True))
    return 0


def run_provider_replay(
    provider: pathlib.Path,
    events: pathlib.Path,
    blob_paths: list[pathlib.Path],
    expected_output: Optional[pathlib.Path] = None,
    expected_source: Optional[pathlib.Path] = None,
    output_path: Optional[pathlib.Path] = None,
    partition_mode: Optional[str] = None,
    identity: Optional[pathlib.Path] = None,
) -> dict[str, Any]:
    arguments = [str(provider), "replay", str(events)]
    for path in blob_paths:
        arguments.extend(("--blob", str(path)))
    if expected_output is not None:
        arguments.extend(("--expected-rgba", str(expected_output)))
    if expected_source is not None:
        arguments.extend(("--expected-source-rgba", str(expected_source)))
    if output_path is not None:
        arguments.extend(("--output-rgba", str(output_path)))
    if partition_mode is not None:
        arguments.extend(("--partition-mode", partition_mode))
    if identity is not None:
        arguments.extend(("--identity", str(identity)))
    completed = subprocess.run(
        arguments,
        check=False,
        text=True,
        capture_output=True,
    )
    output = completed.stdout.strip()
    if not output:
        detail = completed.stderr.strip() or "provider emitted no result"
        raise SystemExit(f"render tape provider replay failed: {detail}")
    try:
        result = json.loads(output)
    except json.JSONDecodeError as error:
        detail = completed.stderr.strip()
        suffix = f": {detail}" if detail else ""
        raise SystemExit(
            f"render tape provider emitted invalid JSON: {error}{suffix}"
        ) from error
    result["provider_exit_code"] = completed.returncode
    return result


def provider_oracle_accepts(
    result: dict[str, Any], require_non_degenerate: bool = False
) -> bool:
    validity = result.get("validity")
    coverage = result.get("coverage")
    conservation = result.get("conservation")
    if (
        not isinstance(validity, dict)
        or not isinstance(coverage, dict)
        or not isinstance(conservation, dict)
    ):
        return False
    profile = result.get("profile")
    intervals = result.get("intervals")
    expected_intervals = 2 if profile == "sequence-tape" else 1
    intervals_valid = bool(
        isinstance(intervals, list)
        and len(intervals) == expected_intervals
        and all(
            isinstance(interval, dict)
            and isinstance(interval.get("present_ordinal"), int)
            and interval.get("present_ordinal", 0) > 0
            and isinstance(interval.get("completion_ordinal"), int)
            and interval.get("completion_ordinal", 0) > 0
            and isinstance(interval.get("validity"), dict)
            and interval["validity"].get("output_readback") is True
            and interval["validity"].get("expected_digest_captured") is True
            and interval["validity"].get("output_oracle_matched") is True
            and (
                not require_non_degenerate
                or interval["validity"].get("output_non_degenerate") is True
            )
            and isinstance(interval["validity"].get("output_sha256"), str)
            and len(interval["validity"]["output_sha256"]) == 64
            for interval in intervals
        )
    )
    if profile == "sequence-tape" and intervals_valid:
        intervals_valid = (
            intervals[0]["present_ordinal"] < intervals[1]["present_ordinal"]
            and intervals[0]["completion_ordinal"]
            < intervals[1]["completion_ordinal"]
            and intervals[0]["validity"]["output_sha256"]
            != intervals[1]["validity"]["output_sha256"]
        )
    conservation_counters = (
        conservation.get("input_blobs"),
        conservation.get("referenced_blobs"),
        conservation.get("objects_created"),
        conservation.get("objects_released"),
    )
    conservation_valid = bool(
        all(isinstance(value, int) and value >= 0 for value in conservation_counters)
        and conservation_counters[0] == conservation_counters[1]
        and conservation_counters[2] == conservation_counters[3]
        and isinstance(conservation.get("present_ordinal"), int)
        and conservation.get("present_ordinal", 0) > 0
        and isinstance(conservation.get("completion_ordinal"), int)
        and conservation.get("completion_ordinal", 0) > 0
        and isinstance(intervals, list)
        and conservation["present_ordinal"] == intervals[-1]["present_ordinal"]
        and conservation["completion_ordinal"]
        == intervals[-1]["completion_ordinal"]
    )
    coverage_valid = bool(
        isinstance(coverage.get("present_records"), int)
        and coverage.get("present_records") == expected_intervals
        and isinstance(coverage.get("present_source_mappings"), int)
        and coverage.get("present_source_mappings") == expected_intervals
        and isinstance(coverage.get("present_outputs"), int)
        and coverage.get("present_outputs") == expected_intervals
        and (
            profile != "sequence-tape"
            or (
                isinstance(coverage.get("seed_mutations"), int)
                and coverage.get("seed_mutations") == 2
            )
        )
    )
    return bool(
        result.get("provider_exit_code") == 0
        and profile in ("frame-tape", "sequence-tape")
        and result.get("status") == "complete"
        and validity.get("structurally_valid") is True
        and validity.get("digests_valid") is True
        and validity.get("output_readback") is True
        and validity.get("expected_digest_captured") is True
        and validity.get("output_oracle_matched") is True
        and (
            not require_non_degenerate
            or validity.get("output_non_degenerate") is True
        )
        and isinstance(validity.get("output_sha256"), str)
        and len(validity["output_sha256"]) == 64
        and conservation_valid
        and coverage_valid
        and intervals_valid
    )


def bundle_requires_non_degenerate(manifest: dict[str, Any]) -> bool:
    """Identify bundles whose output is promotion evidence rather than a fixture."""
    scope = manifest.get("scope")
    components = manifest.get("components")
    return bool(
        isinstance(scope, dict)
        and scope.get("production_capture") is True
    ) or bool(
        isinstance(components, dict)
        and isinstance(components.get("output_oracle"), dict)
    )


def replay_identity(result: dict[str, Any]) -> dict[str, Any]:
    return {
        key: result.get(key)
        for key in (
            "profile",
            "status",
            "failed_event",
            "requirements",
            "validity",
            "coverage",
            "conservation",
            "identity_evidence",
            "source_boundary_audit",
            "source_replay",
            "audit_batch_cuts",
            "intervals",
        )
    }


def replay_with_policy(
    provider: pathlib.Path,
    events: pathlib.Path,
    blob_paths: list[pathlib.Path],
    warmup: int,
    repeat: int,
    require_non_degenerate: bool = False,
    expected_output: Optional[pathlib.Path] = None,
    partition_mode: Optional[str] = None,
    expected_source: Optional[pathlib.Path] = None,
    identity: Optional[pathlib.Path] = None,
) -> dict[str, Any]:
    if warmup < 0 or repeat < 1 or warmup + repeat > MAX_REPLAY_RUNS:
        raise SystemExit(
            f"warmup + repeat must be in [1, {MAX_REPLAY_RUNS}] with repeat >= 1"
        )
    warmup_results = []
    for _ in range(warmup):
        result = run_provider_replay(
            provider, events, blob_paths, expected_output,
            expected_source=expected_source,
            partition_mode=partition_mode,
            identity=identity,
        )
        if not provider_oracle_accepts(result, require_non_degenerate):
            result["oracle_accepted"] = False
            result["failed_phase"] = "warmup"
            result["replay_policy"] = {
                "reset": "fresh-process-device",
                "warmup": warmup,
                "repeat": repeat,
            }
            result["warmup_runs"] = warmup_results + [replay_identity(result)]
            result["runs"] = []
            return result
        warmup_results.append(replay_identity(result))
    runs = []
    last_result: dict[str, Any] = {}
    for _ in range(repeat):
        last_result = run_provider_replay(
            provider, events, blob_paths, expected_output,
            expected_source=expected_source,
            partition_mode=partition_mode,
            identity=identity,
        )
        if not provider_oracle_accepts(last_result, require_non_degenerate):
            last_result["oracle_accepted"] = False
            last_result["replay_policy"] = {
                "reset": "fresh-process-device",
                "warmup": warmup,
                "repeat": repeat,
            }
            last_result["runs"] = runs + [replay_identity(last_result)]
            return last_result
        runs.append(replay_identity(last_result))
    baseline = runs[0]
    all_runs = warmup_results + runs
    deterministic = all(run == baseline for run in all_runs)
    result = dict(last_result)
    result["provider_exit_code"] = 0
    result["oracle_accepted"] = True
    result["deterministic"] = deterministic
    result["replay_policy"] = {
        "reset": "fresh-process-device",
        "warmup": warmup,
        "repeat": repeat,
    }
    result["warmup_runs"] = warmup_results
    result["runs"] = runs
    envelope_used = any(
        run.get("validity", {}).get("oracle_mode") == "pixel-envelope"
        for run in all_runs
    )
    if envelope_used and len(all_runs) < 2:
        result["status"] = "insufficient-envelope-repeats"
        result["oracle_accepted"] = False
    if not deterministic:
        result["status"] = "nondeterministic"
        result["oracle_accepted"] = False
    return result


def producer_revision() -> str:
    root = pathlib.Path(__file__).resolve().parents[2]
    completed = subprocess.run(
        ["git", "-C", str(root), "rev-parse", "HEAD"],
        check=False,
        text=True,
        capture_output=True,
    )
    return completed.stdout.strip() if completed.returncode == 0 else "unknown"


def pack(args: argparse.Namespace) -> int:
    events = args.events.resolve()
    identity_source = args.identity.resolve() if args.identity else None
    output_source = args.output_rgba.resolve() if args.output_rgba else None
    source_oracle_source = (
        args.source_rgba.resolve() if args.source_rgba else None
    )
    blob_sources = [path.resolve() for path in args.blob]
    blobs_by_digest: dict[str, tuple[pathlib.Path, int]] = {}
    for path in blob_sources:
        sha256 = digest(path)
        size = path.stat().st_size
        blobs_by_digest.setdefault(sha256, (path, size))
    blob_refs = [
        f"{sha256}:{size}"
        for sha256, (_, size) in blobs_by_digest.items()
    ]
    validator_result = run_validator(
        args.validator, "validate", events, blob_refs
    )
    profile = validator_result.get("profile")
    if profile not in ("frame-tape", "sequence-tape"):
        raise SystemExit(f"validator returned unsupported render tape profile: {profile}")
    if identity_source is not None:
        run_identity_validator(args.validator, events, identity_source, blob_refs)
    if output_source is not None:
        inspected = run_validator(args.validator, "inspect", events, blob_refs)
        if (
            profile != "frame-tape"
            or inspected.get("expected_output_sha256") != digest(output_source)
        ):
            raise SystemExit(
                "render tape output sidecar does not match the captured Present digest"
            )
    output = args.output_dir.absolute()
    with bundle_transaction(output) as staging:
        destination = staging / EVENTS_NAME
        copy_flushed(events, destination)
        event_digest = digest(destination)
        blob_components = []
        blobs_directory = staging / "blobs"
        if blobs_by_digest:
            blobs_directory.mkdir()
        for sha256, (source, size) in blobs_by_digest.items():
            blob_name = f"{sha256}.bin"
            copy_flushed(source, blobs_directory / blob_name)
            blob_components.append(
                {
                    "path": f"blobs/{blob_name}",
                    "bytes": size,
                    "sha256": sha256,
                }
            )
        manifest = {
        "schema": SCHEMA,
        "profile": profile,
        "producer": {
            "path": "scripts/tools/run_dxmt9_render_tape.py",
            "git_revision": producer_revision(),
        },
        "stage": "dump-extract",
        "domain": "replay",
        "inputs": [{"path": str(events), "sha256": digest(events)}],
        "env_snapshot": {},
        "validity": {
            "structural": True,
            "reference_replay": False,
            "validator": validator_result,
        },
        "components": {
            "events": {
                "path": EVENTS_NAME,
                "bytes": destination.stat().st_size,
                "sha256": event_digest,
            },
            "blobs": blob_components,
        },
        "scope": {
            "production_capture": False,
            "production_provider_replay": False,
            "output_oracle": output_source is not None,
            "source_oracle": source_oracle_source is not None,
            **({"test_input": True} if getattr(args, "test_input", False) else {}),
        },
        }
        if identity_source is not None:
            identity_target = staging / IDENTITY_NAME
            copy_flushed(identity_source, identity_target)
            manifest["components"]["identity"] = {
                "path": IDENTITY_NAME,
                "schema": IDENTITY_SCHEMA,
                "bytes": identity_target.stat().st_size,
                "sha256": digest(identity_target),
            }
        if output_source is not None:
            output_target = staging / "output.rgba"
            copy_flushed(output_source, output_target)
            manifest["components"]["output_oracle"] = {
                "path": "output.rgba",
                "bytes": output_target.stat().st_size,
                "sha256": digest(output_target),
            }
        if source_oracle_source is not None:
            source_oracle_target = staging / "source.rgba"
            copy_flushed(source_oracle_source, source_oracle_target)
            manifest["components"]["source_oracle"] = {
                "path": "source.rgba",
                "bytes": source_oracle_target.stat().st_size,
                "sha256": digest(source_oracle_target),
            }
        write_flushed(
            staging / "manifest.json",
            (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode(),
        )
    manifest_path = output / "manifest.json"
    print(json.dumps({"bundle": str(output), "manifest": str(manifest_path)}))
    return 0


def load_bundle(
    bundle: pathlib.Path,
) -> tuple[
    dict[str, Any], pathlib.Path, list[str], list[pathlib.Path],
    Optional[pathlib.Path], Optional[pathlib.Path]
]:
    manifest_path = bundle.resolve() / "manifest.json"
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise SystemExit(f"cannot read render tape manifest {manifest_path}: {error}")
    if manifest.get("schema") != SCHEMA:
        raise SystemExit(f"unsupported render tape bundle schema: {manifest.get('schema')}")
    if manifest.get("profile") not in ("frame-tape", "sequence-tape"):
        raise SystemExit(f"unsupported render tape profile: {manifest.get('profile')}")
    component = manifest.get("components", {}).get("events")
    if not isinstance(component, dict) or component.get("path") != EVENTS_NAME:
        raise SystemExit("render tape bundle must declare components.events.path=events.bin")
    events = manifest_path.parent / EVENTS_NAME
    if not events.is_file():
        raise SystemExit(f"render tape events component is missing: {events}")
    actual_bytes = events.stat().st_size
    if component.get("bytes") != actual_bytes:
        raise SystemExit(
            f"render tape events size mismatch: manifest={component.get('bytes')} actual={actual_bytes}"
        )
    actual_digest = digest(events)
    if component.get("sha256") != actual_digest:
        raise SystemExit(
            "render tape events digest mismatch: "
            f"manifest={component.get('sha256')} actual={actual_digest}"
        )
    blob_components = manifest.get("components", {}).get("blobs", [])
    if not isinstance(blob_components, list):
        raise SystemExit("render tape components.blobs must be an array")
    blob_refs = []
    blob_paths = []
    seen_digests: set[str] = set()
    declared_blob_names: set[str] = set()
    for blob in blob_components:
        if not isinstance(blob, dict):
            raise SystemExit("render tape blob component must be an object")
        claimed_digest = blob.get("sha256")
        if (
            not isinstance(claimed_digest, str)
            or len(claimed_digest) != 64
            or any(value not in "0123456789abcdef" for value in claimed_digest)
        ):
            raise SystemExit("render tape blob digest must be lowercase SHA-256")
        if claimed_digest in seen_digests:
            raise SystemExit(f"duplicate render tape blob digest: {claimed_digest}")
        seen_digests.add(claimed_digest)
        expected_path = f"blobs/{claimed_digest}.bin"
        if blob.get("path") != expected_path:
            raise SystemExit(
                "render tape blob path must be canonical: "
                f"expected={expected_path} actual={blob.get('path')}"
            )
        path = manifest_path.parent / expected_path
        if not path.is_file():
            raise SystemExit(f"render tape blob component is missing: {path}")
        actual_bytes = path.stat().st_size
        actual_digest = digest(path)
        if blob.get("bytes") != actual_bytes:
            raise SystemExit(
                f"render tape blob size mismatch: manifest={blob.get('bytes')} actual={actual_bytes}"
            )
        if claimed_digest != actual_digest:
            raise SystemExit(
                "render tape blob digest mismatch: "
                f"manifest={claimed_digest} actual={actual_digest}"
            )
        blob_refs.append(f"{actual_digest}:{actual_bytes}")
        blob_paths.append(path)
        declared_blob_names.add(f"{claimed_digest}.bin")
    blobs_directory = manifest_path.parent / "blobs"
    if blobs_directory.exists():
        if not blobs_directory.is_dir():
            raise SystemExit("render tape blobs component path is not a directory")
        actual_blob_names = {path.name for path in blobs_directory.iterdir()}
        if actual_blob_names != declared_blob_names:
            extras = sorted(actual_blob_names - declared_blob_names)
            missing = sorted(declared_blob_names - actual_blob_names)
            raise SystemExit(
                "render tape blob directory does not match the manifest: "
                f"extra={extras} missing={missing}"
            )
    elif declared_blob_names:
        raise SystemExit("render tape blobs directory is missing")
    output_oracle = manifest.get("components", {}).get("output_oracle")
    if output_oracle is not None:
        if not isinstance(output_oracle, dict):
            raise SystemExit("render tape components.output_oracle must be an object")
        if output_oracle.get("path") != "output.rgba":
            raise SystemExit(
                "render tape output oracle path must be canonical: output.rgba"
            )
        claimed_digest = output_oracle.get("sha256")
        if (
            not isinstance(claimed_digest, str)
            or len(claimed_digest) != 64
            or any(value not in "0123456789abcdef" for value in claimed_digest)
        ):
            raise SystemExit("render tape output oracle digest must be lowercase SHA-256")
        output_path = manifest_path.parent / "output.rgba"
        if not output_path.is_file():
            raise SystemExit(f"render tape output oracle is missing: {output_path}")
        actual_bytes = output_path.stat().st_size
        if output_oracle.get("bytes") != actual_bytes:
            raise SystemExit(
                "render tape output oracle size mismatch: "
                f"manifest={output_oracle.get('bytes')} actual={actual_bytes}"
            )
        actual_digest = digest(output_path)
        if claimed_digest != actual_digest:
            raise SystemExit(
                "render tape output oracle digest mismatch: "
                f"manifest={claimed_digest} actual={actual_digest}"
            )
    source_oracle_path = None
    source_oracle = manifest.get("components", {}).get("source_oracle")
    if source_oracle is not None:
        if not isinstance(source_oracle, dict):
            raise SystemExit("render tape components.source_oracle must be an object")
        if source_oracle.get("path") != "source.rgba":
            raise SystemExit(
                "render tape source oracle path must be canonical: source.rgba"
            )
        claimed_digest = source_oracle.get("sha256")
        if (
            not isinstance(claimed_digest, str)
            or len(claimed_digest) != 64
            or any(value not in "0123456789abcdef" for value in claimed_digest)
        ):
            raise SystemExit("render tape source oracle digest must be lowercase SHA-256")
        source_oracle_path = manifest_path.parent / "source.rgba"
        if not source_oracle_path.is_file():
            raise SystemExit(
                f"render tape source oracle is missing: {source_oracle_path}"
            )
        actual_bytes = source_oracle_path.stat().st_size
        if source_oracle.get("bytes") != actual_bytes:
            raise SystemExit("render tape source oracle size mismatch")
        actual_digest = digest(source_oracle_path)
        if claimed_digest != actual_digest:
            raise SystemExit(
                "render tape source oracle digest mismatch: "
                f"manifest={claimed_digest} actual={actual_digest}"
            )
    identity_path = None
    identity = manifest.get("components", {}).get("identity")
    if identity is not None:
        if not isinstance(identity, dict):
            raise SystemExit("render tape components.identity must be an object")
        if identity.get("path") != IDENTITY_NAME:
            raise SystemExit("render tape identity path must be canonical: identity.bin")
        if identity.get("schema") != IDENTITY_SCHEMA:
            raise SystemExit("render tape identity schema is unsupported")
        claimed_digest = identity.get("sha256")
        if (
            not isinstance(claimed_digest, str)
            or len(claimed_digest) != 64
            or any(value not in "0123456789abcdef" for value in claimed_digest)
        ):
            raise SystemExit("render tape identity digest must be lowercase SHA-256")
        identity_path = manifest_path.parent / IDENTITY_NAME
        if not identity_path.is_file():
            raise SystemExit(f"render tape identity component is missing: {identity_path}")
        actual_bytes = identity_path.stat().st_size
        if identity.get("bytes") != actual_bytes:
            raise SystemExit("render tape identity size mismatch")
        if digest(identity_path) != claimed_digest:
            raise SystemExit("render tape identity digest mismatch")
    allowed = {"manifest.json", EVENTS_NAME}
    if blobs_directory.exists():
        allowed.add("blobs")
    if output_oracle is not None:
        allowed.add("output.rgba")
    if source_oracle_path is not None:
        allowed.add("source.rgba")
    if identity_path is not None:
        allowed.add(IDENTITY_NAME)
    actual = {path.name for path in manifest_path.parent.iterdir()}
    if actual != allowed:
        raise SystemExit(
            "render tape bundle contains unlisted top-level components: "
            f"{sorted(actual - allowed)}"
        )
    return manifest, events, blob_refs, blob_paths, identity_path, source_oracle_path


def ensure_empty_output(output: pathlib.Path) -> None:
    if os.path.lexists(output):
        raise SystemExit(f"output path already exists: {output}")


def write_reduced_bundle(
    output: pathlib.Path,
    events: pathlib.Path,
    source_manifest: dict[str, Any],
    source_blobs: list[pathlib.Path],
    referenced_digests: list[str],
    reducer: dict[str, Any],
) -> pathlib.Path:
    ensure_empty_output(output)
    source_by_digest = {digest(path): path for path in source_blobs}
    if len(referenced_digests) != len(set(referenced_digests)):
        raise SystemExit("reducer returned duplicate referenced blob digests")
    missing = [value for value in referenced_digests if value not in source_by_digest]
    if missing:
        raise SystemExit(f"reducer returned unknown referenced blob digest: {missing[0]}")
    with bundle_transaction(output) as staging:
        destination = staging / EVENTS_NAME
        copy_flushed(events, destination)
        blob_components = []
        if referenced_digests:
            (staging / "blobs").mkdir()
        for sha256 in referenced_digests:
            source = source_by_digest[sha256]
            target = staging / "blobs" / f"{sha256}.bin"
            copy_flushed(source, target)
            blob_components.append(
                {
                    "path": f"blobs/{sha256}.bin",
                    "bytes": target.stat().st_size,
                    "sha256": sha256,
                }
            )
        manifest = {
        "schema": SCHEMA,
        "profile": "frame-tape",
        "producer": {
            "path": "scripts/tools/run_dxmt9_render_tape.py",
            "git_revision": producer_revision(),
        },
        "stage": "reduce",
        "domain": "replay",
        "inputs": [
            {
                "sha256": source_manifest["components"]["events"]["sha256"],
                "schema": source_manifest["schema"],
            }
        ],
        "env_snapshot": {},
        "validity": {
            "structural": True,
            "reference_replay": True,
            "provider_oracle": True,
        },
        "components": {
            "events": {
                "path": EVENTS_NAME,
                "bytes": destination.stat().st_size,
                "sha256": digest(destination),
            },
            "blobs": blob_components,
        },
        "scope": {
            "production_capture": False,
            "production_provider_replay": True,
            "output_oracle": False,
        },
        "reducer": reducer,
        }
        write_flushed(
            staging / "manifest.json",
            (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode(),
        )
    return output / "manifest.json"


def run_native_reduce(
    validator: pathlib.Path,
    source_events: pathlib.Path,
    output_events: pathlib.Path,
    blob_refs: list[str],
    selections: list[int],
) -> dict[str, Any]:
    arguments = [
        str(validator),
        "reduce",
        str(source_events),
        str(output_events),
    ]
    for selection in selections:
        arguments.extend(("--select-command-event", str(selection)))
    for reference in blob_refs:
        arguments.extend(("--verified-blob", reference))
    completed = subprocess.run(arguments, check=False, text=True, capture_output=True)
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip()
        raise RuntimeError(detail or "native reducer rejected the candidate")
    try:
        result = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise RuntimeError(f"native reducer emitted invalid JSON: {error}") from error
    if not output_events.is_file():
        raise RuntimeError("native reducer reported success without an output tape")
    return result


def inspect_bundle_commands(
    validator: pathlib.Path,
    events: pathlib.Path,
    blob_refs: list[str],
) -> tuple[list[int], int]:
    inspected = run_validator(validator, "inspect", events, blob_refs)
    commands = inspected.get("command_event_indices")
    present = inspected.get("present_command_event")
    if (
        not isinstance(commands, list)
        or not commands
        or any(not isinstance(value, int) or value < 0 for value in commands)
        or not isinstance(present, int)
        or present not in commands
    ):
        raise SystemExit("validator did not expose a canonical Present command event")
    return commands, present


def validate_or_inspect(args: argparse.Namespace) -> int:
    manifest, events, blob_refs, _, identity, _source = load_bundle(args.bundle)
    if identity is not None:
        run_identity_validator(args.validator, events, identity, blob_refs)
    result = run_validator(args.validator, args.command, events, blob_refs)
    result["bundle_schema"] = manifest["schema"]
    result["events_sha256"] = manifest["components"]["events"]["sha256"]
    result["scope"] = manifest.get("scope", {})
    print(json.dumps(result, sort_keys=True))
    return 0


def provider_replay(args: argparse.Namespace) -> int:
    manifest, events, _, blob_paths, identity, source_oracle = load_bundle(args.bundle)
    if identity is not None:
        run_identity_validator(args.validator, events, identity, [
            f"{digest(path)}:{path.stat().st_size}" for path in blob_paths
        ])
    expected_output = None
    if isinstance(manifest.get("components", {}).get("output_oracle"), dict):
        expected_output = events.parent / "output.rgba"
    result = replay_with_policy(
        args.provider,
        events,
        blob_paths,
        args.warmup,
        args.repeat,
        manifest.get("scope", {}).get("production_capture") is True,
        expected_output,
        args.partition_mode,
        source_oracle,
        identity,
    )
    validity = result.get("validity", {})
    output_oracle = bool(
        result.get("oracle_accepted") is True
        and validity.get("output_readback")
        and validity.get("expected_digest_captured")
        and validity.get("output_oracle_matched")
    )
    scope = dict(manifest.get("scope", {}))
    scope["production_provider_replay"] = result.get("oracle_accepted") is True
    scope["output_oracle"] = output_oracle
    scope["source_oracle"] = bool(
        source_oracle is not None
        and result.get("oracle_accepted") is True
        and validity.get("source_readback") is True
        and validity.get("source_oracle_matched") is True
    ) if source_oracle is not None else False
    result["bundle_schema"] = manifest["schema"]
    result["events_sha256"] = manifest["components"]["events"]["sha256"]
    result["scope"] = scope
    print(json.dumps(result, sort_keys=True))
    return 0 if result.get("oracle_accepted") else 1


def _source_boundary_audit_is_default(value: Any) -> bool:
    if not isinstance(value, dict) or value.get("requested") is not False:
        return False
    return all(
        type(value.get(counter)) is int and value[counter] == 0
        for counter in ("batch_cut_count", "completed_batch_cut_count")
    )


def _source_replay_is_default(value: Any) -> bool:
    if (not isinstance(value, dict) or
            any(value.get(key) is not False
                for key in ("requested", "validated", "complete")) or
            value.get("plan_sha256") != "0" * 64):
        return False
    if (value.get("storage_overflow", False) is not False
            or ("sources" in value and value["sources"] != [])):
        return False
    return all(type(value.get(key)) is int and value[key] == 0 for key in (
        "source_count", "completed_source_count", "publication_count",
        "empty_source_count", "present_source_count"))


def experiment_accepts(result: dict[str, Any], *, warmup: int, repeat: int,
                       reset: str, route: str, require_path: bool,
                       non_degenerate: bool, cpu_policy: dict[str, Any] | None = None,
                       input_components: dict[str, Any] | None = None) -> bool:
    """Fail closed on partial runs and nonexecuted policy candidates."""
    capture_source = bool(cpu_policy and cpu_policy.get("schema") ==
                          render_tape_cpu_policy.SOURCE_SCHEMA)
    expected_mode = "capture-source" if capture_source else "chunk"
    if result.get("source_boundary_mode", "chunk") != expected_mode:
        return False
    if capture_source:
        try:
            cpu_policy = render_tape_cpu_policy.validate_policy(cpu_policy)
        except render_tape_cpu_policy.PolicyError:
            return False
        if reset != "cold-device" or cpu_policy["replay_route"] != route:
            return False
    requested_batch_cuts = result.get("audit_batch_cuts")
    if ("audit_batch_cuts" in result and
            (not isinstance(requested_batch_cuts, list) or requested_batch_cuts)):
        return False
    if ("source_boundary_audit" in result and
            not _source_boundary_audit_is_default(result["source_boundary_audit"])):
        return False
    if (result.get("schema") != "dxmt9.render_tape.experiment.v1"
            or result.get("status") != "complete"
            or result.get("backend") != "metal"
            or result.get("reset") != reset
            or result.get("replay_route") != route
            or result.get("prepare_count") != 1
            or result.get("audit_source_boundaries", False) is not False
            or result.get("audit_texture_input", False) is not False
            or result.get("pixel_deterministic") is not True):
        return False
    iterations = result.get("iterations")
    if not isinstance(iterations, list) or len(iterations) != warmup + repeat:
        return False
    reference = None
    for index, iteration in enumerate(iterations):
        if not isinstance(iteration, dict):
            return False
        evidence = iteration.get("provider_result")
        counters = iteration.get("path_counters")
        if (iteration.get("index") != index
                or iteration.get("warmup") is not (index < warmup)
                or iteration.get("reset_ok") is not True
                or not isinstance(evidence, dict)
                or not provider_oracle_accepts(
                    {**evidence, "provider_exit_code": 0}, non_degenerate)
                or not isinstance(counters, dict)
                or counters.get("gpu_errors") != 0):
            return False
        source_boundary_audit = evidence.get("source_boundary_audit")
        if not render_tape_gpu_policy.texture_input_audit_is_default(evidence):
            return False
        source_replay = evidence.get("source_replay")
        if capture_source:
            import render_tape_source_policy
            if not render_tape_source_policy.source_replay_accepts(
                    source_replay, cpu_policy, input_components):
                return False
            validity = evidence.get("validity", {})
            if (validity.get("oracle_mode") != "strict"
                    or validity.get("expected_digest_matched") is not True
                    or validity.get("source_oracle_exact_matched") is not True):
                return False
        elif ("source_replay" in evidence and
              not _source_replay_is_default(source_replay)):
            return False
        if ("source_boundary_audit" in evidence and
                not _source_boundary_audit_is_default(source_boundary_audit)):
            return False
        if (type(counters.get("encode_chunks")) is not int
                or counters["encode_chunks"] <= 0
                or (route == "compatibility" and
                    (counters.get("direct_lease_draws") != 0 or
                     counters.get("direct_leases") != 0))
                or (evidence["validity"].get("source_oracle_requested") is True
                    and evidence["validity"].get("source_oracle_matched") is not True)):
            return False
        direct = counters.get("direct_lease_draws")
        compatibility = counters.get("compatibility_draws")
        if any(type(value) is not int or value < 0 for value in (direct, compatibility)):
            return False
        observed = ("mixed" if compatibility else "direct-slot") if direct else (
            "compatibility" if compatibility else "no-qualified-draw-path")
        if iteration.get("observed_replay_path") != observed:
            return False
        for name in ("setup_ns", "replay_ns", "reset_ns"):
            value = iteration.get(name)
            if type(value) is not int or value < 0:
                return False
        signature = [(item["validity"]["output_sha256"],
                      item["validity"]["source_sha256"])
                     for item in evidence["intervals"]]
        if reference is not None and reference != signature:
            return False
        reference = signature
        if require_path:
            key = "direct_lease_draws" if route == "production-slot" else "compatibility_draws"
            count = counters.get(key)
            if type(count) is not int or count <= 0:
                return False
    return True


def experiment_images_stable(result: dict[str, Any]) -> bool:
    images = result.get("loaded_images")
    if not isinstance(images, list) or not images:
        return False
    for image in images:
        try:
            image_path = pathlib.Path(image["path"])
            if image_path.stat().st_size != image["bytes"] or digest(image_path) != image["sha256"]:
                return False
        except (OSError, KeyError, TypeError):
            return False
    return True


def experiment_readbacks_valid(
    result: dict[str, Any], readback_directory: pathlib.Path
) -> bool:
    """Validate provider-owned RGBA artifacts, including failed receipts."""
    if result.get("readback_artifacts_enabled") is not True:
        return False
    iterations = result.get("iterations")
    if not isinstance(iterations, list) or not iterations:
        return False
    try:
        root = readback_directory.resolve(strict=True)
    except OSError:
        return False
    if readback_directory.is_symlink() or not root.is_dir():
        return False
    expected_names: set[str] = set()
    for index, iteration in enumerate(iterations):
        if not isinstance(iteration, dict):
            return False
        artifacts = iteration.get("readback_artifacts")
        if not isinstance(artifacts, dict):
            return False
        provider_result = iteration.get("provider_result")
        provider_validity = (provider_result.get("validity")
                             if isinstance(provider_result, dict) else None)
        if not isinstance(provider_validity, dict):
            return False
        for kind in ("output", "source"):
            key = f"{kind}_rgba"
            evidence = artifacts.get(key)
            expected_name = f"iteration-{index:06d}-{kind}.rgba"
            expected_names.add(expected_name)
            if not isinstance(evidence, dict) or evidence.get("path") != expected_name:
                return False
            claimed_bytes = evidence.get("bytes")
            claimed_digest = evidence.get("sha256")
            if (type(claimed_bytes) is not int or claimed_bytes < 0
                    or not isinstance(claimed_digest, str)
                    or len(claimed_digest) != 64
                    or any(value not in "0123456789abcdef" for value in claimed_digest)):
                return False
            validity_bytes = provider_validity.get(f"{kind}_bytes")
            validity_digest = provider_validity.get(f"{kind}_sha256")
            if (provider_validity.get(f"{kind}_readback") is True and
                    (validity_bytes != claimed_bytes or
                     validity_digest != claimed_digest)):
                return False
            artifact = root / expected_name
            try:
                if artifact.is_symlink() or not artifact.is_file():
                    return False
                if artifact.resolve(strict=True).parent != root:
                    return False
                if artifact.stat().st_size != claimed_bytes or digest(artifact) != claimed_digest:
                    return False
            except OSError:
                return False
    try:
        actual_names = {path.name for path in root.iterdir()}
    except OSError:
        return False
    return actual_names == expected_names


def experiment_renderer_environment(environment: dict[str, str]) -> dict[str, str]:
    # Do not dump the process environment: it can contain account credentials.
    # Metal validation is inherited by the provider and changes both timing and
    # occasionally pixels, so DXMT-only provenance is insufficient.
    metal_keys = {"MTL_DEBUG_LAYER", "MTL_SHADER_VALIDATION",
                  "MTL_CAPTURE_ENABLED", "METAL_CAPTURE_ENABLED", "MTL_HUD_ENABLED"}
    return {key: value for key, value in sorted(environment.items())
            if key.startswith(("DXMT_", "DXMT9_")) or key in metal_keys}


def experiment_metal_validation_enabled(environment: dict[str, str]) -> bool:
    # Unknown nonempty settings are conservatively diagnostic, not silently
    # qualified as an uninstrumented policy measurement.
    return any(environment.get(key, "") not in ("", "0")
               for key in ("MTL_DEBUG_LAYER", "MTL_SHADER_VALIDATION"))


def experiment_inline_observer_enabled(environment: dict[str, str]) -> bool:
    # Pass-end readbacks do not change the requested source plan, but are still
    # perturbative diagnostics. Paths named "0" are real enabled directories,
    # not boolean flags. Do not qualify them through a forged diagnostic flag.
    return any(environment.get(key, "") != "" for key in (
        "DXMT9_DUMP_COLOR_ATTACHMENT_DIR", "DXMT9_DUMP_COLOR_ATTACHMENT_PATH",
        "DXMT9_DUMP_COLOR_ATTACHMENT_ROI_SUMMARY_PATH",
        "DXMT9_DUMP_DRAW_TEXTURE_DIR", "DXMT_DUMP_SHADER_DIR",
        "DXMT9_PROBE_VERTEX_POSITION_DIR", "DXMT9_PROBE_NO_CONTRACT_SOURCE_SHA256")) or (
            environment.get("DXMT9_EFFECT_DRAW_TRACE", "") not in ("", "0"))


def fixed_input_experiment(args: argparse.Namespace) -> int:
    provider_environment = dict(os.environ)
    source_boundary_mode = getattr(args, "source_boundary_mode", "chunk")
    capture_source = source_boundary_mode == "capture-source"
    source_plan_digest = getattr(args, "source_plan_sha256", None)
    if source_plan_digest is not None and not capture_source:
        raise SystemExit("source-plan SHA-256 requires capture-source mode")
    policy_values = {
        "draw_command_limit": getattr(args, "draw_command_limit", None),
        "slot_headroom_bytes": getattr(args, "slot_headroom_bytes", None),
        "aggregate_headroom_bytes": getattr(args, "aggregate_headroom_bytes", None),
    }
    policy_requested = (source_plan_digest is not None or
                        any(value is not None for value in policy_values.values()))
    gpu_policy = None
    if policy_requested:
        # Keep the experiment's policy grammar exactly coupled to the canonical
        # CPU policy module.  The values are cached by the provider at startup,
        # so each policy child must receive all three knobs explicitly.
        gpu_policy = render_tape_cpu_policy.validate_policy({
            "schema": (render_tape_cpu_policy.SOURCE_SCHEMA if capture_source else
                       render_tape_cpu_policy.SCHEMA),
            "replay_route": args.replay_route,
            **{key: value if value is not None else 0
               for key, value in policy_values.items()},
            **({"source_boundary_mode": source_boundary_mode,
                "source_plan_sha256": source_plan_digest} if capture_source else {}),
        })
        provider_environment.update({
            "DXMT9_DRAW_CHUNK_COMMAND_LIMIT": str(gpu_policy["draw_command_limit"]),
            "DXMT9_DIRECT_SLOT_HEADROOM_BYTES": str(gpu_policy["slot_headroom_bytes"]),
            "DXMT9_DIRECT_SLOT_AGGREGATE_HEADROOM_BYTES": str(
                gpu_policy["aggregate_headroom_bytes"]),
        })
    validation_diagnostic = experiment_metal_validation_enabled(provider_environment)
    audit_initial_seeds = getattr(args, "audit_initial_seeds", False)
    audit_fresh_seed_drains = getattr(args, "audit_fresh_seed_drains", False)
    audit_texture_prestage = getattr(args, "audit_texture_prestage", False)
    audit_source_boundaries = getattr(args, "audit_source_boundaries", False)
    audit_texture_input = getattr(args, "audit_texture_input", None)
    input_audit_request = None
    input_audit_bytes = None
    if audit_texture_input is not None:
        fields = audit_texture_input.split(":", 4)
        if (len(fields) != 5 or not all(re.fullmatch(r"[1-9][0-9]*", f) for f in fields[:3])
                or not re.fullmatch(r"[0-9a-f]{64}", fields[3]) or not fields[4]
                or int(fields[0]) > 0xffffffff or any(int(f) > 0xffffffffffffffff for f in fields[1:3])):
            raise SystemExit("texture input audit requires GEN:ID:AFTERSEQ:SHA256:PATH")
        if not capture_source or source_plan_digest is None or args.reset != "cold-device":
            raise SystemExit("texture input audit requires cold capture-source and exact source-plan SHA-256")
        path = pathlib.Path(fields[4])
        if path.is_symlink() or not path.is_file() or not 0 < path.stat().st_size <= 64 * 1024 * 1024:
            raise SystemExit("texture input must be a bounded regular file")
        with path.open("rb") as stream:
            input_audit_bytes = stream.read(64 * 1024 * 1024 + 1)
        if not 0 < len(input_audit_bytes) <= 64 * 1024 * 1024:
            raise SystemExit("texture input must be a bounded nonempty file")
        if hashlib.sha256(input_audit_bytes).hexdigest() != fields[3]:
            raise SystemExit("texture input SHA-256 mismatch")
        input_audit_request = {"generation": int(fields[0]), "object_id": int(fields[1]),
                               "after_captured_seq": int(fields[2]), "sha256": fields[3],
                               "bytes": len(input_audit_bytes), "source_path": str(path.resolve())}
    audit_batch_cuts = list(getattr(args, "audit_batch_cuts", []) or [])
    seed_diagnostic = audit_initial_seeds or audit_fresh_seed_drains or audit_texture_prestage
    if capture_source and (seed_diagnostic or audit_source_boundaries or
                           audit_batch_cuts or args.reset != "cold-device"):
        raise SystemExit(
            "normal capture-source replay requires cold-device, no diagnostics "
            "and an authenticated v2 plan for policy knobs"
        )
    if audit_batch_cuts and not audit_source_boundaries:
        raise SystemExit(
            "source-boundary batch cuts require --audit-source-boundaries"
        )
    diagnostic = (input_audit_request is not None or seed_diagnostic or audit_source_boundaries or validation_diagnostic
                  or experiment_inline_observer_enabled(provider_environment))
    if audit_source_boundaries and (
        seed_diagnostic or args.reset != "cold-device"
        or args.replay_route != "compatibility"
    ):
        raise SystemExit(
            "source-boundary audit requires a cold-device compatibility run "
            "and is exclusive with other audits"
        )
    if audit_texture_prestage and (audit_initial_seeds or audit_fresh_seed_drains):
        raise SystemExit("texture prestage must be a separate diagnostic")
    if seed_diagnostic and args.reset != "cold-device":
        raise SystemExit("seed diagnostics require cold-device reset")
    if args.warmup < 0 or args.repeat < 1 or args.warmup + args.repeat > MAX_REPLAY_RUNS:
        raise SystemExit(f"warmup + repeat must be in [1, {MAX_REPLAY_RUNS}]")
    if args.timeout_seconds <= 0:
        raise SystemExit("timeout must be positive")
    # Copy then reauthenticate: measurements never race a capture publisher or
    # read an input the user is modifying. Controlled-input failures retain
    # their partial evidence; ordinary runs keep their historical policy.
    with bundle_transaction(args.output_dir,
                            preserve_rejected=input_audit_request is not None,
                            failure_metadata={"lane": "texture-input-audit"}) as output:
        staged = output / "input"
        readback_directory = output / "readback"
        copy_input_owned(args.bundle.resolve(), staged)
        manifest, events, refs, blobs, identity, source = load_bundle(staged)
        if (audit_source_boundaries or capture_source) and identity is None:
            raise SystemExit(
                "capture source boundaries require authenticated components/identity.bin"
            )
        validator_digest = digest(args.validator.resolve())
        run_validator(args.validator, "validate", events, refs)
        if identity is not None:
            run_identity_validator(args.validator, events, identity, refs)
        provider = args.provider.resolve()
        candidate_digest = digest(provider)
        command = [str(provider), "experiment", str(events),
                   "--warmup", str(args.warmup), "--repeat", str(args.repeat),
                   "--reset", args.reset, "--replay-route", args.replay_route,
                   "--partition-mode", args.partition_mode,
                   "--readback-dir", str(readback_directory)]
        if audit_initial_seeds:
            command += ["--audit-initial-seeds"]
        if audit_fresh_seed_drains:
            command += ["--audit-fresh-seed-drains"]
        if audit_texture_prestage:
            command += ["--audit-texture-prestage"]
        if audit_source_boundaries:
            command += ["--audit-source-boundaries"]
        if capture_source:
            command += ["--source-boundary-mode", source_boundary_mode]
            if source_plan_digest is not None:
                command += ["--source-plan-sha256", source_plan_digest]
        if input_audit_request is not None:
            audit_path = output / "texture-input-audit.r32f"
            write_flushed(audit_path, input_audit_bytes)
            request = input_audit_request
            command += ["--audit-texture-input", ":".join((str(request["generation"]),
                str(request["object_id"]), str(request["after_captured_seq"]),
                request["sha256"], str(audit_path)))]
        for batch_cut in audit_batch_cuts:
            command += ["--audit-batch-cut", batch_cut]
        for blob in blobs:
            command += ["--blob", str(blob)]
        if identity is not None:
            command += ["--identity", str(identity)]
        if source is not None:
            command += ["--expected-source-rgba", str(source)]
        if "output_oracle" in manifest["components"]:
            command += ["--expected-rgba", str(events.parent / "output.rgba")]
        try:
            completed = subprocess.run(command, check=False, capture_output=True,
                                       text=True, timeout=args.timeout_seconds,
                                       env=provider_environment)
            stdout, stderr = completed.stdout, completed.stderr
            exit_code = completed.returncode
        except subprocess.TimeoutExpired as error:
            stdout = error.stdout or b""
            stderr = error.stderr or b""
            stdout = stdout.decode(errors="replace") if isinstance(stdout, bytes) else stdout
            stderr = stderr.decode(errors="replace") if isinstance(stderr, bytes) else stderr
            exit_code = None
        write_flushed(output / "provider.stdout", stdout.encode())
        write_flushed(output / "provider.stderr", stderr.encode())
        try:
            result = json.loads(stdout)
            if not isinstance(result, dict):
                raise ValueError("non-object receipt")
        except (json.JSONDecodeError, ValueError):
            result = {"status": "timeout" if exit_code is None else "invalid-receipt"}
        images_stable = experiment_images_stable(result)
        readbacks_valid = experiment_readbacks_valid(result, readback_directory)
        accepted = (not diagnostic
                    and result.get("audit_initial_seeds", False) is False
                    and result.get("audit_fresh_seed_drains", False) is False
                    and result.get("audit_texture_prestage", False) is False
                    and result.get("audit_source_boundaries", False) is False
                    and result.get("audit_texture_input", False) is False
                    and exit_code == 0 and digest(provider) == candidate_digest
                    and digest(args.validator.resolve()) == validator_digest
                    and images_stable and readbacks_valid
                    and experiment_accepts(
                        result, warmup=args.warmup, repeat=args.repeat,
                        reset=args.reset, route=args.replay_route,
                        require_path=args.require_path,
                        non_degenerate=bundle_requires_non_degenerate(manifest),
                        cpu_policy=gpu_policy, input_components=manifest["components"]))
        receipt = {
            "schema": "dxmt9.render_tape.experiment_artifact.v1",
            "accepted": accepted, "provider_exit_code": exit_code,
            "diagnostic": diagnostic,
            "source_boundary_mode": source_boundary_mode,
            "audit_source_boundaries": audit_source_boundaries,
            "audit_texture_input": input_audit_request is not None,
            "audit_batch_cuts": audit_batch_cuts,
            "metal_validation_diagnostic": validation_diagnostic,
            "loaded_images_stable": images_stable,
            "readback_artifacts_valid": readbacks_valid,
            "provider": {"path": str(provider), "sha256": candidate_digest},
            "provider_command": command,
            "validator": {"path": str(args.validator.resolve()),
                          "sha256": validator_digest},
            "input_components": manifest["components"],
            "input_scope": manifest.get("scope", {}),
            "renderer_environment": experiment_renderer_environment(provider_environment),
            "forced_environment": {
                "DXMT_DEBUG_DISABLE_SHADER_ARCHIVE": "1",
                "DXMT9_PREWARM": "disabled",
                "DXMT9_RENDER_PARTITION_MODE": args.partition_mode,
                "DXMT_PERF_COUNTERS": "1",
                **({
                    "DXMT9_DRAW_CHUNK_COMMAND_LIMIT": str(gpu_policy["draw_command_limit"]),
                    "DXMT9_DIRECT_SLOT_HEADROOM_BYTES": str(gpu_policy["slot_headroom_bytes"]),
                    "DXMT9_DIRECT_SLOT_AGGREGATE_HEADROOM_BYTES": str(
                        gpu_policy["aggregate_headroom_bytes"]),
                } if gpu_policy is not None else {}),
            },
            "requested": {"warmup": args.warmup, "repeat": args.repeat,
                          "reset": args.reset, "replay_route": args.replay_route,
                          "partition_mode": args.partition_mode,
                          "audit_initial_seeds": audit_initial_seeds,
                          "audit_fresh_seed_drains": audit_fresh_seed_drains,
                          "audit_texture_prestage": audit_texture_prestage,
                          "audit_source_boundaries": audit_source_boundaries,
                          "audit_texture_input": input_audit_request,
                          "source_boundary_mode": source_boundary_mode,
                          "source_plan_sha256": source_plan_digest,
                          "audit_batch_cuts": audit_batch_cuts,
                          "require_path": args.require_path,
                          "cpu_policy": gpu_policy,
                          "cpu_policy_sha256": (
                              render_tape_cpu_policy.policy_digest(gpu_policy)
                              if gpu_policy is not None else None),
                          "policy_lane": policy_requested},
            "result": result,
        }
        write_flushed(output / "result.json",
                      (json.dumps(receipt, indent=2, sort_keys=True) + "\n").encode())
    print(json.dumps({"accepted": accepted, "output": str(args.output_dir.resolve()),
                      "status": result.get("status")}, sort_keys=True))
    return 0 if accepted else 1


def _cpu_general_input_counts(input_inspection: dict[str, Any]) -> dict[str, int] | None:
    """Project only authenticated inspect metadata into the general counters."""
    if (input_inspection.get("valid") is not True
            or input_inspection.get("schema") != "dxmt9.render_tape.v2"):
        return None
    aliases = {
        "events": ("events",),
        "objects": ("objects", "defines"),
        "mutations": ("mutations",),
        "chunks": ("chunks",),
        "records": ("records",),
        "draws": ("draws",),
    }
    values: dict[str, int] = {}
    for field, names in aliases.items():
        value = next((input_inspection.get(name) for name in names
                      if name in input_inspection), None)
        if type(value) is not int or value < 0:
            value = None
        if value is None and field == "draws":
            record_types = input_inspection.get("record_types")
            if record_types is None:
                grammar = input_inspection.get("grammar")
                record_types = (grammar.get("record_types")
                                if isinstance(grammar, dict) else None)
            if isinstance(record_types, dict):
                draw_names = ("draw_primitive", "draw_indexed_primitive",
                              "draw_primitive_up", "draw_indexed_primitive_up")
                draw_values = [record_types.get(name, 0) for name in draw_names]
                if all(type(item) is int and item >= 0 for item in draw_values):
                    value = sum(draw_values)
        if value is None:
            return None
        values[field] = value
    if any(values[field] > CPU_GENERAL_INPUT_LIMITS[field]
           for field in values):
        return None
    return values


def _cpu_general_mipmap_count(input_inspection: dict[str, Any]) -> int | None:
    """Read the authenticated GENERATE_MIPMAPS record count.

    This is intentionally sourced only from the validator's record-type
    histogram.  A candidate receipt must not be able to introduce or infer a
    mip-generation count from its own command counters.
    """
    record_types = input_inspection.get("record_types")
    if record_types is None:
        grammar = input_inspection.get("grammar")
        record_types = (grammar.get("record_types")
                        if isinstance(grammar, dict) else None)
    if not isinstance(record_types, dict):
        return None
    # The validator emits sparse histograms, so a valid histogram that omits
    # this zero-count record type authenticates as zero.  A malformed or
    # absent histogram remains fail-closed.
    value = record_types.get("generate_mipmaps", 0)
    return value if type(value) is int and value >= 0 else None


def _cpu_decimal_uint64(value: Any) -> bool:
    return (isinstance(value, str) and bool(value) and len(value) <= 20
            and (len(value) == 1 or value[0] != "0")
            and all("0" <= character <= "9" for character in value)
            and int(value) <= 0xffffffffffffffff)


def _cpu_general_next_work_event(event_types: list[Any], event: int) -> int | None:
    """Find the first authenticated non-destroy event after a command.

    General-frame completion is event-qualified.  ObjectDestroy events are
    deliberately skipped: the controller must withhold acknowledgment until
    all intervening destroys, immediately before the next piece of
    work (including PresentComplete).
    """
    return next(
        (index + 1 for index in range(event, len(event_types))
         if event_types[index] != 3),
        None,
    )


def _cpu_window_plan_rows(
    input_inspection: dict[str, Any], event_types: list[Any], *, require_bootstrap: bool,
) -> list[dict[str, Any]] | None:
    """Validate the authenticated CPU source-window witness.

    This is deliberately independent of the provider receipt.  In particular,
    the ``eligible`` bit is checked against the shape fields instead of being
    treated as authorization supplied by the candidate.
    """
    plan = input_inspection.get("cpu_window_plan")
    if not isinstance(plan, dict) or set(plan) != {
            "schema", "page_size", "bootstrap_state_only", "sources"}:
        return None
    if (plan.get("schema") != CPU_WINDOW_PLAN_SCHEMA
            or type(plan.get("page_size")) is not int
            or plan["page_size"] != CPU_WINDOW_PAGE_SIZE
            or type(plan.get("bootstrap_state_only")) is not bool
            or (require_bootstrap and plan["bootstrap_state_only"] is not True)):
        return None
    command_indices = input_inspection.get("command_event_indices")
    if (not isinstance(command_indices, list)
            or any(type(index) is not int or index < 0 for index in command_indices)
            or any(index >= len(event_types) or event_types[index] != 5
                   for index in command_indices)
            or len(set(command_indices)) != len(command_indices)):
        return None
    sources = plan.get("sources")
    if not isinstance(sources, list) or len(sources) != len(command_indices):
        return None
    row_keys = {
        "event", "record_count", "allowed_record_count", "partitioned",
        "lease_eligible", "coordinator_count", "ordered_control_count",
        "compatibility_record_count", "eligible", "lease_spans",
    }
    span_keys = {"first_record_index", "record_count", "owns_lease", "final_lease_span"}
    rows: list[dict[str, Any]] = []
    for row, command_index in zip(sources, command_indices):
        if not isinstance(row, dict) or set(row) != row_keys:
            return None
        if (type(row.get("event")) is not int
                or row.get("event") != command_index + 1):
            return None
        for field in ("record_count", "allowed_record_count", "coordinator_count",
                      "ordered_control_count", "compatibility_record_count"):
            if type(row.get(field)) is not int or row[field] < 0:
                return None
        if (row["record_count"] > 0xffffffff
                or row["allowed_record_count"] > row["record_count"]):
            return None
        for field in ("partitioned", "lease_eligible", "eligible"):
            if type(row.get(field)) is not bool:
                return None
        spans = row.get("lease_spans")
        if not isinstance(spans, list):
            return None
        previous_end = 0
        for span in spans:
            if not isinstance(span, dict) or set(span) != span_keys:
                return None
            first = span.get("first_record_index")
            count = span.get("record_count")
            if (type(first) is not int or first < 0
                    or type(count) is not int or count <= 0
                    or first > 0xffffffff or count > 0xffffffff
                    or first < previous_end
                    or first + count > row["record_count"]
                    or first + count > 0x100000000):
                return None
            if (type(span.get("owns_lease")) is not bool
                    or type(span.get("final_lease_span")) is not bool):
                return None
            previous_end = first + count
        derived = (
            0 < row["record_count"] <= 64
            and row["allowed_record_count"] == row["record_count"]
            and row["partitioned"] is True
            and row["lease_eligible"] is True
            and row["coordinator_count"] == 0
            and row["ordered_control_count"] == 0
            and row["compatibility_record_count"] == 0
            and len(spans) == 1
            and spans[0]["first_record_index"] == 0
            and spans[0]["record_count"] == row["record_count"]
            and spans[0]["owns_lease"] is True
            and spans[0]["final_lease_span"] is True
        )
        if row["eligible"] is not derived:
            return None
        rows.append(row)
    return rows


def _cpu_window_schedule(
    event_types: list[int], plan_rows: list[dict[str, Any]],
) -> tuple[dict[int, int], list[dict[str, Any]], int, int, int] | None:
    """Derive the max-two FIFO schedule from event kinds and plan rows."""
    rows_by_event = {row["event"]: row for row in plan_rows}
    rows_by_ordinal = {ordinal: row for ordinal, row in enumerate(plan_rows, 1)}
    command_events = [index + 1 for index, kind in enumerate(event_types) if kind == 5]
    if [row["event"] for row in plan_rows] != command_events:
        return None
    pending: list[int] = []
    # Keys are source ordinals in command-event order.  The provider's queue
    # sequence IDs are intentionally not assumed to be contiguous or to start
    # at one; those are bound only after the trace is validated.
    releases: dict[int, int] = {}
    destroy_windows: list[dict[str, Any]] = []
    pending_peak = 0
    deferred_releases = 0
    destroys_with_pending = 0

    def drain(event: int) -> None:
        nonlocal deferred_releases
        while pending:
            submission = pending.pop(0)
            releases[submission] = event
            deferred_releases += 1

    submission_for_event = {
        event: submission for submission, event in enumerate(command_events, 1)
    }
    for event, kind in enumerate(event_types, 1):
        if kind == 3:
            if pending:
                destroys_with_pending += 1
                destroy_windows.append({
                    "event": event,
                    "pending_ordinals": list(pending),
                })
            continue
        if kind == 5:
            row = rows_by_event[event]
            if row["eligible"]:
                # A lease-eligible incoming source may overlap only an epoch
                # whose existing owners are all independently eligible.  An
                # ineligible source may remain pending across Destroy, but it
                # must be drained before any eligible source is admitted.
                if any(not rows_by_ordinal[ordinal]["eligible"]
                       for ordinal in pending):
                    drain(event)
                elif len(pending) >= 2:
                    submission = pending.pop(0)
                    releases[submission] = event
                    deferred_releases += 1
            else:
                drain(event)
            pending.append(submission_for_event[event])
            pending_peak = max(pending_peak, len(pending))
            continue
        drain(event)
    if pending:
        # before-next-work has no valid completion point after the final event.
        return None
    return releases, destroy_windows, pending_peak, deferred_releases, destroys_with_pending


def _cpu_general_counts_match(
    actual: Any, expected: dict[str, int]
) -> bool:
    """Compare authenticated general input counts without bool/int coercion."""
    return bool(
        isinstance(actual, dict)
        and actual.keys() == expected.keys()
        and all(
            type(actual[field]) is int and actual[field] == expected[field]
            for field in expected
        )
    )


def _cpu_native_time_valid(result: dict[str, Any], iteration: dict[str, Any]) -> bool:
    if "native_cpu_ns" not in iteration:
        return False
    value = iteration["native_cpu_ns"]
    if result.get("native_cpu_clock") == "unavailable":
        return value is None
    return (result.get("native_cpu_clock") == "process"
            and type(value) is int and value >= 0)


def _cpu_diagnostic_accounting_accepts(iteration: dict[str, Any]) -> bool:
    ledger = iteration.get("diagnostic_accounting")
    names = {"bookkeeping", "fixture_setup", "admission", "harness_setup",
             "object_setup", "mutation", "replay", "publication", "completion",
             "cleanup", "fixture_teardown"}
    if (not isinstance(ledger, dict) or ledger.keys() != {"valid", "wall_ns", "cpu_ns", "phases"}
            or ledger["valid"] is not True or not isinstance(ledger["phases"], dict)
            or ledger["phases"].keys() != names):
        return False
    for row in ledger["phases"].values():
        if (not isinstance(row, dict) or row.keys() != {"wall_ns", "cpu_ns", "entries"}
                or any(type(value) is not int or value < 0 for value in row.values())):
            return False
    for clock, outer in (("wall_ns", "elapsed_ns"), ("cpu_ns", "native_cpu_ns")):
        if (type(ledger.get(clock)) is not int or ledger[clock] <= 0
                or type(iteration.get(outer)) is not int or ledger[clock] > iteration[outer]
                or sum(row[clock] for row in ledger["phases"].values()) != ledger[clock]):
            return False
    return True


def _cpu_general_frame_accepts(
    result: dict[str, Any], *, repeat: int, route: str, require_path: bool,
    completion: str, resource_backend: str, publication: str, fault: str,
    completion_plan: Optional[dict[str, Any]],
    input_inspection: dict[str, Any] | None,
    max_pending: int | None = None,
    source_binding: dict[str, Any] | None = None,
    cpu_policy: dict[str, Any] | None = None,
    measurement: bool = False,
    diagnostic: bool = False,
) -> bool:
    """Strict acceptance for the complete-frame CPU resource lane.

    This lane has a deliberately separate contract from the bounded32-event
    oracle.  In particular, it does not infer completion from a candidate
    receipt, and it never treats physical command/submission topology as a
    semantic equality claim.
    """
    if diagnostic and not measurement:
        return False
    if (not isinstance(input_inspection, dict) or completion_plan is not None
            or completion not in ("after-command", "before-next-work")
            or publication != "per-command"
            or resource_backend != "cpu-metal-resources" or fault != "none"):
        return False
    if max_pending is not None and type(max_pending) is not int:
        return False
    effective_max_pending = 1 if max_pending is None else max_pending
    if effective_max_pending not in (1, 2):
        return False
    if effective_max_pending == 2 and (
            completion != "before-next-work" or source_binding is not None):
        return False
    if effective_max_pending == 2:
        inspected_event_types = input_inspection.get("event_types")
        if not isinstance(inspected_event_types, list):
            return False
        plan_rows = _cpu_window_plan_rows(
            input_inspection, inspected_event_types,
            require_bootstrap=True)
        if plan_rows is None:
            return False
    else:
        plan_rows = None
    if source_binding is not None:
        if (completion != "before-next-work" or repeat < 2
                or result.get("max_pending") != 1
                or result.get("hold_safe_buffer_mutation") is not True
                or result.get("source_projection_sha256") != source_binding.get("projection_sha256")
                or result.get("source_oracle_sha256") != source_binding.get("oracle_sha256")):
            return False
        from render_tape_cpu_source_projection import validate_cpu_source_closures
        try:
            if validate_cpu_source_closures(result, input_inspection) != source_binding.get("sources"):
                return False
        except ValueError:
            return False
    elif (result.get("hold_safe_buffer_mutation", False) is not False
          or result.get("source_projection_sha256") is not None
          or result.get("source_oracle_sha256") is not None):
        return False
    expected_counts = _cpu_general_input_counts(input_inspection)
    expected_mipmap_count = _cpu_general_mipmap_count(input_inspection)
    if expected_counts is None or expected_mipmap_count is None:
        return False
    if measurement or cpu_policy is not None:
        if (completion != "after-command" or effective_max_pending != 1
                or source_binding is not None or cpu_policy is None):
            return False
        try:
            cpu_policy = render_tape_cpu_policy.validate_policy(cpu_policy)
            actual_policy = render_tape_cpu_policy.validate_policy(result.get("cpu_policy"))
        except ValueError:
            return False
        if (cpu_policy["replay_route"] != route
                or actual_policy != cpu_policy
                or result.get("run_mode") != ("diagnose" if diagnostic else "measure" if measurement else "verify")
                or result.get("phase_accounting") !=
                   ("exclusive-diagnostic-wall-process" if diagnostic else
                    "iteration-only" if measurement else "scoped-observations")
                or result.get("runtime_perf_counters") is not True):
            return False
    elif result.get("run_mode", "verify") != "verify":
        return False
    policy = result.get("effective_cpu_policy")
    expected_policy = {
        "reset": "fresh-device", "cpu_ready_arena": False,
        "draw_command_limit": cpu_policy["draw_command_limit"] if cpu_policy else 0,
        "draw_payload_limit": 0,
        "early_prefix": False, "gpu_submit": False}
    if (not isinstance(policy, dict) or policy.keys() != expected_policy.keys()
            or any(type(policy[key]) is not type(value) or policy[key] != value
                   for key, value in expected_policy.items())):
        return False
    event_types = input_inspection.get("event_types")
    if (not isinstance(event_types, list)
            or len(event_types) != expected_counts["events"]
            or any(type(kind) is not int or not 1 <= kind <= 7
                   for kind in event_types)):
        return False
    # The native general executor authenticates this once at import/admission
    # and reports it at result level.  Accept an optional per-iteration copy as
    # a stronger self-contained receipt, but never accept a mismatching copy.
    if not _cpu_general_counts_match(result.get("input_counts"), expected_counts):
        return False
    if (result.get("schema") != "dxmt9.render_tape.cpu_experiment.v1"
            or result.get("grammar") != "general-frame"
            or result.get("semantic_projection") != "final-slot-general-v2"
            or result.get("attachment_contents") != "backend-command-owned-v1"
            or result.get("gpu_readback") is not False
            or result.get("reset_policy") != "fresh-device"
            or result.get("cache_policy") != "fresh-runtime"
            or result.get("shared_native_resource_device") is not True
            or result.get("backend") != resource_backend
            or result.get("resource_execution") != "cpu-visible-metal-no-gpu-submit"
            or result.get("status") != "complete"
            or result.get("replay_route") != route
            or result.get("completion") != completion
            or result.get("publication") != publication
            or result.get("fault") != fault
            or type(result.get("repeat")) is not int or result["repeat"] != repeat
            or result.get("completion_plan_sha256") is not None
            or result.get("immutable_input_owned") is not True
            or result.get("deterministic") is not True):
        return False
    if (max_pending is not None
            and (type(result.get("max_pending")) is not int
                 or result.get("max_pending") != max_pending)):
        return False
    if effective_max_pending == 2:
        runtime = result.get("window_runtime")
        if (not isinstance(runtime, dict)
                or set(runtime) != {"page_size", "offload", "cpu_ready_arena",
                                    "workers_disabled"}
                or type(runtime.get("page_size")) is not int
                or runtime.get("page_size") != CPU_WINDOW_PAGE_SIZE
                or runtime.get("offload") is not False
                or runtime.get("cpu_ready_arena") is not False
                or runtime.get("workers_disabled") is not True):
            return False
        if result.get("hold_safe_buffer_mutation") is not False:
            return False
    iterations = result.get("iterations")
    if not isinstance(iterations, list) or len(iterations) != repeat:
        return False
    reference_digest = None
    for index, iteration in enumerate(iterations):
        if (not isinstance(iteration, dict)
                or type(iteration.get("index")) is not int
                or iteration.get("index") != index
                or iteration.get("complete") is not True
                or iteration.get("reset_ok") is not True
                or iteration.get("fault_triggered") is not False
                or iteration.get("failure") != ""):
            return False
        if ("input_counts" in iteration
                and not _cpu_general_counts_match(
                    iteration.get("input_counts"), expected_counts)):
            return False
        phase_ns = iteration.get("phase_ns")
        if (not isinstance(phase_ns, dict)
                or set(phase_ns) != {"setup", "replay", "observe", "completion",
                                     "cleanup"}
                or any(type(value) is not int or value < 0
                       for value in phase_ns.values())):
            return False
        if not _cpu_native_time_valid(result, iteration):
            return False
        if measurement:
            if (any(phase_ns.values())
                    or iteration.get("full_oracle_calls") != 0
                    or iteration.get("semantic_digest") != "0"
                    or iteration.get("draw_observations") != []
                    or iteration.get("logical_resource_oracle_complete") is not False
                    or iteration.get("physical_texture_oracle") is not False
                    or iteration.get("buffer_backing_oracle_complete") is not False
                    or iteration.get("buffer_backing_observed") is not False
                    or any(iteration.get(key) != 0 for key in (
                        "buffer_snapshot_observations", "buffer_live_binding_observations",
                        "buffer_binding_observations"))):
                return False
        elif (iteration.get("logical_resource_oracle_complete") is not True
                or iteration.get("physical_texture_oracle") is not False
                or iteration.get("buffer_backing_oracle_complete") is not True
                or iteration.get("buffer_backing_observed") is not True
                or iteration.get("buffer_backing_oracle") != "real-metal-cpu-visible"):
            return False
        snapshots = iteration.get("buffer_snapshot_observations")
        live_bindings = iteration.get("buffer_live_binding_observations")
        bindings = iteration.get("buffer_binding_observations")
        if not measurement and (type(snapshots) is not int or snapshots < 0
                or type(live_bindings) is not int or live_bindings < 0
                or type(bindings) is not int or bindings <= 0
                or bindings != snapshots + live_bindings
                or bindings > expected_counts["draws"] * 17):
            return False
        counters = ("command_count", "draw_count", "direct_draws",
                    "compatibility_draws", "submitted", "completed", "reclaimed",
                    "resources_created", "resources_released", "elapsed_ns",
                    "pending_peak", "deferred_releases", "destroys_with_pending",
                    "retained_snapshots_after_destroy", "mipmap_command_count")
        if diagnostic:
            if not _cpu_diagnostic_accounting_accepts(iteration):
                return False
        elif "diagnostic_accounting" in iteration:
            return False
        if any(type(iteration.get(field)) is not int or iteration[field] < 0
               for field in counters):
            return False
        if iteration["mipmap_command_count"] != expected_mipmap_count:
            return False
        # The pinned policy emits one exact source per command and withholds
        # its acknowledgment across at most one intervening work boundary.
        if effective_max_pending == 1 and iteration["pending_peak"] != 1:
            return False
        schedule = None
        if effective_max_pending == 2:
            schedule = _cpu_window_schedule(event_types, plan_rows or [])
            if schedule is None:
                return False
            _, expected_windows, expected_peak, expected_deferred, expected_destroys = schedule
            if (expected_peak != 2
                    or iteration["pending_peak"] != expected_peak
                    or iteration["deferred_releases"] != expected_deferred
                    or iteration["destroys_with_pending"] != expected_destroys):
                return False
            windows = iteration.get("destroy_windows")
            if not isinstance(windows, list) or len(windows) != len(expected_windows):
                return False
            for actual_window, expected_window in zip(windows, expected_windows):
                pending_submissions = (actual_window.get("pending_submissions")
                                       if isinstance(actual_window, dict) else None)
                if (not isinstance(actual_window, dict)
                        or set(actual_window) != {"event", "pending_submissions",
                                                 "owners_checked", "retention_valid"}
                        or type(actual_window.get("event")) is not int
                        or actual_window.get("event") != expected_window["event"]
                        or (not isinstance(pending_submissions, list)
                            or any(type(value) is not int or value <= 0
                                   for value in pending_submissions))
                        or type(actual_window.get("owners_checked")) is not int
                        or actual_window["owners_checked"] !=
                           len(expected_window["pending_ordinals"])
                        or actual_window.get("retention_valid") is not True):
                    return False
        if source_binding is not None and (
                type(iteration.get("source_oracle_checked_sources")) is not int
                or iteration["source_oracle_checked_sources"] != iteration["submitted"]):
            return False
        if completion == "before-next-work" and iteration["deferred_releases"] <= 0:
            return False
        if (not iteration["command_count"] or not iteration["submitted"]
                or iteration["submitted"] != iteration["completed"]
                or iteration["completed"] != iteration["reclaimed"]
                or iteration["resources_created"] != iteration["resources_released"]
                or iteration["resources_created"] != expected_counts["objects"]
                or iteration["direct_draws"] + iteration["compatibility_draws"]
                   != iteration["draw_count"]
                or iteration["draw_count"] != expected_counts["draws"]
                or (route == "compatibility" and iteration["direct_draws"] != 0)):
            return False
        if cpu_policy is not None:
            policy_counters = ("source_summary_commands", "source_summary_draws",
                               "slot_provisioned", "slot_provision_reused",
                               "slot_provision_failed", "capacity_lease_denied",
                               "draw_limit_publishes", "full_oracle_calls")
            if (any(type(iteration.get(key)) is not int or iteration[key] < 0
                    for key in policy_counters)
                    or iteration["source_summary_draws"] != expected_counts["draws"]
                    or (not measurement and iteration["full_oracle_calls"] == 0)):
                return False
        if not measurement and not _cpu_decimal_uint64(iteration.get("semantic_digest")):
            return False
        if (source_binding is not None and iteration["semantic_digest"] !=
                source_binding.get("reference_semantic_digest")):
            return False
        if reference_digest is not None and reference_digest != iteration["semantic_digest"]:
            return False
        reference_digest = iteration["semantic_digest"]
        trace = iteration.get("trace")
        if not isinstance(trace, list) or not 1 <= len(trace) == iteration["submitted"] <= 2048:
            return False
        if effective_max_pending == 2 and len(trace) != len(plan_rows or []):
            return False
        previous_submission = previous_event_submission = 0
        source_keys: set[tuple[int, int]] = set()
        storage_ranges: list[tuple[int, int, int]] = []
        for row in trace:
            if (not isinstance(row, dict)
                    or type(row.get("iteration")) is not int
                    or row.get("iteration") != index + 1
                    or type(row.get("submission")) is not int
                    or row["submission"] <= previous_submission
                    or type(row.get("source")) is not int or row["source"] < 0
                    or type(row.get("source_generation")) is not int
                    or row["source_generation"] <= 0
                    or row.get("submitted") is not True
                    or row.get("completed") is not True):
                return False
            fields = ("first_event", "last_event", "event", "event_submission",
                      "released_at_event", "storage_first_page",
                      "storage_page_count", "storage_generation")
            if any(type(row.get(field)) is not int for field in fields):
                return False
            if (not 0 < row["first_event"] <= row["last_event"]
                    or row["last_event"] != row["event"]
                    or row["last_event"] > expected_counts["events"]
                    or any(kind != 5 for kind in input_inspection["event_types"][
                        row["first_event"] - 1:row["last_event"]])
                    or row["released_at_event"] < row["event"]):
                return False
            if effective_max_pending == 2:
                event_submission = row["event_submission"]
                if (not 1 <= event_submission <= len(plan_rows or [])
                        or row["event"] !=
                           (plan_rows or [])[event_submission - 1]["event"]):
                    return False
            if row["first_event"] != row["last_event"]:
                return False
            expected_release = row["event"]
            if completion == "before-next-work":
                expected_release = _cpu_general_next_work_event(
                    input_inspection["event_types"], row["event"])
                if expected_release is None:
                    return False
            if effective_max_pending == 2:
                if schedule is None:
                    return False
                expected_releases = schedule[0]
                expected_release = expected_releases.get(row["event_submission"])
                if expected_release is None:
                    return False
            held_event = row.get("held_mutation_event", 0)
            held_identity = row.get("held_mutation_identity")
            if type(held_event) is not int or held_event < 0:
                return False
            if held_event:
                if source_binding is None or held_event != expected_release:
                    return False
                mutations = [mutation for mutation in input_inspection.get("buffer_mutations", [])
                             if mutation.get("event") == held_event]
                if len(mutations) != 1:
                    return False
                mutation = mutations[0]
                if (held_identity != mutation.get("identity")
                        or mutation.get("mutation_kind") not in (1, 2)
                        or mutation.get("disposition") not in (0, 2)):
                    return False
                definitions = [definition for definition in input_inspection["buffer_definitions"]
                               if definition["identity"] == held_identity
                               and definition["event"] < held_event]
                if len(definitions) != 1 or definitions[0]["pool"] != 0:
                    return False
                referenced = held_identity in row["buffer_closure"]
                dynamic = bool(definitions[0]["usage"] & 0x200)
                if mutation["disposition"] == 0:
                    if referenced or dynamic:
                        return False
                elif not referenced or not dynamic:
                    return False
                expected_release = _cpu_general_next_work_event(event_types, held_event)
                if expected_release is None:
                    return False
            elif held_identity is not None:
                return False
            if row["released_at_event"] != expected_release:
                return False
            if effective_max_pending == 2 and (
                    row["first_event"] != row["event"]
                    or row["event"] not in {
                        index + 1 for index, kind in enumerate(event_types) if kind == 5
                    }):
                return False
            source_key = (row["source"], row["source_generation"])
            first, count, generation = (row["storage_first_page"],
                                         row["storage_page_count"],
                                         row["storage_generation"])
            if (source_key in source_keys or first < 0 or count <= 0
                    or generation <= 0 or first + count > 0x100000000
                    or any(generation == old_generation and first < old_end
                           and old_first < first + count
                           for old_first, old_end, old_generation in storage_ranges)):
                return False
            source_keys.add(source_key)
            storage_ranges.append((first, first + count, generation))
            if row["event_submission"] != (previous_event_submission + 1):
                return False
            previous_submission = row["submission"]
            previous_event_submission = row["event_submission"]
        if effective_max_pending == 2:
            submission_by_ordinal = [row["submission"] for row in trace]
            for actual_window, expected_window in zip(
                    iteration["destroy_windows"], schedule[1] if schedule else []):
                expected_submissions = [
                    submission_by_ordinal[ordinal - 1]
                    for ordinal in expected_window["pending_ordinals"]
                    if 0 < ordinal <= len(submission_by_ordinal)
                ]
                if (len(expected_submissions) != len(expected_window["pending_ordinals"])
                        or actual_window["pending_submissions"] != expected_submissions):
                    return False
        expected_deferred_releases = sum(
            row["released_at_event"] > row["event"] for row in trace
        )
        expected_destroy_events = {
            destroy_event
            for row in trace
            for destroy_event in range(row["event"] + 1,
                                       row["released_at_event"])
            if input_inspection["event_types"][destroy_event - 1] == 3
        }
        if (iteration["deferred_releases"] != expected_deferred_releases
                or iteration["destroys_with_pending"] !=
                   len(expected_destroy_events)):
            return False
        if (not expected_destroy_events
                and iteration["retained_snapshots_after_destroy"] != 0):
            return False
        if source_binding is not None:
            mutation_counters = ("deferred_buffer_mutations", "deferred_discard_mutations",
                                 "retained_buffers_after_mutation", "mutation_boundary_releases")
            if any(type(iteration.get(key)) is not int or iteration[key] < 0
                   for key in mutation_counters):
                return False
            held_events = {row["held_mutation_event"] for row in trace
                           if row.get("held_mutation_event", 0)}
            discards = sum(mutation["event"] in held_events and mutation["disposition"] == 2
                           for mutation in input_inspection["buffer_mutations"])
            boundary_releases = sum(event_types[row["released_at_event"] - 1] == 4
                                    for row in trace)
            if (iteration["deferred_buffer_mutations"] != len(held_events)
                    or iteration["deferred_discard_mutations"] != discards
                    or iteration["mutation_boundary_releases"] != boundary_releases
                    or (discards and iteration["retained_buffers_after_mutation"] < discards)
                    or (not held_events and iteration["retained_buffers_after_mutation"] != 0)):
                return False
        if measurement:
            if require_path and iteration[
                    "direct_draws" if route == "production-slot" else "compatibility_draws"] <= 0:
                return False
            continue
        observations = iteration.get("draw_observations")
        if not isinstance(observations, list) or len(observations) != iteration["draw_count"]:
            return False
        observation_failures: list[dict[str, Any]] = []
        if (_cpu_compare_draw_observations(
                {"iterations": [iteration]}, route=route, iteration=0,
                failures=observation_failures) is None):
            return False
        if source_binding is not None and [
                row["cumulative_semantic_digest"] for row in observations
        ] != source_binding.get("reference_draw_digests"):
            return False
        if require_path:
            key = "direct_draws" if route == "production-slot" else "compatibility_draws"
            if iteration[key] <= 0:
                return False
    return True


def cpu_experiment_accepts(result: dict[str, Any], *, repeat: int, route: str,
                           completion: str, require_path: bool,
                           completion_plan: Optional[dict[str, Any]] = None,
                           resource_backend: str = "cpu-no-gpu",
                           require_backing_oracle: bool = False,
                           fault: str = "none",
                           publication: str = "per-command",
                           input_inspection: dict[str, Any] | None = None,
                           grammar: str = "bounded",
                           max_pending: int | None = None,
                           source_binding: dict[str, Any] | None = None) -> bool:
    if grammar == "general-frame":
        return _cpu_general_frame_accepts(
            result, repeat=repeat, route=route, require_path=require_path,
            completion=completion, resource_backend=resource_backend,
            publication=publication, fault=fault,
            completion_plan=completion_plan, input_inspection=input_inspection,
            max_pending=max_pending,
            source_binding=source_binding)
    if source_binding is not None:
        return False
    if grammar != "bounded":
        return False
    # This projection comes from the independent validator over authenticated
    # input, never from the candidate's own claimed completion coordinates.
    if not isinstance(input_inspection, dict) or input_inspection.get("valid") is not True:
        return False
    event_types = input_inspection.get("event_types")
    dynamic_buffers = input_inspection.get("dynamic_buffers")
    if (not isinstance(event_types, list) or not 1 <= len(event_types) <= 32
            or any(type(kind) is not int or not 1 <= kind <= 7 for kind in event_types)
            or type(dynamic_buffers) is not int or dynamic_buffers not in (0, 1)
            or input_inspection.get("events") != len(event_types)):
        return False
    if (fault not in ("none", "managed-allocation")
            or publication not in ("per-command", "draw-island")):
        return False
    if (result.get("schema") != "dxmt9.render_tape.cpu_experiment.v1"
            or result.get("backend") != resource_backend
            or result.get("status") != "complete"
            or result.get("replay_route") != route
            or result.get("completion") != completion
            or result.get("publication") != publication
            or result.get("fault") != fault
            or type(result.get("repeat")) is not int or result["repeat"] != repeat
            or result.get("immutable_input_owned") is not True
            or result.get("deterministic") is not True):
        return False
    # Bounded experiments include nested observation/completion callbacks.
    # Their cold phase ledger pauses the outer phase: these wall components
    # are exclusive, not native thread CPU times or GPU/encode timings.
    if (result.get("phase_accounting") != "exclusive-wall"
            or result.get("clock") != "steady_clock"
            or result.get("reset_policy") != "fresh-runtime"
            or result.get("cache_policy") != "fresh-runtime"
            or result.get("shared_native_resource_device") is not
                (resource_backend == "cpu-metal-resources")
            or any(type(result.get(field)) is not int or result[field] < 0
                   for field in ("import_ns", "shared_device_setup_ns"))):
        return False
    if completion not in ("external", "after-command", "after-destroy"):
        return False
    if completion == "external":
        if completion_plan is None or completion_plan.get("events_count") != len(event_types):
            return False
        try:
            expected_plan_digest = hashlib.sha256(encode_cpu_completion_plan(
                completion_plan, events_sha256=completion_plan["events_sha256"],
                repeat=repeat, route=route)).hexdigest()
        except (KeyError, TypeError, ValueError):
            return False
    else:
        if completion_plan is not None:
            return False
        expected_plan_digest = None
    if ("completion_plan_sha256" not in result
            or result["completion_plan_sha256"] != expected_plan_digest):
        return False
    if resource_backend == "cpu-metal-resources":
        if result.get("resource_execution") != "cpu-visible-metal-no-gpu-submit":
            return False
    elif resource_backend != "cpu-no-gpu" or require_backing_oracle:
        return False
    iterations = result.get("iterations")
    if not isinstance(iterations, list) or len(iterations) != repeat:
        return False
    reference = None
    for index, iteration in enumerate(iterations):
        if (not isinstance(iteration, dict) or type(iteration.get("index")) is not int
                or iteration["index"] != index):
            return False
        phases = iteration.get("phase_ns")
        elapsed = iteration.get("elapsed_ns")
        if (not _cpu_native_time_valid(result, iteration)
                or type(elapsed) is not int or elapsed < 0
                or not isinstance(phases, dict)
                or set(phases) != {"setup", "replay", "observe", "completion",
                                   "cleanup"}
                or any(type(value) is not int or value < 0
                       for value in phases.values())
                or sum(phases.values()) > elapsed):
            return False
        if (iteration.get("reset_ok") is not True
                or iteration.get("fault_triggered") is not (fault != "none")
                or iteration.get("failure") != ""):
            return False
        if publication == "draw-island":
            for field in ("grouped_ranges", "grouped_events", "grouped_observed_draws",
                          "capacity_rejected", "populated_fallback", "capacity_rotated"):
                if type(iteration.get(field)) is not int or iteration[field] < 0:
                    return False
            if (iteration["grouped_ranges"] == 0
                    or iteration["grouped_events"] <= iteration["grouped_ranges"]
                    or iteration["grouped_observed_draws"] == 0
                    or iteration.get("grouped_publication_verified") is not True
                    or iteration["capacity_rotated"] != 0):
                return False
        if fault == "managed-allocation":
            if (resource_backend != "cpu-metal-resources"
                    or iteration.get("fault_receipt") !=
                       "managed-mutation-allocation-rejected-pre-effect-public-unlock"
                    or iteration.get("fault_preeffect_ownership_unchanged") is not True
                    or iteration.get("fault_retry_succeeded") is not True
                    or type(iteration.get("fault_mutation_applies")) is not int
                    or iteration["fault_mutation_applies"] != 1):
                return False
        if require_backing_oracle:
            if (iteration.get("buffer_backing_oracle") != "real-metal-cpu-visible"
                    or iteration.get("buffer_backing_observed") is not True
                    or iteration.get("buffer_backing_revision_advanced") is not True
                    or type(iteration.get("captured_A_required_retention")) is not bool):
                return False
            if (iteration["captured_A_required_retention"] and
                    (iteration.get("captured_A_backing_preserved") is not True
                     or iteration.get("distinct_dynamic_backings") is not True)):
                return False
        for field in ("command_count", "draw_count", "direct_draws", "compatibility_draws",
                      "submitted", "completed", "reclaimed", "resources_created",
                      "resources_released", "elapsed_ns"):
            if type(iteration.get(field)) is not int or iteration[field] < 0:
                return False
        if (not iteration["command_count"] or not iteration["submitted"]
                or iteration["submitted"] != iteration["completed"]
                or iteration["completed"] != iteration["reclaimed"]
                or iteration["resources_created"] != iteration["resources_released"]
                or iteration["direct_draws"] + iteration["compatibility_draws"]
                   != iteration["draw_count"]
                or (route == "compatibility" and iteration["direct_draws"] != 0)):
            return False
        if (publication == "draw-island"
                and iteration["grouped_observed_draws"] > iteration["draw_count"]):
            return False
        if (completion == "after-destroy" and iteration["resources_created"] > 0
                and iteration.get("retained_before_completion") is not True):
            return False
        trace = iteration.get("trace")
        if not isinstance(trace, list) or len(trace) != iteration["submitted"]:
            return False
        previous_submission = 0
        previous_first_event = 0
        previous_last_event = 0
        previous_event_submission = 0
        grouped_rows = 0
        grouped_events = 0
        source_keys = set()
        storage_ranges = []
        for row in trace:
            if (not isinstance(row, dict) or type(row.get("iteration")) is not int
                    or row["iteration"] != index + 1
                    or type(row.get("submission")) is not int
                    or row["submission"] <= previous_submission
                    or type(row.get("source")) is not int or row["source"] < 0
                    or type(row.get("source_generation")) is not int
                    or row["source_generation"] <= 0
                    or row.get("submitted") is not True
                    or row.get("completed") is not True):
                return False
            if (type(row.get("first_event")) is not int
                    or type(row.get("last_event")) is not int
                    or type(row.get("event")) is not int
                    or type(row.get("event_submission")) is not int
                    or not 0 < row["first_event"] <= row["last_event"]
                    or row["last_event"] != row["event"]
                    or row["last_event"] > len(event_types)
                    or any(kind != 5 for kind in
                           event_types[row["first_event"] - 1:row["last_event"]])):
                return False
            if any(type(row.get(field)) is not int for field in
                   ("released_at_event", "storage_first_page", "storage_page_count",
                    "storage_generation")):
                return False
            if not row["event"] <= row["released_at_event"] <= len(event_types):
                return False
            if completion != "external":
                if completion == "after-command":
                    expected_release = row["event"]
                else:
                    # Dynamic physical A is inspected through PresentComplete;
                    # the plain preset drains at Destroy, then at frame closure.
                    release_kinds = (7,) if dynamic_buffers else (3, 7)
                    expected_release = next(
                        (i + 1 for i in range(row["event"] - 1, len(event_types))
                         if event_types[i] in release_kinds), None)
                if row["released_at_event"] != expected_release:
                    return False
            source_key = (row["source"], row["source_generation"])
            first, count, generation = (row["storage_first_page"],
                                         row["storage_page_count"],
                                         row["storage_generation"])
            if (source_key in source_keys or first < 0 or count <= 0
                    or generation <= 0 or first + count > 0x100000000):
                return False
            if any(generation == old_generation and first < old_end
                   and old_first < first + count
                   for old_first, old_end, old_generation in storage_ranges):
                return False
            source_keys.add(source_key)
            storage_ranges.append((first, first + count, generation))
            # One ordinary command may publish several queue sources. A
            # grouped range, however, has exactly one actual source witness.
            same_singleton = (previous_first_event == previous_last_event
                              == row["first_event"] == row["last_event"])
            if ((row["first_event"] <= previous_last_event and not same_singleton)
                    or row["event_submission"] != (
                        previous_event_submission + 1 if same_singleton else 1)):
                return False
            if row["first_event"] != row["last_event"]:
                if publication != "draw-island":
                    return False
                grouped_rows += 1
                grouped_events += row["last_event"] - row["first_event"] + 1
            previous_first_event = row["first_event"]
            previous_last_event = row["last_event"]
            previous_event_submission = row["event_submission"]
            previous_submission = row["submission"]
        if (publication == "draw-island" and
                (grouped_rows != iteration["grouped_ranges"]
                 or grouped_events != iteration["grouped_events"])):
            return False
        if completion == "external":
            expected = [(row["event"], row["submission"], release["at_event"])
                        for row, release in zip(completion_plan["submits"],
                                                completion_plan["releases"])]
            actual = [(row.get("event"), row.get("event_submission"),
                       row.get("released_at_event")) for row in trace]
            if actual != expected:
                return False
        signature = iteration.get("semantic_digest")
        if not isinstance(signature, str) or not signature:
            return False
        if reference is not None and reference != signature:
            return False
        reference = signature
        key = "direct_draws" if route == "production-slot" else "compatibility_draws"
        if require_path and iteration[key] <= 0:
            return False
    return True


def encode_cpu_completion_plan(plan: dict[str, Any], *, events_sha256: str,
                               repeat: int, route: str) -> bytes:
    """Encode event-qualified actions; actual Queue identities are bound natively."""
    if (type(repeat) is not int or not 1 <= repeat <= 32
            or route not in ("production-slot", "compatibility")
            or not isinstance(events_sha256, str) or len(events_sha256) != 64):
        raise ValueError("invalid completion plan execution context")
    fields = {"schema", "events_sha256", "events_count", "repeat", "replay_route",
              "submits", "releases"}
    if (not isinstance(plan, dict) or set(plan) != fields
            or plan["schema"] != "dxmt9.render_tape.completion_plan.v1"
            or plan["events_sha256"] != events_sha256
            or plan["replay_route"] != route
            or type(plan["repeat"]) is not int or plan["repeat"] != repeat):
        raise ValueError("completion plan input/route/repeat binding mismatch")
    event_count = plan["events_count"]
    if type(event_count) is not int or not 1 <= event_count <= 0xffffffff:
        raise ValueError("invalid completion plan event count")
    submissions, releases = plan["submits"], plan["releases"]
    if (not isinstance(submissions, list) or not 1 <= len(submissions) <= 32
            or not isinstance(releases, list) or len(releases) != len(submissions)):
        raise ValueError("completion plan requires 1..32 matched submit/release rows")

    def checked_row(row, keys):
        if (not isinstance(row, dict) or set(row) != set(keys)
                or any(type(row[k]) is not int or not 1 <= row[k] <= 0xffffffff
                       for k in keys)):
            raise ValueError("invalid completion plan row")
        return tuple(row[k] for k in keys)

    submit_rows = [checked_row(row, ("event", "submission")) for row in submissions]
    release_rows = [checked_row(row, ("at_event", "event", "submission"))
                    for row in releases]
    previous_event, previous_local = 0, 0
    for event, local in submit_rows:
        if (event > event_count or event < previous_event
                or local != (previous_local + 1 if event == previous_event else 1)):
            raise ValueError("non-contiguous completion plan submission order")
        previous_event, previous_local = event, local
    previous_release = 0
    for (at_event, event, local), expected in zip(release_rows, submit_rows):
        if ((event, local) != expected or not event <= at_event <= event_count
                or at_event < previous_release):
            raise ValueError("completion plan must release every source in order")
        previous_release = at_event
    # Explicit reserved words and fixed offsets match the native POD schema.
    header = struct.pack("<Q10I32s", 0x31564e414c505844, 1, 80, 720,
                         event_count, len(submit_rows), len(release_rows), repeat,
                         1 if route == "production-slot" else 2, 0, 0,
                         bytes.fromhex(events_sha256))
    return (header + b"".join(struct.pack("<2I", *row) for row in submit_rows)
            + bytes((32 - len(submit_rows)) * 8)
            + b"".join(struct.pack("<3I", *row) for row in release_rows)
            + bytes((32 - len(release_rows)) * 12))


def decode_cpu_completion_plan_json(source: bytes) -> dict[str, Any]:
    def unique_object(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError("duplicate completion plan JSON key")
            result[key] = value
        return result
    if len(source) > 65536:
        raise ValueError("completion plan JSON exceeds 64 KiB")
    return json.loads(source, object_pairs_hook=unique_object)


# These are the policy-independent fields.  The remaining counters are
# retained in each comparison row as physical-layout diagnostics; they are
# intentionally not differentially compared because batching and source
# allocation can legitimately differ between routes.
CPU_COMPARE_LOGICAL_FIELDS = ("draw_count", "resources_created", "resources_released")
CPU_COMPARE_PHYSICAL_FIELDS = (
    "command_count", "submitted", "completed", "reclaimed",
)


def _cpu_compare_evidence(receipt: dict[str, Any]) -> dict[str, Any] | None:
    """Return a child result from either an artifact receipt or a bare result."""
    if not isinstance(receipt, dict):
        return None
    result = receipt.get("result")
    return result if isinstance(result, dict) else receipt


def _cpu_compare_failure(name: str, **fields: Any) -> dict[str, Any]:
    return {"name": name, **fields}


def _cpu_compare_draw_observations(
    result: dict[str, Any], *, route: str, iteration: int,
    failures: list[dict[str, Any]],
) -> list[dict[str, Any]] | None:
    evidence = result.get("iterations")
    current = (evidence[iteration] if isinstance(evidence, list)
               and iteration < len(evidence) else None)
    observations = (current.get("draw_observations")
                   if isinstance(current, dict) else None)
    draw_count = current.get("draw_count") if isinstance(current, dict) else None
    if not isinstance(observations, list) or type(draw_count) is not int:
        failures.append(_cpu_compare_failure(
            "draw-observations-invalid", route=route, iteration=iteration,
            reason="missing-or-invalid-list"))
        return None
    if len(observations) != draw_count:
        failures.append(_cpu_compare_failure(
            "draw-observations-count-mismatch", route=route, iteration=iteration,
            expected=draw_count, actual=len(observations)))
        return None
    trace = current.get("trace") if isinstance(current, dict) else None
    qualified_ranges: list[tuple[int, int]] = []
    if isinstance(trace, list):
        for row in trace:
            if not isinstance(row, dict):
                continue
            first_event = row.get("first_event")
            last_event = row.get("last_event")
            if (type(first_event) is int and type(last_event) is int
                    and 0 < first_event <= last_event):
                qualified_ranges.append((first_event, last_event))
    for ordinal, observation in enumerate(observations, 1):
        if not isinstance(observation, dict):
            failures.append(_cpu_compare_failure(
                "draw-observations-invalid", route=route, iteration=iteration,
                ordinal=ordinal, reason="non-object"))
            return None
        digest_value = observation.get("cumulative_semantic_digest")
        canonical_digest = (
            isinstance(digest_value, str) and bool(digest_value)
            and (len(digest_value) == 1 or digest_value[0] != "0")
            and len(digest_value) <= 20
            and all("0" <= character <= "9" for character in digest_value)
            and int(digest_value) <= 0xffffffffffffffff
        )
        if (type(observation.get("ordinal")) is not int
                or observation.get("ordinal") != ordinal
                or type(observation.get("publication_event")) is not int
                or observation["publication_event"] <= 0
                or not any(first <= observation["publication_event"] <= last
                           for first, last in qualified_ranges)
                or not canonical_digest):
            failures.append(_cpu_compare_failure(
                "draw-observations-invalid", route=route, iteration=iteration,
                ordinal=ordinal, actual=observation))
            return None
    return observations


def compare_cpu_experiment_results(
    reference: dict[str, Any],
    candidate: dict[str, Any],
    *,
    repeat: int,
    completion: str,
    publication: str,
    resource_backend: str,
    input_inspection: dict[str, Any],
    max_pending: int | None = None,
    require_backing_oracle: bool = False,
    reference_input_components: dict[str, Any] | None = None,
    candidate_input_components: dict[str, Any] | None = None,
    reference_binary_sha256: str | None = None,
    candidate_binary_sha256: str | None = None,
    grammar: str = "bounded",
    source_binding: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """Compare two already-authenticated CPU child receipts by logical evidence.

    The route-specific source IDs and storage layout are deliberately excluded:
    they describe physical queue allocation, while this gate compares the
    semantic replay and lifetime accounting.  This function performs no I/O and
    is consequently also the unit-test seam for corrupted receipts.
    """
    failures: list[dict[str, Any]] = []
    reference_result = _cpu_compare_evidence(reference)
    candidate_result = _cpu_compare_evidence(candidate)
    child_options = {
        "repeat": repeat,
        "completion": completion,
        "require_path": True,
        "resource_backend": resource_backend,
        "require_backing_oracle": require_backing_oracle,
        "publication": publication,
        "input_inspection": input_inspection,
        "grammar": grammar,
        "max_pending": max_pending,
            "source_binding": source_binding,
    }

    if reference_result is None:
        failures.append(_cpu_compare_failure("reference-receipt-missing"))
    if candidate_result is None:
        failures.append(_cpu_compare_failure("candidate-receipt-missing"))
    if reference_result is not None and not cpu_experiment_accepts(
        reference_result, route="compatibility", **child_options
    ):
        failures.append(_cpu_compare_failure("reference-child-rejected"))
    if candidate_result is not None and not cpu_experiment_accepts(
        candidate_result, route="production-slot", **child_options
    ):
        failures.append(_cpu_compare_failure("candidate-child-rejected"))

    expected_components = reference_input_components
    for label, receipt, components in (
        ("reference", reference, reference_input_components),
        ("candidate", candidate, candidate_input_components),
    ):
        if not isinstance(receipt, dict):
            continue
        if receipt.get("accepted") is not True:
            failures.append(_cpu_compare_failure(f"{label}-child-not-accepted"))
        if receipt.get("execution_valid") is not True:
            failures.append(_cpu_compare_failure(f"{label}-execution-invalid"))
        provider_exit_code = receipt.get("provider_exit_code")
        if type(provider_exit_code) is not int or provider_exit_code != 0:
            failures.append(_cpu_compare_failure(
                f"{label}-provider-failed", actual=provider_exit_code))
        renderer_environment = receipt.get("renderer_environment", {})
        if (not isinstance(renderer_environment, dict)
                or receipt.get("diagnostic", False) is not False
                or receipt.get("metal_validation_diagnostic", False) is not False
                or experiment_metal_validation_enabled(renderer_environment)):
            failures.append(_cpu_compare_failure(f"{label}-diagnostic-child"))
        if receipt.get("gpu_evidence") is not False:
            failures.append(_cpu_compare_failure(f"{label}-gpu-evidence-present"))
        wrapper_exit_code = receipt.get("compare_wrapper_exit_code")
        if type(wrapper_exit_code) is not int or wrapper_exit_code != 0:
            failures.append(_cpu_compare_failure(
                f"{label}-wrapper-failed", actual=wrapper_exit_code))
        if receipt.get("loaded_images_stable") is not True:
            failures.append(_cpu_compare_failure(
                f"{label}-provider-image-authentication-failed"))
        if receipt.get("input_inspection") != input_inspection:
            failures.append(_cpu_compare_failure(f"{label}-input-authentication-mismatch"))
        if receipt.get("source_binding") != source_binding:
            failures.append(_cpu_compare_failure(f"{label}-source-projection-mismatch"))
        if expected_components is not None and components != expected_components:
            failures.append(_cpu_compare_failure(f"{label}-input-identity-mismatch"))
        if (expected_components is not None
                and receipt.get("input_components") != expected_components):
            failures.append(_cpu_compare_failure(f"{label}-input-identity-mismatch"))
        if max_pending is not None:
            requested = receipt.get("requested")
            evidence = _cpu_compare_evidence(receipt)
            if (not isinstance(requested, dict)
                    or type(requested.get("max_pending")) is not int
                    or requested.get("max_pending") != max_pending
                    or not isinstance(evidence, dict)
                    or type(evidence.get("max_pending")) is not int
                    or evidence.get("max_pending") != max_pending):
                failures.append(_cpu_compare_failure(
                    f"{label}-completion-bound-mismatch",
                    expected=max_pending,
                    actual=(requested.get("max_pending")
                            if isinstance(requested, dict) else None)))
    if (reference_input_components is not None
            and candidate_input_components is not None
            and reference_input_components != candidate_input_components):
        failures.append(_cpu_compare_failure("child-input-identities-differ"))
    reference_provider = reference.get("provider") if isinstance(reference, dict) else None
    candidate_provider = candidate.get("provider") if isinstance(candidate, dict) else None
    if reference_binary_sha256 is not None:
        actual = (reference_provider.get("sha256")
                  if isinstance(reference_provider, dict) else None)
        if actual != reference_binary_sha256:
            failures.append(_cpu_compare_failure(
                "reference-binary-identity-mismatch",
                expected=reference_binary_sha256, actual=actual))
    if candidate_binary_sha256 is not None:
        actual = (candidate_provider.get("sha256")
                  if isinstance(candidate_provider, dict) else None)
        if actual != candidate_binary_sha256:
            failures.append(_cpu_compare_failure(
                "candidate-binary-identity-mismatch",
                expected=candidate_binary_sha256, actual=actual))

    iterations: list[dict[str, Any]] = []
    first_differing_draw: int | None = None
    reference_iterations = (reference_result.get("iterations", [])
                            if reference_result is not None else [])
    candidate_iterations = (candidate_result.get("iterations", [])
                            if candidate_result is not None else [])
    if not isinstance(reference_iterations, list):
        reference_iterations = []
    if not isinstance(candidate_iterations, list):
        candidate_iterations = []
    for index in range(max(len(reference_iterations), len(candidate_iterations), repeat)):
        reference_iteration = (reference_iterations[index]
                               if index < len(reference_iterations)
                               and isinstance(reference_iterations[index], dict)
                               else None)
        candidate_iteration = (candidate_iterations[index]
                               if index < len(candidate_iterations)
                               and isinstance(candidate_iterations[index], dict)
                               else None)
        row: dict[str, Any] = {"iteration": index}
        if reference_iteration is None or candidate_iteration is None:
            failures.append(_cpu_compare_failure(
                "iteration-missing", iteration=index,
                reference_present=reference_iteration is not None,
                candidate_present=candidate_iteration is not None))
            iterations.append(row)
            continue
        ref_digest = reference_iteration.get("semantic_digest")
        cand_digest = candidate_iteration.get("semantic_digest")
        row["reference_semantic_digest"] = ref_digest
        row["candidate_semantic_digest"] = cand_digest
        row["reference_counts"] = {
            field: reference_iteration.get(field)
            for field in (*CPU_COMPARE_LOGICAL_FIELDS, *CPU_COMPARE_PHYSICAL_FIELDS)
        }
        row["candidate_counts"] = {
            field: candidate_iteration.get(field)
            for field in (*CPU_COMPARE_LOGICAL_FIELDS, *CPU_COMPARE_PHYSICAL_FIELDS)
        }
        row["reference_physical_counts"] = {
            field: reference_iteration.get(field)
            for field in CPU_COMPARE_PHYSICAL_FIELDS
        }
        row["candidate_physical_counts"] = {
            field: candidate_iteration.get(field)
            for field in CPU_COMPARE_PHYSICAL_FIELDS
        }
        if ref_digest != cand_digest:
            failures.append(_cpu_compare_failure(
                "semantic-digest-mismatch", iteration=index,
                reference=ref_digest, candidate=cand_digest))
        for field in CPU_COMPARE_LOGICAL_FIELDS:
            reference_value = reference_iteration.get(field)
            candidate_value = candidate_iteration.get(field)
            if reference_value != candidate_value:
                failures.append(_cpu_compare_failure(
                    ("logical-draw-count-mismatch" if field == "draw_count"
                     else "logical-resource-count-mismatch"), iteration=index,
                    field=field, reference=reference_value, candidate=candidate_value))
        reference_observations = _cpu_compare_draw_observations(
            reference_result, route="compatibility", iteration=index,
            failures=failures)
        candidate_observations = _cpu_compare_draw_observations(
            candidate_result, route="production-slot", iteration=index,
            failures=failures)
        if reference_observations is not None:
            row["reference_draw_observations"] = reference_observations
        if candidate_observations is not None:
            row["candidate_draw_observations"] = candidate_observations
        if reference_observations is not None and candidate_observations is not None:
            for ordinal, (reference_observation, candidate_observation) in enumerate(
                zip(reference_observations, candidate_observations), 1
            ):
                reference_digest = reference_observation["cumulative_semantic_digest"]
                candidate_digest = candidate_observation["cumulative_semantic_digest"]
                if reference_digest != candidate_digest:
                    failures.append(_cpu_compare_failure(
                        "draw-observation-mismatch", iteration=index,
                        ordinal=ordinal, reference=reference_digest,
                        candidate=candidate_digest))
                    if first_differing_draw is None:
                        first_differing_draw = ordinal
        iterations.append(row)
    first_difference = next(
        (failure.get("iteration") for failure in failures
         if isinstance(failure.get("iteration"), int)),
        None,
    )
    return {
        "accepted": not failures,
        "failures": failures,
        "first_differing_iteration": first_difference,
        "first_differing_draw": first_differing_draw,
        "iterations": iterations,
        "reference_route": "compatibility",
        "candidate_route": "production-slot",
        "grammar": grammar,
        "physical_layout_compared": False,
    }


def cpu_compare_accepts(*args: Any, **kwargs: Any) -> bool:
    """Boolean projection of :func:`compare_cpu_experiment_results`."""
    return compare_cpu_experiment_results(*args, **kwargs).get("accepted") is True


def cpu_fixed_input_experiment(args: argparse.Namespace) -> int:
    provider_environment = dict(os.environ)
    validation_diagnostic = experiment_metal_validation_enabled(provider_environment)
    grammar = getattr(args, "grammar", "bounded")
    run_mode = getattr(args, "run_mode", "verify")
    policy_lane = bool(getattr(args, "policy_lane", False))
    source_mode = getattr(args, "source_boundary_mode", "chunk")
    source_plan_digest = getattr(args, "source_plan_sha256", None)
    capture_source = source_mode == "capture-source"
    if source_plan_digest is not None and not capture_source:
        raise SystemExit("source-plan SHA-256 requires capture-source mode")
    policy = render_tape_cpu_policy.validate_policy({
        "schema": (render_tape_cpu_policy.SOURCE_SCHEMA if capture_source else
                   render_tape_cpu_policy.SCHEMA),
        "replay_route": args.replay_route,
        "draw_command_limit": getattr(args, "draw_command_limit", 0),
        "slot_headroom_bytes": getattr(args, "slot_headroom_bytes", 0),
        "aggregate_headroom_bytes": getattr(args, "aggregate_headroom_bytes", 0),
        **({"source_boundary_mode": source_mode,
            "source_plan_sha256": source_plan_digest} if capture_source else {}),
    })
    if capture_source and not policy_lane:
        raise SystemExit("capture-source requires --policy-lane")
    if run_mode not in ("verify", "measure", "diagnose"):
        raise SystemExit("CPU run mode must be verify, measure or diagnose")
    if grammar != "general-frame" and run_mode != "verify":
        raise SystemExit("measure/diagnose mode requires general-frame")
    warmup = getattr(args, "warmup", 0)
    if warmup:
        raise SystemExit("warmup is owned by cpu-policy-compare, not cpu-experiment")
    has_policy = any(policy[key] for key in (
        "draw_command_limit", "slot_headroom_bytes", "aggregate_headroom_bytes"))
    if (run_mode != "verify" or has_policy) and not policy_lane:
        raise SystemExit("measurement and nondefault policies require --policy-lane")
    if policy_lane and (grammar != "general-frame" or args.completion != "after-command"
                        or args.max_pending != 1 or args.completion_plan is not None
                        or getattr(args, "source_projection", None) is not None):
        raise SystemExit("policy lane requires general-frame, after-command, max-pending=1")
    source_projection = getattr(args, "source_projection", None)
    if source_projection is not None and (
            grammar != "general-frame" or args.completion != "before-next-work"
            or args.publication != "per-command" or args.max_pending != 1
            or args.resource_backend != "cpu-metal-resources"
            or args.fault != "none" or args.completion_plan is not None
            or args.repeat < 2):
        raise SystemExit("source projection requires general-frame before-next-work, "
                         "per-command, cpu-metal-resources, max-pending=1, "
                         "repeat>=2 and no fault/completion plan")
    if grammar not in ("bounded", "general-frame"):
        raise SystemExit("CPU grammar must be bounded or general-frame")
    if not 1 <= args.repeat <= 32 or warmup < 0 or args.repeat + warmup > 32 or args.timeout_seconds <= 0:
        raise SystemExit("CPU repeat must be in [1,32] and timeout must be positive")
    if not 0 <= args.max_pending <= 32:
        raise SystemExit("CPU max-pending must be in [0,32]")
    if grammar == "general-frame":
        if args.resource_backend != "cpu-metal-resources":
            raise SystemExit("general-frame requires --resource-backend cpu-metal-resources")
        if args.max_pending not in (1, 2):
            raise SystemExit("general-frame max-pending must be 1 or 2")
        if args.max_pending == 2 and args.completion != "before-next-work":
            raise SystemExit("general-frame max-pending=2 requires --completion before-next-work")
        if args.completion_plan is not None:
            raise SystemExit("general-frame does not accept external completion plans")
        if (args.completion not in ("after-command", "before-next-work")
                or args.publication != "per-command"):
            raise SystemExit(
                "general-frame requires --completion after-command or "
                "before-next-work and --publication per-command")
        if args.fault != "none":
            raise SystemExit("general-frame requires --fault none")
    elif args.completion == "before-next-work":
        raise SystemExit("before-next-work requires --grammar general-frame")
    validator_digest = digest(args.validator)
    failure_metadata: dict[str, Any] = {}
    with bundle_transaction(
            args.output_dir, preserve_rejected=True,
            failure_metadata=failure_metadata) as output:
        staged = output / "input"
        copy_input_owned(args.bundle.resolve(), staged)
        manifest, events, refs, blobs, identity, _ = load_bundle(staged)
        input_inspection = run_validator(args.validator, "inspect", events, refs)
        if identity is not None:
            run_identity_validator(args.validator, events, identity, refs)
        provider = args.provider.resolve()
        binary_digest = digest(provider)
        if capture_source and identity is None:
            raise SystemExit("capture-source requires authenticated identity.bin")
        source_binding = None
        if source_projection is not None:
            try:
                source_binding = stage_cpu_source_projection(
                    source_projection, output / "source-projection", input_inspection,
                    manifest["components"], binary_digest, validator_digest)
            except (OSError, ValueError) as error:
                rejected = {"schema": "dxmt9.render_tape.cpu_experiment_artifact.v1",
                            "accepted": False, "provider_exit_code": None,
                            "gpu_evidence": False, "source_projection_rejected": str(error)}
                write_cpu_receipt(
                    output / "result.json",
                    (json.dumps(rejected, indent=2) + "\n").encode(),
                    failure_metadata)
                print(json.dumps({**rejected, "output": str(args.output_dir.resolve())}))
                return 1
        completion = "external" if args.completion_plan else args.completion
        plan_digest = None
        plan = None
        command = [str(provider), "--tape-experiment", str(events),
                   "--repeat", str(args.repeat), "--route", args.replay_route,
                   "--grammar", grammar,
                   "--resource-backend", args.resource_backend,
                   "--publication", args.publication,
                   "--completion", args.completion,
                   "--fault", args.fault, "--max-pending", str(args.max_pending)]
        if grammar == "general-frame":
            command += ["--run-mode", run_mode]
        if capture_source:
            command += render_tape_cpu_policy.source_policy_arguments(policy)
            command += ["--identity", str(identity)]
        if policy_lane:
            command += [
                        "--draw-command-limit", str(policy["draw_command_limit"]),
                        "--slot-headroom-bytes", str(policy["slot_headroom_bytes"]),
                        "--aggregate-headroom-bytes", str(policy["aggregate_headroom_bytes"])]
        if source_binding is not None:
            command += ["--source-oracle", str(output / "source-projection" / "source-oracle.bin"),
                        "--source-projection-sha256", source_binding["projection_sha256"]]
        if args.completion_plan:
            with args.completion_plan.open("rb") as plan_stream:
                plan_source = plan_stream.read(65537)
            plan = decode_cpu_completion_plan_json(plan_source)
            encoded_plan = encode_cpu_completion_plan(
                plan, events_sha256=digest(events), repeat=args.repeat,
                route=args.replay_route)
            write_flushed(output / "completion-plan.json", plan_source)
            plan_path = output / "completion-plan.bin"
            write_flushed(plan_path, encoded_plan)
            plan_digest = digest(plan_path)
            command += ["--completion-plan", str(plan_path)]
        for blob in blobs:
            command += ["--blob", str(blob)]
        try:
            completed = subprocess.run(command, check=False, capture_output=True,
                                       text=True, timeout=args.timeout_seconds,
                                       env=provider_environment)
            stdout, stderr, exit_code = completed.stdout, completed.stderr, completed.returncode
        except subprocess.TimeoutExpired as error:
            stdout, stderr, exit_code = error.stdout or b"", error.stderr or b"", None
            stdout = stdout.decode(errors="replace") if isinstance(stdout, bytes) else stdout
            stderr = stderr.decode(errors="replace") if isinstance(stderr, bytes) else stderr
        write_flushed(output / "provider.stdout", stdout.encode())
        write_flushed(output / "provider.stderr", stderr.encode())
        try:
            result = json.loads(stdout)
            if not isinstance(result, dict):
                raise ValueError("non-object receipt")
        except (json.JSONDecodeError, ValueError):
            result = {"status": "timeout" if exit_code is None else "invalid-receipt"}
        images_stable = experiment_images_stable(result)
        policy_valid = True
        if policy_lane:
            policy_valid = _cpu_general_frame_accepts(
                result, repeat=args.repeat, route=args.replay_route,
                require_path=args.require_path, completion=completion,
                resource_backend=args.resource_backend, publication=args.publication,
                fault=args.fault, completion_plan=plan,
                input_inspection=input_inspection, max_pending=args.max_pending,
                cpu_policy=policy, measurement=run_mode != "verify",
                diagnostic=run_mode == "diagnose")
            policy_valid = policy_valid and render_tape_cpu_policy.source_receipts_accept(
                {"result": result, "input_components": manifest["components"]}, policy)
        execution_valid = (exit_code == 0
                    and result.get("fault") == args.fault
                    and type(result.get("max_pending")) is int
                    and result.get("max_pending") == args.max_pending
                    and result.get("completion_plan_sha256") == plan_digest
                    and digest(provider) == binary_digest and images_stable
                    and digest(args.validator) == validator_digest
                    and policy_valid
                    and (policy_lane or cpu_experiment_accepts(
                        result, repeat=args.repeat, route=args.replay_route,
                        completion=completion, require_path=args.require_path,
                        completion_plan=plan, resource_backend=args.resource_backend,
                        require_backing_oracle=args.require_backing_oracle,
                        fault=args.fault, publication=args.publication,
                        input_inspection=input_inspection, grammar=grammar,
                        max_pending=args.max_pending,
                        source_binding=source_binding)))
        accepted = (run_mode == "verify" and args.fault == "none"
                    and execution_valid and not validation_diagnostic)
        fault_oracle_passed = args.fault == "managed-allocation" and execution_valid
        receipt = {"schema": "dxmt9.render_tape.cpu_experiment_artifact.v1",
                   "accepted": accepted, "provider_exit_code": exit_code,
                   "diagnostic": args.fault != "none" or validation_diagnostic or run_mode == "diagnose",
                   "metal_validation_diagnostic": validation_diagnostic,
                   "execution_valid": execution_valid,
                   "semantic_qualified": accepted,
                   "renderer_environment": experiment_renderer_environment(provider_environment),
                   "fault_oracle_passed": fault_oracle_passed,
                   "loaded_images_stable": images_stable,
                   "provider": {"path": str(provider), "sha256": binary_digest},
                   "validator": {"path": str(args.validator.resolve()), "sha256": validator_digest},
                   "input_components": manifest["components"],
                   "input_inspection": input_inspection,
                   "source_binding": source_binding,
                   "forced_environment": {"DXMT9_OFFLOAD_COMMIT_REPLAY":
                                              "1" if args.fault == "managed-allocation" else "0",
                                          "DXMT_PERF_COUNTERS": "1",
                                          "DXMT9_DISABLE_PRESENT_BOUNDARY": "1",
                                          "DXMT9_SYNC_PRESENT_FLUSH": "0",
                                          **({"DXMT9_DRAW_CHUNK_COMMAND_LIMIT": str(policy["draw_command_limit"]),
                                              "DXMT9_DIRECT_SLOT_HEADROOM_BYTES": str(policy["slot_headroom_bytes"]),
                                              "DXMT9_DIRECT_SLOT_AGGREGATE_HEADROOM_BYTES": str(policy["aggregate_headroom_bytes"])}
                                             if policy_lane else {}),
                                          **({"DXMT9_DIRECT_SLOT_HEADROOM_BYTES": "0",
                                              "DXMT9_DIRECT_SLOT_AGGREGATE_HEADROOM_BYTES": "0"}
                                             if args.publication == "draw-island" else {})},
                   "requested": {"repeat": args.repeat, "replay_route": args.replay_route,
                                 "grammar": grammar,
                                 "completion": completion, "completion_plan_sha256": plan_digest,
                                 "require_path": args.require_path,
                                 "resource_backend": args.resource_backend,
                                 "publication": args.publication,
                                 "require_backing_oracle": args.require_backing_oracle,
                                 "fault": args.fault, "max_pending": args.max_pending,
                                 "run_mode": run_mode, "warmup": warmup,
                                 "cpu_policy": policy, "policy_lane": policy_lane},
                   "result": result, "gpu_evidence": False}
        # Preserve every observation without multiplying indentation across
        # per-iteration source traces and the paired parent's embedded copies.
        write_cpu_receipt(
            output / "result.json",
            (json.dumps(receipt, separators=(",", ":"), sort_keys=True) + "\n").encode(),
            failure_metadata)
    print(json.dumps({"accepted": accepted, "output": str(args.output_dir.resolve()),
                      "fault_oracle_passed": fault_oracle_passed,
                      "status": result.get("status")}, sort_keys=True))
    return 0 if accepted or fault_oracle_passed or (
        policy_lane and run_mode in ("measure", "diagnose") and execution_valid
        and not validation_diagnostic) else 1


def _cpu_compare_failed_artifacts(
    output: pathlib.Path, child_names: tuple[str, ...]
) -> list[dict[str, str]]:
    artifacts: list[dict[str, str]] = []
    for child_name in child_names:
        child = output / child_name
        if not child.is_dir() or child.is_symlink():
            continue
        for path in sorted(child.rglob("*")):
            if path.is_file() and not path.is_symlink():
                artifacts.append({
                    "child": child_name,
                    "path": str(path.relative_to(output)),
                })
    return artifacts


def _cpu_compare_child(
    *,
    bundle: pathlib.Path,
    output: pathlib.Path,
    provider: pathlib.Path,
    validator: pathlib.Path,
    completion: str,
    repeat: int,
    publication: str,
    resource_backend: str,
    max_pending: int,
    require_backing_oracle: bool,
    timeout_seconds: int,
    route: str,
    grammar: str = "bounded",
    source_projection: pathlib.Path | None = None,
) -> dict[str, Any]:
    """Run the public child workflow quietly and retain its exact receipt."""
    root = pathlib.Path(__file__).resolve().parents[2]
    launcher = root / "scripts" / "run_python.sh"
    command = [
        str(launcher), str(pathlib.Path(__file__).resolve()), "cpu-experiment",
        str(bundle), "--provider", str(provider), "--validator", str(validator),
        "--replay-route", route, "--completion", completion,
        "--grammar", grammar,
        "--resource-backend", resource_backend, "--publication", publication,
        "--repeat", str(repeat), "--max-pending", str(max_pending),
        "--timeout-seconds", str(timeout_seconds), "--require-path",
        "--output-dir", str(output),
    ]
    if require_backing_oracle:
        command.append("--require-backing-oracle")
    if source_projection is not None:
        command += ["--source-projection", str(source_projection)]
    try:
        completed = subprocess.run(
            command, check=False, capture_output=True, text=True,
            timeout=timeout_seconds + 5,
        )
        stdout, stderr = completed.stdout, completed.stderr
        exit_code: int | None = completed.returncode
    except subprocess.TimeoutExpired as error:
        stdout = error.stdout or b""
        stderr = error.stderr or b""
        stdout = stdout.decode(errors="replace") if isinstance(stdout, bytes) else stdout
        stderr = stderr.decode(errors="replace") if isinstance(stderr, bytes) else stderr
        exit_code = None
    # The child owns result.json; these command streams preserve wrapper
    # diagnostics without allowing them to pollute the parent JSON protocol.
    output.mkdir(parents=True, exist_ok=True)
    for name, contents in (("command.stdout", stdout), ("command.stderr", stderr)):
        destination = output / name
        if destination.exists():
            destination.unlink()
        write_flushed(destination, contents.encode())
    result_path = output / "result.json"
    try:
        receipt = json.loads(result_path.read_text(encoding="utf-8"))
        if not isinstance(receipt, dict):
            raise ValueError("non-object child receipt")
    except (OSError, json.JSONDecodeError, ValueError):
        receipt = {
            "schema": "dxmt9.render_tape.cpu_experiment_artifact.v1",
            "accepted": False,
            "provider_exit_code": exit_code,
            "result": {"status": "missing-child-receipt"},
        }
    receipt["compare_wrapper_exit_code"] = exit_code
    return receipt


def cpu_compare(args: argparse.Namespace) -> int:
    grammar = getattr(args, "grammar", "bounded")
    source_projection = getattr(args, "source_projection", None)
    if source_projection is not None and (
            grammar != "general-frame" or args.completion != "before-next-work"
            or args.publication != "per-command" or args.max_pending != 1
            or args.resource_backend != "cpu-metal-resources" or args.repeat < 2):
        raise SystemExit("source projection requires general-frame before-next-work, "
                         "per-command, cpu-metal-resources, max-pending=1 and repeat>=2")
    if grammar not in ("bounded", "general-frame"):
        raise SystemExit("CPU grammar must be bounded or general-frame")
    if args.completion_plan is not None:
        # Route-coupled external plans need an Any-route encoding and two
        # native controllers. Reject before opening the parent transaction so
        # an unsupported request has no filesystem effect.
        raise SystemExit(
            "cpu-compare does not support external completion plans; "
            "use a built-in completion preset"
        )
    if not 1 <= args.repeat <= 32 or args.timeout_seconds <= 0:
        raise SystemExit("CPU compare repeat must be in [1,32] and timeout must be positive")
    if not 0 <= args.max_pending <= 32:
        raise SystemExit("CPU compare max-pending must be in [0,32]")
    if getattr(args, "fault", None) is not None:
        raise SystemExit("cpu-compare does not accept fault injection")
    if grammar == "general-frame":
        if args.resource_backend != "cpu-metal-resources":
            raise SystemExit("general-frame requires --resource-backend cpu-metal-resources")
        if args.max_pending not in (1, 2):
            raise SystemExit("general-frame max-pending must be 1 or 2")
        if args.max_pending == 2 and args.completion != "before-next-work":
            raise SystemExit("general-frame max-pending=2 requires --completion before-next-work")
        if (args.completion not in ("after-command", "before-next-work")
                or args.publication != "per-command"):
            raise SystemExit(
                "general-frame requires --completion after-command or "
                "before-next-work and --publication per-command")
    elif args.completion == "before-next-work":
        raise SystemExit("before-next-work requires --grammar general-frame")

    bundle = args.bundle.resolve()
    provider = args.provider.resolve()
    validator = args.validator.resolve()
    binary_digest = digest(provider)
    validator_digest = digest(validator)
    failure_metadata: dict[str, Any] = {}
    with bundle_transaction(
            args.output_dir, preserve_rejected=True,
            failure_metadata=failure_metadata) as output:
        staged = output / "input"
        copy_input_owned(bundle, staged)
        manifest, events, refs, _blobs, identity, _source = load_bundle(staged)
        run_validator(validator, "validate", events, refs)
        if identity is not None:
            run_identity_validator(validator, events, identity, refs)
        input_inspection = run_validator(validator, "inspect", events, refs)
        input_components = manifest["components"]
        source_binding = None
        if source_projection is not None:
            try:
                source_binding = stage_cpu_source_projection(
                    source_projection, output / "source-projection", input_inspection,
                    input_components, binary_digest, validator_digest)
            except (OSError, ValueError) as error:
                rejected = {"schema": "dxmt9.render_tape.cpu_compare_artifact.v1",
                            "accepted": False, "gpu_evidence": False,
                            "source_projection_rejected": str(error)}
                write_cpu_receipt(
                    output / "result.json",
                    (json.dumps(rejected, indent=2) + "\n").encode(),
                    failure_metadata)
                print(json.dumps({**rejected, "output": str(args.output_dir.resolve())}))
                return 1
        children: dict[str, dict[str, Any]] = {}
        for route, child_name in (
            ("compatibility", "reference"),
            ("production-slot", "candidate"),
        ):
            children[child_name] = _cpu_compare_child(
                bundle=staged, output=output / child_name, provider=provider,
                validator=validator, completion=args.completion,
                repeat=args.repeat, publication=args.publication,
                resource_backend=args.resource_backend,
                max_pending=args.max_pending,
                require_backing_oracle=args.require_backing_oracle,
                timeout_seconds=args.timeout_seconds, route=route, grammar=grammar,
                source_projection=output / "source-projection" if source_binding else None,
            )
        comparison = compare_cpu_experiment_results(
            children["reference"], children["candidate"], repeat=args.repeat,
            completion=args.completion, publication=args.publication,
            resource_backend=args.resource_backend,
            input_inspection=input_inspection,
            max_pending=args.max_pending,
            require_backing_oracle=args.require_backing_oracle,
            reference_input_components=input_components,
            candidate_input_components=input_components,
            reference_binary_sha256=binary_digest,
            candidate_binary_sha256=binary_digest,
            grammar=grammar,
            source_binding=source_binding,
        )
        for child_name in ("reference", "candidate"):
            result = _cpu_compare_evidence(children[child_name])
            if result is None or not experiment_images_stable(result):
                comparison["failures"].append(_cpu_compare_failure(
                    f"{child_name}-provider-image-authentication-failed"))
        if digest(validator) != validator_digest:
            comparison["failures"].append(_cpu_compare_failure("validator-image-changed"))
        comparison["accepted"] = not comparison["failures"]
        failed_artifacts = (
            _cpu_compare_failed_artifacts(output, ("reference", "candidate"))
            if comparison["failures"] else []
        )
        receipt = {
            "schema": "dxmt9.render_tape.cpu_compare_artifact.v1",
            "accepted": comparison["accepted"],
            "diagnostic": any(child.get("diagnostic") is True for child in children.values()),
            "provider_exit_code": 0 if comparison["accepted"] else None,
            "gpu_evidence": False,
            "timing_accepted": None,
            "input": {
                "path": "input",
                "authenticated_once": True,
                "components": input_components,
                "inspection": input_inspection,
            },
            "provider": {"path": str(provider), "sha256": binary_digest},
            "requested": {
                "repeat": args.repeat,
                "grammar": grammar,
                "completion": args.completion,
                "publication": args.publication,
                "resource_backend": args.resource_backend,
                "require_backing_oracle": args.require_backing_oracle,
                "max_pending": args.max_pending,
                "fault": None,
            },
            "reference": children["reference"],
            "candidate": children["candidate"],
            "children": children,
            "comparison": comparison,
            "failed_artifacts": failed_artifacts,
        }
        write_cpu_receipt(
            output / "result.json",
            (json.dumps(receipt, separators=(",", ":"), sort_keys=True) + "\n").encode(),
            failure_metadata)
    print(json.dumps({
        "accepted": receipt["accepted"],
        "output": str(args.output_dir.resolve()),
        "status": "complete" if receipt["accepted"] else "rejected",
        "first_differing_iteration": receipt["comparison"]["first_differing_iteration"],
        "failures": receipt["comparison"]["failures"],
    }, sort_keys=True))
    return 0 if receipt["accepted"] else 1


def stage_cpu_source_projection(
    artifact: pathlib.Path, output: pathlib.Path, inspection: dict[str, Any],
    components: dict[str, Any], provider_sha256: str, validator_sha256: str,
) -> dict[str, Any]:
    """Bind immutable observation evidence; the native gate still decides holds."""
    from render_tape_cpu_source_projection import validate_cpu_projection_binding

    output.mkdir()
    for relative in ("result.json", "source-projection.json", "reference/result.json"):
        source = artifact.resolve() / relative
        # Cold artifacts are bounded separately from the input bundle. Reject
        # unexpectedly large receipts before reading or allocating native rows.
        if source.stat().st_size > 32 * 1024 * 1024:
            raise ValueError("source-projection-receipt-too-large")
        destination = output / relative
        destination.parent.mkdir(exist_ok=True)
        shutil.copyfile(source, destination)
    projection_path = output / "source-projection.json"
    reference_path = output / "reference/result.json"
    receipt = json.loads((output / "result.json").read_text())
    projection = json.loads(projection_path.read_text())
    reference = json.loads(reference_path.read_text())
    if not all(isinstance(value, dict) for value in (receipt, projection, reference)):
        raise ValueError("source-projection-receipt-shape")
    projection_digest = digest(projection_path)
    if (receipt.get("schema") != "dxmt9.render_tape.cpu_source_projection_artifact.v1"
            or receipt.get("accepted") is not True or receipt.get("failures") != []
            or receipt.get("source_projection_sha256") != projection_digest
            or projection.get("reference_receipt_sha256") != digest(reference_path)):
        raise ValueError("source-projection-receipt-digest")
    sources = validate_cpu_projection_binding(
        projection, reference, inspection, components, provider_sha256, validator_sha256)
    native = _cpu_compare_evidence(reference)
    if native is None or not experiment_images_stable(native):
        raise ValueError("source-projection-loaded-images-changed")
    if (not cpu_experiment_accepts(
            native, repeat=projection["repeat"], route="compatibility",
            completion="after-command", require_path=True,
            resource_backend="cpu-metal-resources", publication="per-command",
            input_inspection=inspection, grammar="general-frame")
            or type(receipt.get("source_count")) is not int
            or receipt["source_count"] != len(sources)
            or not 1 <= len(sources) <= 128):
        raise ValueError("source-projection-reference-rejected")
    encoded = bytearray(b"D9CSRC01")
    encoded.extend(struct.pack("<I", len(sources)))
    for row in sources:
        encoded.extend(struct.pack("<II", row["event"], len(row["buffer_closure"])))
        for identity in row["buffer_closure"]:
            encoded.extend(struct.pack("<IIQ", identity["kind"], identity["generation"],
                                       int(identity["object_id"])))
    write_flushed(output / "source-oracle.bin", encoded)
    return {"projection_sha256": projection_digest,
            "oracle_sha256": digest(output / "source-oracle.bin"),
            "sources": sources,
            "reference_semantic_digest": native["iterations"][0]["semantic_digest"],
            "reference_draw_digests": [
                row["cumulative_semantic_digest"]
                for row in native["iterations"][0]["draw_observations"]]}


def cpu_source_project(args: argparse.Namespace) -> int:
    """Produce a qualified source projection through real compatibility replay.

    This is an observation artifact, not authorization to defer completion.
    Future schedules must bind its input, executable and loaded-image evidence.
    """
    from render_tape_cpu_source_projection import validate_cpu_source_closures

    if not 2 <= args.repeat <= 32 or args.timeout_seconds <= 0:
        raise SystemExit("cpu-source-project needs repeat in [2,32] and a positive timeout")
    provider, validator = args.provider.resolve(), args.validator.resolve()
    provider_digest, validator_digest = digest(provider), digest(validator)
    failure_metadata: dict[str, Any] = {}
    with bundle_transaction(
            args.output_dir, preserve_rejected=True,
            failure_metadata=failure_metadata) as output:
        staged = output / "input"
        manifest, inspection, native = None, None, None
        failures: list[str] = []
        sources = []
        setup_error = None
        try:
            copy_input_owned(args.bundle.resolve(), staged)
            manifest, events, refs, _blobs, identity, _source = load_bundle(staged)
            run_validator(validator, "validate", events, refs)
            if identity is not None:
                run_identity_validator(validator, events, identity, refs)
            inspection = run_validator(validator, "inspect", events, refs)
            child = _cpu_compare_child(
                bundle=staged, output=output / "reference", provider=provider,
                validator=validator, completion="after-command", repeat=args.repeat,
                publication="per-command", resource_backend="cpu-metal-resources",
                max_pending=1, require_backing_oracle=False,
                timeout_seconds=args.timeout_seconds, route="compatibility",
                grammar="general-frame")
            native = _cpu_compare_evidence(child)
            if (child.get("accepted") is not True or child.get("provider_exit_code") != 0
                    or child.get("compare_wrapper_exit_code") != 0 or native is None):
                failures.append("reference-execution-rejected")
            if (digest(provider) != provider_digest or digest(validator) != validator_digest
                    or child.get("provider", {}).get("sha256") != provider_digest
                    or child.get("input_components") != manifest["components"]
                    or child.get("input_inspection") != inspection
                    or native is None or not experiment_images_stable(native)):
                failures.append("projection-provenance-mismatch")
            if not failures:
                sources = validate_cpu_source_closures(native, inspection)
        except (SystemExit, OSError, ValueError, subprocess.SubprocessError) as error:
            # Preserve the copied input and any child evidence on ordinary
            # admission/execution rejection. Interrupts and publication I/O
            # failures still escape the transaction rather than claiming success.
            failures.append("projection-setup-or-validation-rejected")
            setup_error = str(error)
            sources = []
        projection = {
            "schema": "dxmt9.render_tape.cpu_source_projection.v1",
            "accepted": not failures,
            "failures": failures,
            "input_components": manifest["components"] if manifest else None,
            "setup_error": setup_error,
            "provider": {"path": str(provider), "sha256": provider_digest},
            "validator": {"path": str(validator), "sha256": validator_digest},
            "reference_receipt_sha256": (
                digest(output / "reference" / "result.json")
                if (output / "reference" / "result.json").is_file() else None),
            "loaded_images": native.get("loaded_images") if native else None,
            "repeat": args.repeat,
            "source": "production-compatibility-replay-resource-visitor",
            "scope": "per-command-buffer-closure-not-mutation-authorization",
            "input_inspection": inspection,
            "sources": sources,
        }
        write_flushed(output / "source-projection.json",
                      (json.dumps(projection, indent=2, sort_keys=True) + "\n").encode())
        receipt = {"schema": "dxmt9.render_tape.cpu_source_projection_artifact.v1",
                   "accepted": not failures, "failures": failures,
                   "source_projection_sha256": digest(output / "source-projection.json"),
                   "source_count": len(sources), "gpu_evidence": False}
        write_cpu_receipt(
            output / "result.json",
            (json.dumps(receipt, indent=2, sort_keys=True) + "\n").encode(),
            failure_metadata)
    print(json.dumps({**receipt, "output": str(args.output_dir.resolve())}, sort_keys=True))
    return 0 if receipt["accepted"] else 1


def strict_production_oracle_accepts(
    result: dict[str, Any], require_non_degenerate: bool = False
) -> bool:
    """Require the provider's authenticated, strict output oracle.

    ``provider_oracle_accepts`` intentionally also permits the bounded pixel
    envelope used by non-production reduction candidates.  Parallel join
    evidence is a production comparison, so an envelope or a hand-written
    result is not sufficient here.  Promotion bundles additionally require a
    non-degenerate output; generic ``provider-replay`` fixtures continue to use
    the less restrictive oracle.
    """
    validity = result.get("validity")
    return bool(
        result.get("schema") == PROVIDER_SCHEMA
        and result.get("archive_policy") == "disabled"
        and provider_oracle_accepts(result, require_non_degenerate)
        and isinstance(validity, dict)
        and validity.get("oracle_mode") == "strict"
        and validity.get("expected_digest_matched") is True
    )


def strict_production_oracle_failure(
    result: dict[str, Any], require_non_degenerate: bool = False
) -> str:
    """Return a stable diagnostic for a rejected parallel production result."""
    validity = result.get("validity")
    if not isinstance(validity, dict):
        return "strict production oracle requires validity evidence"
    if require_non_degenerate and validity.get("output_non_degenerate") is not True:
        return "strict production oracle requires output_non_degenerate=true"
    if validity.get("oracle_mode") != "strict":
        return "strict production oracle requires oracle_mode=strict"
    if validity.get("expected_digest_matched") is not True:
        return "strict production oracle requires expected_digest_matched=true"
    return "strict production oracle rejected result"


def _typed_nonnegative_counter(value: Any) -> bool:
    # bool is an int subclass, but it is not a counter value in the provider
    # ABI.  Keep this check strict so malformed JSON cannot pass a cold gate.
    return type(value) is int and value >= 0


def parallel_counters(result: dict[str, Any]) -> dict[str, int]:
    counters = result.get("parallel_counters")
    if not isinstance(counters, dict):
        raise ValueError("provider omitted typed parallel_counters")
    values: dict[str, int] = {}
    for field in PARALLEL_COUNTER_FIELDS:
        value = counters.get(field)
        if not _typed_nonnegative_counter(value):
            raise ValueError(
                f"provider parallel_counters.{field} must be a nonnegative integer"
            )
        values[field] = value
    return values


def _partition_mode(result: dict[str, Any], requested: str) -> dict[str, str]:
    value = result.get("partition_mode")
    if not isinstance(value, dict):
        raise ValueError("provider omitted partition_mode")
    actual_requested = value.get("requested")
    actual_resolved = value.get("resolved")
    if actual_requested != requested or actual_resolved != requested:
        raise ValueError(
            "provider partition mode mismatch: "
            f"requested={actual_requested!r} resolved={actual_resolved!r} "
            f"expected={requested!r}"
        )
    return {"requested": requested, "resolved": requested}


def parallel_verify(args: argparse.Namespace) -> int:
    """Join identity and ExplicitParallel provider evidence atomically.

    Every provider invocation is a separate subprocess.  No replay output is
    printed until all repetitions have completed and all gates have passed,
    keeping stdout a single machine-readable result and avoiding any output
    artifact mutation.
    """
    manifest, events, blob_refs, blob_paths, identity, source_oracle = load_bundle(args.bundle)
    # Validate the complete bundle before either fresh provider process can
    # create a device or touch Metal.  This keeps the join pre-effect and
    # gives both branches the same authenticated input closure.
    run_validator(args.validator, "validate", events, blob_refs)
    if identity is not None:
        run_identity_validator(args.validator, events, identity, blob_refs)
    expected_output = None
    if isinstance(manifest.get("components", {}).get("output_oracle"), dict):
        expected_output = events.parent / "output.rgba"

    identity_runs: list[dict[str, Any]] = []
    parallel_runs: list[dict[str, Any]] = []
    require_non_degenerate = bundle_requires_non_degenerate(manifest)
    try:
        for _ in range(PARALLEL_VERIFY_RUNS):
            identity_runs.append(
                run_provider_replay(
                    args.provider,
                    events,
                    blob_paths,
                    expected_output=expected_output,
                    expected_source=source_oracle,
                    partition_mode="identity",
                    identity=identity,
                )
            )
        for _ in range(PARALLEL_VERIFY_RUNS):
            parallel_runs.append(
                run_provider_replay(
                    args.provider,
                    events,
                    blob_paths,
                    expected_output=expected_output,
                    expected_source=source_oracle,
                    partition_mode="parallel",
                    identity=identity,
                )
            )
    except SystemExit as error:
        rejected = {
            "bundle_schema": manifest["schema"],
            "events_sha256": manifest["components"]["events"]["sha256"],
            "oracle_accepted": False,
            "failed_gate": str(error),
            "identity_runs": identity_runs,
            "parallel_runs": parallel_runs,
        }
        print(json.dumps(rejected, sort_keys=True))
        return 1

    try:
        for index, result in enumerate(identity_runs, start=1):
            if not strict_production_oracle_accepts(
                result, require_non_degenerate
            ):
                raise ValueError(
                    "identity provider production oracle rejected run "
                    f"{index}: {strict_production_oracle_failure(result, require_non_degenerate)}"
                )
            _partition_mode(result, "identity")
        for index, result in enumerate(parallel_runs, start=1):
            if not strict_production_oracle_accepts(
                result, require_non_degenerate
            ):
                raise ValueError(
                    "parallel provider production oracle rejected run "
                    f"{index}: {strict_production_oracle_failure(result, require_non_degenerate)}"
                )
            _partition_mode(result, "parallel")

        identity_replays = [replay_identity(result) for result in identity_runs]
        parallel_replays = [replay_identity(result) for result in parallel_runs]
        if any(replay != identity_replays[0] for replay in identity_replays[1:]):
            raise ValueError("identity replay runs are nondeterministic")
        if any(replay != parallel_replays[0] for replay in parallel_replays[1:]):
            raise ValueError("ExplicitParallel replay runs are nondeterministic")
        if identity_replays[0] != parallel_replays[0]:
            raise ValueError("identity and ExplicitParallel replay identities differ")

        counter_runs = [parallel_counters(result) for result in parallel_runs]
        for field in PARALLEL_DETERMINISTIC_COUNTER_FIELDS:
            if any(
                counters[field] != counter_runs[0][field]
                for counters in counter_runs[1:]
            ):
                raise ValueError(
                    "ExplicitParallel counter evidence is nondeterministic: "
                    f"{field}"
                )
        for index, counters in enumerate(counter_runs, start=1):
            if counters["selected"] <= 0:
                raise ValueError(
                    f"parallel verification is vacuous in run {index}: selected == 0"
                )
            if counters["children"] < 2:
                raise ValueError(
                    f"parallel verification is vacuous in run {index}: children < 2"
                )
            if counters["draws"] <= 0:
                raise ValueError(
                    f"parallel verification is vacuous in run {index}: draws == 0"
                )
            if counters["worker_batches"] <= 0:
                raise ValueError(
                    f"parallel verification is vacuous in run {index}: "
                    "worker_batches == 0"
                )
            if counters["worker_tasks"] <= 0:
                raise ValueError(
                    f"parallel verification is vacuous in run {index}: "
                    "worker_tasks == 0"
                )
            if counters["worker_active_peak"] <= 0:
                raise ValueError(
                    f"parallel verification is vacuous in run {index}: "
                    "worker_active_peak == 0"
                )
            if counters["gpu_command_buffer_errors"] != 0:
                raise ValueError(
                    f"parallel verification rejected GPU command-buffer errors "
                    f"in run {index}"
                )
    except ValueError as error:
        # Keep the command's result atomic even for an authenticated but
        # rejected provider response.  The provider itself has no output path
        # here, so this branch cannot leave a replay artifact behind.
        rejected = {
            "bundle_schema": manifest["schema"],
            "events_sha256": manifest["components"]["events"]["sha256"],
            "oracle_accepted": False,
            "failed_gate": str(error),
            "identity_runs": identity_runs,
            "parallel_runs": parallel_runs,
        }
        print(json.dumps(rejected, sort_keys=True))
        return 1

    result = {
        "schema": "dxmt9.render_tape.parallel_verify.v1",
        "bundle_schema": manifest["schema"],
        "events_sha256": manifest["components"]["events"]["sha256"],
        "oracle_accepted": True,
        "strict_identity_equal": True,
        "non_vacuous": True,
        "partition_mode": _partition_mode(parallel_runs[0], "parallel"),
        "identity_partition_mode": _partition_mode(identity_runs[0], "identity"),
        "parallel_counters": counter_runs[0],
        "parallel_counters_runs": counter_runs,
        "identity_runs": identity_runs,
        "parallel_runs": parallel_runs,
        # Compatibility aliases for consumers that only need the final run;
        # the ordered run arrays above are the canonical evidence surface.
        "identity_result": identity_runs[-1],
        "parallel_result": parallel_runs[-1],
        "identity_replay": identity_replays[0],
        "parallel_replay": parallel_replays[0],
        "scope": {
            "production_capture": manifest.get("scope", {}).get(
                "production_capture", False
            ),
            "production_provider_replay": True,
            "parallel_verify": True,
            "output_oracle": True,
        },
    }
    print(json.dumps(result, sort_keys=True))
    return 0


def run_native_materialize(
    validator: pathlib.Path,
    events: pathlib.Path,
    identity: pathlib.Path,
    output: pathlib.Path,
    blob_refs: list[str],
    command_event_ordinal: int,
    first_record: int,
    record_count: int,
    output_sha256: Optional[str] = None,
    output_identity: Optional[pathlib.Path] = None,
) -> dict[str, Any]:
    arguments = [
        str(validator), "materialize", str(events), str(identity), str(output),
        "--command-event-ordinal", str(command_event_ordinal),
        "--first-record", str(first_record),
        "--record-count", str(record_count),
    ]
    if output_sha256 is not None:
        arguments.extend(("--output-sha256", output_sha256))
    if output_identity is not None:
        arguments.extend(("--output-identity", str(output_identity)))
    for reference in blob_refs:
        arguments.extend(("--verified-blob", reference))
    completed = subprocess.run(arguments, check=False, text=True, capture_output=True)
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip()
        raise RuntimeError(detail or "native projection materializer failed")
    try:
        result = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise RuntimeError("native projection materializer emitted invalid JSON") from error
    if not output.is_file():
        raise RuntimeError("native projection materializer emitted no tape")
    if output_identity is not None and not output_identity.is_file():
        raise RuntimeError("native projection materializer emitted no identity")
    return result


def projection_provider_run(
    provider: pathlib.Path,
    events: pathlib.Path,
    blob_paths: list[pathlib.Path],
    output: pathlib.Path,
    expected: Optional[pathlib.Path] = None,
    identity: Optional[pathlib.Path] = None,
) -> dict[str, Any]:
    result = run_provider_replay(
        provider, events, blob_paths, expected_output=expected, output_path=output,
        identity=identity,
    )
    validity = result.get("validity", {})
    if (
        result.get("provider_exit_code") != 0
        or result.get("schema") != PROVIDER_SCHEMA
        or result.get("archive_policy") != "disabled"
        or result.get("status") != "complete"
        or validity.get("structurally_valid") is not True
        or validity.get("digests_valid") is not True
        or validity.get("output_readback") is not True
        or not output.is_file()
        or validity.get("output_bytes") != output.stat().st_size
        or validity.get("output_sha256") != digest(output)
    ):
        raise RuntimeError("projection provider did not produce authenticated output")
    if expected is None and (
        validity.get("expected_digest_captured") is True
        or validity.get("expected_digest_matched") is True
        or validity.get("expected_pixels_compared") is True
        or validity.get("pixel_envelope_matched") is True
        or validity.get("output_oracle_matched") is True
        or validity.get("oracle_mode") != "rejected"
    ):
        raise RuntimeError("projection candidate made an oracle claim before sealing")
    if expected is not None and (
        not provider_oracle_accepts(result)
        or validity.get("oracle_mode") != "strict"
        or validity.get("expected_digest_matched") is not True
        or validity.get("pixel_envelope_matched") is True
    ):
        raise RuntimeError("projection provider rejected the strict projected oracle")
    return result


def executable_project(args: argparse.Namespace) -> int:
    manifest, events, blob_refs, blob_paths, identity, _source = load_bundle(args.bundle)
    if manifest["profile"] != "frame-tape":
        raise SystemExit("executable projection supports frame-tape bundles only")
    if identity is None:
        raise SystemExit("executable projection requires authenticated identity.bin")
    identity_result = run_identity_validator(
        args.validator, events, identity, blob_refs
    )
    output = args.output_dir.absolute()
    try:
        with bundle_transaction(output) as staging:
            candidate = staging / "candidate.events.bin"
            candidate_identity = staging / "candidate.identity.bin"
            native = run_native_materialize(
                args.validator, events, identity, candidate, blob_refs,
                args.command_event_ordinal, args.first_record, args.record_count,
                output_identity=candidate_identity,
            )
            referenced = native.get("referenced_blobs")
            if not isinstance(referenced, list) or any(
                not isinstance(value, str) for value in referenced
            ):
                raise RuntimeError("materializer omitted exact blob closure")
            source_by_digest = {digest(path): path for path in blob_paths}
            try:
                projected_blobs = [source_by_digest[value] for value in referenced]
            except KeyError as error:
                raise RuntimeError(
                    f"materializer referenced unknown blob {error.args[0]}"
                ) from error
            projected_refs = [
                f"{value}:{source_by_digest[value].stat().st_size}"
                for value in referenced
            ]
            run_validator(args.validator, "validate", candidate, projected_refs)

            first_output = staging / "candidate-1.rgba"
            second_output = staging / "candidate-2.rgba"
            first = projection_provider_run(
                args.provider, candidate, projected_blobs, first_output,
                identity=candidate_identity,
            )
            second = projection_provider_run(
                args.provider, candidate, projected_blobs, second_output,
                identity=candidate_identity,
            )
            if (first_output.read_bytes() != second_output.read_bytes() or
                    replay_identity(first) != replay_identity(second)):
                raise RuntimeError("projected provider output is nondeterministic")
            projected_digest = digest(first_output)

            final_events = staging / EVENTS_NAME
            final_identity = staging / IDENTITY_NAME
            final_native = run_native_materialize(
                args.validator, events, identity, final_events, blob_refs,
                args.command_event_ordinal, args.first_record, args.record_count,
                projected_digest,
                output_identity=final_identity,
            )
            if final_native.get("referenced_blobs") != referenced:
                raise RuntimeError("final oracle rewrite changed projection closure")
            run_validator(args.validator, "validate", final_events, projected_refs)
            output_oracle = staging / "output.rgba"
            os.replace(first_output, output_oracle)
            second_output.unlink()
            candidate.unlink()
            candidate_identity.unlink()
            final_runs = []
            for index in range(2):
                repeated = staging / f"final-{index}.rgba"
                result = projection_provider_run(
                    args.provider, final_events, projected_blobs, repeated,
                    output_oracle,
                    identity=final_identity,
                )
                if repeated.read_bytes() != output_oracle.read_bytes():
                    raise RuntimeError("strict projected provider repeat changed bytes")
                repeated.unlink()
                final_runs.append(replay_identity(result))
            if final_runs[0] != final_runs[1]:
                raise RuntimeError("strict projected provider evidence is nondeterministic")

            blob_components = []
            if referenced:
                (staging / "blobs").mkdir()
            for sha256 in referenced:
                target = staging / "blobs" / f"{sha256}.bin"
                copy_flushed(source_by_digest[sha256], target)
                blob_components.append({
                    "path": f"blobs/{sha256}.bin",
                    "bytes": target.stat().st_size,
                    "sha256": sha256,
                })
            projected_manifest = {
                "schema": SCHEMA,
                "profile": "frame-tape",
                "producer": {
                    "path": "scripts/tools/run_dxmt9_render_tape.py",
                    "git_revision": producer_revision(),
                },
                "stage": "executable-projection",
                "domain": "replay",
                "inputs": [{
                    "schema": manifest["schema"],
                    "events_sha256": manifest["components"]["events"]["sha256"],
                    "identity_sha256": manifest["components"]["identity"]["sha256"],
                }],
                "validity": {
                    "structural": True,
                    "provider_replay": True,
                    "strict_projected_oracle": True,
                    "fresh_process_repeats": 2,
                },
                "components": {
                    "events": {
                        "path": EVENTS_NAME,
                        "bytes": final_events.stat().st_size,
                        "sha256": digest(final_events),
                    },
                    "identity": {
                        "path": IDENTITY_NAME,
                        "bytes": final_identity.stat().st_size,
                        "sha256": digest(final_identity),
                        "schema": IDENTITY_SCHEMA,
                    },
                    "blobs": blob_components,
                    "output_oracle": {
                        "path": "output.rgba",
                        "bytes": output_oracle.stat().st_size,
                        "sha256": projected_digest,
                    },
                },
                "scope": {
                    "production_capture": False,
                    "production_provider_replay": True,
                    "output_oracle": True,
                    "executable_projection": True,
                    "source_full_frame_oracle_copied": False,
                },
                "projection": {
                    "command_event_ordinal": args.command_event_ordinal,
                    "first_record": args.first_record,
                    "record_count": args.record_count,
                    "logical_pass_id": native.get("logical_pass_id"),
                    "identity": identity_result,
                    "oracle_mode": "strict",
                    "provider_runs": final_runs,
                },
            }
            write_flushed(
                staging / "manifest.json",
                (json.dumps(projected_manifest, indent=2, sort_keys=True) + "\n").encode(),
            )
    except RuntimeError as error:
        raise SystemExit(f"render tape executable projection failed: {error}") from error
    print(json.dumps({
        "bundle": str(output),
        "manifest": str(output / "manifest.json"),
        "events_sha256": digest(output / EVENTS_NAME),
        "output_sha256": digest(output / "output.rgba"),
        "oracle_accepted": True,
    }, sort_keys=True))
    return 0


def reduced_candidate(
    validator: pathlib.Path,
    provider: pathlib.Path,
    events: pathlib.Path,
    blob_refs: list[str],
    blob_paths: list[pathlib.Path],
    selections: list[int],
    directory: pathlib.Path,
    warmup: int,
    repeat: int,
) -> tuple[pathlib.Path, dict[str, Any], dict[str, Any]]:
    output_events = directory / "events.bin"
    native = run_native_reduce(
        validator, events, output_events, blob_refs, selections
    )
    referenced = native.get("referenced_blobs")
    if not isinstance(referenced, list) or any(
        not isinstance(value, str) for value in referenced
    ):
        raise RuntimeError("native reducer omitted referenced_blobs")
    paths_by_digest = {digest(path): path for path in blob_paths}
    try:
        candidate_paths = [paths_by_digest[value] for value in referenced]
    except KeyError as error:
        raise RuntimeError(f"native reducer referenced unknown blob {error.args[0]}") from error
    candidate_refs = [f"{digest(path)}:{path.stat().st_size}" for path in candidate_paths]
    # The production validator must accept the complete candidate before the
    # provider process is started and can perform device effects.
    run_validator(validator, "validate", output_events, candidate_refs)
    replay = replay_with_policy(
        provider, output_events, candidate_paths, warmup, repeat
    )
    if not replay.get("oracle_accepted"):
        raise RuntimeError("provider oracle rejected the reduced candidate")
    return output_events, native, replay


def reduce_bundle(args: argparse.Namespace) -> int:
    manifest, events, blob_refs, blob_paths, _, _source = load_bundle(args.bundle)
    if manifest["profile"] != "frame-tape":
        raise SystemExit("render tape reduction supports frame-tape bundles only")
    ensure_empty_output(args.output_dir.absolute())
    selections = sorted(args.select_command_event)
    if len(selections) != len(set(selections)):
        raise SystemExit("duplicate --select-command-event value")
    try:
        with tempfile.TemporaryDirectory(prefix="dxmt9-render-tape-reduce-") as root:
            candidate_events, native, replay = reduced_candidate(
                args.validator,
                args.provider,
                events,
                blob_refs,
                blob_paths,
                selections,
                pathlib.Path(root),
                args.warmup,
                args.repeat,
            )
            referenced = native["referenced_blobs"]
            manifest_path = write_reduced_bundle(
                args.output_dir.absolute(),
                candidate_events,
                manifest,
                blob_paths,
                referenced,
                {
                    "algorithm": "explicit-whole-command-selection-v1",
                    "selected_source_command_events": selections,
                    "retained_source_events": native.get("retained_source_events", []),
                    "provider_identity": replay_identity(replay),
                    "replay_policy": replay["replay_policy"],
                },
            )
    except RuntimeError as error:
        raise SystemExit(f"render tape reduction failed: {error}") from error
    print(
        json.dumps(
            {
                "bundle": str(args.output_dir.absolute()),
                "manifest": str(manifest_path),
                "selected_command_events": selections,
                "events_sha256": digest(args.output_dir.absolute() / EVENTS_NAME),
                "oracle_accepted": True,
            },
            sort_keys=True,
        )
    )
    return 0


def partition(values: list[int], count: int) -> list[list[int]]:
    return [values[index::count] for index in range(count) if values[index::count]]


def bisect_bundle(args: argparse.Namespace) -> int:
    manifest, events, blob_refs, blob_paths, _, _source = load_bundle(args.bundle)
    if manifest["profile"] != "frame-tape":
        raise SystemExit("render tape bisection supports frame-tape bundles only")
    ensure_empty_output(args.output_dir.absolute())
    commands, present = inspect_bundle_commands(args.validator, events, blob_refs)
    selected = list(commands)
    attempts = 0
    accepted = 0
    granularity = 2
    last_native: dict[str, Any] | None = None
    last_replay: dict[str, Any] | None = None
    with tempfile.TemporaryDirectory(prefix="dxmt9-render-tape-bisect-") as root:
        root_path = pathlib.Path(root)
        while len(selected) > 1:
            removable = [value for value in selected if value != present]
            if not removable:
                break
            subsets = partition(removable, min(granularity, len(removable)))
            reduced = False
            for subset in subsets:
                candidate = [value for value in selected if value not in subset]
                attempt_dir = root_path / f"attempt-{attempts:06d}"
                attempt_dir.mkdir()
                attempts += 1
                try:
                    _, native, replay = reduced_candidate(
                        args.validator,
                        args.provider,
                        events,
                        blob_refs,
                        blob_paths,
                        candidate,
                        attempt_dir,
                        args.warmup,
                        args.repeat,
                    )
                except RuntimeError:
                    continue
                selected = candidate
                last_native = native
                last_replay = replay
                accepted += 1
                granularity = max(2, granularity - 1)
                reduced = True
                break
            if reduced:
                continue
            if granularity >= len(removable):
                break
            granularity = min(len(removable), granularity * 2)

        final_dir = root_path / "final"
        final_dir.mkdir()
        candidate_events, native, replay = reduced_candidate(
            args.validator,
            args.provider,
            events,
            blob_refs,
            blob_paths,
            selected,
            final_dir,
            args.warmup,
            args.repeat,
        )
        last_native = native
        last_replay = replay
        manifest_path = write_reduced_bundle(
            args.output_dir.absolute(),
            candidate_events,
            manifest,
            blob_paths,
            native["referenced_blobs"],
            {
                "algorithm": "deterministic-ddmin-whole-command-events-v1",
                "source_command_events": commands,
                "present_command_event": present,
                "selected_source_command_events": selected,
                "retained_source_events": native.get("retained_source_events", []),
                "attempts": attempts,
                "accepted_candidates": accepted,
                "provider_identity": replay_identity(replay),
                "replay_policy": replay["replay_policy"],
            },
        )
    assert last_native is not None and last_replay is not None
    print(
        json.dumps(
            {
                "bundle": str(args.output_dir.absolute()),
                "manifest": str(manifest_path),
                "source_command_events": commands,
                "selected_command_events": selected,
                "attempts": attempts,
                "accepted_candidates": accepted,
                "events_sha256": digest(args.output_dir.absolute() / EVENTS_NAME),
                "oracle_accepted": True,
            },
            sort_keys=True,
        )
    )
    return 0


def parser() -> argparse.ArgumentParser:
    value = argparse.ArgumentParser(description="Pack and inspect dxmt9 Render Tape bundles")
    subparsers = value.add_subparsers(dest="command", required=True)
    pack_parser = subparsers.add_parser("pack", help="pack a validated v2 event tape")
    pack_parser.add_argument("--events", type=pathlib.Path, required=True)
    pack_parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    pack_parser.add_argument("--blob", type=pathlib.Path, action="append", default=[])
    pack_parser.add_argument("--identity", type=pathlib.Path)
    pack_parser.add_argument("--output-rgba", type=pathlib.Path)
    pack_parser.add_argument("--source-rgba", type=pathlib.Path)
    pack_parser.add_argument(
        "--test-input", action="store_true",
        help="label an independently constructed test fixture; never production capture evidence",
    )
    pack_parser.add_argument(
        "--validator", type=pathlib.Path,
        default=pathlib.Path("build/tools/dxmt9-render-tape"),
    )
    pack_parser.set_defaults(function=pack)
    for command in ("validate", "inspect"):
        command_parser = subparsers.add_parser(command)
        command_parser.add_argument("bundle", type=pathlib.Path)
        command_parser.add_argument(
            "--validator", type=pathlib.Path,
            default=pathlib.Path("build/tools/dxmt9-render-tape"),
        )
        command_parser.set_defaults(function=validate_or_inspect)
    policy_parser = subparsers.add_parser(
        "policy-explore",
        help="enumerate authenticated structural parallel-policy candidates",
    )
    policy_parser.add_argument("bundle", type=pathlib.Path)
    policy_parser.add_argument(
        "--validator", type=pathlib.Path,
        default=pathlib.Path("build/tools/dxmt9-render-tape"),
    )
    policy_parser.set_defaults(function=policy_explore)
    provider_parser = subparsers.add_parser(
        "provider-replay", help="replay a validated bundle through the production provider"
    )
    provider_parser.add_argument("bundle", type=pathlib.Path)
    provider_parser.add_argument(
        "--provider", type=pathlib.Path,
        default=pathlib.Path("build/tools/dxmt9-render-tape-provider"),
    )
    provider_parser.add_argument(
        "--validator", type=pathlib.Path,
        default=pathlib.Path("build/tools/dxmt9-render-tape"),
    )
    provider_parser.add_argument("--warmup", type=int, default=0)
    provider_parser.add_argument("--repeat", type=int, default=1)
    provider_parser.add_argument(
        "--partition-mode", choices=("identity", "serial", "parallel")
    )
    provider_parser.set_defaults(function=provider_replay)
    experiment_parser = subparsers.add_parser(
        "experiment", help="repeat immutable prepared input in one provider process")
    experiment_parser.add_argument("bundle", type=pathlib.Path)
    experiment_parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    experiment_parser.add_argument("--provider", type=pathlib.Path,
                                   default=pathlib.Path("build/tools/dxmt9-render-tape-provider"))
    experiment_parser.add_argument("--validator", type=pathlib.Path,
                                   default=pathlib.Path("build/tools/dxmt9-render-tape"))
    experiment_parser.add_argument("--warmup", type=int, default=1)
    experiment_parser.add_argument("--repeat", type=int, default=5)
    experiment_parser.add_argument("--timeout-seconds", type=int, default=120)
    experiment_parser.add_argument("--reset", choices=("cold-device", "warm-device"),
                                   default="cold-device")
    experiment_parser.add_argument("--replay-route", choices=("compatibility", "production-slot"),
                                   default="compatibility")
    experiment_parser.add_argument("--source-boundary-mode",
                                   choices=("chunk", "capture-source"), default="chunk",
                                   help="normal authenticated source cuts")
    experiment_parser.add_argument("--source-plan-sha256",
                                   help="expected authenticated source plan for a v2 policy")
    experiment_parser.add_argument("--partition-mode", choices=("identity", "serial", "parallel"),
                                   default="identity")
    experiment_parser.add_argument("--require-path", action="store_true",
                                   help="reject iterations with zero selected-route draws")
    experiment_parser.add_argument("--draw-command-limit", type=int,
                                   help="canonical policy DrawLimit knob")
    experiment_parser.add_argument("--slot-headroom-bytes", type=int,
                                   help="canonical policy direct-slot headroom knob")
    experiment_parser.add_argument("--aggregate-headroom-bytes", type=int,
                                   help="canonical policy aggregate headroom knob")
    experiment_parser.add_argument("--audit-initial-seeds", action="store_true",
                                   help="cold diagnostic readback of seed resources; never a policy qualification")
    experiment_parser.add_argument("--audit-texture-input", metavar="GEN:ID:AFTERSEQ:SHA256:PATH",
                                   help="one-shot R32F input intervention after an authenticated source; never qualifies")
    experiment_parser.add_argument("--audit-fresh-seed-drains", action="store_true",
                                   help="cold diagnostic fresh-owner drain bypass; never a policy qualification")
    experiment_parser.add_argument("--audit-texture-prestage", action="store_true",
                                   help="cold diagnostic independent texture seed prestaging; never a policy qualification")
    experiment_parser.add_argument("--audit-source-boundaries", action="store_true",
                                   help="cold diagnostic source-boundary replay; never a policy qualification")
    experiment_parser.add_argument("--audit-batch-cut", dest="audit_batch_cuts",
                                   action="append", default=[], metavar="EVENT:RECORD",
                                   help="source-boundary diagnostic cut; repeatable and requires --audit-source-boundaries")
    experiment_parser.set_defaults(function=fixed_input_experiment)
    cpu_parser = subparsers.add_parser(
        "cpu-experiment", help="bounded no-GPU Tape replay with controlled completion")
    cpu_parser.add_argument("bundle", type=pathlib.Path)
    cpu_parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    cpu_parser.add_argument("--provider", type=pathlib.Path,
                            default=pathlib.Path("build/tests/native/backend/dxmt9-cpu-ready-production-routing-spec"))
    cpu_parser.add_argument("--validator", type=pathlib.Path,
                            default=pathlib.Path("build/tools/dxmt9-render-tape"))
    cpu_parser.add_argument("--repeat", type=int, default=16)
    cpu_parser.add_argument("--run-mode", choices=("verify", "measure", "diagnose"), default="verify",
                            help="full oracle, low-observer timing, or diagnostic-only phase clocks")
    cpu_parser.add_argument("--policy-lane", action="store_true", help=argparse.SUPPRESS)
    cpu_parser.add_argument("--source-boundary-mode",
                            choices=("chunk", "capture-source"), default="chunk")
    cpu_parser.add_argument("--source-plan-sha256")
    cpu_parser.add_argument("--grammar", choices=("bounded", "general-frame"),
                            default="bounded",
                            help="bounded32-event oracle, or complete-frame general oracle")
    cpu_parser.add_argument("--publication", choices=("per-command", "draw-island"),
                            default="per-command",
                            help="publish each command event, or group adjacent plain draw events")
    cpu_parser.add_argument("--timeout-seconds", type=int, default=30)
    cpu_parser.add_argument("--replay-route", choices=("compatibility", "production-slot"),
                            default="production-slot")
    cpu_parser.add_argument("--resource-backend", choices=("cpu-no-gpu", "cpu-metal-resources"),
                            default="cpu-no-gpu",
                            help="shadow-only, or real CPU-visible Metal buffers without GPU execution")
    cpu_parser.add_argument("--require-backing-oracle", action="store_true",
                            help="require the dynamic Tape backing-byte and conditional retention oracle")
    cpu_parser.add_argument("--completion", choices=("after-command", "after-destroy",
                                                       "before-next-work"),
                            default="after-destroy")
    cpu_parser.add_argument("--completion-plan", type=pathlib.Path,
                            help="authenticated event-qualified JSON schedule; overrides completion preset")
    cpu_parser.add_argument("--source-projection", type=pathlib.Path,
                            help="validated cpu-source-project artifact; permits bounded native mutation qualification")
    cpu_parser.add_argument("--fault", choices=("none", "before-submit", "before-release",
                                               "managed-allocation"),
                            default="none", help="CPU adapter boundary failure; never a passing policy sample")
    cpu_parser.add_argument("--max-pending", type=int, default=32,
                            help="bounded pending completion budget; zero is a negative control")
    cpu_parser.add_argument("--draw-command-limit", type=int, default=0)
    cpu_parser.add_argument("--slot-headroom-bytes", type=int, default=0)
    cpu_parser.add_argument("--aggregate-headroom-bytes", type=int, default=0)
    cpu_parser.add_argument("--require-path", action="store_true")
    cpu_parser.set_defaults(function=cpu_fixed_input_experiment)
    policy_compare_parser = subparsers.add_parser(
        "cpu-policy-compare",
        help="qualify two versioned CPU policies, then measure them without semantic observation",
    )
    policy_compare_parser.add_argument("bundle", type=pathlib.Path)
    policy_compare_parser.add_argument("--reference-policy", type=pathlib.Path, required=True)
    policy_compare_parser.add_argument("--candidate-policy", type=pathlib.Path, required=True)
    policy_compare_parser.add_argument("--repeat", type=int, default=16)
    policy_compare_parser.add_argument("--warmup", type=int, default=2)
    policy_compare_parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    policy_compare_parser.add_argument("--provider", type=pathlib.Path, default=pathlib.Path(
        "build/tests/native/backend/dxmt9-cpu-ready-production-routing-spec"))
    policy_compare_parser.add_argument("--gpu-provider", type=pathlib.Path, default=pathlib.Path(
        "build/tools/dxmt9-render-tape-provider"))
    policy_compare_parser.add_argument("--validator", type=pathlib.Path, default=pathlib.Path(
        "build/tools/dxmt9-render-tape"))
    policy_compare_parser.add_argument("--timeout-seconds", type=int, default=30)
    policy_compare_parser.add_argument("--gpu-qualify", action="store_true",
                                       help="qualify both policies with strict real-Metal readback before measurement")
    policy_compare_parser.add_argument("--gpu-reset", choices=("cold-device", "warm-device"),
                                       default="cold-device",
                                       help="explicit reset policy for GPU qualification children")
    policy_compare_parser.set_defaults(function=render_tape_cpu_policy.cpu_policy_compare)
    compare_parser = subparsers.add_parser(
        "cpu-compare",
        help="compare compatibility and production-slot CPU Tape replays",
    )
    compare_parser.add_argument("bundle", type=pathlib.Path)
    compare_parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    compare_parser.add_argument(
        "--provider", type=pathlib.Path,
        default=pathlib.Path(
            "build/tests/native/backend/dxmt9-cpu-ready-production-routing-spec"
        ),
    )
    compare_parser.add_argument(
        "--validator", type=pathlib.Path,
        default=pathlib.Path("build/tools/dxmt9-render-tape"),
    )
    compare_parser.add_argument("--repeat", type=int, default=16)
    compare_parser.add_argument("--grammar", choices=("bounded", "general-frame"),
                                default="bounded",
                                help="bounded32-event oracle, or complete-frame general oracle")
    compare_parser.add_argument(
        "--publication", choices=("per-command", "draw-island"),
        default="per-command",
    )
    compare_parser.add_argument("--timeout-seconds", type=int, default=30)
    compare_parser.add_argument(
        "--resource-backend", choices=("cpu-no-gpu", "cpu-metal-resources"),
        default="cpu-no-gpu",
    )
    compare_parser.add_argument(
        "--require-backing-oracle", action="store_true",
        help="require dynamic CPU-visible backing and retention evidence",
    )
    compare_parser.add_argument(
        "--completion", choices=("after-command", "after-destroy", "before-next-work"),
        default="after-destroy",
    )
    compare_parser.add_argument(
        "--completion-plan", type=pathlib.Path,
        help="reserved for route-coupled schedules; currently rejected",
    )
    compare_parser.add_argument(
        "--source-projection", type=pathlib.Path,
        help="same qualified source oracle for both general-frame routes",
    )
    compare_parser.add_argument(
        "--max-pending", type=int, default=32,
        help="bounded pending completion budget shared by both children",
    )
    compare_parser.add_argument(
        "--require-path", action="store_true",
        help="accepted for symmetry; both route paths are always required",
    )
    compare_parser.set_defaults(function=cpu_compare)
    projection_parser = subparsers.add_parser(
        "cpu-source-project", help="qualify source buffer closure through production CPU replay")
    projection_parser.add_argument("bundle", type=pathlib.Path)
    projection_parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    projection_parser.add_argument("--provider", type=pathlib.Path,
        default=pathlib.Path("build/tests/native/backend/dxmt9-cpu-ready-production-routing-spec"))
    projection_parser.add_argument("--validator", type=pathlib.Path,
        default=pathlib.Path("build/tools/dxmt9-render-tape"))
    projection_parser.add_argument("--repeat", type=int, default=2)
    projection_parser.add_argument("--timeout-seconds", type=int, default=60)
    projection_parser.set_defaults(function=cpu_source_project)
    parallel_parser = subparsers.add_parser(
        "parallel-verify",
        help="compare fresh identity and ExplicitParallel production replays",
    )
    parallel_parser.add_argument("bundle", type=pathlib.Path)
    parallel_parser.add_argument(
        "--provider", type=pathlib.Path,
        default=pathlib.Path("build/tools/dxmt9-render-tape-provider"),
    )
    parallel_parser.add_argument(
        "--validator", type=pathlib.Path,
        default=pathlib.Path("build/tools/dxmt9-render-tape"),
    )
    parallel_parser.set_defaults(function=parallel_verify)
    project_parser = subparsers.add_parser(
        "executable-project",
        help="materialize and replay one identity-proven Draw range",
    )
    project_parser.add_argument("bundle", type=pathlib.Path)
    project_parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    project_parser.add_argument("--command-event-ordinal", type=int, required=True)
    project_parser.add_argument("--first-record", type=int, required=True)
    project_parser.add_argument("--record-count", type=int, required=True)
    project_parser.add_argument(
        "--validator", type=pathlib.Path,
        default=pathlib.Path("build/tools/dxmt9-render-tape"),
    )
    project_parser.add_argument(
        "--provider", type=pathlib.Path,
        default=pathlib.Path("build/tools/dxmt9-render-tape-provider"),
    )
    project_parser.set_defaults(function=executable_project)
    for command, function, help_text in (
        (
            "reduce",
            reduce_bundle,
            "retain explicitly selected whole command events and require the provider oracle",
        ),
        (
            "bisect",
            bisect_bundle,
            "deterministically minimize whole command events through the provider oracle",
        ),
    ):
        reduce_parser = subparsers.add_parser(command, help=help_text)
        reduce_parser.add_argument("bundle", type=pathlib.Path)
        reduce_parser.add_argument("--output-dir", type=pathlib.Path, required=True)
        reduce_parser.add_argument(
            "--validator", type=pathlib.Path,
            default=pathlib.Path("build/tools/dxmt9-render-tape"),
        )
        reduce_parser.add_argument(
            "--provider", type=pathlib.Path,
            default=pathlib.Path("build/tools/dxmt9-render-tape-provider"),
        )
        reduce_parser.add_argument("--warmup", type=int, default=0)
        reduce_parser.add_argument("--repeat", type=int, default=1)
        if command == "reduce":
            reduce_parser.add_argument(
                "--select-command-event",
                type=int,
                action="append",
                required=True,
            )
        reduce_parser.set_defaults(function=function)
    return value


def main() -> int:
    args = parser().parse_args()
    return args.function(args)


if __name__ == "__main__":
    raise SystemExit(main())
