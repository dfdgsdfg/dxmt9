#!/usr/bin/env python3
"""Negative fixtures for the PE ABI/codegen audit's pure checks."""

from __future__ import annotations

import importlib.util
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
AUDIT_PATH = ROOT / "scripts/check/audit_d3d9_pe_abi_codegen.py"
SPEC = importlib.util.spec_from_file_location("pe_abi_audit", AUDIT_PATH)
assert SPEC and SPEC.loader
AUDIT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(AUDIT)


def expect_failure(label: str, operation) -> None:
    try:
        operation()
    except AssertionError:
        return
    raise AssertionError(f"negative fixture unexpectedly passed: {label}")


x86_machine = {
    "system": "windows",
    "cpu_family": "x86",
    "cpu": "i686",
    "is_64_bit": False,
}
expect_failure(
    "x86 artifact asserted as x64",
    lambda: AUDIT.validate_arch_identity(
        {"host": x86_machine, "target": x86_machine}, "x64", "fixture"),
)

candidate_identity = {
    "buildtype": "release",
    "wine_builtin_dll": True,
    "machines": {"host": x86_machine, "target": x86_machine},
    "compilers": {"cpp": {"full_version": "clang fixture A"}},
}
baseline_identity = {
    **candidate_identity,
    "compilers": {"cpp": {"full_version": "clang fixture B"}},
}
expect_failure(
    "compiler/config identity mismatch",
    lambda: AUDIT.require_matching_artifact_identities(
        candidate_identity, baseline_identity),
)

expect_failure(
    "nonzero hot metric",
    lambda: AUDIT.require_zero_hot_metrics([{
        "symbol": "Present",
        "delta": {"bytes": 1, "instructions": 0, "direct_calls": 0},
    }]),
)

metrics = AUDIT.disassembly_text_metrics(
    """
1000: 90                            nop
1001: 55                            pushq %rbp
1002: 90                            nop
1003: cc                            int3
""",
    "fixture",
)
assert metrics["bytes"] == 2, metrics
assert metrics["instructions"] == 1, metrics

manifest = {
    "owners": {
        "query_interface": "src/d3d9/d3d9_pe_device.cpp",
        "vtable": "src/d3d9/d3d9_pe_device.cpp",
        "rtti": "src/d3d9/d3d9_pe_device.cpp",
        "vtable_refptr": ["src/d3d9/d3d9_pe_device_com_cold.cpp"],
    },
}
owner_text = "\n".join((
    "lib.a:d3d9_pe_device.cpp.obj: 0 T __ZN14D3D9DeviceImpl14QueryInterface",
    "lib.a:d3d9_pe_device.cpp.obj: 0 D __ZTV14D3D9DeviceImpl",
    "lib.a:d3d9_pe_device.cpp.obj: 0 D __ZTI14D3D9DeviceImpl",
    "lib.a:d3d9_pe_device_com_cold.cpp.obj: 0 D __.refptr.__ZTV14D3D9DeviceImpl",
    "lib.a:d3d9_pe_device_diag.cpp.obj: 0 D __.refptr.__ZTV14D3D9DeviceImpl",
))
expect_failure(
    "unexpected vtable refptr owner",
    lambda: AUDIT.archive_owner_text_check(manifest, owner_text),
)

print("pe-abi-codegen-audit-spec: PASS")
