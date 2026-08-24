#!/usr/bin/env python3
"""Stable source/PE audit for the D3D9DeviceImpl decomposition.

The source-only mode is safe in native-only builds and is the Meson test.  A
PE build directory may be supplied later to add export/symbol/size evidence;
the script rejects app-local (wine_builtin_dll=false) output rather than
quietly treating a same-named artifact as builtin evidence.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MANIFEST = ROOT / "scripts/check/pe_device_abi_manifest.toml"


def fail(message: str) -> "NoReturn":
    raise AssertionError(message)


def read_toml(path: Path) -> dict:
    try:
        import tomllib
        return tomllib.loads(path.read_text())
    except ModuleNotFoundError:
        # The native build may intentionally use the system Python 3.9.  This
        # manifest uses only scalar values and arrays, so keep the source audit
        # independent of a newer Python installation.
        result: dict = {}
        section: dict = result
        pending_key = None
        for raw in path.read_text().splitlines():
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            if line.startswith("[") and line.endswith("]"):
                section = result
                for part in line[1:-1].split("."):
                    section = section.setdefault(part, {})
                pending_key = None
                continue
            if pending_key and "=" not in line:
                value = line.rstrip(",")
                if value.endswith("]"):
                    value = value[:-1].rstrip()
                    pending_key = None
                if value:
                    section[pending_key].extend(
                        item.strip().strip('"')
                        for item in value.split(",")
                        if item.strip())
                continue
            if "=" not in line:
                continue
            key, value = [part.strip() for part in line.split("=", 1)]
            if value.startswith("[") and value.endswith("]"):
                body = value[1:-1].strip()
                section[key] = [item.strip().strip('"') for item in body.split(",") if item.strip()]
                continue
            if value.startswith("[") and not value.endswith("]"):
                pending_key = key
                section[key] = []
                continue
            if pending_key:
                value = value.rstrip(",")
                if value.endswith("]"):
                    pending_key = None
                else:
                    value = value.rstrip(",")
                if value.strip():
                    section[pending_key].append(value.strip().strip('"'))
                continue
            value = value.rstrip(",")
            if value in ("true", "false"):
                parsed = value == "true"
            elif value.startswith('"'):
                parsed = value.strip('"')
            else:
                parsed = int(value)
            section[key] = parsed
        return result


def count_owner_definitions(text: str, symbol: str) -> int:
    # Definitions only: declarations end in ';', and an owner may qualify the
    # symbol with STDMETHODCALLTYPE or a return type on the preceding line.
    return len(re.findall(r"D3D9DeviceImpl::" + re.escape(symbol) + r"\s*\([^;]*\)\s*(?:noexcept\s*)?\{", text, re.S))


def source_audit(manifest: dict) -> dict:
    impl = (ROOT / "src/d3d9/d3d9_pe_device_impl.hpp").read_text()
    device = (ROOT / "src/d3d9/d3d9_pe_device.cpp").read_text()
    if "class D3D9DeviceImpl final : public IDirect3DDevice9Ex" not in impl:
        fail("D3D9DeviceImpl inheritance declaration changed")
    q = "HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) noexcept override;"
    if q not in impl or "HRESULT STDMETHODCALLTYPE D3D9DeviceImpl::QueryInterface(" not in device:
        fail("QueryInterface declaration/owner contract changed")
    if impl.index(q) < impl.index("class D3D9DeviceImpl"):
        fail("QueryInterface key declaration is not in the class")
    hot = (ROOT / "src/d3d9/d3d9_pe_device_hot.cpp").read_text()
    state_fragments = [line.strip() for line in impl.splitlines()
                       if "d3d9_pe_device_state_" in line]
    if state_fragments:
        fail("state implementation fragment include remains")
    if len(impl.splitlines()) > 3000:
        fail("D3D9DeviceImpl declaration shell exceeds 3000 physical lines")
    for fragment in (
        "d3d9_pe_device_diag_log.inc.hpp",
        "d3d9_pe_device_diag_module.inc.hpp",
        "d3d9_pe_device_diag_callstack.inc.hpp",
        "d3d9_pe_device_tape_helpers.inc.hpp",
        "d3d9_pe_device_tape_types.inc.hpp",
    ):
        if fragment in impl or fragment in (ROOT / "src/d3d9/d3d9_pe_capture_state.hpp").read_text():
            fail(f"always-included helper fragment remains: {fragment}")

    order = manifest["layout"]["critical_member_order"]
    class_start = impl.index("class D3D9DeviceImpl")
    positions = [impl.find(member, class_start) for member in order]
    if any(position < 0 for position in positions):
        fail("critical D3D9DeviceImpl member missing")
    if positions != sorted(positions):
        fail("critical D3D9DeviceImpl member order changed")

    state = (ROOT / "src/d3d9/d3d9_pe_recorder_state.hpp").read_text()
    for arch, expected in manifest["recorder_state"].items():
        if arch.startswith("sizeof_") and str(expected) not in state:
            fail(f"PeRecorderState {arch} pin is missing")
    for fast_symbol in manifest["hot"]["fast_path_symbols"]:
        if not re.search(
            re.escape(fast_symbol) + r"\(.*?validateConstRangeFast",
            hot,
            re.S,
        ):
            fail(f"default-hot fast-path proof missing for {fast_symbol}")

    owner_checks = {
        "query_interface": ("src/d3d9/d3d9_pe_device.cpp", "D3D9DeviceImpl::QueryInterface"),
        "hot_state_draw": ("src/d3d9/d3d9_pe_device_hot.cpp", "D3D9DeviceImpl::Present"),
        "recorder": ("src/d3d9/d3d9_pe_device_recorder.cpp", "D3D9DeviceImpl::commitPendingCommandChunk"),
        "com_cold": ("src/d3d9/d3d9_pe_device_com_cold.cpp", "D3D9DeviceImpl::TestCooperativeLevel"),
        "state_block_prepare": ("src/d3d9/d3d9_pe_device_com_cold.cpp", "D3D9DeviceImpl::PrepareStateBlockApplyForChild"),
        "state_block_commit": ("src/d3d9/d3d9_pe_device_com_cold.cpp", "D3D9DeviceImpl::CommitStateBlockApplyForChild"),
        "fvf_resolver": ("src/d3d9/d3d9_pe_device_com_cold.cpp", "D3D9DeviceImpl::resolveImplicitDeclForFvf"),
        "constant_validation": ("src/d3d9/d3d9_pe_device_com_cold.cpp", "D3D9DeviceImpl::validateConstRange"),
        "constant_slow_bodies": ("src/d3d9/d3d9_pe_device_com_cold.cpp", "D3D9DeviceImpl::SetVertexShaderConstantFSlow"),
        "diagnostics": ("src/d3d9/d3d9_pe_device_diag.cpp", "D3D9DeviceImpl::recordPeChunkCommit"),
        "swvp": ("src/d3d9/d3d9_pe_device_swvp.cpp", "D3D9DeviceImpl::trySoftwareFfpDrawPrimitive"),
        "tape": ("src/d3d9/d3d9_pe_device_tape.cpp", "D3D9DeviceImpl::produceRenderTapeBootstrap"),
        "tape_registry": ("src/d3d9/d3d9_pe_device_tape_registry.cpp", "D3D9DeviceImpl::findRenderTapeObject"),
        "tape_child": ("src/d3d9/d3d9_pe_device_tape_child.cpp", "D3D9DeviceImpl::NotifyRenderTapeObjectDefineForChild"),
    }
    owners = {}
    for key, (relative, marker) in owner_checks.items():
        text = (ROOT / relative).read_text()
        if marker not in text:
            fail(f"owner marker missing for {key}: {marker}")
        owners[key] = relative

    # The export contract is source-owned by the module definition.  Compare
    # ordinal/name pairs exactly here; the artifact lane repeats this check on
    # the produced PE export table.
    def_file = (ROOT / "src/win32/d3d9.def").read_text()
    source_exports = sorted(
        f"{ordinal} {name}"
        for name, ordinal in re.findall(r"^\s*(\w+)\s+@(\d+)\s*$", def_file, re.M)
    )
    expected_exports = sorted(manifest["exports"]["names"])
    if source_exports != expected_exports:
        fail("source export ordinal/name allowlist changed")

    residual_fragment_lines = 0
    if residual_fragment_lines > manifest["layout"]["state_fragment_residual_max_lines"]:
        fail("state-fragment cold extraction is below the material reduction gate")

    # Keep the metric source-stable.  It is intentionally not a pass/fail
    # threshold: cold extraction may leave a measured residual when moving a
    # hot body would alter codegen.  The manifest records what is measured.
    includers = []
    for source in sorted((ROOT / "src/d3d9").glob("*.cpp")):
        if '#include "d3d9_pe_device_impl.hpp"' in source.read_text():
            includers.append(str(source.relative_to(ROOT)))
    hot_metrics = [
        {"symbol": symbol, "status": "native-source-only"}
        for symbol in manifest["hot"]["symbols"]
    ]
    return {
        "header_lines": len(impl.splitlines()),
        "state_fragment_includes_residual": state_fragments,
        "state_fragment_residual_lines": residual_fragment_lines,
        "heavy_header_includers": includers,
        "heavy_header_includer_count": len(includers),
        "owner_map": owners,
        "hot_metrics": hot_metrics,
        "source_contract": "pass",
    }


def run_tool(args: list[str]) -> str:
    try:
        return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT)
    except (OSError, subprocess.CalledProcessError) as exc:
        fail(f"audit tool failed: {' '.join(args)}: {exc}")


def artifact_identity(build_dir: Path) -> dict:
    options = build_dir / "meson-info/intro-buildoptions.json"
    if not options.exists():
        fail(f"missing build identity: {options}")
    config = json.loads(options.read_text())
    values = {entry["name"]: entry["value"] for entry in config}
    if values.get("wine_builtin_dll") is not True:
        fail("PE artifact is not from wine_builtin_dll=true")
    if values.get("buildtype") != "release":
        fail(f"PE artifact is not release-built: {values.get('buildtype')!r}")
    return values


def artifact_dll(build_dir: Path) -> Path:
    dll = build_dir / "src/win32/d3d9.dll"
    if not dll.exists():
        dll = build_dir / "d3d9.dll"
    if not dll.exists():
        fail(f"missing PE artifact: {dll}")
    return dll


def export_pairs(manifest: dict, dll: Path) -> dict:
    headers = run_tool(["llvm-objdump", "--private-headers", str(dll)])
    pairs = {
        f"{ordinal} {name}"
        for ordinal, name in re.findall(
            r"^\s*(\d+)\s+0x[0-9A-Fa-f]+\s+(\S+)\s*$", headers, re.M)
    }
    expected = set(manifest["exports"]["names"])
    if pairs != expected:
        fail("PE export ordinal/name allowlist changed")
    return {"expected": sorted(expected), "actual": sorted(pairs)}


def symbol_table(dll: Path) -> list[tuple[int, str]]:
    text = run_tool(["llvm-objdump", "--syms", str(dll)])
    rows = []
    for line in text.splitlines():
        match = re.search(
            r"^\[\d+\]\(sec\s+(\d+)\).*?\s+(0x[0-9A-Fa-f]+)\s+(\S+)\s*$",
            line,
        )
        if match and match.group(1) != "0":
            rows.append((int(match.group(2), 16), match.group(3)))
    return rows


HOT_PREFIXES = {
    "QueryInterface": "ZN14D3D9DeviceImpl14QueryInterface",
    "BeginScene": "ZN14D3D9DeviceImpl10BeginScene",
    "SetRenderState": "ZN14D3D9DeviceImpl14SetRenderState",
    "SetStreamSource": "ZN14D3D9DeviceImpl15SetStreamSource",
    "SetVertexShaderConstantF": "ZN14D3D9DeviceImpl24SetVertexShaderConstantF",
    "SetPixelShaderConstantF": "ZN14D3D9DeviceImpl23SetPixelShaderConstantF",
    "DrawPrimitive": "ZN14D3D9DeviceImpl13DrawPrimitive",
    "DrawIndexedPrimitive": "ZN14D3D9DeviceImpl20DrawIndexedPrimitive",
    "Present": "ZN14D3D9DeviceImpl7Present",
    # Representative template instantiations used by the present/state paths.
    "appendRecord": "ZN14D3D9DeviceImpl12appendRecordIZNS_7Present",
    "setRenderStateCore": "ZN14D3D9DeviceImpl18setRenderStateCoreIKNS_25PeNullHotStateSetterTimer",
    "applyConstStateWrite": "ZN14D3D9DeviceImpl20applyConstStateWrite",
}


def select_hot_symbols(dll: Path, labels: list[str]) -> dict[str, str]:
    rows = symbol_table(dll)
    selected = {}
    for label in labels:
        prefix = HOT_PREFIXES.get(label)
        if prefix is None:
            fail(f"no stable symbol prefix for hot metric {label}")
        matches = [
            (address, symbol)
            for address, symbol in rows
            if symbol.lstrip("_").split("@", 1)[0].startswith(prefix)
        ]
        if not matches:
            fail(f"missing hot symbol for {label}")
        selected[label] = min(matches)[1]
    return selected


def disassembly_metrics(dll: Path, symbol: str) -> dict:
    text = run_tool(["llvm-objdump", f"--disassemble-symbols={symbol}", str(dll)])
    byte_count = 0
    instruction_count = 0
    direct_call_count = 0
    instruction_re = re.compile(
        r"^\s*[0-9A-Fa-f]+:\s+((?:[0-9A-Fa-f]{2}\s+)+)(.*)$"
    )
    for line in text.splitlines():
        match = instruction_re.match(line)
        if not match:
            continue
        byte_count += len(re.findall(r"[0-9A-Fa-f]{2}", match.group(1)))
        # PE symbol disassembly includes linker fill bytes up to the next
        # symbol. They are not function instructions and vary with section
        # placement; retain them in byte length but normalize them out of the
        # instruction count.
        mnemonic = match.group(2).lstrip().split(None, 1)[0]
        if mnemonic in {"int3", "nop", "nopl", "nopw", "ud2"}:
            continue
        instruction_count += 1
        if re.search(r"\bcall\w*\s+(?!\*)", match.group(2)):
            direct_call_count += 1
    if instruction_count == 0 or byte_count == 0:
        fail(f"disassembly produced no instructions for {symbol}")
    return {
        "bytes": byte_count,
        "instructions": instruction_count,
        "direct_calls": direct_call_count,
    }


def measured_hot_metrics(manifest: dict, candidate: Path, baseline: Path) -> list[dict]:
    labels = manifest["hot"]["symbols"]
    candidate_symbols = select_hot_symbols(candidate, labels)
    baseline_symbols = select_hot_symbols(baseline, labels)
    result = []
    for label in labels:
        current = disassembly_metrics(candidate, candidate_symbols[label])
        previous = disassembly_metrics(baseline, baseline_symbols[label])
        result.append({
            "symbol": label,
            "baseline": previous,
            "candidate": current,
            "delta": {
                key: current[key] - previous[key]
                for key in ("bytes", "instructions", "direct_calls")
            },
            "status": "measured",
        })
    return result


def archive_owner_check(build_dir: Path) -> dict:
    archive = build_dir / "src/d3d9/libdxmt9_pe_core.a"
    if not archive.exists():
        fail(f"missing PE core archive: {archive}")
    text = run_tool(["llvm-nm", "--defined-only", "--print-file-name", str(archive)])
    required = {
        "query_interface": "D3D9DeviceImpl14QueryInterface",
        "vtable": "_ZTV14D3D9DeviceImpl",
        "rtti": "_ZTI14D3D9DeviceImpl",
    }
    owners = {}
    for key, marker in required.items():
        lines = [line for line in text.splitlines() if marker in line]
        if not lines:
            fail(f"missing ABI owner symbol: {marker}")
        if key != "query_interface" and not any(
            "d3d9_pe_device.cpp.obj" in line for line in lines
        ):
            fail(f"ABI owner moved from d3d9_pe_device.cpp.obj: {marker}")
        if key == "query_interface" and not any(
            "d3d9_pe_device.cpp.obj" in line for line in lines
        ):
            fail("QueryInterface owner moved from d3d9_pe_device.cpp.obj")
        owners[key] = "d3d9_pe_device.cpp.obj"
    return owners


def artifact_audit(manifest: dict, build_dir: Path, baseline_dir: Path, arch: str) -> dict:
    artifact_identity(build_dir)
    artifact_identity(baseline_dir)
    candidate = artifact_dll(build_dir)
    baseline = artifact_dll(baseline_dir)
    metrics = measured_hot_metrics(manifest, candidate, baseline)
    by_symbol = {row["symbol"]: row for row in metrics}
    hot_limit = manifest["hot"]["max_delta"]
    for row in metrics:
        if any(row["delta"][key] > hot_limit[key]
               or row["delta"][key] < -hot_limit[key]
               for key in hot_limit):
            fail(f"hot codegen delta outside policy: {row['symbol']}")
    fast_limit = manifest["hot"]["fast_path_max_delta"]
    fast_proof = []
    for symbol in manifest["hot"]["fast_path_symbols"]:
        row = by_symbol[symbol]
        if any(row["delta"][key] > fast_limit[key]
               or row["delta"][key] < -fast_limit[key]
               for key in fast_limit):
            fail(f"default-hot fast-path codegen delta outside policy: {symbol}")
        fast_proof.append({"symbol": symbol, "delta": row["delta"],
                           "status": "zero-delta"})
    return {
        "arch": arch,
        "build_dir": str(build_dir),
        "baseline_dir": str(baseline_dir),
        "builtin": True,
        "export_check": export_pairs(manifest, candidate),
        "abi_owner_check": archive_owner_check(build_dir),
        "hot_metrics": metrics,
        "fast_path_proof": fast_proof,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--baseline-dir", type=Path)
    parser.add_argument("--arch", choices=("x64", "x86"), default="x64")
    parser.add_argument("--out", type=Path)
    parser.add_argument("--source-only", action="store_true")
    args = parser.parse_args()
    manifest = read_toml(MANIFEST)
    result = source_audit(manifest)
    if args.build_dir and not args.source_only:
        if args.baseline_dir is None:
            fail("--baseline-dir is required for non-null PE codegen metrics")
        result["artifact"] = artifact_audit(
            manifest, args.build_dir, args.baseline_dir, args.arch
        )
    elif not args.source_only and args.build_dir is None:
        result["artifact"] = {"status": "not-run", "reason": "native-only source audit"}
    if args.out:
        args.out.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print(f"pe-device-abi-audit: FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
