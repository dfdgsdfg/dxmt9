#!/usr/bin/env python3

import argparse
import contextlib
import hashlib
import json
import os
import pathlib
import shutil
import tempfile
import subprocess
import sys
from typing import Any, Optional


SCHEMA = "dxmt9.render_tape.bundle.v2"
EVENTS_NAME = "events.bin"
IDENTITY_NAME = "identity.bin"
IDENTITY_SCHEMA = "dxmt9.render_tape.identity.v1"
MAX_REPLAY_RUNS = 64
PARALLEL_VERIFY_RUNS = 2
PROVIDER_SCHEMA = "dxmt9.render_tape.provider_replay.v1"
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


@contextlib.contextmanager
def bundle_transaction(output: pathlib.Path):
    output = output.absolute()
    ensure_empty_output(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    staging = pathlib.Path(
        tempfile.mkdtemp(prefix=f".{output.name}.staging-", dir=output.parent)
    )
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
        try:
            sync_directory(output.parent)
        except OSError:
            # The destination is already complete and atomically claimable.
            # Do not report a post-rename failure that cannot be rolled back.
            pass
    except BaseException:
        shutil.rmtree(staging, ignore_errors=True)
        raise


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


def run_provider_replay(
    provider: pathlib.Path,
    events: pathlib.Path,
    blob_paths: list[pathlib.Path],
    expected_output: Optional[pathlib.Path] = None,
    expected_source: Optional[pathlib.Path] = None,
    output_path: Optional[pathlib.Path] = None,
    partition_mode: Optional[str] = None,
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
) -> dict[str, Any]:
    arguments = [
        str(validator), "materialize", str(events), str(identity), str(output),
        "--command-event-ordinal", str(command_event_ordinal),
        "--first-record", str(first_record),
        "--record-count", str(record_count),
    ]
    if output_sha256 is not None:
        arguments.extend(("--output-sha256", output_sha256))
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
    return result


def projection_provider_run(
    provider: pathlib.Path,
    events: pathlib.Path,
    blob_paths: list[pathlib.Path],
    output: pathlib.Path,
    expected: Optional[pathlib.Path] = None,
) -> dict[str, Any]:
    result = run_provider_replay(provider, events, blob_paths, expected, output)
    validity = result.get("validity", {})
    if (
        result.get("provider_exit_code") != 0
        or result.get("schema") != "dxmt9.render_tape.provider_replay.v1"
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
            native = run_native_materialize(
                args.validator, events, identity, candidate, blob_refs,
                args.command_event_ordinal, args.first_record, args.record_count,
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
                args.provider, candidate, projected_blobs, first_output
            )
            second = projection_provider_run(
                args.provider, candidate, projected_blobs, second_output
            )
            if (first_output.read_bytes() != second_output.read_bytes() or
                    replay_identity(first) != replay_identity(second)):
                raise RuntimeError("projected provider output is nondeterministic")
            projected_digest = digest(first_output)

            final_events = staging / EVENTS_NAME
            final_native = run_native_materialize(
                args.validator, events, identity, final_events, blob_refs,
                args.command_event_ordinal, args.first_record, args.record_count,
                projected_digest,
            )
            if final_native.get("referenced_blobs") != referenced:
                raise RuntimeError("final oracle rewrite changed projection closure")
            run_validator(args.validator, "validate", final_events, projected_refs)
            output_oracle = staging / "output.rgba"
            os.replace(first_output, output_oracle)
            second_output.unlink()
            candidate.unlink()
            final_runs = []
            for index in range(2):
                repeated = staging / f"final-{index}.rgba"
                result = projection_provider_run(
                    args.provider, final_events, projected_blobs, repeated,
                    output_oracle,
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
