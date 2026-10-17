#!/usr/bin/env python3
"""Validate external probe evidence and decode bounded Linux perf ring snapshots.

This module does not install probes. Runtime adapters must pass capability
preflight and implement the ownership callbacks supplied to ``ProbeSession``.
The current macOS path imports existing PE sampler summaries as guest-PC
population evidence; it does not turn a static REA manifest into runtime hits.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import platform
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Iterable


SCHEMA = "dxmt9-external-probe-v1"
SUPPORTED_PROBE_TYPES = {"address-hit", "cpu-sample", "debugger-stop", "guest-marker"}
STATES = ("idle", "prepared", "attached", "armed", "collecting", "disabled", "drained", "detached", "failed")


class ProbeError(ValueError):
    """An invalid manifest, unsupported capability, or unsafe state transition."""


def _need(obj: dict[str, Any], key: str, kind: type) -> Any:
    value = obj.get(key)
    if not isinstance(value, kind):
        raise ProbeError(f"{key} must be {kind.__name__}")
    return value


def validate_manifest(manifest: dict[str, Any]) -> dict[str, Any]:
    """Validate the versioned, pointer-free probe plan without asserting hits."""
    if manifest.get("schema") != SCHEMA:
        raise ProbeError(f"unsupported manifest schema: {manifest.get('schema')!r}")
    target = _need(manifest, "target", dict)
    for key in ("module_sha256", "architecture", "address_space", "address_convention"):
        _need(target, key, str)
    digest = target["module_sha256"].lower()
    if len(digest) != 64 or any(c not in "0123456789abcdef" for c in digest):
        raise ProbeError("target.module_sha256 must be a SHA-256 hex digest")
    if target["address_space"] not in ("native", "guest"):
        raise ProbeError("target.address_space must be native or guest")
    modules = _need(manifest, "modules", list)
    if not modules:
        raise ProbeError("modules must contain the analyzed module")
    module_ids: set[str] = set()
    for module in modules:
        if not isinstance(module, dict):
            raise ProbeError("each module must be an object")
        module_id = _need(module, "id", str)
        if module_id in module_ids:
            raise ProbeError(f"duplicate module id: {module_id}")
        module_ids.add(module_id)
        module_digest = _need(module, "sha256", str).lower()
        if len(module_digest) != 64 or any(c not in "0123456789abcdef" for c in module_digest):
            raise ProbeError(f"module {module_id} has invalid SHA-256")
        _need(module, "architecture", str)
        _need(module, "rea_provider", str)
        _need(module, "rea_version", str)
        _need(module, "address_convention", str)
    probes = _need(manifest, "probes", list)
    if not probes:
        raise ProbeError("probes must not be empty")
    probe_ids: set[str] = set()
    for probe in probes:
        if not isinstance(probe, dict):
            raise ProbeError("each probe must be an object")
        probe_id = _need(probe, "id", str)
        if probe_id in probe_ids:
            raise ProbeError(f"duplicate probe id: {probe_id}")
        probe_ids.add(probe_id)
        probe_type = _need(probe, "type", str)
        if probe_type not in SUPPORTED_PROBE_TYPES:
            raise ProbeError(f"unsupported probe type: {probe_type}")
        if probe_type in ("address-hit", "debugger-stop"):
            if probe.get("module_id") not in module_ids:
                raise ProbeError(f"probe {probe_id} references an unknown module")
            _int(probe, "rva")
            _need(probe, "instruction_bytes", str)
            _need(probe, "instruction_semantics", str)
            _need(probe, "abi", str)
            registers = _need(probe, "registers", list)
            if any(not isinstance(reg, str) or not reg for reg in registers):
                raise ProbeError(f"probe {probe_id}.registers must contain names")
        if probe_type == "cpu-sample":
            collection = _need(manifest, "collection", dict)
            if not any(k in collection for k in ("period", "frequency")):
                raise ProbeError("cpu-sample requires collection.period or collection.frequency")
    collection = _need(manifest, "collection", dict)
    for key in ("api", "tool", "tool_version", "os", "architecture", "execution_mode"):
        _need(collection, key, str)
    _need(collection, "required_capabilities", list)
    coverage = _need(manifest, "coverage", dict)
    for key in ("thread_selection", "new_thread_policy", "fork_policy", "exec_policy"):
        _need(coverage, key, str)
    control = _need(manifest, "control", dict)
    for key in ("owner", "deadline_ms", "restore_policy"):
        if key == "deadline_ms":
            _int(control, key)
        else:
            _need(control, key, str)
    if control["deadline_ms"] <= 0:
        raise ProbeError("control.deadline_ms must be positive")
    return manifest


def _int(obj: dict[str, Any], key: str) -> int:
    value = obj.get(key)
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise ProbeError(f"{key} must be a non-negative integer")
    return value


def resolve_site(manifest: dict[str, Any], probe_id: str, *, module_sha256: str,
                 loaded_base: int, load_generation: int, image_size: int,
                 instruction_bytes_at_rva: bytes) -> dict[str, Any]:
    """Resolve an RVA only after hash, bounds, bytes, and load generation checks."""
    validate_manifest(manifest)
    module = next((m for m in manifest["modules"] if m["id"] ==
                   next((p.get("module_id") for p in manifest["probes"] if p["id"] == probe_id), None)), None)
    probe = next((p for p in manifest["probes"] if p["id"] == probe_id), None)
    if probe is None or module is None:
        raise ProbeError(f"unknown address probe: {probe_id}")
    if probe["type"] not in ("address-hit", "debugger-stop"):
        raise ProbeError("selected probe does not resolve to an instruction address")
    if module["sha256"].lower() != module_sha256.lower():
        raise ProbeError("loaded module hash mismatch")
    rva = probe["rva"]
    expected = bytes.fromhex(probe["instruction_bytes"].replace(" ", ""))
    if not expected or rva + len(expected) > image_size:
        raise ProbeError("probe RVA is outside the loaded image")
    if instruction_bytes_at_rva[:len(expected)] != expected:
        raise ProbeError("instruction bytes mismatch")
    if loaded_base < 0 or load_generation < 0:
        raise ProbeError("invalid loaded module identity")
    return {"probe_id": probe_id, "module_id": module["id"], "module_sha256": module_sha256.lower(),
            "load_generation": load_generation, "address_space": manifest["target"]["address_space"],
            "rva": rva, "resolved_address": loaded_base + rva,
            "instruction_bytes": expected.hex(), "semantics": probe["instruction_semantics"],
            "abi": probe["abi"], "requested_registers": list(probe["registers"]),
            "static_manifest_only": True, "runtime_hits_established": False}


def preflight(manifest: dict[str, Any], *, system: str | None = None,
              machine: str | None = None, capabilities: Iterable[str] = ()) -> dict[str, Any]:
    """Strictly report adapter support; no fallback from requested probe type."""
    validate_manifest(manifest)
    host_os = system or platform.system()
    host_arch = machine or platform.machine()
    requested = set(manifest["collection"]["required_capabilities"])
    available = set(capabilities)
    api = manifest["collection"]["api"]
    if api in {"ptrace", "perf_event_open", "perf-record", "perftrace"} and host_os != "Linux":
        return {"supported": False, "adapter": api, "host_os": host_os,
                "host_architecture": host_arch, "reason": "linux_api_on_non_linux_host",
                "missing_capabilities": sorted(requested)}
    # Parsing and manifest validation are portable. No active Linux collector is
    # admitted until permission, thread inheritance, and teardown probes exist.
    runtime_adapter = api in {"ptrace", "perf_event_open", "perf-record", "perftrace", "macos-mach", "macos-debugger"}
    if runtime_adapter:
        missing = sorted(requested - available)
        return {"supported": False, "adapter": api, "host_os": host_os,
                "host_architecture": host_arch, "reason": "runtime_adapter_not_qualified" if not missing else "missing_capabilities",
                "missing_capabilities": missing or ["qualified_attach_arm_drain_detach_protocol"]}
    if api == "dxmt9-pe-sampler-import":
        if host_os != "Darwin":
            return {"supported": False, "adapter": api, "host_os": host_os,
                    "host_architecture": host_arch, "reason": "sampler_import_requires_darwin_evidence"}
        return {"supported": True, "mode": "read-only-import", "adapter": api,
                "host_os": host_os, "host_architecture": host_arch,
                "qualification": "guest_pc_population_only; not address-hit or exclusive CPU"}
    return {"supported": False, "adapter": api, "host_os": host_os,
            "host_architecture": host_arch, "reason": "unknown_collection_api"}


def qualify_installation_coverage(*, requested_threads: Iterable[int], requested_sites: Iterable[str],
                                 installations: Iterable[dict[str, Any]], hardware_slots: int) -> dict[str, Any]:
    """Retain failed thread/site installs; never treat omitted coverage as zero hits."""
    threads = set(requested_threads)
    sites = set(requested_sites)
    rows = list(installations)
    if hardware_slots < 0:
        raise ProbeError("hardware slot capacity cannot be negative")
    per_thread: dict[int, set[str]] = {}
    failures: list[dict[str, Any]] = []
    for row in rows:
        if not isinstance(row, dict) or isinstance(row.get("tid"), bool) or not isinstance(row.get("tid"), int):
            raise ProbeError("installation rows require integer tid")
        tid = row["tid"]
        site = row.get("probe_id")
        if tid not in threads:
            failures.append({"tid": tid, "probe_id": site, "reason": "unrequested_thread"})
            continue
        if site not in sites:
            failures.append({"tid": tid, "probe_id": site, "reason": "unrequested_probe"})
            continue
        if row.get("installed") is True:
            per_thread.setdefault(tid, set()).add(site)
        else:
            failures.append({"tid": tid, "probe_id": site,
                             "reason": row.get("failure", "installation_failed")})
    required = len(threads) * len(sites)
    installed = sum(len(per_thread.get(tid, set())) for tid in threads)
    missing = [{"tid": tid, "probe_id": site, "reason": "not_reported"}
               for tid in sorted(threads) for site in sorted(sites)
               if site not in per_thread.get(tid, set()) and
               not any(f.get("tid") == tid and f.get("probe_id") == site for f in failures)]
    failures.extend(missing)
    # Hardware breakpoints usually apply per thread. Capacity here is the
    # advertised per-thread site budget, so reject before making completeness.
    over_budget = len(sites) > hardware_slots
    return {"requested_threads": sorted(threads), "requested_sites": sorted(sites),
            "installed_pairs": installed, "required_pairs": required,
            "hardware_slots_per_thread": hardware_slots, "over_budget": over_budget,
            "failures": failures,
            "complete": required > 0 and installed == required and not failures and not over_budget,
            "hit_count_claim_allowed": required > 0 and installed == required and not failures and not over_budget}


PERF_HEADER = struct.Struct("=IHH")
PERF_RECORD_LOST = 2
PERF_RECORD_SAMPLE = 9


def _ring_copy(data: bytes | bytearray | memoryview, offset: int, size: int) -> bytes:
    capacity = len(data)
    if size < 0 or size > capacity:
        raise ProbeError("ring record exceeds buffer capacity")
    start = offset % capacity
    first = min(size, capacity - start)
    return bytes(data[start:start + first]) + bytes(data[:size - first])


def decode_perf_ring(*, data: bytes | bytearray | memoryview, data_head: int, data_tail: int,
                     sample_type: int, sample_regs_user: int = 0,
                     abi_word_bytes: int = 8, record_limit: int = 100_000) -> dict[str, Any]:
    """Decode a published perf ring interval, including wrap and loss records.

    ``data`` represents the mmap data area, not the metadata page. Caller must
    acquire-load ``data_head`` before calling and publish ``data_tail`` only
    after consumption. Sample payload uses the Linux perf_event sample order.
    """
    if abi_word_bytes not in (4, 8):
        raise ProbeError("perf ABI word size must be 4 or 8")
    capacity = len(data)
    if not capacity or capacity & (capacity - 1):
        raise ProbeError("perf ring capacity must be a non-zero power of two")
    if data_head < data_tail or data_tail < 0:
        raise ProbeError("invalid perf ring head/tail ordering")
    available = data_head - data_tail
    if available > capacity:
        raise ProbeError("perf ring overrun: published interval exceeds capacity")
    records: list[dict[str, Any]] = []
    lost_total = 0
    offset = data_tail
    while offset < data_head:
        if len(records) >= record_limit:
            raise ProbeError("perf ring record limit exceeded")
        header_data = _ring_copy(data, offset, PERF_HEADER.size)
        record_type, misc, size = PERF_HEADER.unpack(header_data)
        if size < PERF_HEADER.size:
            raise ProbeError("perf record size is smaller than its header")
        if size > data_head - offset:
            raise ProbeError("perf record extends beyond published head")
        raw = _ring_copy(data, offset, size)
        payload = raw[PERF_HEADER.size:]
        item: dict[str, Any] = {"type": record_type, "misc": misc, "size": size,
                                "source_offset": offset, "raw_hex": raw.hex()}
        if record_type == PERF_RECORD_LOST:
            if len(payload) < 16:
                raise ProbeError("truncated PERF_RECORD_LOST")
            event_id, lost = struct.unpack_from("=QQ", payload)
            item.update({"kind": "lost", "event_id": str(event_id), "lost": lost})
            lost_total += lost
        elif record_type == PERF_RECORD_SAMPLE:
            item.update({"kind": "sample", **_decode_sample(payload, sample_type,
                                                            sample_regs_user, abi_word_bytes)})
        else:
            item["kind"] = "unknown"
        records.append(item)
        offset += size
    return {"schema": "linux-perf-ring-v1", "abi_word_bytes": abi_word_bytes,
            "sample_type": sample_type, "sample_regs_user": str(sample_regs_user),
            "data_tail": data_tail, "data_head": data_head, "capacity": capacity,
            "records": records, "lost_records": sum(r["kind"] == "lost" for r in records),
            "lost_samples": lost_total, "complete": lost_total == 0,
            "loss_scope": "published_interval" if lost_total else None}


def _take(payload: bytes, pos: int, size: int, field: str) -> tuple[bytes, int]:
    if pos + size > len(payload):
        raise ProbeError(f"truncated perf sample {field}")
    return payload[pos:pos + size], pos + size


def _u(payload: bytes, pos: int, size: int, field: str) -> tuple[int, int]:
    chunk, pos = _take(payload, pos, size, field)
    return int.from_bytes(chunk, sys.byteorder), pos


def _decode_sample(payload: bytes, sample_type: int, regs_mask: int, word: int) -> dict[str, Any]:
    # PERF_SAMPLE_* order is defined by linux/perf_event.h; unsupported fields
    # are rejected so a newer ABI cannot be misparsed as a familiar sample.
    known = 1 | 2 | 4 | 8 | 16 | 32 | 64 | 128 | 256 | 512 | 1024 | 2048 | 4096 | 8192 | 16384 | 32768 | 65536 | 131072 | 262144 | 524288
    if sample_type & ~known:
        raise ProbeError("unsupported PERF_SAMPLE field bit")
    pos = 0
    out: dict[str, Any] = {"requested_register_mask": str(regs_mask), "valid_register_mask": "0"}
    if sample_type & 1:  # IP
        out["ip"], pos = _u(payload, pos, 8, "ip")
    if sample_type & 2:  # TID
        pid, pos = _u(payload, pos, 4, "pid")
        tid, pos = _u(payload, pos, 4, "tid")
        out.update({"pid": pid, "tid": tid})
    if sample_type & 4:  # TIME
        out["time"], pos = _u(payload, pos, 8, "time")
    if sample_type & 8:  # ADDR
        out["addr"], pos = _u(payload, pos, 8, "addr")
    if sample_type & 16:  # READ: variable; preserve bytes only when ABI is not configured
        raise ProbeError("PERF_SAMPLE_READ requires an explicit read-format schema")
    if sample_type & 32:  # CALLCHAIN
        nr, pos = _u(payload, pos, 8, "callchain_count")
        if nr > 1_000_000 or pos + nr * 8 > len(payload):
            raise ProbeError("invalid perf callchain length")
        out["callchain"] = list(struct.unpack_from(f"={nr}Q", payload, pos))
        pos += nr * 8
    if sample_type & 64:  # ID
        out["id"], pos = _u(payload, pos, 8, "id")
    if sample_type & 128:  # CPU
        cpu, pos = _u(payload, pos, 4, "cpu")
        _, pos = _u(payload, pos, 4, "cpu_reserved")
        out["cpu"] = cpu
    if sample_type & 256:  # PERIOD
        out["period"], pos = _u(payload, pos, 8, "period")
    if sample_type & 512:  # STREAM_ID
        out["stream_id"], pos = _u(payload, pos, 8, "stream_id")
    if sample_type & 1024:  # RAW
        raw_size, pos = _u(payload, pos, 4, "raw_size")
        out["raw"], pos = _take(payload, pos, raw_size, "raw")
        pos = (pos + 7) & ~7
    if sample_type & 2048:  # BRANCH_STACK
        raise ProbeError("PERF_SAMPLE_BRANCH_STACK is not decoded by this adapter")
    if sample_type & 4096:  # REGS_USER
        abi, pos = _u(payload, pos, 8, "regs_abi")
        out["register_abi"] = abi
        if abi:
            values = []
            for bit in range(regs_mask.bit_length()):
                if regs_mask & (1 << bit):
                    value, pos = _u(payload, pos, 8, "user_register")
                    values.append({"mask_bit": bit, "value": value})
            out["registers"] = values
            out["valid_register_mask"] = str(regs_mask)
    if sample_type & 8192:  # STACK_USER
        size, pos = _u(payload, pos, 8, "stack_user_size")
        if size > 16 * 1024 * 1024:
            raise ProbeError("perf user stack exceeds decoder limit")
        stack, pos = _take(payload, pos, size, "stack_user")
        out["stack_user_hex"] = stack.hex()
        if size:
            _, pos = _u(payload, pos, 8, "stack_dynamic_size")
    if sample_type & 16384:  # WEIGHT
        out["weight"], pos = _u(payload, pos, 8, "weight")
    if sample_type & 32768:  # DATA_SRC
        out["data_src"], pos = _u(payload, pos, 8, "data_src")
    if sample_type & 65536:  # IDENTIFIER
        out["identifier"], pos = _u(payload, pos, 8, "identifier")
    if sample_type & 131072:  # TRANSACTION
        out["transaction"], pos = _u(payload, pos, 8, "transaction")
    if sample_type & 262144:  # REGS_INTR
        raise ProbeError("PERF_SAMPLE_REGS_INTR is not decoded by this adapter")
    if sample_type & 524288:  # PHYS_ADDR
        out["phys_addr"], pos = _u(payload, pos, 8, "phys_addr")
    if pos != len(payload):
        raise ProbeError("perf sample has trailing bytes or mismatched ABI")
    return out


@dataclass
class ProbeSession:
    """Bounded ownership state machine over adapter operations.

    Callbacks must perform only state they own. This controller never sends a
    signal, resumes a target, kills it, or claims teardown succeeded without a
    successful callback receipt.
    """

    manifest: dict[str, Any]
    adapter: str
    state: str = "idle"
    receipt: dict[str, Any] = field(default_factory=dict)
    _owned: set[str] = field(default_factory=set)

    def prepare(self, result: dict[str, Any]) -> None:
        self._transition("idle", "prepared")
        if result.get("supported") is not True:
            self.state = "failed"
            self.receipt.update({"preflight": result, "complete": False})
            raise ProbeError(f"preflight rejected adapter: {result.get('reason', 'unsupported')}")
        self.receipt["preflight"] = result

    def attach(self, operation: Callable[[], dict[str, Any]]) -> None:
        self._transition("prepared", "attached")
        self._owned.add("attachment")
        self._operate("attach", operation)

    def arm(self, operation: Callable[[], dict[str, Any]]) -> None:
        self._transition("attached", "armed")
        self._owned.add("probes")
        self._operate("arm", operation)

    def collect(self, operation: Callable[[], dict[str, Any]]) -> None:
        self._transition("armed", "collecting")
        self._operate("collect", operation)

    def disable(self, operation: Callable[[], dict[str, Any]]) -> None:
        self._transition(("armed", "collecting"), "disabled")
        self._operate("disable", operation)
        self._owned.discard("probes")

    def drain(self, operation: Callable[[], dict[str, Any]]) -> None:
        self._transition("disabled", "drained")
        self._operate("drain", operation)

    def detach(self, operation: Callable[[], dict[str, Any]]) -> None:
        self._transition(("drained", "attached", "prepared"), "detached")
        if "attachment" in self._owned:
            self._operate("detach", operation)
            self._owned.discard("attachment")
        self.receipt["complete"] = not self._owned and self.state == "detached"

    def cleanup(self, *, disable: Callable[[], dict[str, Any]],
                drain: Callable[[], dict[str, Any]], detach: Callable[[], dict[str, Any]]) -> dict[str, Any]:
        """Try owned-state cleanup after a failed operation; never resumes/kills."""
        cleanup: dict[str, Any] = {}
        for name, operation, owned_key in (("disable", disable, "probes"),
                                           ("drain", drain, None),
                                           ("detach", detach, "attachment")):
            if owned_key and owned_key not in self._owned:
                continue
            if name == "drain" and "disable" not in cleanup:
                continue
            try:
                outcome = operation()
                if not isinstance(outcome, dict) or outcome.get("ok") is not True:
                    raise ProbeError(f"{name} cleanup did not report ok=true")
                cleanup[name] = outcome
                if owned_key:
                    self._owned.discard(owned_key)
            except Exception as exc:
                cleanup[name] = {"ok": False, "error": str(exc)}
        self.receipt["cleanup"] = cleanup
        self.receipt["owned_state_remaining"] = sorted(self._owned)
        self.receipt["teardown_complete"] = not self._owned
        self.receipt["complete"] = not self._owned and self.receipt.get("failure") is None
        if not self._owned:
            self.state = "detached"
        return cleanup

    def _operate(self, name: str, operation: Callable[[], dict[str, Any]]) -> None:
        try:
            outcome = operation()
            if not isinstance(outcome, dict) or outcome.get("ok") is not True:
                raise ProbeError(f"{name} operation did not report ok=true")
            self.receipt[name] = outcome
        except Exception as exc:
            self.state = "failed"
            self.receipt.update({"failure": name, "error": str(exc), "teardown_complete": False,
                                 "owned_state_remaining": sorted(self._owned)})
            raise

    def _transition(self, allowed: str | tuple[str, ...], target: str) -> None:
        allowed_states = (allowed,) if isinstance(allowed, str) else allowed
        if self.state not in allowed_states:
            raise ProbeError(f"invalid state transition {self.state} -> {target}")
        self.state = target


def import_pe_sampler(log_text: str) -> dict[str, Any]:
    """Import cumulative existing PE sampler rows without claiming exact sites."""
    from symbolicate_xctrace_pe import parse_sampler_log, validate_sampler_coverage

    class TextSource:
        def read_text(self, **_kwargs: Any) -> str:
            return log_text

    report = parse_sampler_log(TextSource())
    if report is None:
        raise ProbeError("no PE sampler report found")
    errors = validate_sampler_coverage(report)
    if errors or not report.coverage or report.coverage.get("complete") != 1:
        raise ProbeError(f"incomplete PE sampler report: {errors}")
    if any(report.header.get(key, -1) != 0 for key in ("suspend_failures", "ctx_failures", "resume_failures")):
        raise ProbeError("PE sampler reports failed operations")
    omitted = report.samples - sum(value for _, value in report.modules)
    if omitted < 0:
        raise ProbeError("PE sampler module population exceeds total")
    modules = [dict(module=name, samples=count) for name, count in report.modules]
    modules.append({"module": "unreported-module-tail", "samples": omitted, "attributed": False})
    return {"schema": "dxmt9-pe-sampler-import-v1", "observation_kind": "sampled-guest-pc-population",
            "samples": report.samples, "frequency_hz": report.hz, "modules": modules,
            "native_guest_mapping": "unqualified", "address_hits": False,
            "exclusive_cpu": False, "static_manifest_proves_hits": False,
            "source_sha256": hashlib.sha256(log_text.encode()).hexdigest(),
            "qualification": "existing in-process PE sampler; guest PC population, not an external hit stream"}


def _main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    validate_cmd = commands.add_parser("validate", help="validate a probe manifest")
    validate_cmd.add_argument("manifest", type=Path)
    preflight_cmd = commands.add_parser("preflight", help="validate manifest and report adapter support")
    preflight_cmd.add_argument("manifest", type=Path)
    ring_cmd = commands.add_parser("decode-ring", help="decode a captured perf ring data area")
    ring_cmd.add_argument("input", type=Path, help="JSON with hex data and perf metadata")
    args = parser.parse_args()
    try:
        if args.command in ("validate", "preflight"):
            manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
            validate_manifest(manifest)
            output = {"valid": True, "schema": SCHEMA} if args.command == "validate" else preflight(manifest)
        else:
            source = json.loads(args.input.read_text(encoding="utf-8"))
            output = decode_perf_ring(data=bytes.fromhex(source["data_hex"]), data_head=source["data_head"],
                                      data_tail=source["data_tail"], sample_type=source["sample_type"],
                                      sample_regs_user=source.get("sample_regs_user", 0),
                                      abi_word_bytes=source.get("abi_word_bytes", 8))
        print(json.dumps(output, indent=2))
        return 0 if output.get("supported", True) else 3
    except (ProbeError, OSError, KeyError, TypeError, json.JSONDecodeError) as error:
        parser.exit(2, f"external probe input rejected: {error}\n")


if __name__ == "__main__":
    raise SystemExit(_main())
