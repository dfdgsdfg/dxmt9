#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import struct
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "scripts" / "tools" / "tracing_external.py"
SPEC = importlib.util.spec_from_file_location("tracing_external", MODULE_PATH)
external = importlib.util.module_from_spec(SPEC)
assert SPEC and SPEC.loader
sys.modules[SPEC.name] = external
SPEC.loader.exec_module(external)


def manifest(api: str = "perf_event_open", probe_type: str = "address-hit") -> dict:
    digest = "a" * 64
    probe = {"id": "p0", "type": probe_type}
    if probe_type in ("address-hit", "debugger-stop"):
        probe.update({"module_id": "app", "rva": 16, "instruction_bytes": "90",
                      "instruction_semantics": "single-byte nop; hit is post-instruction",
                      "abi": "sysv-x86_64", "registers": ["rax", "rdi"]})
    collection = {"api": api, "tool": "fixture", "tool_version": "1.0", "os": "Linux",
                  "architecture": "x86_64", "execution_mode": "native",
                  "required_capabilities": ["perf_event_open"]}
    if probe_type == "cpu-sample":
        collection["period"] = 100000
    return {"schema": external.SCHEMA,
            "target": {"module_sha256": digest, "architecture": "x86_64", "address_space": "native",
                       "address_convention": "module-rva"},
            "modules": [{"id": "app", "sha256": digest, "architecture": "x86_64",
                         "rea_provider": "ghidra", "rea_version": "12.1", "address_convention": "RVA"}],
            "probes": [probe], "collection": collection,
            "coverage": {"thread_selection": "all-existing", "new_thread_policy": "exclude-and-report",
                         "fork_policy": "exclude", "exec_policy": "stop-and-report"},
            "control": {"owner": "fixture", "deadline_ms": 1000,
                        "restore_policy": "restore-owned-only"}}


def record(record_type: int, payload: bytes) -> bytes:
    size = 8 + len(payload)
    return struct.pack("=IHH", record_type, 0, size) + payload


class ManifestTests(unittest.TestCase):
    def test_rejects_bad_schema_and_malformed_digest(self) -> None:
        value = manifest()
        value["schema"] = "future"
        with self.assertRaisesRegex(external.ProbeError, "unsupported manifest schema"):
            external.validate_manifest(value)
        value = manifest()
        value["modules"][0]["sha256"] = "abc"
        with self.assertRaisesRegex(external.ProbeError, "invalid SHA-256"):
            external.validate_manifest(value)

    def test_resolution_checks_loaded_hash_bytes_bounds_and_generation(self) -> None:
        value = manifest()
        site = external.resolve_site(value, "p0", module_sha256="a" * 64, loaded_base=0x100000,
                                     load_generation=7, image_size=32, instruction_bytes_at_rva=b"\x90")
        self.assertEqual(site["resolved_address"], 0x100010)
        self.assertEqual(site["load_generation"], 7)
        self.assertFalse(site["runtime_hits_established"])
        with self.assertRaisesRegex(external.ProbeError, "hash mismatch"):
            external.resolve_site(value, "p0", module_sha256="b" * 64, loaded_base=0,
                                  load_generation=8, image_size=32, instruction_bytes_at_rva=b"\x90")
        with self.assertRaisesRegex(external.ProbeError, "bytes mismatch"):
            external.resolve_site(value, "p0", module_sha256="a" * 64, loaded_base=0,
                                  load_generation=8, image_size=32, instruction_bytes_at_rva=b"\xcc")
        with self.assertRaisesRegex(external.ProbeError, "outside"):
            external.resolve_site(value, "p0", module_sha256="a" * 64, loaded_base=0,
                                  load_generation=8, image_size=16, instruction_bytes_at_rva=b"\x90")

    def test_linux_collector_is_rejected_on_macos_without_fallback(self) -> None:
        result = external.preflight(manifest(), system="Darwin", machine="arm64",
                                    capabilities={"perf_event_open"})
        self.assertFalse(result["supported"])
        self.assertEqual(result["reason"], "linux_api_on_non_linux_host")
        self.assertEqual(result["adapter"], "perf_event_open")

    def test_thread_installation_omission_and_slot_limit_fail_coverage(self) -> None:
        coverage = external.qualify_installation_coverage(
            requested_threads={10, 11}, requested_sites={"p0", "p1"},
            installations=[{"tid": 10, "probe_id": "p0", "installed": True}], hardware_slots=1)
        self.assertFalse(coverage["complete"])
        self.assertFalse(coverage["hit_count_claim_allowed"])
        self.assertTrue(coverage["over_budget"])
        self.assertEqual(len(coverage["failures"]), 3)


class PerfRingTests(unittest.TestCase):
    def test_wraparound_and_sample_register_mask(self) -> None:
        # PERF_SAMPLE_IP | TID | TIME | REGS_USER; ABI=1 then two u64 regs.
        sample_payload = struct.pack("=QIIQ", 0x1234, 10, 11, 99) + struct.pack("=QQQ", 1, 0xAA, 0xBB)
        sample = record(external.PERF_RECORD_SAMPLE, sample_payload)
        capacity = 64
        start = capacity - 5
        ring = bytearray(capacity)
        first = min(len(sample), capacity - start)
        ring[start:start + first] = sample[:first]
        ring[:len(sample) - first] = sample[first:]
        result = external.decode_perf_ring(data=ring, data_tail=start, data_head=start + len(sample),
                                           sample_type=1 | 2 | 4 | 4096, sample_regs_user=(1 << 1) | (1 << 5))
        item = result["records"][0]
        self.assertEqual(item["ip"], 0x1234)
        self.assertEqual(item["tid"], 11)
        self.assertEqual(item["time"], 99)
        self.assertEqual(item["valid_register_mask"], str((1 << 1) | (1 << 5)))
        self.assertEqual([r["value"] for r in item["registers"]], [0xAA, 0xBB])
        self.assertTrue(result["complete"])

    def test_loss_is_preserved_and_disqualifies_interval(self) -> None:
        payload = struct.pack("=QQ", 44, 3)
        raw = record(external.PERF_RECORD_LOST, payload)
        result = external.decode_perf_ring(data=raw + bytes(8), data_tail=0, data_head=len(raw), sample_type=0)
        self.assertEqual(result["lost_samples"], 3)
        self.assertFalse(result["complete"])
        self.assertEqual(result["records"][0]["event_id"], "44")

    def test_malformed_head_wrap_size_and_record_rejected(self) -> None:
        with self.assertRaisesRegex(external.ProbeError, "overrun"):
            external.decode_perf_ring(data=bytes(16), data_tail=0, data_head=17, sample_type=0)
        with self.assertRaisesRegex(external.ProbeError, "smaller than its header"):
            external.decode_perf_ring(data=struct.pack("=IHH", 9, 0, 7) + bytes(8),
                                      data_tail=0, data_head=16, sample_type=0)
        malformed = record(external.PERF_RECORD_SAMPLE, b"\x01")
        with self.assertRaisesRegex(external.ProbeError, "truncated perf sample ip"):
            external.decode_perf_ring(data=malformed + bytes(7), data_tail=0, data_head=len(malformed),
                                      sample_type=1)

    def test_sample_abi_with_trailing_fields_is_rejected(self) -> None:
        malformed = record(external.PERF_RECORD_SAMPLE, struct.pack("=QQ", 0x1000, 0xDEADBEEF))
        with self.assertRaisesRegex(external.ProbeError, "trailing bytes or mismatched ABI"):
            external.decode_perf_ring(data=malformed + bytes(8), data_tail=0, data_head=len(malformed),
                                      sample_type=1)


class LifecycleTests(unittest.TestCase):
    @staticmethod
    def ok(**extra):
        return {"ok": True, **extra}

    def test_bounded_lifecycle_receipt_and_invalid_order(self) -> None:
        session = external.ProbeSession(manifest(), "fixture")
        session.prepare({"supported": True})
        session.attach(lambda: self.ok(owned=True))
        session.arm(lambda: self.ok(installed=1))
        session.collect(lambda: self.ok(records=2))
        session.disable(lambda: self.ok(disabled=True))
        session.drain(lambda: self.ok(drained=2))
        session.detach(lambda: self.ok(detached=True))
        self.assertEqual(session.state, "detached")
        self.assertTrue(session.receipt["complete"])
        with self.assertRaisesRegex(external.ProbeError, "invalid state transition"):
            session.collect(lambda: self.ok())

    def test_failed_operation_reports_owned_state(self) -> None:
        session = external.ProbeSession(manifest(), "fixture")
        session.prepare({"supported": True})
        with self.assertRaisesRegex(RuntimeError, "attach failure"):
            session.attach(lambda: (_ for _ in ()).throw(RuntimeError("attach failure")))
        self.assertEqual(session.state, "failed")
        self.assertFalse(session.receipt["teardown_complete"])
        self.assertEqual(session.receipt["owned_state_remaining"], ["attachment"])
        cleanup = session.cleanup(disable=lambda: self.ok(), drain=lambda: self.ok(),
                                  detach=lambda: self.ok(detached=True))
        self.assertNotIn("disable", cleanup)
        self.assertNotIn("drain", cleanup)
        self.assertTrue(session.receipt["teardown_complete"])
        self.assertEqual(session.state, "detached")


if __name__ == "__main__":
    unittest.main()
