#!/usr/bin/env python3
"""Private bounded diagnostic replay of an existing authenticated V4 capture.

Run through scripts/run_python.sh. This does not enable any public V4 execution
command, recapture input, reduce a tape, or qualify a policy/performance result.
Explicit repetition shares immutable prepared input, not device or reset authority.
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
import stat
import subprocess
import sys
import time
from typing import Any


RECEIPT_SCHEMA = "dxmt9.render_tape.v4_capture_diagnostic_run.v1"
NATIVE_SCHEMA = "dxmt9.render_tape.v4_capture_diagnostic.v1"
REPEAT_NATIVE_SCHEMA = "dxmt9.render_tape.v4_capture_diagnostic_repeat.v1"
REPLAY_POLICY_SCHEMA = "dxmt9.render_tape.v4_replay_policy.v1"
TIMING_MEASUREMENTS_SCHEMA = "dxmt9.render_tape.v4_diagnostic_timing.v1"
TIMING_MEASUREMENTS_V2_SCHEMA = "dxmt9.render_tape.v4_diagnostic_timing.v2"
QUERY_TIMING_MEASUREMENTS_SCHEMA = "dxmt9.render_tape.v4_query_timing.v1"
V4_PHASE_TIMING_MEASUREMENTS_SCHEMA = "dxmt9.render_tape.v4_phase_timing.v1"
V4_PHASE_TIMING_MEASUREMENTS_V2_SCHEMA = "dxmt9.render_tape.v4_phase_timing.v2"
PROVIDER_CALL_TIMING_MEASUREMENTS_SCHEMA = "dxmt9.render_tape.v4_provider_call_timing.v1"
PROVIDER_CALL_TIMING_MEASUREMENTS_V2_SCHEMA = "dxmt9.render_tape.v4_provider_call_timing.v2"
IMMUTABLE_V4_PREPARATION_SCHEMA = "dxmt9.render_tape.v4_immutable_preparation.v1"
ADMISSION_EXECUTION_SCHEMA = "dxmt9.render_tape.v4_admission_execution.v1"
MAX_DIAGNOSTIC_TIMING_NS = (1 << 64) - 1
MAX_QUERY_TIMING_COUNT = (1 << 32) - 1
MAX_ADMISSION_COUNT = (1 << 32) - 1
PRODUCER_CONTRACTS = ("none", "pe-present-reservation-v1")
PUBLICATION_MODES = ("exact-source-diagnostic", "semantic-mapping")
ACTION_PROFILE_COMPONENT = ("action_profile.bin", "--action-profile-sha256")
MAX_MANIFEST_BYTES = 2 * 1024 * 1024
MAX_INPUT_BYTES = 1024 * 1024 * 1024
MAX_BLOBS = 65536
MAX_RESULT_JSON_BYTES = 16 * 1024 * 1024
MAX_REPEAT_JSON_BYTES = 128 * 1024 * 1024
MAX_REPEAT_READBACK_BYTES = 512 * 1024 * 1024
MAX_REPEAT_ITERATIONS = 64
MAX_LOADED_IMAGES = 256
MIN_SLOT_HEADROOM_BYTES = 1024 * 1024
MAX_SLOT_HEADROOM_BYTES = 64 * 1024 * 1024
MAX_AGGREGATE_HEADROOM_BYTES = 512 * 1024 * 1024
MIN_DRAW_COMMAND_LIMIT = 3
MAX_DRAW_COMMAND_LIMIT = 64
REQUIRED_RUNTIME_IMAGES = frozenset(("libdxmt9_native.dylib", "winemetal_dxmt9.so"))
REPLAY_STAGE_ENVIRONMENT = {"DXMT_DEBUG_DISABLE_SHADER_ARCHIVE": "1",
                            "DXMT9_PREWARM": "disabled", "DXMT_PERF_COUNTERS": "1",
                            "DXMT9_DIRECT_SLOT_HEADROOM_BYTES": "0",
                            "DXMT9_DIRECT_SLOT_AGGREGATE_HEADROOM_BYTES": "0",
                            "DXMT9_DRAW_CHUNK_COMMAND_LIMIT": "0",
                            "DXMT9_CHUNK_DRAW_PAYLOAD_ARENA_LIMIT_BYTES": "0"}
COMPONENTS = {
    "events": ("events.bin", "--events-sha256"),
    "identity": ("identity.bin", "--identity-sha256"),
    "execution_topology": ("execution_topology.bin", "--execution-topology-sha256"),
    "source_oracle": ("source.rgba", "--source-sha256"),
    "output_oracle": ("output.rgba", "--output-sha256"),
}
GATES = ("query_qualified", "pixels_qualified", "source_exact", "output_exact")


class Rejected(RuntimeError):
    pass


def require_time(deadline: float) -> float:
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise Rejected("shared diagnostic deadline exhausted")
    return remaining


def sha256_nonzero(value: Any) -> bool:
    return isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value) is not None and value != "0" * 64


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise Rejected(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def reject_constant(value: str):
    raise Rejected(f"non-finite JSON constant: {value}")


def parse_json(raw: bytes) -> Any:
    return json.loads(raw, object_pairs_hook=unique_object, parse_constant=reject_constant)


def read_capped(path: pathlib.Path, limit: int) -> bytes:
    flags = os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC | os.O_NONBLOCK
    with os.fdopen(os.open(path, flags), "rb") as stream:
        if not stat.S_ISREG(os.fstat(stream.fileno()).st_mode):
            raise Rejected(f"diagnostic input/result must be a regular non-symlink file: {path.name}")
        raw = stream.read(limit + 1)
    if len(raw) > limit:
        raise Rejected(f"diagnostic byte cap exceeded: {path.name}")
    return raw


def write_receipt(path: pathlib.Path, receipt: dict[str, Any]) -> None:
    with path.open("w", encoding="utf-8") as stream:
        json.dump(receipt, stream, indent=2, sort_keys=True, allow_nan=False)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())


def load_bundle_tool():
    path = pathlib.Path(__file__).resolve().parents[2] / "scripts/tools/run_dxmt9_render_tape.py"
    spec = importlib.util.spec_from_file_location("v4_capture_diagnostic_bundle_tool", path)
    if spec is None or spec.loader is None:
        raise Rejected("cannot load the shared render tape bundle authenticator")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def bound_inputs(bundle: pathlib.Path, deadline: float) -> tuple[dict, bytes, int]:
    """Precheck immutable-input capacity; these are not admission proofs."""
    require_time(deadline)
    raw = read_capped(bundle / "manifest.json", MAX_MANIFEST_BYTES)
    require_time(deadline)
    manifest = parse_json(raw)
    if not isinstance(manifest, dict) or not isinstance(manifest.get("components"), dict):
        raise Rejected("bundle manifest/components must be objects")
    components = manifest["components"]
    entries = []
    for name, (path, _option) in COMPONENTS.items():
        entry = components.get(name)
        if not isinstance(entry, dict) or entry.get("path") != path:
            raise Rejected(f"private diagnostic requires canonical components.{name}")
        entries.append(entry)
    if "action_profile" in components:
        entry = components["action_profile"]
        if not isinstance(entry, dict) or entry.get("path") != ACTION_PROFILE_COMPONENT[0]:
            raise Rejected("private diagnostic requires canonical components.action_profile")
        entries.append(entry)
    blobs = components.get("blobs", [])
    if not isinstance(blobs, list) or len(blobs) > MAX_BLOBS:
        raise Rejected("bundle blob list exceeds the diagnostic count cap")
    for blob in blobs:
        if (not isinstance(blob, dict) or not isinstance(blob.get("path"), str)
                or re.fullmatch(r"blobs/[0-9a-f]{64}\.bin", blob["path"]) is None):
            raise Rejected("noncanonical blob path before bounded authentication")
        entries.append(blob)
    total = len(raw)
    for entry in entries:
        require_time(deadline)
        size = (bundle / entry["path"]).stat().st_size
        if (type(entry.get("bytes")) is not int or entry["bytes"] != size
                or size <= 0 or size > MAX_INPUT_BYTES - total):
            raise Rejected("component size mismatch or diagnostic total-input byte cap exceeded")
        total += size
    blobs_directory = bundle / "blobs"
    if blobs_directory.exists():
        with os.scandir(blobs_directory) as entries_in_directory:
            for count, _entry in enumerate(entries_in_directory, 1):
                require_time(deadline)
                if count > MAX_BLOBS:
                    raise Rejected("blob directory exceeds the diagnostic count cap")
    require_time(deadline)
    return manifest, raw, total


def require_regular_manifest(bundle: pathlib.Path, deadline: float) -> None:
    require_time(deadline)
    path = bundle / "manifest.json"
    if not stat.S_ISREG(path.lstat().st_mode) or path.resolve().parent != bundle:
        raise Rejected("authenticated manifest must be a regular non-symlink file")


def require_regular_inputs(bundle: pathlib.Path, manifest: dict, deadline: float | None = None) -> None:
    entries = ["manifest.json"]
    entries.extend(value[0] for value in COMPONENTS.values())
    if "action_profile" in manifest["components"]:
        entries.append(ACTION_PROFILE_COMPONENT[0])
    entries.extend(blob["path"] for blob in manifest["components"].get("blobs", []))
    for relative in entries:
        if deadline is not None:
            require_time(deadline)
        path = bundle / relative
        if not stat.S_ISREG(path.lstat().st_mode) or path.resolve().parent != path.parent:
            raise Rejected(f"authenticated input must be a regular non-symlink file: {relative}")
    blobs = bundle / "blobs"
    if blobs.exists() and (blobs.is_symlink() or not blobs.is_dir()):
        raise Rejected("authenticated blob directory must not be a symlink")


def fingerprint_file(path: pathlib.Path, runner, deadline: float) -> dict:
    require_time(deadline)
    signature = lambda value: (value.st_dev, value.st_ino, value.st_size, value.st_mtime_ns, value.st_ctime_ns)
    flags = os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC | os.O_NONBLOCK
    with os.fdopen(os.open(path, flags), "rb") as stream:
        before = os.fstat(stream.fileno())
        if not stat.S_ISREG(before.st_mode) or not 0 < before.st_size <= MAX_INPUT_BYTES:
            raise Rejected("authenticated executable/image must be a bounded regular non-symlink file")
        digest, total = hashlib.sha256(), 0
        while True:
            require_time(deadline)
            chunk = stream.read(65536)
            if not chunk:
                break
            total += len(chunk)
            if total > before.st_size:
                raise Rejected("authenticated executable/image grew during hashing")
            digest.update(chunk)
        after = os.fstat(stream.fileno())
        value = digest.hexdigest()
        if (total != before.st_size or signature(before) != signature(after)
                or signature(path.lstat()) != signature(after) or not sha256_nonzero(value)):
            raise Rejected("authenticated executable/image changed during hashing")
    require_time(deadline)
    return {"bytes": before.st_size, "sha256": value}


def validate_loaded_images(result: dict, diagnostic: pathlib.Path, binary: dict,
                           runner, deadline: float) -> None:
    images = result.get("loaded_images")
    if not isinstance(images, list) or not 1 <= len(images) <= MAX_LOADED_IMAGES:
        raise Rejected("native loaded image evidence is absent or exceeds its count cap")
    seen, basenames, total = set(), set(), 0
    for entry in images:
        if not isinstance(entry, dict) or not isinstance(entry.get("path"), str):
            raise Rejected("native loaded image entry is malformed")
        path = pathlib.Path(entry["path"])
        size, digest = entry.get("bytes"), entry.get("sha256")
        try:
            canonical = path.resolve(strict=True)
        except (OSError, RuntimeError) as error:
            raise Rejected("native loaded image path cannot be canonicalized") from error
        if (not path.is_absolute() or str(path) != entry["path"] or canonical != path or path in seen
                or type(size) is not int or not 0 < size <= MAX_INPUT_BYTES
                or not sha256_nonzero(digest)):
            raise Rejected("native loaded image path/size/digest is malformed")
        seen.add(path)
        basenames.add(path.name)
        total += size
        if total > MAX_INPUT_BYTES:
            raise Rejected("native loaded image total byte cap exceeded")
        expected = {"bytes": size, "sha256": digest}
        if fingerprint_file(path, runner, deadline) != expected:
            raise Rejected("native loaded image fingerprint changed")
        if path == diagnostic and expected != binary:
            raise Rejected("native loaded executable does not match launcher authentication")
    if (diagnostic not in seen or not REQUIRED_RUNTIME_IMAGES.issubset(basenames)
            or not runner.experiment_images_stable(result)):
        raise Rejected("native loaded executable/image reauthentication failed")
    require_time(deadline)


def validate_readbacks(result: dict, output: pathlib.Path, bundle: pathlib.Path,
                       manifest: dict, output_identity: tuple[int, int], deadline: float,
                       *, recorded_directory: str | None = None) -> dict:
    require_time(deadline)
    directory_status = output.lstat()
    if (not stat.S_ISDIR(directory_status.st_mode) or output.resolve() != output
            or (directory_status.st_dev, directory_status.st_ino) != output_identity
            or directory_status.st_uid != os.geteuid()):
        raise Rejected("readback artifact directory is no longer the owned output directory")
    artifacts = result.get("readback_artifacts")
    if (not isinstance(artifacts, dict) or artifacts.get("requested") is not True
            or artifacts.get("attempted") is not True or artifacts.get("failure") != "none"
            or artifacts.get("directory") != (recorded_directory if recorded_directory is not None else str(output))):
        raise Rejected("native readback artifact ownership/status evidence rejected")
    verified = {}
    for name in ("source", "output"):
        component = manifest["components"][f"{name}_oracle"]
        expected_size, expected_digest = component["bytes"], component["sha256"]
        artifact = artifacts.get(name)
        if (not isinstance(artifact, dict) or artifact.get("attempted") is not True
                or artifact.get("written") is not True or artifact.get("failure") != "none"
                or type(artifact.get("bytes")) is not int or artifact["bytes"] != expected_size
                or type(artifact.get("written_bytes")) is not int or artifact["written_bytes"] != expected_size
                or artifact.get("sha256") != expected_digest):
            raise Rejected(f"native {name} readback size/hash/status does not match the authenticated oracle")
        path, original = output / f"replay-{name}.rgba", bundle / component["path"]
        open_flags = os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC | os.O_NONBLOCK
        with os.fdopen(os.open(path, open_flags), "rb") as actual, \
             os.fdopen(os.open(original, open_flags), "rb") as oracle:
            actual_before, oracle_before = os.fstat(actual.fileno()), os.fstat(oracle.fileno())
            if (not stat.S_ISREG(actual_before.st_mode) or actual_before.st_uid != os.geteuid()
                    or actual_before.st_nlink != 1 or actual_before.st_size != expected_size
                    or not stat.S_ISREG(oracle_before.st_mode) or oracle_before.st_size != expected_size):
                raise Rejected(f"{name} readback must be a regular owned exact-size exclusive artifact")
            actual_digest, oracle_digest, compared = hashlib.sha256(), hashlib.sha256(), 0
            while True:
                require_time(deadline)
                actual_bytes, oracle_bytes = actual.read(65536), oracle.read(65536)
                if actual_bytes != oracle_bytes:
                    raise Rejected(f"{name} readback bytes differ from the authenticated original oracle")
                if not actual_bytes:
                    break
                compared += len(actual_bytes)
                if compared > expected_size:
                    raise Rejected(f"{name} readback grew beyond the authenticated oracle size")
                actual_digest.update(actual_bytes)
                oracle_digest.update(oracle_bytes)
            actual_after, oracle_after = os.fstat(actual.fileno()), os.fstat(oracle.fileno())
            signature = lambda value: (value.st_dev, value.st_ino, value.st_size, value.st_mtime_ns, value.st_ctime_ns)
            if (compared != expected_size or actual_digest.hexdigest() != expected_digest
                    or oracle_digest.hexdigest() != expected_digest
                    or signature(actual_before) != signature(actual_after)
                    or signature(oracle_before) != signature(oracle_after)
                    or signature(path.lstat()) != signature(actual_after)
                    or signature(original.lstat()) != signature(oracle_after)):
                raise Rejected(f"{name} readback/oracle changed across independent comparison")
        verified[name] = {"path": path.name, "bytes": compared, "sha256": expected_digest,
                          "regular_owned_artifact": True, "exact_original_bytes": True}
    require_time(deadline)
    directory_after = output.lstat()
    if (not stat.S_ISDIR(directory_after.st_mode) or output.resolve() != output
            or (directory_after.st_dev, directory_after.st_ino) != output_identity
            or directory_after.st_uid != os.geteuid()):
        raise Rejected("readback artifact directory changed across independent comparison")
    return verified


def environment_provenance() -> dict[str, str]:
    result = {}
    for key, value in sorted(os.environ.items()):
        if not key.startswith(("DXMT", "MTL", "DYLD")):
            continue
        sensitive = any(part in key.upper() for part in ("SECRET", "PASSWORD", "CREDENTIAL", "API_KEY", "PRIVATE_KEY", "TOKEN"))
        result[key] = "<redacted>" if sensitive else value
    return result


def child_json(path: pathlib.Path) -> dict:
    result = parse_json(read_capped(path, MAX_RESULT_JSON_BYTES))
    if not isinstance(result, dict):
        raise Rejected("native diagnostic result is not a JSON object")
    return result


def run_child(name: str, argv: list[str], output: pathlib.Path, deadline: float,
              receipt: dict[str, Any], receipt_path: pathlib.Path) -> dict:
    step = receipt["steps"][name]
    step.update(argv=argv, status="running", timeout_seconds=require_time(deadline))
    write_receipt(receipt_path, receipt)
    started = time.monotonic()
    with (output / step["stdout"]).open("wb") as stdout, (output / step["stderr"]).open("wb") as stderr:
        try:
            completed = subprocess.run(argv, stdin=subprocess.DEVNULL, stdout=stdout,
                                       stderr=stderr, check=False, timeout=require_time(deadline))
            step.update(status="exited", returncode=completed.returncode, timed_out=False)
        except subprocess.TimeoutExpired:
            step.update(status="child-timeout", returncode=None, timed_out=True)
        except KeyboardInterrupt:
            step.update(status="interrupted", returncode=None, timed_out=False)
            raise
        except OSError as error:
            step.update(status="spawn-failed", returncode=None, timed_out=False, failure=str(error))
        except Rejected as error:
            step.update(status="deadline-exhausted", returncode=None, timed_out=False, failure=str(error))
            raise
        finally:
            step["elapsed_seconds"] = time.monotonic() - started
            stdout.flush()
            os.fsync(stdout.fileno())
            stderr.flush()
            os.fsync(stderr.fileno())
            write_receipt(receipt_path, receipt)
    if step["status"] != "exited":
        raise Rejected(f"{name} {step['status']}")
    result = child_json(output / step["stdout"])
    step["native_result"] = result
    write_receipt(receipt_path, receipt)
    if step["returncode"] != 0:
        raise Rejected(f"{name} rejected with exit code {step['returncode']}")
    require_time(deadline)
    return result


def validate_producer_contract(manifest: dict, contract: str, supported_schema: str) -> None:
    if contract == "none":
        return
    producer, scope = manifest.get("producer"), manifest.get("scope")
    if (manifest.get("schema") != supported_schema or manifest.get("profile") != "frame-tape"
            or not isinstance(producer, dict) or producer.get("path") != "dxmt9-pe-render-tape"
            or manifest.get("stage") != "production-capture" or not isinstance(scope, dict)
            or any(scope.get(field) is not True
                   for field in ("production_capture", "output_oracle", "source_oracle"))):
        raise Rejected("selected producer contract requires authenticated production PE frame-tape provenance and both oracle scopes")


def validate_common(result: dict, producer_contract: str, publication_mode: str) -> None:
    if (result.get("schema") != NATIVE_SCHEMA or result.get("diagnostic") is not True
            or result.get("policy_qualified") is not False or result.get("timing_eligible") is not False):
        raise Rejected("native result has an unsupported schema or unsafe diagnostic scope")
    if result.get("producer_contract") != producer_contract:
        raise Rejected("native producer contract echo does not match the selected contract")
    if result.get("publication_mode") != publication_mode:
        raise Rejected("native publication mode echo does not match the selected mode")
    validate_timing_measurements(result)
    validate_query_timing_measurements(result)
    validate_v4_phase_timing_measurements(result)
    validate_provider_call_timing_measurements(result)
    validate_immutable_v4_preparation(result)
    validate_admission_execution(result)
    validate_v4_preparation_consistency(result)
    if publication_mode == "semantic-mapping":
        action_profile = result.get("action_profile")
        if (not isinstance(action_profile, dict) or action_profile.get("authenticated") is not True
                or action_profile.get("stable_exact") is not True):
            raise Rejected("native semantic diagnostic lacks independently authenticated stable exact action witness")


def validate_timing_measurements(result: dict) -> None:
    # Old native diagnostics have no block. A present block is a strict cold
    # service-cost contract, never authority for policy or performance timing.
    if "timing_measurements" not in result:
        return
    timing = result["timing_measurements"]
    if (not isinstance(timing, dict) or type(timing.get("schema")) is not str
            or timing.get("schema") not in (TIMING_MEASUREMENTS_SCHEMA, TIMING_MEASUREMENTS_V2_SCHEMA)):
        raise Rejected("native diagnostic timing measurements are absent or malformed")
    literals = {"schema": timing["schema"],
                "authority": "diagnostic-service-cost-only",
                "clock": "std::chrono::steady_clock", "units": "nanoseconds",
                "thread_cpu_time": False, "additive": False, "overlapping_scopes": True}
    groups = {"prepared_phases", "runtime_snapshot_delta", "unavailable"}
    if not isinstance(timing, dict) or set(timing) != set(literals) | groups:
        raise Rejected("native diagnostic timing measurements are absent or malformed")
    for field, expected in literals.items():
        actual = timing[field]
        if type(actual) is not type(expected) or actual != expected:
            raise Rejected(f"native diagnostic timing authority/scope is invalid: {field}")

    prepared_literals = {
        "scope": "owned-input-preparation-and-prepared-replay-phases;excludes-file-loading-factory-creation-device-teardown",
        "zero_semantics": "not-entered-or-below-clock-resolution",
        "prepare_wall_scope": ("owned-tape-blob-copy-general-provider-preflight-and-immutable-v4-preparation"
                               if timing["schema"] == TIMING_MEASUREMENTS_V2_SCHEMA
                               else "owned-tape-blob-copy-and-provider-preflight"),
        "resource_setup_scope": "object-bootstrap-and-mutations-including-synchronous-drains",
        "chunk_replay_scope": "provider-import-resolve-dispatch-including-synchronous-drains;not-encode-thread",
        "readback_drain_scope": "flush-completion-readback-format-packing-digest;not-gpu-only",
        "cleanup_scope": "provider-flush-and-release;not-device-teardown",
        "reset_scope": "fresh-device-no-warm-reset",
        "replay_wall_scope": "replay-general-plan-after-policy-admission-and-reset;not-entire-provider-call"}
    prepared_fields = {"prepare_wall_ns", "reset_wall_ns", "resource_setup_ns",
                       "chunk_replay_ns", "readback_drain_ns", "cleanup_ns", "replay_wall_ns"}
    runtime_literals = {
        "scope": "before-fresh-factory-create-through-after-device-factory-teardown",
        "zero_semantics": "no-recorded-interval;gated-counter-zero-does-not-prove-no-work",
        "prepared_serial_build_gate": "DXMT9_PREPARED_BUILD_TIMING=1-required",
        "resource_mark_scope": "nested-in-prepare-slot",
        "prepared_serial_build_scope": "prepared-serial-read-planning-plus-resource-marking;not-framegraph-planning",
        "encode_chunk_scope": "actual-encode-thread-inclusive-render-pass-setup-pso-work-and-metal-encoding;not-encoder-only"}
    runtime_fields = {"prepare_slot_wall_ns", "resource_mark_wall_ns",
                      "prepared_serial_build_wall_ns", "encode_chunk_wall_ns"}
    unavailable_fields = {"framegraph_planning_ns", "render_pass_preparation_ns", "encoder_only_ns",
                          "gpu_execution_ns", "gpu_completion_wait_ns", "readback_only_ns"}
    for name, expected_literals, fields, flags in (
            ("prepared_phases", prepared_literals, prepared_fields, {"observed"}),
            ("runtime_snapshot_delta", runtime_literals, runtime_fields, {"observed", "monotonic"})):
        group = timing[name]
        if not isinstance(group, dict) or set(group) != set(expected_literals) | fields | flags:
            raise Rejected(f"native diagnostic timing group is malformed: {name}")
        for field, expected in expected_literals.items():
            if group[field] != expected:
                raise Rejected(f"native diagnostic timing elapsed scope is invalid: {name}.{field}")
        if any(type(group[field]) is not bool for field in flags):
            raise Rejected(f"native diagnostic timing observation flags are malformed: {name}")
        if name == "runtime_snapshot_delta" and not group["observed"] and group["monotonic"]:
            raise Rejected("native diagnostic timing unobserved runtime counters cannot be monotonic")
        observed = group["observed"]
        if observed:
            stages = result.get("stages")
            if (result.get("status") != "complete" or result.get("failure") != "none"
                    or result.get("qualified") is not True or result.get("device_created") is not True
                    or type(result.get("execution_count")) is not int or result["execution_count"] != 1
                    or not isinstance(stages, dict) or stages.get("actual_metal") is not True
                    or stages.get("fresh_device_teardown_complete") is not True):
                raise Rejected("native diagnostic timing observations require successful actual-Metal replay")
        available = observed and (name != "runtime_snapshot_delta" or group["monotonic"])
        for field in fields:
            value = group[field]
            if available:
                if type(value) is not int or not 0 <= value <= MAX_DIAGNOSTIC_TIMING_NS:
                    raise Rejected(f"native diagnostic timing nanoseconds are malformed: {name}.{field}")
            elif value is not None:
                raise Rejected(f"native diagnostic timing unavailable dimension must be null: {name}.{field}")
    unavailable = timing["unavailable"]
    if (not isinstance(unavailable, dict) or set(unavailable) != unavailable_fields
            or any(value is not None for value in unavailable.values())):
        raise Rejected("native diagnostic timing unavailable dimensions must remain explicit nulls")


def validate_query_timing_measurements(result: dict) -> None:
    if "query_timing_measurements" not in result:
        return
    timing = result["query_timing_measurements"]
    literals = {
        "schema": QUERY_TIMING_MEASUREMENTS_SCHEMA,
        "authority": "diagnostic-service-cost-only",
        "clock": "std::chrono::steady_clock", "units": "nanoseconds",
        "thread_cpu_time": False, "additive": False,
        "nesting_scope": "within-prepared-replay-wall;not-additive-with-other-service-scopes",
        "zero_semantics": "no-recorded-interval-or-below-clock-resolution;not-proof-of-no-query-work",
        "null_semantics": "unobserved-or-unqualified",
        "checkpoint_setup_scope": "checkpoint-event-create-end-snapshot-flush-poll-yield-and-receipt-bookkeeping;not-gpu-only",
        "transcript_scope": "captured-query-getdata-validation-dispatch-including-flush-snapshot-result-comparison-and-row-bookkeeping;excludes-query-issue-records",
        "oracle_scope": "post-transcript-deferred-query-obligation-snapshot-validation-promotion-flush-poll-yield-and-result-comparison;not-gpu-only",
        "checkpoint_setup_count_scope": "setup-end-calls-queue-publication-deltas-poll-calls-and-pending-poll-yield-retries;not-blocking-wait-time"}
    nanoseconds = {"checkpoint_setup_wall_ns", "transcript_wall_ns", "oracle_wall_ns"}
    counts = {"checkpoint_setup_end_count", "checkpoint_setup_queue_publication_count",
              "checkpoint_setup_poll_count", "checkpoint_setup_pending_poll_yield_count"}
    if (not isinstance(timing, dict) or set(timing) != set(literals) | nanoseconds | counts | {"observed"}
            or type(timing["observed"]) is not bool):
        raise Rejected("native query timing measurements are absent or malformed")
    for field, expected in literals.items():
        if type(timing[field]) is not type(expected) or timing[field] != expected:
            raise Rejected(f"native query timing authority/elapsed scope is invalid: {field}")
    if timing["observed"]:
        stages, query = result.get("stages"), result.get("query_actual")
        if (result.get("status") != "complete" or result.get("failure") != "none"
                or result.get("qualified") is not True or result.get("device_created") is not True
                or result.get("query_qualified") is not True
                or type(result.get("execution_count")) is not int or result["execution_count"] != 1
                or not isinstance(stages, dict) or stages.get("actual_metal") is not True
                or stages.get("fresh_device_teardown_complete") is not True
                or not isinstance(query, dict) or any(query.get(field) is not True for field in
                    ("present", "actual_metal", "transcript_complete", "oracle_complete"))):
            raise Rejected("native query timing observations require successful qualified actual-Metal query replay")
    # Existing provider counters are uint32; clocks are uint64. Neither a zero
    # clock nor pending-poll yields establish GPU-only or blocking-wait cost.
    for fields, maximum in ((nanoseconds, MAX_DIAGNOSTIC_TIMING_NS), (counts, MAX_QUERY_TIMING_COUNT)):
        for field in fields:
            value = timing[field]
            if timing["observed"]:
                if type(value) is not int or not 0 <= value <= maximum:
                    raise Rejected(f"native query timing unsigned value is malformed: {field}")
            elif value is not None:
                raise Rejected(f"native query timing unobserved dimension must be null: {field}")


def validate_v4_phase_timing_measurements(result: dict) -> None:
    if "v4_phase_timing_measurements" not in result:
        return
    timing = result["v4_phase_timing_measurements"]
    if (not isinstance(timing, dict) or type(timing.get("schema")) is not str
            or timing.get("schema") not in (V4_PHASE_TIMING_MEASUREMENTS_SCHEMA,
                                           V4_PHASE_TIMING_MEASUREMENTS_V2_SCHEMA)):
        raise Rejected("native V4 phase timing measurements are absent or malformed")
    literals = {
        "schema": timing["schema"],
        "authority": "diagnostic-service-cost-only",
        "clock": "std::chrono::steady_clock", "units": "nanoseconds",
        "thread_cpu_time": False, "additive": False,
        "outer_plan_admission_scope": (
            "fresh-v4-execution-initialization-and-initial-setup-accounting-from-immutable-preparation;outside-prepared-replay-wall"
            if timing["schema"] == V4_PHASE_TIMING_MEASUREMENTS_V2_SCHEMA else
            "outer-v4-plan-build-digest-check-execution-admit-and-initial-setup-validation;outside-prepared-replay-wall"),
        "replay_admission_scope": "replay-general-plan-v4-execution-admit;within-prepared-replay-wall",
        "pre_arm_checkpoint_scope": "begin-render-tape-checkpoint-pre-arm;within-prepared-replay-wall",
        "finish_scope": "v4-execution-finish-including-actual-qualification;within-prepared-replay-wall"}
    nanoseconds = {"outer_plan_admission_ns", "replay_admission_ns", "pre_arm_checkpoint_ns", "finish_ns"}
    if (not isinstance(timing, dict) or set(timing) != set(literals) | nanoseconds | {"observed"}
            or type(timing["observed"]) is not bool):
        raise Rejected("native V4 phase timing measurements are absent or malformed")
    for field, expected in literals.items():
        if type(timing[field]) is not type(expected) or timing[field] != expected:
            raise Rejected(f"native V4 phase timing authority/elapsed scope is invalid: {field}")
    if timing["observed"]:
        stages = result.get("stages")
        if (result.get("status") != "complete" or result.get("failure") != "none"
                or result.get("qualified") is not True or result.get("device_created") is not True
                or type(result.get("execution_count")) is not int or result["execution_count"] != 1
                or not isinstance(stages, dict) or stages.get("actual_metal") is not True
                or stages.get("fresh_device_teardown_complete") is not True):
            raise Rejected("native V4 phase timing observations require successful qualified actual-Metal replay")
    for field in nanoseconds:
        value = timing[field]
        if timing["observed"]:
            if type(value) is not int or not 0 <= value <= MAX_DIAGNOSTIC_TIMING_NS:
                raise Rejected(f"native V4 phase timing nanoseconds are malformed: {field}")
        elif value is not None:
            raise Rejected(f"native V4 phase timing unobserved dimension must be null: {field}")


def validate_provider_call_timing_measurements(result: dict) -> None:
    if "provider_call_timing_measurements" not in result:
        return
    timing = result["provider_call_timing_measurements"]
    if (not isinstance(timing, dict) or type(timing.get("schema")) is not str
            or timing.get("schema") not in (PROVIDER_CALL_TIMING_MEASUREMENTS_SCHEMA,
                                           PROVIDER_CALL_TIMING_MEASUREMENTS_V2_SCHEMA)):
        raise Rejected("native provider-call timing measurements are absent or malformed")
    literals = {
        "schema": timing["schema"],
        "authority": "diagnostic-service-cost-only",
        "clock": "std::chrono::steady_clock", "units": "nanoseconds",
        "thread_cpu_time": False, "additive": False,
        "provider_call_scope": (
            "caller-full-expression-replayPreparedRenderTapeIdentityV4-including-return-local-destruction-result-assignment-and-existing-result-replacement;excludes-input-preparation-execution-option-setup-factory-creation-device-teardown"
            if timing["schema"] == PROVIDER_CALL_TIMING_MEASUREMENTS_V2_SCHEMA else
            "caller-full-expression-replayPreparedRenderTapeIdentity-including-return-local-destruction-result-assignment-and-existing-result-replacement;excludes-input-preparation-execution-option-setup-factory-creation-device-teardown"),
        "included_phases": (
            "fresh-v4-execution-initialization-initial-setup-accounting-and-executor-destruction"
            if timing["schema"] == PROVIDER_CALL_TIMING_MEASUREMENTS_V2_SCHEMA else
            "outer-v4-admission-and-executor-destruction"),
        "nesting_scope": "includes-prepared-replay-wall-and-v4-phases;not-additive-with-other-service-scopes",
        "zero_semantics": "below-clock-resolution;not-proof-of-no-provider-work",
        "null_semantics": "unobserved-or-unqualified"}
    if (not isinstance(timing, dict) or set(timing) != set(literals) | {"observed", "provider_call_wall_ns"}
            or type(timing["observed"]) is not bool):
        raise Rejected("native provider-call timing measurements are absent or malformed")
    for field, expected in literals.items():
        if type(timing[field]) is not type(expected) or timing[field] != expected:
            raise Rejected(f"native provider-call timing authority/elapsed scope is invalid: {field}")
    if timing["observed"]:
        stages = result.get("stages")
        if (result.get("status") != "complete" or result.get("failure") != "none"
                or result.get("qualified") is not True or result.get("device_created") is not True
                or type(result.get("execution_count")) is not int or result["execution_count"] != 1
                or not isinstance(stages, dict) or stages.get("actual_metal") is not True
                or stages.get("fresh_device_teardown_complete") is not True):
            raise Rejected("native provider-call timing observations require successful qualified actual-Metal replay")
        value = timing["provider_call_wall_ns"]
        # Exact bounded integers are finite nanoseconds; bools and floats cannot
        # turn a malformed clock into diagnostic service-cost evidence.
        if type(value) is not int or not 0 <= value <= MAX_DIAGNOSTIC_TIMING_NS:
            raise Rejected("native provider-call timing nanoseconds are malformed")
    elif timing["provider_call_wall_ns"] is not None:
        raise Rejected("native provider-call timing unobserved dimension must be null")


def validate_immutable_v4_preparation(result: dict) -> None:
    if "immutable_v4_preparation" not in result:
        return
    preparation = result["immutable_v4_preparation"]
    literals = {
        "schema": IMMUTABLE_V4_PREPARATION_SCHEMA,
        "authority": "diagnostic-immutable-input-validity-only",
        "clock": "std::chrono::steady_clock", "units": "nanoseconds",
        "thread_cpu_time": False, "additive": False,
        "preparation_scope": "one-off-provider-v4-source-plan-execution-template-and-initial-setup-plan-construction;excludes-owned-tape-blob-copy-general-preflight-independent-diagnostic-inspection-and-device-effects",
        "sharing_scope": "one-off-provider-preparation-reading-repeated-per-execution;not-per-iteration-work-or-an-aggregate",
        "zero_semantics": "below-clock-resolution;not-proof-of-no-preparation-work",
        "null_semantics": "unobserved-or-unqualified"}
    flags = {"immutable_preparation_owned", "reused_for_execution", "prepared_handoff_consumed"}
    counts = {"preparation_count", "fresh_execution_admission_count"}
    measurements = flags | counts | {"v4_prepare_wall_ns"}
    if (not isinstance(preparation, dict) or set(preparation) != set(literals) | measurements | {"observed"}
            or type(preparation["observed"]) is not bool):
        raise Rejected("native immutable V4 preparation evidence is absent or malformed")
    for field, expected in literals.items():
        if type(preparation[field]) is not type(expected) or preparation[field] != expected:
            raise Rejected(f"native immutable V4 preparation authority/scope is invalid: {field}")
    if preparation["observed"]:
        stages = result.get("stages")
        if (result.get("status") != "complete" or result.get("failure") != "none"
                or result.get("qualified") is not True or result.get("device_created") is not True
                or type(result.get("execution_count")) is not int or result["execution_count"] != 1
                or not isinstance(stages, dict) or stages.get("actual_metal") is not True
                or stages.get("fresh_device_teardown_complete") is not True):
            raise Rejected("native immutable V4 preparation observations require successful qualified actual-Metal replay")
        for field in flags:
            if preparation[field] is not True:
                raise Rejected(f"native immutable V4 preparation requires provider-owned preparation and fresh execution reuse: {field}")
        for field in counts:
            if type(preparation[field]) is not int or preparation[field] != 1:
                raise Rejected(f"native immutable V4 preparation requires exactly one preparation and fresh admission: {field}")
        value = preparation["v4_prepare_wall_ns"]
        if type(value) is not int or not 0 <= value <= MAX_DIAGNOSTIC_TIMING_NS:
            raise Rejected("native immutable V4 preparation nanoseconds are malformed")
    elif any(preparation[field] is not None for field in measurements):
        raise Rejected("native immutable V4 preparation unobserved dimensions must be null")


def validate_admission_execution(result: dict) -> None:
    if "admission_execution" not in result:
        return
    execution = result["admission_execution"]
    fields = {"schema", "observed", "admission_count", "prepared_handoff_consumed"}
    if (not isinstance(execution, dict) or set(execution) != fields
            or execution["schema"] != ADMISSION_EXECUTION_SCHEMA
            or type(execution["observed"]) is not bool):
        raise Rejected("native admission execution evidence is absent or malformed")
    if execution["observed"]:
        stages = result.get("stages")
        if (result.get("status") != "complete" or result.get("failure") != "none"
                or result.get("qualified") is not True or result.get("device_created") is not True
                or type(result.get("execution_count")) is not int or result["execution_count"] != 1
                or not isinstance(stages, dict) or stages.get("actual_metal") is not True
                or stages.get("fresh_device_teardown_complete") is not True):
            raise Rejected("native admission execution observations require successful qualified actual-Metal replay")
        count = execution["admission_count"]
        if type(count) is not int or not 0 <= count <= MAX_ADMISSION_COUNT or count != 1:
            raise Rejected("native admission execution requires exactly one uint32 admission")
        if execution["prepared_handoff_consumed"] is not True:
            raise Rejected("native admission execution requires a consumed prepared handoff")
    elif (execution["admission_count"] is not None
            or execution["prepared_handoff_consumed"] is not None):
        raise Rejected("native admission execution unobserved dimensions must be null")


def validate_v4_preparation_consistency(result: dict) -> None:
    timing = result.get("timing_measurements")
    named_v4_observed = (isinstance(timing, dict) and timing.get("schema") == TIMING_MEASUREMENTS_V2_SCHEMA
                         and any(timing[group]["observed"] for group in
                                 ("prepared_phases", "runtime_snapshot_delta")))
    for block, schema in (("v4_phase_timing_measurements", V4_PHASE_TIMING_MEASUREMENTS_V2_SCHEMA),
                          ("provider_call_timing_measurements", PROVIDER_CALL_TIMING_MEASUREMENTS_V2_SCHEMA)):
        measurement = result.get(block)
        named_v4_observed |= (isinstance(measurement, dict) and measurement.get("schema") == schema
                              and measurement.get("observed") is True)
    preparation = result.get("immutable_v4_preparation")
    preparation_observed = isinstance(preparation, dict) and preparation.get("observed") is True
    if named_v4_observed and not preparation_observed:
        raise Rejected("native observed V2 timing requires immutable V4 preparation evidence")
    if preparation_observed:
        admission = result.get("admission_execution")
        if (not isinstance(admission, dict) or admission.get("observed") is not True
                or admission.get("admission_count") != preparation["fresh_execution_admission_count"]
                or admission.get("prepared_handoff_consumed") is not preparation["prepared_handoff_consumed"]):
            raise Rejected("native immutable V4 preparation requires matching observed admission evidence")


def make_capacity_policy(slot_headroom_bytes: int, aggregate_headroom_bytes: int) -> dict:
    if type(slot_headroom_bytes) is not int or type(aggregate_headroom_bytes) is not int:
        raise Rejected("capacity headroom values must be exact integers")
    if not ((slot_headroom_bytes == 0 and aggregate_headroom_bytes == 0)
            or MIN_SLOT_HEADROOM_BYTES <= slot_headroom_bytes <= MAX_SLOT_HEADROOM_BYTES
            and slot_headroom_bytes <= aggregate_headroom_bytes <= MAX_AGGREGATE_HEADROOM_BYTES):
        raise Rejected("capacity requires exact 0/0 or slot 1..64 MiB and aggregate >= slot within 512 MiB")
    return {"slot_headroom_bytes": slot_headroom_bytes, "aggregate_headroom_bytes": aggregate_headroom_bytes}


def validate_capacity_policy(result: dict, expected_policy: dict) -> None:
    policy = result.get("capacity_policy")
    fields = {"slot_headroom_bytes", "aggregate_headroom_bytes"}
    if (not isinstance(policy, dict) or set(policy) != fields
            or not isinstance(expected_policy, dict) or set(expected_policy) != fields):
        raise Rejected("native capacity policy identity is absent or malformed")
    actual = make_capacity_policy(policy["slot_headroom_bytes"], policy["aggregate_headroom_bytes"])
    expected = make_capacity_policy(expected_policy["slot_headroom_bytes"], expected_policy["aggregate_headroom_bytes"])
    if actual != expected:
        raise Rejected("native capacity policy does not match the requested bounded pair")


def make_replay_policy(route: str, capacity_policy: dict | None = None,
                       draw_command_limit: int = 0) -> dict:
    policy = capacity_policy if capacity_policy is not None else make_capacity_policy(0, 0)
    validate_capacity_policy({"capacity_policy": policy}, policy)
    if route not in ("inspection", "compatibility", "production-slot"):
        raise Rejected("replay policy route is unsupported")
    if type(draw_command_limit) is not int or not (
            draw_command_limit == 0 or MIN_DRAW_COMMAND_LIMIT <= draw_command_limit <= MAX_DRAW_COMMAND_LIMIT):
        raise Rejected("draw command limit requires exact 0 or an integer within 3..64")
    if draw_command_limit and policy["slot_headroom_bytes"]:
        raise Rejected("grouped draw and retained-capacity replay policy axes cannot be mixed")
    if route == "production-slot" and draw_command_limit:
        raise Rejected("positive draw command limit requires Compatibility")
    if route == "compatibility" and policy["slot_headroom_bytes"]:
        raise Rejected("positive capacity requires ProductionSlot")
    # Inspection authenticates the requested numeric axes without claiming an
    # actual route. A nonzero axis determines its admission domain above.
    return {"schema": REPLAY_POLICY_SCHEMA, "route": route,
            "draw_command_limit": draw_command_limit, **policy}


def validate_replay_policy(result: dict, expected_policy: dict) -> None:
    policy = result.get("replay_policy")
    fields = {"schema", "route", "draw_command_limit", "slot_headroom_bytes", "aggregate_headroom_bytes"}
    if (not isinstance(policy, dict) or set(policy) != fields
            or not isinstance(expected_policy, dict) or set(expected_policy) != fields
            or policy["schema"] != REPLAY_POLICY_SCHEMA or expected_policy["schema"] != REPLAY_POLICY_SCHEMA):
        raise Rejected("native replay policy identity is absent or malformed")
    actual = make_replay_policy(policy["route"],
        make_capacity_policy(policy["slot_headroom_bytes"], policy["aggregate_headroom_bytes"]),
        policy["draw_command_limit"])
    expected = make_replay_policy(expected_policy["route"],
        make_capacity_policy(expected_policy["slot_headroom_bytes"], expected_policy["aggregate_headroom_bytes"]),
        expected_policy["draw_command_limit"])
    if actual != expected:
        raise Rejected("native replay policy does not match the requested bounded identity")


def expected_stage_environment(capacity_policy: dict | None = None, draw_command_limit: int = 0) -> dict:
    policy = make_replay_policy("inspection", capacity_policy, draw_command_limit)
    expected = dict(REPLAY_STAGE_ENVIRONMENT)
    expected["DXMT9_DIRECT_SLOT_HEADROOM_BYTES"] = str(policy["slot_headroom_bytes"])
    expected["DXMT9_DIRECT_SLOT_AGGREGATE_HEADROOM_BYTES"] = str(policy["aggregate_headroom_bytes"])
    expected["DXMT9_DRAW_CHUNK_COMMAND_LIMIT"] = str(policy["draw_command_limit"])
    return expected


def validate_stage_environment(result: dict, replay: bool, capacity_policy: dict | None = None,
                               draw_command_limit: int = 0) -> None:
    environment = result.get("effective_stage_environment")
    expected = expected_stage_environment(capacity_policy, draw_command_limit) if replay else {}
    if not isinstance(environment, dict) or environment != expected:
        raise Rejected("native effective stage environment does not match the checked diagnostic pins")


def unsigned_capacity_bytes(value: str) -> int:
    if re.fullmatch(r"[0-9]+", value) is None:
        raise argparse.ArgumentTypeError("capacity headroom must be unsigned decimal bytes")
    return int(value)


def unsigned_draw_command_limit(value: str) -> int:
    if re.fullmatch(r"[0-9]+", value) is None:
        raise argparse.ArgumentTypeError("draw command limit must be unsigned decimal")
    return int(value)


class CapacityOptionOnce(argparse.Action):
    def __call__(self, parser, namespace, value, option_string=None):
        if getattr(namespace, self.dest, None) is not None:
            parser.error(f"duplicate replay policy option: {option_string}")
        setattr(namespace, self.dest, value)


def require_direct_candidate_path(result: dict) -> None:
    counters = result.get("path_counters")
    if (not isinstance(counters, dict) or counters.get("observed") is not True
            or counters.get("monotonic") is not True):
        raise Rejected("requested direct route lacks observed monotonic path counters")
    values = []
    for field in ("direct_leases", "direct_lease_draws"):
        value = counters.get(field)
        if type(value) is not int or not 0 <= value <= (1 << 64) - 1:
            raise Rejected(f"requested direct route has malformed path counter: {field}")
        values.append(value)
    if not all(values):
        raise Rejected("requested-direct-route-not-exercised")


def make_repetition_policy(warmup: int, repeat: int, reset_policy: str) -> dict:
    if (type(warmup) is not int or type(repeat) is not int
            or not 0 <= warmup <= MAX_REPEAT_ITERATIONS
            or not 1 <= repeat <= MAX_REPEAT_ITERATIONS
            or warmup + repeat > MAX_REPEAT_ITERATIONS):
        raise Rejected("repetition requires warmup >= 0, repeat >= 1 and total <= 64")
    if reset_policy != "fresh-device":
        raise Rejected("V4 repetition supports only fresh-device; warm-device is unqualified")
    return {"warmup": warmup, "repeat": repeat, "reset_policy": reset_policy,
            "prepared_input_shared": True,
            "input_storage": "prepared-owner-plus-retained-loader-copy",
            "cache_qualification": "uncontrolled-process-and-driver-caches",
            "shared_preparation": "once;per-iteration-prepare-wall-ns-is-the-same-one-off-reading"}


def unsigned_repetition_count(value: str) -> int:
    if re.fullmatch(r"[0-9]+", value) is None:
        raise argparse.ArgumentTypeError("repetition count must be unsigned decimal")
    return int(value)


def iteration_identity(ordinal: int, policy: dict) -> dict:
    kind = "warmup" if ordinal < policy["warmup"] else "measured"
    index = ordinal if kind == "warmup" else ordinal - policy["warmup"]
    return {"ordinal": ordinal, "kind": kind, "kind_index": index}


def iteration_directory(identity: dict) -> str:
    return f"iterations/{identity['kind']}-{identity['kind_index']:04d}"


def require_owned_directory(path: pathlib.Path, identity: tuple[int, int]) -> None:
    status = path.lstat()
    if (not stat.S_ISDIR(status.st_mode) or path.resolve() != path
            or (status.st_dev, status.st_ino) != identity or status.st_uid != os.geteuid()
            or status.st_mode & 0o077):
        raise Rejected("repetition artifact directory is no longer the private owned directory")


def create_iteration_directories(output: pathlib.Path, policy: dict, deadline: float) -> dict:
    (output / "iterations").mkdir(mode=0o700, exist_ok=False)
    identities = {}
    for ordinal in range(policy["warmup"] + policy["repeat"]):
        require_time(deadline)
        relative = iteration_directory(iteration_identity(ordinal, policy))
        directory = output / relative
        directory.mkdir(mode=0o700, exist_ok=False)
        status = directory.stat()
        identities[relative] = (status.st_dev, status.st_ino)
    return identities


def validate_replay_gates(result: dict, plan: str, route: str, publication_mode: str) -> None:
    semantic = publication_mode == "semantic-mapping"
    if (result.get("status") != "complete" or result.get("qualified") is not True
            or result.get("plan_sha256") != plan or result.get("expected_plan_sha256") != plan
            or result.get("replay_route") != route or type(result.get("execution_count")) is not int
            or result["execution_count"] != 1 or any(result.get(gate) is not True for gate in GATES)
            or result.get("semantic_mapping_qualified") is not semantic
            or result.get("exact_source_diagnostic_complete") is not (not semantic)):
        raise Rejected("replay did not independently qualify exact query/source/output diagnostic gates")


def validate_repeat_envelope(result: dict, policy: dict, plan: str, args,
                             capacity_policy: dict, draw_command_limit: int) -> list:
    total = policy["warmup"] + policy["repeat"]
    actual_policy = result.get("repetition_policy")
    if (not isinstance(actual_policy, dict) or set(actual_policy) != set(policy)
            or any(type(actual_policy[key]) is not type(value) or actual_policy[key] != value
                   for key, value in policy.items())):
        raise Rejected("native repetition policy is absent, malformed or changed")
    if (result.get("schema") != REPEAT_NATIVE_SCHEMA or result.get("diagnostic") is not True
            or result.get("policy_qualified") is not False or result.get("timing_eligible") is not False
            or result.get("status") != "complete" or result.get("failure") != "none"
            or result.get("qualified") is not True or type(result.get("prepare_count")) is not int
            or result["prepare_count"] != 1 or type(result.get("execution_count")) is not int
            or result["execution_count"] != total or result.get("producer_contract") != args.producer_contract
            or result.get("publication_mode") != args.publication_mode
            or result.get("plan_sha256") != plan or result.get("expected_plan_sha256") != plan
            or result.get("replay_route") != args.route
            or "timing_measurements" in result or "query_timing_measurements" in result
            or "v4_phase_timing_measurements" in result
            or "provider_call_timing_measurements" in result
            or "immutable_v4_preparation" in result
            or "admission_execution" in result
            or result.get("aggregate") is not None):
        raise Rejected("native repetition envelope lacks complete diagnostic-only execution authority")
    validate_capacity_policy(result, capacity_policy)
    validate_replay_policy(result, make_replay_policy(args.route, capacity_policy, draw_command_limit))
    validate_stage_environment(result, True, capacity_policy, draw_command_limit)
    rows = result.get("iterations")
    if not isinstance(rows, list) or len(rows) != total:
        raise Rejected("native repetition must retain every requested warmup and measured iteration")
    return rows


def bounded_iteration_rows(rows: list, output: pathlib.Path, policy: dict,
                            directory_identities: dict, deadline: float) -> list:
    # Check the whole declared/actual JSON budget before loading any row.
    fields = {"ordinal", "kind", "kind_index", "result_path", "result_bytes", "result_sha256",
              "readback_directory", "status"}
    total = 0
    checked = []
    for ordinal, row in enumerate(rows):
        require_time(deadline)
        identity = iteration_identity(ordinal, policy)
        directory = iteration_directory(identity)
        if (not isinstance(row, dict) or set(row) != fields
                or any(type(row[key]) is not type(value) or row[key] != value for key, value in identity.items())
                or row["result_path"] != directory + "/native.json"
                or row["readback_directory"] != directory or row["status"] != "complete"
                or type(row["result_bytes"]) is not int or not 0 < row["result_bytes"] <= MAX_RESULT_JSON_BYTES
                or not sha256_nonzero(row["result_sha256"])):
            raise Rejected("native repetition row identity/path/status/hash is malformed")
        if directory not in directory_identities:
            raise Rejected("native repetition row names an unowned directory")
        require_owned_directory(output / directory, directory_identities[directory])
        path = output / row["result_path"]
        status = path.lstat()
        if (not stat.S_ISREG(status.st_mode) or status.st_uid != os.geteuid() or status.st_nlink != 1
                or status.st_size != row["result_bytes"]):
            raise Rejected("native repetition JSON must be a regular owned exact-size exclusive artifact")
        total += status.st_size
        if total > MAX_REPEAT_JSON_BYTES:
            raise Rejected("native repetition aggregate JSON byte cap exceeded before loading rows")
        checked.append(row)
    return checked


def load_iteration_result(path: pathlib.Path, row: dict) -> dict:
    flags = os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC | os.O_NONBLOCK
    signature = lambda value: (value.st_dev, value.st_ino, value.st_size, value.st_mtime_ns, value.st_ctime_ns)
    with os.fdopen(os.open(path, flags), "rb") as stream:
        before = os.fstat(stream.fileno())
        if (not stat.S_ISREG(before.st_mode) or before.st_uid != os.geteuid() or before.st_nlink != 1
                or before.st_size != row["result_bytes"]):
            raise Rejected("native repetition JSON ownership/size changed")
        raw = stream.read(MAX_RESULT_JSON_BYTES + 1)
        after = os.fstat(stream.fileno())
    if (len(raw) != row["result_bytes"] or hashlib.sha256(raw).hexdigest() != row["result_sha256"]
            or signature(before) != signature(after) or signature(path.lstat()) != signature(after)):
        raise Rejected("native repetition JSON fingerprint changed")
    result = parse_json(raw)
    if not isinstance(result, dict):
        raise Rejected("native repetition result is not a JSON object")
    return result


def verify_repeat_iterations(envelope: dict, output: pathlib.Path, output_identity: tuple[int, int],
                              policy: dict, directory_identities: dict, plan: str, args,
                              capacity_policy: dict, draw_command_limit: int, bundle: pathlib.Path,
                              manifest: dict, diagnostic: pathlib.Path, binary: dict, runner,
                              deadline: float) -> list:
    require_owned_directory(output, output_identity)
    rows = validate_repeat_envelope(envelope, policy, plan, args, capacity_policy, draw_command_limit)
    rows = bounded_iteration_rows(rows, output, policy, directory_identities, deadline)
    summaries = []
    shared_immutable_preparation = None
    for row in rows:
        require_time(deadline)
        directory = output / row["readback_directory"]
        native = load_iteration_result(output / row["result_path"], row)
        identity = iteration_identity(row["ordinal"], policy)
        actual_identity = native.get("iteration")
        if (not isinstance(actual_identity, dict) or set(actual_identity) != set(identity)
                or any(type(actual_identity[key]) is not type(value) or actual_identity[key] != value
                       for key, value in identity.items())):
            raise Rejected("native repetition full result has a foreign iteration identity")
        validate_common(native, args.producer_contract, args.publication_mode)
        immutable_preparation = native.get("immutable_v4_preparation")
        if row["ordinal"] == 0:
            shared_immutable_preparation = immutable_preparation
        elif immutable_preparation != shared_immutable_preparation:
            raise Rejected("native immutable V4 preparation changed between repetitions")
        validate_capacity_policy(native, capacity_policy)
        validate_replay_policy(native, make_replay_policy(args.route, capacity_policy, draw_command_limit))
        validate_stage_environment(native, True, capacity_policy, draw_command_limit)
        validate_replay_gates(native, plan, args.route, args.publication_mode)
        stages, prepared, v4 = native.get("stages"), native.get("prepared"), native.get("v4")
        if (native.get("failure") != "none" or native.get("device_created") is not True
                or not isinstance(stages, dict)
                or any(stages.get(field) is not True for field in
                       ("fresh_device_created", "actual_metal", "fresh_device_teardown_complete"))
                or not isinstance(prepared, dict)
                or any(prepared.get(field) is not True for field in
                       ("immutable_input_owned", "preflight_completed", "plan_reused"))
                or type(prepared.get("prepare_count")) is not int or prepared["prepare_count"] != 1
                or not isinstance(v4, dict)
                or any(v4.get(field) is not True for field in
                       ("validated", "armed", "complete", "canonical_traversal_complete", "raw_ack_coverage_complete",
                        "actual_attempts_observed", "actual_lifecycle_complete", "actual_topology_complete"))):
            raise Rejected("native repetition iteration lacks fresh-device/prepared/lifecycle authority")
        if args.publication_mode == "semantic-mapping" and args.route == "production-slot":
            require_direct_candidate_path(native)
        if native.get("loaded_images") != envelope.get("loaded_images"):
            raise Rejected("native repetition loaded images changed between iterations")
        validate_loaded_images(native, diagnostic, binary, runner, deadline)
        verified = validate_readbacks(native, directory, bundle, manifest,
                                     directory_identities[row["readback_directory"]], deadline)
        summary = {**row, "native_result_valid": True, "loaded_images_stable": True,
                   "readback_artifacts_valid": True, "verified_readback_artifacts": verified}
        if "timing_measurements" in native:
            summary["timing_measurements"] = native["timing_measurements"]
        if "query_timing_measurements" in native:
            summary["query_timing_measurements"] = native["query_timing_measurements"]
        if "v4_phase_timing_measurements" in native:
            summary["v4_phase_timing_measurements"] = native["v4_phase_timing_measurements"]
        if "provider_call_timing_measurements" in native:
            summary["provider_call_timing_measurements"] = native["provider_call_timing_measurements"]
        if "immutable_v4_preparation" in native:
            summary["immutable_v4_preparation"] = native["immutable_v4_preparation"]
        if "admission_execution" in native:
            summary["admission_execution"] = native["admission_execution"]
        summaries.append(summary)
        require_owned_directory(directory, directory_identities[row["readback_directory"]])
    require_owned_directory(output, output_identity)
    return summaries


def parser() -> argparse.ArgumentParser:
    value = argparse.ArgumentParser(description=__doc__)
    value.add_argument("--bundle", type=pathlib.Path, required=True)
    value.add_argument("--diagnostic", type=pathlib.Path, required=True, help="private native diagnostic executable")
    value.add_argument("--output-dir", type=pathlib.Path, required=True, help="must not exist, including a symlink")
    value.add_argument("--route", choices=("production-slot", "compatibility"), required=True)
    value.add_argument("--observe-encode-draws", action="store_true",
                       help="cold native draw/storage witness; never enables policy timing")
    value.add_argument("--producer-contract", choices=PRODUCER_CONTRACTS, default="none",
                       help="explicit original PE mirror-reservation preflush contract; never inferred from source cuts")
    value.add_argument("--publication-mode", choices=PUBLICATION_MODES, default="exact-source-diagnostic",
                       help="private provenance qualification; semantic correspondence permits genuine changed batching")
    value.add_argument("--expected-plan-digest", help="optional original plan SHA-256 required before replay")
    value.add_argument("--slot-headroom-bytes", type=unsigned_capacity_bytes, action=CapacityOptionOnce,
                       help="paired bounded retained capacity; positive only for semantic ProductionSlot")
    value.add_argument("--aggregate-headroom-bytes", type=unsigned_capacity_bytes, action=CapacityOptionOnce,
                       help="required with --slot-headroom-bytes; exact-fit default is 0/0")
    value.add_argument("--draw-command-limit", type=unsigned_draw_command_limit, action=CapacityOptionOnce,
                       help="exact default 0; positive 3..64 only for semantic Compatibility")
    value.add_argument("--warmup", type=unsigned_repetition_count, action=CapacityOptionOnce,
                       help="explicit private in-process repetition warmups (default 0; total <= 64)")
    value.add_argument("--repeat", type=unsigned_repetition_count, action=CapacityOptionOnce,
                       help="measured diagnostic iterations (default 1; no statistical qualification)")
    value.add_argument("--reset-policy", choices=("fresh-device",), action=CapacityOptionOnce,
                       help="explicit repetition uses a fresh device per iteration; warm-device is unsupported")
    value.add_argument("--timeout-seconds", type=float, default=180.0,
                       help="shared deadline for authentication, inspection and all replay work (1..600; default 180)")
    return value


def main() -> int:
    arguments = parser()
    args = arguments.parse_args()
    repeat_mode = any(value is not None for value in (args.warmup, args.repeat, args.reset_policy))
    repetition_policy = None
    if repeat_mode:
        try:
            repetition_policy = make_repetition_policy(
                args.warmup if args.warmup is not None else 0,
                args.repeat if args.repeat is not None else 1,
                args.reset_policy if args.reset_policy is not None else "fresh-device")
        except Rejected as error:
            arguments.error(str(error))
        if args.observe_encode_draws:
            arguments.error("--observe-encode-draws is unsupported with explicit repetition controls")
    if not math.isfinite(args.timeout_seconds) or not 1 <= args.timeout_seconds <= 600:
        arguments.error("--timeout-seconds must be finite and within 1..600")
    if args.expected_plan_digest is not None and not sha256_nonzero(args.expected_plan_digest):
        arguments.error("--expected-plan-digest must be a nonzero lowercase SHA-256")
    if (args.slot_headroom_bytes is None) != (args.aggregate_headroom_bytes is None):
        arguments.error("capacity headroom options must be supplied together, including explicit 0/0")
    try:
        capacity_policy = make_capacity_policy(
            args.slot_headroom_bytes if args.slot_headroom_bytes is not None else 0,
            args.aggregate_headroom_bytes if args.aggregate_headroom_bytes is not None else 0)
        draw_command_limit = args.draw_command_limit if args.draw_command_limit is not None else 0
        replay_policy = make_replay_policy(args.route, capacity_policy, draw_command_limit)
        inspection_policy = make_replay_policy("inspection", capacity_policy, draw_command_limit)
    except Rejected as error:
        arguments.error(str(error))
    if capacity_policy["slot_headroom_bytes"] and (
            args.publication_mode != "semantic-mapping" or args.route != "production-slot"):
        arguments.error("positive capacity requires semantic-mapping ProductionSlot")
    if draw_command_limit and args.publication_mode != "semantic-mapping":
        arguments.error("positive draw command limit requires semantic-mapping Compatibility")
    bundle, diagnostic = args.bundle.resolve(), args.diagnostic.resolve()
    output = args.output_dir.resolve()
    if args.output_dir.exists() or args.output_dir.is_symlink() or output.exists():
        arguments.error("--output-dir must name a new path")
    if output.is_relative_to(bundle):
        arguments.error("--output-dir must not be inside the immutable input bundle")
    if not bundle.is_dir() or not diagnostic.is_file() or not os.access(diagnostic, os.X_OK):
        arguments.error("--bundle must be a directory and --diagnostic an executable file")
    output.mkdir(mode=0o700, parents=True, exist_ok=False)
    output_status = output.stat()
    output_identity = (output_status.st_dev, output_status.st_ino)
    for name in ("inspect.stdout", "inspect.stderr", "replay.stdout", "replay.stderr"):
        with (output / name).open("xb"):
            pass
    started = time.monotonic()
    deadline = started + args.timeout_seconds
    receipt_path = output / "result.json"
    receipt: dict[str, Any] = {
        "schema": RECEIPT_SCHEMA, "status": "authenticating", "accepted": False,
        "diagnostic": True, "policy_qualified": False, "timing_eligible": False,
        "public_v4_execution_enabled": False, "bundle": str(bundle), "diagnostic_binary": str(diagnostic),
        "manifest_authenticated": False,
        "bundle_reauthenticated": False, "binary_reauthenticated": False,
        "action_profile_reauthenticated": False, "loaded_images_stable": False,
        "readback_artifacts_valid": False, "stage_environment_valid": False,
        "native_capacity_policy_valid": False, "capacity_policy": capacity_policy,
        "native_replay_policy_valid": False, "replay_policy": replay_policy,
        "expected_stage_environment": expected_stage_environment(capacity_policy, draw_command_limit),
        "effective_stage_environment": None,
        "expected_plan_digest": args.expected_plan_digest,
        "output_dir": str(output), "route": args.route, "producer_contract": args.producer_contract,
        "observe_encode_draws": args.observe_encode_draws,
        "publication_mode": args.publication_mode,
        "expected_readback_artifacts": {"source": "replay-source.rgba", "output": "replay-output.rgba"},
        "launcher_argv": sys.argv,
        "cwd": os.getcwd(), "environment": environment_provenance(),
        "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "timeout_seconds": args.timeout_seconds,
        "limits": {"manifest_bytes": MAX_MANIFEST_BYTES, "input_bytes": MAX_INPUT_BYTES,
                   "blobs": MAX_BLOBS, "native_result_json_bytes": MAX_RESULT_JSON_BYTES,
                   "loaded_images": MAX_LOADED_IMAGES,
                   "slot_headroom_bytes_min": MIN_SLOT_HEADROOM_BYTES,
                   "slot_headroom_bytes_max": MAX_SLOT_HEADROOM_BYTES,
                   "aggregate_headroom_bytes_max": MAX_AGGREGATE_HEADROOM_BYTES,
                   "draw_command_limit_min": MIN_DRAW_COMMAND_LIMIT,
                   "draw_command_limit_max": MAX_DRAW_COMMAND_LIMIT,
                   "repeat_iterations": MAX_REPEAT_ITERATIONS,
                   "repeat_json_bytes": MAX_REPEAT_JSON_BYTES,
                   "repeat_readback_bytes": MAX_REPEAT_READBACK_BYTES},
        "authentication_deadline_enforcement": "byte-cap precheck and synchronous-loader before/after checks; no asynchronous interrupt",
        "steps": {name: {"status": "not-started", "argv": None,
                          "stdout": f"{name}.stdout", "stderr": f"{name}.stderr"}
                  for name in ("inspect", "replay")},
    }
    exit_code = 1
    runner, manifest, manifest_bytes, action_profile = None, None, None, None
    initial_authentication_complete = False
    directory_identities = {}
    if repeat_mode:
        receipt.update(repetition_policy=repetition_policy, iterations_valid=False, iterations=[])
    try:
        write_receipt(receipt_path, receipt)
        require_regular_manifest(bundle, deadline)
        bounded_manifest, manifest_bytes, total = bound_inputs(bundle, deadline)
        require_regular_inputs(bundle, bounded_manifest, deadline)
        receipt["input_components"] = bounded_manifest["components"]
        receipt["input_manifest"] = {"path": "manifest.json", "bytes": len(manifest_bytes),
                                     "sha256": hashlib.sha256(manifest_bytes).hexdigest()}
        receipt["bounded_input_bytes"] = total
        write_receipt(receipt_path, receipt)
        runner = load_bundle_tool()
        receipt["diagnostic_executable"] = fingerprint_file(diagnostic, runner, deadline)
        write_receipt(receipt_path, receipt)
        require_time(deadline)
        manifest, _events, _blob_refs, _blob_paths, identity, source = runner.load_bundle(
            bundle, allow_identity_v4=True)
        require_time(deadline)
        current_manifest_bytes = read_capped(bundle / "manifest.json", MAX_MANIFEST_BYTES)
        require_time(deadline)
        if manifest != bounded_manifest or current_manifest_bytes != manifest_bytes:
            raise Rejected("bundle manifest changed during authentication")
        if (identity is None or source is None
                or manifest["components"]["identity"].get("schema") != runner.IDENTITY_V4_SCHEMA):
            raise Rejected("private diagnostic requires authenticated V4 pair and both original oracles")
        receipt["manifest_authenticated"] = True
        validate_producer_contract(manifest, args.producer_contract, runner.SCHEMA)
        action_profile = runner.load_action_profile(bundle, manifest)
        require_time(deadline)
        receipt["action_profile_validation"] = action_profile
        initial_authentication_complete = True
        if args.publication_mode == "semantic-mapping" and (
                action_profile.get("present") is not True or action_profile.get("stable_exact") is not True):
            raise Rejected("semantic mapping requires authenticated stable exact action witness before replay")
        if repeat_mode:
            total_readbacks = sum(manifest["components"][f"{name}_oracle"]["bytes"]
                                  for name in ("source", "output")) * (
                                      repetition_policy["warmup"] + repetition_policy["repeat"])
            if total_readbacks > MAX_REPEAT_READBACK_BYTES:
                raise Rejected("native repetition aggregate readback byte cap exceeded before launch")
            receipt["repetition_readback_bytes"] = total_readbacks
        base = [str(diagnostic), "--bundle", str(bundle), "--producer-contract", args.producer_contract,
                "--publication-mode", args.publication_mode,
                "--slot-headroom-bytes", str(capacity_policy["slot_headroom_bytes"]),
                "--aggregate-headroom-bytes", str(capacity_policy["aggregate_headroom_bytes"]),
                "--draw-command-limit", str(draw_command_limit)]
        for name, (_path, option) in COMPONENTS.items():
            digest = manifest["components"][name].get("sha256")
            if not sha256_nonzero(digest):
                raise Rejected(f"invalid authenticated component digest: {name}")
            base += [option, digest]
        if action_profile["present"]:
            digest = manifest["components"]["action_profile"].get("sha256")
            if not sha256_nonzero(digest):
                raise Rejected("invalid authenticated component digest: action_profile")
            base += [ACTION_PROFILE_COMPONENT[1], digest]
        inspect = run_child("inspect", base + ["--inspect-plan"], output, deadline, receipt, receipt_path)
        validate_common(inspect, args.producer_contract, args.publication_mode)
        validate_capacity_policy(inspect, capacity_policy)
        validate_replay_policy(inspect, inspection_policy)
        validate_stage_environment(inspect, replay=False, capacity_policy=capacity_policy,
                                   draw_command_limit=draw_command_limit)
        validate_loaded_images(inspect, diagnostic, receipt["diagnostic_executable"], runner, deadline)
        plan = inspect.get("plan_sha256")
        if (inspect.get("status") != "inspection-complete" or not sha256_nonzero(plan)
                or type(inspect.get("execution_count")) is not int or inspect["execution_count"] != 0
                or inspect.get("device_created") is not False or inspect.get("qualified") is not False
                or any(inspect.get(gate) is not False for gate in GATES)
                or inspect.get("semantic_mapping_qualified") is not False
                or inspect.get("exact_source_diagnostic_complete") is not False):
            raise Rejected("native inspection did not return a safe, complete authenticated plan")
        receipt["plan_sha256"] = plan
        if args.expected_plan_digest is not None and plan != args.expected_plan_digest:
            raise Rejected("native inspection plan does not match --expected-plan-digest")
        replay_base = base + (["--observe-encode-draws"] if args.observe_encode_draws else [])
        if repeat_mode:
            directory_identities = create_iteration_directories(output, repetition_policy, deadline)
            replay_base += ["--warmup", str(repetition_policy["warmup"]),
                            "--repeat", str(repetition_policy["repeat"]), "--reset-policy", "fresh-device"]
        replay = run_child("replay", replay_base + ["--readback-dir", str(output),
                                            "--expected-plan-digest", plan, "--replay-route", args.route],
                           output, deadline, receipt, receipt_path)
        if repeat_mode:
            receipt["iterations"] = verify_repeat_iterations(
                replay, output, output_identity, repetition_policy, directory_identities, plan, args,
                capacity_policy, draw_command_limit, bundle, manifest, diagnostic,
                receipt["diagnostic_executable"], runner, deadline)
            receipt.update(iterations_valid=True, native_capacity_policy_valid=True,
                           native_replay_policy_valid=True, stage_environment_valid=True,
                           effective_stage_environment=dict(replay["effective_stage_environment"]))
        else:
            validate_common(replay, args.producer_contract, args.publication_mode)
        if not repeat_mode:
            if "timing_measurements" in replay:
                receipt["timing_measurements"] = replay["timing_measurements"]
            if "query_timing_measurements" in replay:
                receipt["query_timing_measurements"] = replay["query_timing_measurements"]
            if "v4_phase_timing_measurements" in replay:
                receipt["v4_phase_timing_measurements"] = replay["v4_phase_timing_measurements"]
            if "provider_call_timing_measurements" in replay:
                receipt["provider_call_timing_measurements"] = replay["provider_call_timing_measurements"]
            if "immutable_v4_preparation" in replay:
                receipt["immutable_v4_preparation"] = replay["immutable_v4_preparation"]
            if "admission_execution" in replay:
                receipt["admission_execution"] = replay["admission_execution"]
        validate_capacity_policy(replay, capacity_policy)
        validate_replay_policy(replay, replay_policy)
        validate_stage_environment(replay, replay=True, capacity_policy=capacity_policy,
                                   draw_command_limit=draw_command_limit)
        receipt["native_capacity_policy_valid"] = True
        receipt["native_replay_policy_valid"] = True
        receipt["stage_environment_valid"] = True
        receipt["effective_stage_environment"] = dict(replay["effective_stage_environment"])
        semantic = args.publication_mode == "semantic-mapping"
        if not repeat_mode:
            validate_replay_gates(replay, plan, args.route, args.publication_mode)
        if args.observe_encode_draws:
            witness = replay.get("encode_draw_witness")
            receipt["encode_draw_witness"] = witness
            if (not isinstance(witness, dict) or witness.get("requested") is not True
                    or witness.get("installed") is not True or witness.get("complete") is not True
                    or witness.get("failure") != "none"):
                raise Rejected("requested Encode draw observation did not complete independently of pixel gates")
            observer = witness.get("original_buffer_observer")
            if (not isinstance(observer, dict)
                    or any(observer.get(field) is not True
                           for field in ("requested", "admitted", "complete", "closed"))
                    or observer.get("failure") != "none"):
                raise Rejected("original BUFFER observer lifecycle did not complete independently of pixel gates")
            context = witness.get("original_context")
            if (not isinstance(context, dict) or context.get("complete") is not True
                    or context.get("failure") != "none"):
                raise Rejected("original BUFFER canonical context did not complete")
            proof = witness.get("original_binding_proof")
            if not isinstance(proof, dict) or proof.get("observation_complete") is not True:
                raise Rejected("original BUFFER binding observation did not complete")
            # Unsupported draw domains remain explicit evidence, not a failed
            # observation or an implicit claim of complete byte qualification.
        if not repeat_mode and semantic and args.route == "production-slot":
            # An observer is optional; actual direct work is not. A complete
            # all-fallback replay retains evidence but cannot qualify this
            # candidate. Natural mixed compatibility fallback is permitted.
            receipt["path_counters"] = replay.get("path_counters")
            require_direct_candidate_path(replay)
        exit_code = 0
    except KeyboardInterrupt:
        receipt.update(status="interrupted", failure="launcher interrupted")
        exit_code = 130
    except (Rejected, OSError, ValueError, SystemExit) as error:
        receipt.update(status="rejected", failure=str(error))
    finally:
        # Post-authentication is independent of native success. Preserve the
        # first rejection plus any later authentication/artifact failures.
        post_failures = []
        if initial_authentication_complete:
            try:
                require_regular_manifest(bundle, deadline)
                current, current_bytes, _total = bound_inputs(bundle, deadline)
                require_regular_inputs(bundle, current, deadline)
                loaded, _events, _refs, _paths, _identity, _source = runner.load_bundle(bundle, allow_identity_v4=True)
                require_time(deadline)
                if (current != manifest or loaded != manifest or current_bytes != manifest_bytes
                        or read_capped(bundle / "manifest.json", MAX_MANIFEST_BYTES) != manifest_bytes):
                    raise Rejected("immutable input bundle/manifest changed across the diagnostic")
                receipt["bundle_reauthenticated"] = True
            except (Rejected, OSError, ValueError, SystemExit) as error:
                post_failures.append(str(error))
            try:
                current_action = runner.load_action_profile(bundle, manifest)
                require_time(deadline)
                if current_action != action_profile:
                    raise Rejected("authenticated action witness changed across the diagnostic")
                validate_producer_contract(manifest, args.producer_contract, runner.SCHEMA)
                receipt["action_profile_reauthenticated"] = True
            except (Rejected, OSError, ValueError, SystemExit) as error:
                post_failures.append(str(error))
            try:
                if fingerprint_file(diagnostic, runner, deadline) != receipt["diagnostic_executable"]:
                    raise Rejected("native executable fingerprint changed across the diagnostic")
                receipt["binary_reauthenticated"] = True
            except (Rejected, OSError, ValueError, SystemExit) as error:
                post_failures.append(str(error))
            image_steps, image_failures = {}, False
            for name, step in receipt["steps"].items():
                native = step.get("native_result")
                if native is None:
                    continue
                image_steps[name] = native.get("loaded_images")
                try:
                    validate_loaded_images(native, diagnostic, receipt["diagnostic_executable"], runner, deadline)
                except (Rejected, OSError, ValueError, SystemExit) as error:
                    image_failures = True
                    post_failures.append(f"{name} loaded images: {error}")
            receipt["loaded_images"] = image_steps
            receipt["loaded_images_stable"] = set(image_steps) == {"inspect", "replay"} and not image_failures
            native = receipt["steps"]["replay"].get("native_result")
            if native is not None:
                try:
                    if repeat_mode:
                        verified_iterations = verify_repeat_iterations(
                            native, output, output_identity, repetition_policy, directory_identities,
                            receipt["plan_sha256"], args, capacity_policy, draw_command_limit,
                            bundle, manifest, diagnostic, receipt["diagnostic_executable"], runner, deadline)
                        if (receipt.get("iterations_valid") is not True
                                or verified_iterations != receipt["iterations"]):
                            raise Rejected("repetition results changed or initial iteration qualification was incomplete")
                    else:
                        receipt["verified_readback_artifacts"] = validate_readbacks(
                            native, output, bundle, manifest, output_identity, deadline)
                    receipt["readback_artifacts_valid"] = True
                except (Rejected, OSError, ValueError, SystemExit) as error:
                    if repeat_mode:
                        receipt["iterations_valid"] = False
                    post_failures.append(f"replay readback artifacts: {error}")
        if post_failures:
            receipt["reauthentication_failures"] = post_failures
            receipt.setdefault("failure", post_failures[0])
            if exit_code != 130:
                exit_code = 1
            receipt["status"] = "interrupted" if exit_code == 130 else "rejected"
        receipt["accepted"] = (exit_code == 0 and (not repeat_mode or receipt.get("iterations_valid") is True)
                               and all(receipt[field] for field in (
            "bundle_reauthenticated", "binary_reauthenticated", "action_profile_reauthenticated",
            "loaded_images_stable", "readback_artifacts_valid", "stage_environment_valid",
            "native_capacity_policy_valid", "native_replay_policy_valid")))
        if receipt["accepted"]:
            receipt["status"] = "complete"
        elif exit_code == 0:
            receipt.update(status="rejected", failure="required post-run authentication incomplete")
            exit_code = 1
        for step in receipt["steps"].values():
            if step["status"] == "not-started":
                step.update(status="skipped", failure="prior authentication/inspection/deadline failure")
        receipt["elapsed_seconds"] = time.monotonic() - started
        write_receipt(receipt_path, receipt)
    print(json.dumps({"schema": RECEIPT_SCHEMA, "status": receipt["status"],
                      "accepted": receipt["accepted"], "receipt": str(receipt_path),
                      "failure": receipt.get("failure")}))
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
