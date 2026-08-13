#!/usr/bin/env python3

import argparse
import hashlib
import json
import pathlib
import shutil
import tempfile
import subprocess
import sys
from typing import Any


SCHEMA = "dxmt9.render_tape.bundle.v2"
EVENTS_NAME = "events.bin"
MAX_REPLAY_RUNS = 64


def digest(path: pathlib.Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


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


def run_provider_replay(
    provider: pathlib.Path,
    events: pathlib.Path,
    blob_paths: list[pathlib.Path],
) -> dict[str, Any]:
    arguments = [str(provider), "replay", str(events)]
    for path in blob_paths:
        arguments.extend(("--blob", str(path)))
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


def provider_oracle_accepts(result: dict[str, Any]) -> bool:
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
            and interval["validity"].get("expected_digest_matched") is True
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
        and validity.get("expected_digest_matched") is True
        and isinstance(validity.get("output_sha256"), str)
        and len(validity["output_sha256"]) == 64
        and conservation_valid
        and coverage_valid
        and intervals_valid
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
) -> dict[str, Any]:
    if warmup < 0 or repeat < 1 or warmup + repeat > MAX_REPLAY_RUNS:
        raise SystemExit(
            f"warmup + repeat must be in [1, {MAX_REPLAY_RUNS}] with repeat >= 1"
        )
    warmup_results = []
    for _ in range(warmup):
        result = run_provider_replay(provider, events, blob_paths)
        if not provider_oracle_accepts(result):
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
        last_result = run_provider_replay(provider, events, blob_paths)
        if not provider_oracle_accepts(last_result):
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
    output = args.output_dir.resolve()
    if output.exists():
        if not output.is_dir():
            raise SystemExit(f"output path is not a directory: {output}")
        if any(output.iterdir()):
            raise SystemExit(f"output directory is not empty: {output}")
    output.mkdir(parents=True, exist_ok=True)
    destination = output / EVENTS_NAME
    shutil.copyfile(events, destination)
    event_digest = digest(destination)
    blob_components = []
    blobs_directory = output / "blobs"
    if blobs_by_digest:
        blobs_directory.mkdir()
    for sha256, (source, size) in blobs_by_digest.items():
        blob_name = f"{sha256}.bin"
        shutil.copyfile(source, blobs_directory / blob_name)
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
            "output_oracle": False,
        },
    }
    manifest_path = output / "manifest.json"
    manifest_path.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(json.dumps({"bundle": str(output), "manifest": str(manifest_path)}))
    return 0


def load_bundle(
    bundle: pathlib.Path,
) -> tuple[dict[str, Any], pathlib.Path, list[str], list[pathlib.Path]]:
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
    return manifest, events, blob_refs, blob_paths


def ensure_empty_output(output: pathlib.Path) -> None:
    if output.exists():
        if not output.is_dir():
            raise SystemExit(f"output path is not a directory: {output}")
        if any(output.iterdir()):
            raise SystemExit(f"output directory is not empty: {output}")


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
    output.mkdir(parents=True, exist_ok=True)
    destination = output / EVENTS_NAME
    shutil.copyfile(events, destination)
    blob_components = []
    if referenced_digests:
        (output / "blobs").mkdir()
    for sha256 in referenced_digests:
        source = source_by_digest[sha256]
        target = output / "blobs" / f"{sha256}.bin"
        shutil.copyfile(source, target)
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
            "output_oracle": True,
        },
        "reducer": reducer,
    }
    manifest_path = output / "manifest.json"
    manifest_path.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return manifest_path


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
    manifest, events, blob_refs, _ = load_bundle(args.bundle)
    result = run_validator(args.validator, args.command, events, blob_refs)
    result["bundle_schema"] = manifest["schema"]
    result["events_sha256"] = manifest["components"]["events"]["sha256"]
    result["scope"] = manifest.get("scope", {})
    print(json.dumps(result, sort_keys=True))
    return 0


def provider_replay(args: argparse.Namespace) -> int:
    manifest, events, _, blob_paths = load_bundle(args.bundle)
    result = replay_with_policy(
        args.provider, events, blob_paths, args.warmup, args.repeat
    )
    validity = result.get("validity", {})
    output_oracle = bool(
        result.get("oracle_accepted") is True
        and validity.get("output_readback")
        and validity.get("expected_digest_captured")
        and validity.get("expected_digest_matched")
    )
    scope = dict(manifest.get("scope", {}))
    scope["production_provider_replay"] = result.get("oracle_accepted") is True
    scope["output_oracle"] = output_oracle
    result["bundle_schema"] = manifest["schema"]
    result["events_sha256"] = manifest["components"]["events"]["sha256"]
    result["scope"] = scope
    print(json.dumps(result, sort_keys=True))
    return 0 if result.get("oracle_accepted") else 1


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
    manifest, events, blob_refs, blob_paths = load_bundle(args.bundle)
    if manifest["profile"] != "frame-tape":
        raise SystemExit("render tape reduction supports frame-tape bundles only")
    ensure_empty_output(args.output_dir.resolve())
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
                args.output_dir.resolve(),
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
                "bundle": str(args.output_dir.resolve()),
                "manifest": str(manifest_path),
                "selected_command_events": selections,
                "events_sha256": digest(args.output_dir.resolve() / EVENTS_NAME),
                "oracle_accepted": True,
            },
            sort_keys=True,
        )
    )
    return 0


def partition(values: list[int], count: int) -> list[list[int]]:
    return [values[index::count] for index in range(count) if values[index::count]]


def bisect_bundle(args: argparse.Namespace) -> int:
    manifest, events, blob_refs, blob_paths = load_bundle(args.bundle)
    if manifest["profile"] != "frame-tape":
        raise SystemExit("render tape bisection supports frame-tape bundles only")
    ensure_empty_output(args.output_dir.resolve())
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
            args.output_dir.resolve(),
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
                "bundle": str(args.output_dir.resolve()),
                "manifest": str(manifest_path),
                "source_command_events": commands,
                "selected_command_events": selected,
                "attempts": attempts,
                "accepted_candidates": accepted,
                "events_sha256": digest(args.output_dir.resolve() / EVENTS_NAME),
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
    provider_parser.add_argument("--warmup", type=int, default=0)
    provider_parser.add_argument("--repeat", type=int, default=1)
    provider_parser.set_defaults(function=provider_replay)
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
