"""Frozen tracing profiles and attempt/artifact helpers for run_experiment.

This module owns runner-side control and provenance only. Collectors own their
raw payload schemas; the runner treats their outputs as opaque bytes and checks
the engine receipt before qualifying a requested capture.
"""
from __future__ import annotations

import hashlib
import json
import os
import platform
import re
import subprocess
import stat
import tempfile
import time
import uuid
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


SCHEMA_MANIFEST = "dxmt9-trace-manifest-v1"
SCHEMA_REQUEST = "dxmt9.trace.request.v1"
SCHEMA_RECEIPT = "dxmt9.trace.receipt.v1"


@dataclass(frozen=True)
class TraceProfile:
    name: str
    question: str
    required_collectors: tuple[str, ...]
    optional_collectors: tuple[str, ...]
    categories: tuple[str, ...]
    window_mode: str
    max_events: int
    max_bytes: int
    finalize_timeout_ms: int
    supported: bool
    unsupported_reason: str | None = None
    max_clock_error_ns: int = 1000
    require_zero_loss: bool = True
    require_closed_window: bool = True
    exact_native_consumer: bool = True

    def frozen_config(self) -> dict[str, Any]:
        value = asdict(self)
        value["required_collectors"] = list(self.required_collectors)
        value["optional_collectors"] = list(self.optional_collectors)
        value["categories"] = list(self.categories)
        value.pop("max_clock_error_ns")
        value.pop("require_zero_loss")
        value.pop("require_closed_window")
        value.pop("exact_native_consumer")
        value["qualification_policy"] = {
            "max_clock_error_ns": self.max_clock_error_ns,
            "require_zero_loss": self.require_zero_loss,
            "require_closed_window": self.require_closed_window,
            "exact_native_consumer": self.exact_native_consumer,
        }
        return value


_COARSE_CATEGORIES = ("import", "replay", "queue", "encode", "wait", "submit", "completion")
TRACE_PROFILES: dict[str, TraceProfile] = {
    "overview": TraceProfile(
        "overview", "Overview of engine CPU, waits, submission and available GPU timing",
        ("engine-perfetto",), ("pe-events", "metal-timing"),
        _COARSE_CATEGORIES, "duration", 1_000_000, 134_217_728, 30_000, True,
    ),
    "baseline": TraceProfile(
        "baseline", "Workload timing without detailed tracing", (), (), (),
        "none", 0, 0, 0, True,
    ),
    "timeline": TraceProfile(
        "timeline", "Correlate bounded engine CPU supply and wait activity",
        ("engine-perfetto",), ("pe-events", "metal-timing"),
        _COARSE_CATEGORIES, "duration", 1_000_000, 134_217_728, 30_000, True,
    ),
    "scheduler": TraceProfile(
        "scheduler", "Qualify scheduler residency and calibrated thread coverage",
        ("scheduler-trace",), (), (), "process-lifetime", 0, 268_435_456, 30_000,
        False, "no admitted macOS scheduler collector is configured by this runner",
    ),
    "target-probe": TraceProfile(
        "target-probe", "Observe selected REA target boundaries",
        ("target-probe",), (), (), "selected-window", 0, 67_108_864, 30_000,
        False, "no qualified target-probe adapter is configured by this runner",
    ),
    "gpu-detail": TraceProfile(
        "gpu-detail", "Measure selected GPU timing scopes",
        ("engine-perfetto", "metal-timing"), (), _COARSE_CATEGORIES + ("gpu",), "duration", 100_000,
        67_108_864, 30_000, True,
    ),
    "emulated-structure": TraceProfile(
        "emulated-structure", "Inspect structure in a separately identified emulated run",
        ("qemu-structure",), (), (), "selected-window", 10_000_000,
        536_870_912, 60_000, False,
        "QEMU is a separate execution lane and is not launched by run_experiment",
    ),
}


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def binary_architecture(path: Path) -> str | None:
    """Read the architecture tag from PE, thin Mach-O, or fat Mach-O headers."""
    try:
        with path.open("rb") as stream:
            prefix = stream.read(64)
            if prefix.startswith(b"MZ") and len(prefix) >= 64:
                pe_offset = int.from_bytes(prefix[0x3C:0x40], "little")
                stream.seek(pe_offset)
                header = stream.read(6)
                if header[:4] != b"PE\0\0":
                    return None
                machine = int.from_bytes(header[4:6], "little")
                return {0x014C: "x86", 0x8664: "x86_64", 0x01C4: "armv7",
                        0xAA64: "arm64"}.get(machine, f"pe-machine-0x{machine:04x}")

            magic = prefix[:4]
            if magic in (b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe"):
                cpu_type = int.from_bytes(prefix[4:8], "little")
                return _mach_cpu_name(cpu_type)
            if magic in (b"\xfe\xed\xfa\xcf", b"\xfe\xed\xfa\xce"):
                cpu_type = int.from_bytes(prefix[4:8], "big")
                return _mach_cpu_name(cpu_type)
            if magic in (b"\xca\xfe\xba\xbe", b"\xca\xfe\xba\xbf"):
                count = int.from_bytes(prefix[4:8], "big")
                if count > 128:
                    return None
                stream.seek(8)
                for _ in range(count):
                    arch = stream.read(20 if magic == b"\xca\xfe\xba\xbe" else 32)
                    if len(arch) < 8:
                        return None
                    cpu_type = int.from_bytes(arch[:4], "big")
                    name = _mach_cpu_name(cpu_type)
                    if name:
                        return name
    except OSError:
        return None
    return None


def _mach_cpu_name(cpu_type: int) -> str:
    cpu_type &= 0xFFFFFFFF
    return {7: "i386", 0x01000007: "x86_64", 12: "arm", 0x0100000C: "arm64"}.get(
        cpu_type, f"macho-cpu-0x{cpu_type:08x}"
    )


def canonical_digest(value: Any) -> str:
    data = json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(data).hexdigest()


def atomic_write_json(path: Path, value: dict[str, Any], *, replace_existing: bool = False) -> None:
    """Atomically publish JSON, refusing collisions unless updating owned state.

    Initial attempt artifacts use exclusive publication. Only the runner's
    evolving trace-manifest explicitly opts into replacement.
    """
    if _path_has_scoped_symlink(path):
        raise OSError(f"refusing to publish through symlinked parent: {path.parent}")
    path.parent.mkdir(parents=True, exist_ok=True)
    if _path_has_scoped_symlink(path):
        raise OSError(f"refusing to publish through symlinked parent: {path.parent}")
    if path.is_symlink():
        raise OSError(f"refusing to replace symlink: {path}")
    if not replace_existing and path.exists():
        raise FileExistsError(f"artifact already exists: {path}")
    temporary = path.with_name(f".{path.name}.{uuid.uuid4().hex}.tmp")
    try:
        temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")
        if replace_existing:
            os.replace(temporary, path)
        else:
            os.link(temporary, path)
            temporary.unlink()
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def source_identity(repo_root: Path) -> dict[str, Any]:
    def git(*args: str) -> str | None:
        try:
            return subprocess.run(
                ["git", *args], cwd=repo_root, check=True, capture_output=True,
                text=True, timeout=5,
            ).stdout.strip()
        except (OSError, subprocess.SubprocessError):
            return None

    revision = git("rev-parse", "HEAD")
    status = git("status", "--porcelain=v1", "-z", "--untracked-files=all")
    tracked_diff = git("diff", "--binary", "HEAD", "--")
    untracked_result = git("ls-files", "--others", "--exclude-standard", "-z")
    if revision is None or status is None or tracked_diff is None or untracked_result is None:
        return {"available": False, "revision": revision, "dirty": None,
                "reason": "git source identity could not be read completely"}
    status_bytes = status.encode("utf-8", errors="surrogateescape")
    diff_bytes = tracked_diff.encode("utf-8", errors="surrogateescape")
    untracked_paths = [p for p in untracked_result.split("\0") if p]
    entries: list[dict[str, Any]] = []
    total_bytes = 0
    complete = True
    for relative in untracked_paths[:10_000]:
        file_path = repo_root / relative
        try:
            metadata = file_path.lstat()
            if not stat.S_ISREG(metadata.st_mode):
                entries.append({"path": relative, "kind": "non-regular", "sha256": None})
                complete = False
                continue
            if metadata.st_size > 64 * 1024 * 1024 or total_bytes + metadata.st_size > 512 * 1024 * 1024:
                entries.append({"path": relative, "kind": "size-limit", "bytes": metadata.st_size,
                                "sha256": None})
                complete = False
                continue
            digest = sha256_file(file_path)
            total_bytes += metadata.st_size
            entries.append({"path": relative, "kind": "file", "bytes": metadata.st_size,
                            "sha256": digest})
        except OSError as exc:
            entries.append({"path": relative, "kind": "unreadable", "sha256": None,
                            "reason": str(exc)})
            complete = False
    if len(untracked_paths) > 10_000:
        omitted = untracked_paths[10_000:]
        entries.append({"kind": "entry-limit", "omitted_count": len(omitted),
                        "omitted_paths_sha256": hashlib.sha256(
                            "\0".join(omitted).encode("utf-8", errors="surrogateescape")
                        ).hexdigest()})
        complete = False
    status_nonempty = bool(status_bytes)
    return {
        "available": True,
        "revision": revision,
        "dirty": True if status_nonempty else False,
        "status_sha256": hashlib.sha256(status_bytes).hexdigest(),
        "tracked_diff_sha256": hashlib.sha256(diff_bytes).hexdigest(),
        "untracked": entries,
        "untracked_content_sha256": canonical_digest(entries),
        "content_identity_complete": complete,
    }


def attempt_paths(output_dir: Path, run_id: str) -> dict[str, Path]:
    root = output_dir / "tracing" / run_id
    return {
        "root": root,
        "manifest": root / "trace-manifest.json",
        "state_manifest": root / "trace-manifest.pending.json",
        "request": root / "trace-request.json",
        "receipt": root / "trace-receipt.json",
        "pftrace": root / "engine.pftrace",
        "observations": root / "observations.sqlite",
    }


def artifact_graph(paths: dict[str, Path]) -> list[dict[str, Any]]:
    """Return the content-addressed graph of artifacts available at seal time."""
    candidates = (
        ("observations-sqlite", paths["observations"]),
        ("request", paths["request"]),
        ("receipt", paths["receipt"]),
        ("completion-proof", Path(str(paths["receipt"]) + ".completion.json")),
        ("engine-trace", paths["pftrace"]),
        ("engine-raw-events", Path(str(paths["pftrace"]) + ".raw.jsonl")),
        ("engine-raw-spool", Path(str(paths["pftrace"]) + ".raw.jsonl.spool")),
        ("normalized-bundle", paths["root"] / "normalized-bundle.json"),
        ("trace-processor-receipt", paths["root"] / "trace-processor-receipt.json"),
    )
    graph = []
    for role, path in candidates:
        try:
            metadata = path.lstat()
            if not stat.S_ISREG(metadata.st_mode):
                continue
            graph.append({"role": role, "path": str(path), "bytes": metadata.st_size,
                          "sha256": sha256_file(path)})
        except OSError:
            continue
    return graph


def publish_observations(paths: dict[str, Path], manifest: dict[str, Any]) -> dict[str, Any]:
    """Preserve even failed attempts in a cold-path, immutable analysis store."""
    from scripts.tools.tracing_store import publish_store, MAX_INPUT_BYTES, safe_path
    from scripts.tools.tracing_normalize import adapt

    run_id = manifest.get("run_id", paths["root"].name)
    attempt_id = manifest.get("attempt_id", run_id)
    bundles = []
    artifacts = []
    errors = []
    engine = paths["root"] / "normalized-bundle.json"
    if not engine.exists():
        engine = Path(str(paths["pftrace"]) + ".raw.jsonl")
    sources = [("engine", engine)] if engine.exists() else []
    environment = manifest.get("runtime", {}).get("env_snapshot", {})
    for key, role in (("DXMT9_CPU_SUPPLY_TRACE", "cpu-supply"),
                      ("DXMT9_WAIT_TRACE", "wait-trace")):
        value = environment.get(key)
        if value and value not in ("0", "perfetto"):
            sources.append((role, Path(value)))
    sources.extend((row["role"], Path(row["path"]))
                   for row in manifest.get("supplemental_bundles", []))
    for role, source in sources:
        try:
            safe_path(source)
            if source.stat().st_size > MAX_INPUT_BYTES:
                raise ValueError("input exceeds observation byte budget")
            adapt(source, "auto", run_id=run_id, process_incarnation="unknown")
            bundles.append((role, source))
        except (OSError, ValueError) as exc:
            errors.append({"role": role, "path": str(source), "error": str(exc)})
        # Preserve failed input evidence without pretending it yielded events.
        artifacts.append({"role": role + "-source", "path": str(source),
                          "required": False, "reason": "immutable acquisition evidence"})
    for row in artifact_graph(paths):
        if row["role"] != "observations-sqlite":
            artifacts.append(row)
    artifacts.extend(manifest.get("supplemental_artifacts", []))
    provenance = {k: v for k, v in manifest.items()
                  if k not in ("artifacts", "observations", "sealed")}
    provenance["normalization_errors"] = errors
    try:
        result = publish_store(paths["observations"], run_id=run_id, attempt_id=attempt_id,
                               provenance=provenance, bundles=bundles, artifacts=artifacts)
    except (ValueError, OSError) as exc:
        if paths["observations"].exists():
            raise
        errors.append({"role": "store-input", "error": str(exc)})
        provenance["normalization_errors"] = errors
        provenance["rejected_inputs"] = artifacts
        result = publish_store(paths["observations"], run_id=run_id, attempt_id=attempt_id,
                               provenance=provenance, bundles=[], artifacts=[])
    result["normalization_errors"] = errors
    result["status"] = "partial" if errors else "published"
    return result


def seal_trace_manifest(paths: dict[str, Path], manifest: dict[str, Any]) -> dict[str, Any]:
    """Publish the final attempt manifest once and bind its artifact graph."""
    if paths["manifest"].exists():
        raise FileExistsError(paths["manifest"])
    manifest["observations"] = publish_observations(paths, manifest)
    if manifest["observations"]["normalization_errors"]:
        manifest.setdefault("qualification", {}).update(
            status="failed", required_trace_qualified=False,
            observation_errors=manifest["observations"]["normalization_errors"])
        manifest["validity"] = {"status": "invalid", "checks": ["observation_store_inputs_failed"]}
    manifest["artifacts"] = artifact_graph(paths)
    manifest["artifact_graph_sha256"] = canonical_digest(manifest["artifacts"])
    manifest["sealed"] = True
    atomic_write_json(paths["manifest"], manifest)
    return {
        "manifest": str(paths["manifest"]),
        "manifest_sha256": sha256_file(paths["manifest"]),
        "artifact_graph_sha256": manifest["artifact_graph_sha256"],
        "artifacts": manifest["artifacts"],
        "observations": manifest["observations"],
    }


def parent_result_binding(*, run_id: str, attempt_id: str, workload_status: str,
                          trace_qualified: bool, sealed: dict[str, Any]) -> dict[str, Any]:
    envelope = {
        "schema": "dxmt9.trace.result-envelope.v1",
        "run_id": run_id,
        "attempt_id": attempt_id,
        "workload_status": workload_status,
        "trace_qualified": trace_qualified,
        "manifest_sha256": sealed["manifest_sha256"],
        "artifact_graph_sha256": sealed["artifact_graph_sha256"],
    }
    return {"parent_result_envelope": envelope,
            "parent_result_envelope_sha256": canonical_digest(envelope)}


def make_trace_request(
    profile: TraceProfile, *, run_id: str, attempt_id: str, paths: dict[str, Path],
    duration_ms: int | None = None, delay_ms: int = 0,
    include_pe: bool = False, include_metal: bool = False,
    include_detail: bool = False,
    gpu_level: str = "command-buffer", gpu_capacity: int = 256,
    drain_mode: str = "deferred", batch_events: int = 64,
    service_interval_ms: int = 10, transport_events: int = 16_384,
) -> dict[str, Any]:
    include_metal = include_metal or profile.name == "overview"
    if gpu_level not in ("command-buffer", "encoder-stage"):
        raise ValueError("GPU timing level must be command-buffer or encoder-stage")
    if isinstance(gpu_capacity, bool) or not isinstance(gpu_capacity, int) or not 1 <= gpu_capacity <= 256:
        raise ValueError("GPU timing capacity must be between 1 and 256")
    if drain_mode not in ("deferred", "async"):
        raise ValueError("trace drain mode must be deferred or async")
    for name, value, minimum, maximum in (
        ("batch events", batch_events, 1, 4_096),
        ("service interval", service_interval_ms, 1, 1_000),
        ("transport events", transport_events, 1, 65_536),
    ):
        if isinstance(value, bool) or not isinstance(value, int) or not minimum <= value <= maximum:
            raise ValueError(f"trace {name} must be between {minimum} and {maximum}")
    if transport_events < batch_events * 4:
        raise ValueError("trace transport events must be at least four batches")
    window: dict[str, Any] = {"mode": profile.window_mode}
    if profile.window_mode == "duration":
        window.update({"delay_ms": delay_ms, "duration_ms": duration_ms or 5_000})
    optional_collectors = []
    requested_categories = list(profile.categories)
    if include_pe:
        optional_collectors.append("pe-events")
        requested_categories.append("pe")
    if include_metal:
        optional_collectors.append("metal-timing")
        requested_categories.append("gpu")
    required_categories = list(profile.categories)
    if include_detail:
        requested_categories.append("detail")
        required_categories.append("detail")
    body: dict[str, Any] = {
        "schema": SCHEMA_REQUEST,
        "run_id": run_id,
        "attempt_id": attempt_id,
        "profile_id": profile.name,
        "question": profile.question,
        "categories": requested_categories,
        "required_categories": required_categories,
        "window": window,
        "limits": {
            "max_events": profile.max_events,
            "max_bytes": profile.max_bytes,
            "finalize_timeout_ms": profile.finalize_timeout_ms,
        },
        "qualification_policy": profile.frozen_config()["qualification_policy"],
        "required_collectors": list(profile.required_collectors),
        "optional_collectors": optional_collectors,
        "transport": {
            "mode": drain_mode,
            "batch_events": batch_events,
            "service_interval_ms": service_interval_ms,
            "transport_events": transport_events,
        },
        "output": {"pftrace": str(paths["pftrace"]), "receipt": str(paths["receipt"])},
    }
    if profile.name == "gpu-detail" or include_metal:
        body["gpu_timing"] = {
            "level": gpu_level,
            "capacity": gpu_capacity,
            "required": profile.name == "gpu-detail",
            "calibration": True,
        }
    body["config_sha256"] = canonical_digest(body)
    return body


def pe_trace_configuration(profile: TraceProfile | None,
                           request: dict[str, Any] | None,
                           *, requested: bool | None = None) -> dict[str, Any]:
    """Resolve the PE sampler opt-in from the frozen attempt request."""
    if profile is None:
        return {"requested": False, "enabled": None, "environment": {}}
    collectors = []
    if isinstance(request, dict):
        required = request.get("required_collectors", [])
        optional = request.get("optional_collectors", [])
        if isinstance(required, list) and isinstance(optional, list):
            collectors = required + optional
    requested_by_request = "pe-events" in collectors
    enabled = profile is not None and requested_by_request
    intent = requested_by_request if requested is None else bool(requested)
    value = "1" if enabled else "0"
    return {
        "requested": intent,
        "enabled": enabled,
        "environment": {"DXMT9_TRACE_PE": value},
    }


def trace_launch_configuration(profile: TraceProfile | None,
                               request: dict[str, Any] | None, *,
                               request_path: Path | None = None,
                               receipt_path: Path | None = None,
                               requested_pe: bool | None = None) -> dict[str, Any]:
    if profile is None:
        return {"profile_active": False, "environment": {}, "suppressed_environment": []}
    pe_config = pe_trace_configuration(profile, request, requested=requested_pe)
    environment = dict(pe_config["environment"])
    engine_request_enabled = isinstance(request, dict)
    suppressed = []
    if engine_request_enabled:
        if request_path is not None:
            environment["DXMT9_TRACE_CONFIG"] = str(request_path)
        if receipt_path is not None:
            environment["DXMT9_TRACE_RECEIPT"] = str(receipt_path)
    else:
        suppressed = ["DXMT9_TRACE_CONFIG", "DXMT9_TRACE_RECEIPT"]
    return {
        "profile_active": True,
        "pe_events": pe_config,
        "engine_request": {
            "enabled": engine_request_enabled,
            "config_path": str(request_path) if engine_request_enabled and request_path else None,
            "receipt_path": str(receipt_path) if engine_request_enabled and receipt_path else None,
            "suppressed_environment": suppressed,
        },
        "environment": environment,
        "suppressed_environment": suppressed,
    }


def apply_trace_launch_configuration(env: dict[str, str],
                                     config: dict[str, Any]) -> None:
    if not config.get("profile_active"):
        return
    for key in config.get("suppressed_environment", []):
        env.pop(key, None)
    env.update(config.get("environment", {}))


def verify_engine_receipt(
    receipt_path: Path, request: dict[str, Any], pftrace_path: Path,
) -> dict[str, Any]:
    """Validate required engine receipt binding and artifact bytes; fail closed."""
    errors: list[str] = []
    if not isinstance(request, dict):
        return {"status": "failed", "qualified": False, "errors": ["request_not_object"]}
    required_request_fields = (
        "run_id", "attempt_id", "profile_id", "config_sha256", "categories", "output"
    )
    if any(key not in request for key in required_request_fields):
        return {"status": "failed", "qualified": False,
                "errors": ["request_required_fields_missing"]}
    if not _safe_artifact_path(receipt_path):
        return {"status": "failed", "qualified": False,
                "errors": ["receipt_path_unsafe"]}
    try:
        if not stat.S_ISREG(receipt_path.lstat().st_mode):
            return {"status": "failed", "qualified": False,
                    "errors": ["receipt_not_regular_file"]}
    except OSError as exc:
        return {"status": "failed", "qualified": False,
                "errors": [f"receipt_unavailable:{exc}"]}
    try:
        receipt = json.loads(receipt_path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        return {"status": "failed", "qualified": False, "errors": [f"receipt_unavailable:{exc}"]}
    if not isinstance(receipt, dict):
        return {"status": "failed", "qualified": False,
                "errors": ["receipt_root_not_object"], "receipt": receipt}
    expected = {
        "schema": SCHEMA_RECEIPT,
        "run_id": request["run_id"],
        "attempt_id": request["attempt_id"],
        "profile_id": request["profile_id"],
        "config_sha256": request["config_sha256"],
    }
    for key, value in expected.items():
        if receipt.get(key) != value:
            errors.append(f"receipt_mismatch:{key}")
    request_payload = {key: value for key, value in request.items() if key != "config_sha256"}
    if request.get("config_sha256") != canonical_digest(request_payload):
        errors.append("request_config_digest_mismatch")
    if receipt.get("status") != "complete":
        errors.append(f"engine_status:{receipt.get('status', 'missing')}")
    if "proof_required" in receipt:
        proof_ref = receipt.get("completion_proof")
        proof_path = Path(str(receipt_path) + ".completion.json")
        if receipt.get("proof_required") is not True:
            errors.append("completion_proof_requirement_invalid")
        if (not isinstance(proof_ref, dict) or proof_ref.get("required") is not True
                or not _same_lexical_path(proof_ref.get("path"), proof_path)):
            errors.append("completion_proof_reference_invalid")
        limits = request.get("limits", {})
        wait_ms = _nonnegative_integer(limits.get("finalize_timeout_ms")) if isinstance(limits, dict) else 0
        proof_deadline = time.monotonic() + min(wait_ms or 0, 120_000) / 1000
        while not proof_path.exists() and time.monotonic() < proof_deadline:
            time.sleep(min(0.02, max(0.0, proof_deadline - time.monotonic())))
        try:
            if not _safe_artifact_path(proof_path):
                raise OSError("completion proof path is unsafe")
            proof_stat = proof_path.lstat()
            if not stat.S_ISREG(proof_stat.st_mode):
                raise OSError("completion proof is not a regular file")
            proof = json.loads(proof_path.read_text())
            if not isinstance(proof, dict):
                raise ValueError("completion proof root is not an object")
        except (OSError, ValueError, json.JSONDecodeError) as exc:
            errors.append(f"completion_proof_unavailable:{exc}")
        else:
            if proof.get("schema") != "dxmt9.trace.completion.v1":
                errors.append("completion_proof_schema_mismatch")
            for key in ("run_id", "attempt_id", "config_sha256"):
                if proof.get(key) != receipt.get(key):
                    errors.append(f"completion_proof_mismatch:{key}")
            if proof.get("generation") != receipt.get("generation"):
                errors.append("completion_proof_mismatch:generation")
            if (not isinstance(receipt.get("generation"), str)
                    or re.fullmatch(r"(?:0|[1-9][0-9]*)", receipt["generation"]) is None):
                errors.append("completion_proof_generation_malformed")
            if proof.get("status") != "complete":
                errors.append(f"completion_proof_status:{proof.get('status', 'missing')}")
            decided_ns = _nonnegative_integer(proof.get("completed_ns"))
            deadline_ns = _nonnegative_integer(proof.get("deadline_ns"))
            if (decided_ns is None or deadline_ns is None
                    or not isinstance(proof.get("completed_ns"), str)
                    or not isinstance(proof.get("deadline_ns"), str)):
                errors.append("completion_proof_deadline_malformed")
            elif decided_ns > deadline_ns:
                errors.append("completion_proof_deadline_exceeded")
            if proof.get("candidate_receipt_sha256") != sha256_file(receipt_path):
                errors.append("completion_proof_candidate_digest_mismatch")
    requested_transport = request.get("transport", {
        "mode": "deferred", "batch_events": 64, "service_interval_ms": 10,
        "transport_events": 16_384,
    })
    receipt_transport = receipt.get("transport")
    if receipt_transport is None and requested_transport.get("mode") == "deferred":
        pass  # Legacy deferred receipts predate transport provenance.
    elif receipt_transport != requested_transport:
        errors.append("receipt_mismatch:transport")
    if requested_transport.get("mode") == "async":
        spool = Path(str(Path(str(pftrace_path) + ".raw.jsonl")) + ".spool")
        spool_artifact = receipt.get("raw_spool_artifact")
        if not isinstance(spool_artifact, dict):
            errors.append("raw_spool_artifact_receipt_missing")
        else:
            if not _safe_artifact_path(spool) or not _same_lexical_path(spool_artifact.get("path"), spool):
                errors.append("raw_spool_artifact_path_mismatch")
            try:
                spool_stat = spool.lstat()
                spool_size = spool_stat.st_size
                if not stat.S_ISREG(spool_stat.st_mode):
                    raise OSError("raw spool artifact is not a regular file")
                spool_digest = sha256_file(spool)
            except OSError:
                errors.append("raw_spool_artifact_missing")
            else:
                if (spool_size <= 0 or not _valid_size(spool_artifact.get("bytes"))
                        or spool_artifact.get("bytes") != spool_size):
                    errors.append("raw_spool_artifact_size_mismatch_or_empty")
                if (not _valid_digest(spool_artifact.get("sha256"))
                        or spool_artifact.get("sha256") != spool_digest):
                    errors.append("raw_spool_artifact_digest_mismatch")
    required_collectors = request.get("required_collectors")
    if not isinstance(required_collectors, list) or any(
        not isinstance(name, str) for name in required_collectors
    ):
        errors.append("request_required_collectors_malformed")
    else:
        collector_status = receipt.get("collector_status", {})
        if any(name != "engine-perfetto" for name in required_collectors) and not isinstance(
            collector_status, dict
        ):
            errors.append("required_collector_outcomes_missing")
        elif isinstance(collector_status, dict):
            for collector in required_collectors:
                if collector == "engine-perfetto":
                    continue
                outcome = collector_status.get(collector)
                if not isinstance(outcome, dict) or outcome.get("status") != "complete":
                    errors.append(f"required_collector_failed:{collector}")
    window = receipt.get("window")
    if not isinstance(window, dict) or window.get("closed") is not True:
        errors.append("capture_window_not_closed")
    counts = receipt.get("counts")
    if not isinstance(counts, dict) or not counts:
        errors.append("source_counts_missing")
    elif any(_nonnegative_integer(value) is None for value in counts.values()):
        errors.append("source_counts_malformed")
    if not isinstance(receipt.get("flush"), dict) or receipt["flush"].get("status") != "complete":
        errors.append("flush_incomplete")
    loss = receipt.get("loss")
    if not isinstance(loss, dict) or not loss or any(
        _nonnegative_integer(value) is None or _nonnegative_integer(value) != 0
        for value in loss.values()
    ):
        errors.append("event_loss_or_missing_loss_table")
    resolved = receipt.get("categories_resolved")
    required_categories = request.get("required_categories", request.get("categories"))
    if (not isinstance(required_categories, list)
            or any(not isinstance(value, str) for value in required_categories)
            or not isinstance(resolved, list)
            or any(not isinstance(value, str) for value in resolved)
            or len(resolved) != len(set(resolved))
            or not set(required_categories).issubset(resolved)):
        errors.append("required_categories_unresolved")
    applicability = receipt.get("applicability")
    if not isinstance(applicability, dict):
        errors.append("category_applicability_missing")
    else:
        for category in required_categories if isinstance(required_categories, list) else []:
            row = applicability.get(category)
            if not isinstance(row, dict) or row.get("status") not in ("observed", "not_applicable"):
                errors.append(f"category_applicability_unqualified:{category}")
                continue
            count = row.get("count")
            if _nonnegative_integer(count) is None:
                errors.append(f"category_count_malformed:{category}")
    optional_status = receipt.get("optional_collectors")
    requested_optional = request.get("optional_collectors", [])
    if (not isinstance(requested_optional, list)
            or any(not isinstance(value, str) for value in requested_optional)):
        errors.append("request_optional_collectors_malformed")
    elif requested_optional and not isinstance(optional_status, dict):
        errors.append("optional_collector_outcomes_missing")
    elif isinstance(optional_status, dict):
        for collector in requested_optional:
            outcome = optional_status.get(collector)
            if not isinstance(outcome, dict) or outcome.get("status") not in (
                "complete", "unavailable", "failed"
            ):
                errors.append(f"optional_collector_outcome_missing:{collector}")
    artifact = receipt.get("artifact")
    if not isinstance(artifact, dict):
        errors.append("artifact_receipt_missing")
    else:
        if not _safe_artifact_path(pftrace_path) or not _same_lexical_path(
            artifact.get("path"), pftrace_path
        ):
            errors.append("artifact_path_mismatch")
        try:
            file_stat = pftrace_path.lstat()
            size = file_stat.st_size
            if not stat.S_ISREG(file_stat.st_mode):
                raise OSError("artifact is not a regular file")
            digest = sha256_file(pftrace_path)
        except OSError:
            errors.append("artifact_missing")
        else:
            if size <= 0 or not _valid_size(artifact.get("bytes")) or artifact.get("bytes") != size:
                errors.append("artifact_size_mismatch_or_empty")
            if not _valid_digest(artifact.get("sha256")) or artifact.get("sha256") != digest:
                errors.append("artifact_digest_mismatch")
    raw_artifact = receipt.get("raw_artifact")
    if not isinstance(raw_artifact, dict):
        errors.append("raw_event_artifact_receipt_missing")
    else:
        expected_raw = Path(str(pftrace_path) + ".raw.jsonl")
        if not _safe_artifact_path(expected_raw) or not _same_lexical_path(
            raw_artifact.get("path"), expected_raw
        ):
            errors.append("raw_event_artifact_path_mismatch")
        try:
            raw_stat = expected_raw.lstat()
            raw_size = raw_stat.st_size
            if not stat.S_ISREG(raw_stat.st_mode):
                raise OSError("raw event artifact is not a regular file")
            raw_digest = sha256_file(expected_raw)
        except OSError:
            errors.append("raw_event_artifact_missing")
        else:
            if raw_size <= 0 or not _valid_size(raw_artifact.get("bytes")) or raw_artifact.get("bytes") != raw_size:
                errors.append("raw_event_artifact_size_mismatch_or_empty")
            if not _valid_digest(raw_artifact.get("sha256")) or raw_artifact.get("sha256") != raw_digest:
                errors.append("raw_event_artifact_digest_mismatch")
    return {"status": "qualified" if not errors else "failed", "qualified": not errors,
            "errors": errors, "receipt": receipt}


def _valid_size(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and value >= 0


def _nonnegative_integer(value: Any) -> int | None:
    if isinstance(value, int) and not isinstance(value, bool) and value >= 0:
        return value
    if isinstance(value, str) and re.fullmatch(r"(?:0|[1-9][0-9]*)", value):
        try:
            return int(value)
        except ValueError:
            return None
    return None


def _valid_digest(value: Any) -> bool:
    return isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value) is not None


def _same_lexical_path(value: Any, expected: Path) -> bool:
    if not isinstance(value, str) or not value:
        return False
    try:
        return Path(os.path.abspath(value)) == Path(os.path.abspath(expected))
    except (OSError, ValueError):
        return False


def _safe_artifact_path(path: Path) -> bool:
    return not _path_has_scoped_symlink(path)


def _path_has_scoped_symlink(path: Path) -> bool:
    absolute = Path(os.path.abspath(path))
    repo_root = Path(__file__).resolve().parents[2]
    temp_root = Path(tempfile.gettempdir()).absolute()
    boundary = None
    for candidate in (repo_root, temp_root):
        try:
            absolute.relative_to(candidate)
            boundary = candidate
            break
        except ValueError:
            continue
    cursor = absolute
    while cursor != boundary and cursor != cursor.parent:
        if cursor.is_symlink():
            return True
        cursor = cursor.parent
    return boundary is None and absolute.is_symlink()


def parent_envelope(*, producer: str, inputs: list[dict[str, str]], env_snapshot: dict[str, str],
                    validity: dict[str, Any]) -> dict[str, Any]:
    return {
        "schema": SCHEMA_MANIFEST,
        "producer": producer,
        "stage": "run-capture",
        "domain": "runner",
        "inputs": inputs,
        "env_snapshot": env_snapshot,
        "validity": validity,
    }


def host_identity() -> dict[str, str]:
    return {"os": platform.platform(), "architecture": platform.machine(),
            "python": platform.python_version()}


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()
