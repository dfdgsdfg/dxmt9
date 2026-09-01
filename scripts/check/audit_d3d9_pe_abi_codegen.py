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
    state_fragments = [line.strip() for line in impl.splitlines()
                       if "d3d9_pe_device_state_" in line]
    if state_fragments:
        fail("state implementation fragment include remains")
    impl_lines = len(impl.splitlines())
    impl_max_lines = manifest["layout"]["device_impl_max_lines"]
    if impl_lines > impl_max_lines:
        fail(
            "D3D9DeviceImpl declaration shell exceeds evidence residual: "
            f"{impl_lines} > {impl_max_lines} physical lines"
        )
    retired_hot = ROOT / "src/d3d9/d3d9_pe_device_hot.cpp"
    if retired_hot.exists() or '#include "d3d9_pe_device_hot.cpp"' in device:
        fail("hot implementation remains a disguised source fragment")
    device_cpp_sources = list((ROOT / "src/d3d9").glob("*.cpp"))
    device_cpp_text = "\n".join(source.read_text() for source in device_cpp_sources)
    if "DXMT9_PE_DEVICE_INLINE" in device_cpp_text or re.search(
        r"(?m)^[^\n;{}]*\binline\b[^\n;{}]*D3D9DeviceImpl::",
        device_cpp_text,
    ) or "__attribute__((used))" in device_cpp_text:
        fail("one-TU inline/used D3D9DeviceImpl workaround is forbidden")
    for entry in manifest["layout"]["retained_inline_entries"]:
        if not re.search(
            re.escape(entry) + r"\([^;]*\)\s*noexcept\s+override\s*\{",
            impl,
            re.S,
        ):
            fail(f"retained legal inline entry body missing: {entry}")
        if re.search(r"D3D9DeviceImpl::" + re.escape(entry) + r"\s*\(", device):
            fail(f"retained inline entry also has an out-of-line body: {entry}")
    for helper in manifest["layout"]["retained_inline_helpers"]:
        if helper not in impl:
            fail(f"retained inline helper body missing: {helper}")
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
    for arch in ("x64", "x86"):
        size = manifest["device_layout"][f"sizeof_{arch}"]
        alignment = manifest["device_layout"][f"alignof_{arch}"]
        if f"static_assert(sizeof(D3D9DeviceImpl) == {size});" not in device:
            fail(f"D3D9DeviceImpl {arch} sizeof pin is missing")
        if f"static_assert(alignof(D3D9DeviceImpl) == {alignment});" not in device:
            fail(f"D3D9DeviceImpl {arch} alignof pin is missing")
    for fast_symbol in manifest["hot"]["fast_path_symbols"]:
        if not re.search(
            re.escape(fast_symbol) + r"\(.*?validateConstRangeFast",
            device,
            re.S,
        ):
            fail(f"default-hot fast-path proof missing for {fast_symbol}")

    owner_markers = {
        "query_interface": "D3D9DeviceImpl::QueryInterface",
        "hot_state_draw": "D3D9DeviceImpl::Present",
        "recorder": "D3D9DeviceImpl::commitPendingCommandChunk",
        "com_cold": "D3D9DeviceImpl::TestCooperativeLevel",
        "state_block_prepare": "D3D9DeviceImpl::PrepareStateBlockApplyForChild",
        "state_block_commit": "D3D9DeviceImpl::CommitStateBlockApplyForChild",
        "fvf_resolver": "D3D9DeviceImpl::resolveImplicitDeclForFvf",
        "constant_validation": "D3D9DeviceImpl::validateConstRange",
        "constant_slow_bodies": "D3D9DeviceImpl::SetVertexShaderConstantFSlow",
        "diagnostics": "D3D9DeviceImpl::recordPeChunkCommit",
        "swvp": "D3D9DeviceImpl::trySoftwareFfpDrawPrimitive",
        "tape": "D3D9DeviceImpl::produceRenderTapeBootstrap",
        "tape_registry": "D3D9DeviceImpl::findRenderTapeObject",
        "tape_child": "D3D9DeviceImpl::NotifyRenderTapeObjectDefineForChild",
    }
    owners = {}
    for key, marker in owner_markers.items():
        relative = manifest["owners"].get(key)
        if not isinstance(relative, str):
            fail(f"manifest owner missing for {key}")
        text = (ROOT / relative).read_text()
        if marker not in text:
            fail(f"owner marker missing for {key}: {marker}")
        owners[key] = relative
    if manifest["owners"]["vtable"] != manifest["owners"]["query_interface"]:
        fail("manifest vtable owner is not the QueryInterface key-function TU")
    if manifest["owners"]["rtti"] != manifest["owners"]["query_interface"]:
        fail("manifest RTTI owner is not the QueryInterface key-function TU")
    query_definitions = sum(
        count_owner_definitions(source.read_text(), "QueryInterface")
        for source in device_cpp_sources
    )
    if query_definitions != 1:
        fail(f"expected one D3D9DeviceImpl QueryInterface definition, found {query_definitions}")

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
        "device_impl_lines": impl_lines,
        "device_impl_max_lines": impl_max_lines,
        "retained_inline_entries": manifest["layout"]["retained_inline_entries"],
        "retained_inline_helpers": manifest["layout"]["retained_inline_helpers"],
        "heavy_header_aggregate_lines": impl_lines * len(includers),
        "owner_map": owners,
        "hot_metrics": hot_metrics,
        "source_contract": "pass",
    }


def run_tool(args: list[str]) -> str:
    try:
        return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT)
    except (OSError, subprocess.CalledProcessError) as exc:
        fail(f"audit tool failed: {' '.join(args)}: {exc}")


ARCH_MACHINES = {
    "x64": {"system": "windows", "cpu_family": "x86_64",
            "cpu": "x86_64", "is_64_bit": True},
    "x86": {"system": "windows", "cpu_family": "x86",
            "cpu": "i686", "is_64_bit": False},
}

COMPILER_IDENTITY_KEYS = (
    "id", "version", "full_version", "exelist", "linker_id",
    "linker_exelist",
)


def read_json(path: Path, label: str) -> object:
    if not path.exists():
        fail(f"missing {label}: {path}")
    try:
        return json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        fail(f"invalid {label}: {path}: {exc}")


def validate_arch_identity(machines: dict, arch: str, label: str) -> dict:
    expected = ARCH_MACHINES[arch]
    actual = {}
    for role in ("host", "target"):
        machine = machines.get(role)
        if not isinstance(machine, dict):
            fail(f"{label} has no {role} machine identity")
        actual[role] = {key: machine.get(key) for key in expected}
        if actual[role] != expected:
            fail(
                f"{label} architecture does not match --arch {arch}: "
                f"{role}={actual[role]!r}, expected={expected!r}"
            )
    return actual


def compiler_identity(compilers: dict, label: str) -> dict:
    host = compilers.get("host")
    if not isinstance(host, dict):
        fail(f"{label} has no host compiler identity")
    identity = {}
    for language in ("c", "cpp"):
        compiler = host.get(language)
        if not isinstance(compiler, dict):
            fail(f"{label} has no host {language} compiler identity")
        missing = [key for key in COMPILER_IDENTITY_KEYS if key not in compiler]
        if missing:
            fail(f"{label} host {language} compiler identity misses {missing}")
        identity[language] = {
            key: compiler[key] for key in COMPILER_IDENTITY_KEYS
        }
    return identity


def artifact_identity(manifest: dict, build_dir: Path, arch: str,
                      label: str) -> dict:
    options = build_dir / "meson-info/intro-buildoptions.json"
    config = read_json(options, f"{label} build identity")
    if not isinstance(config, list) or not all(
        isinstance(entry, dict) and "name" in entry and "value" in entry
        for entry in config
    ):
        fail(f"invalid {label} build identity shape")
    values = {entry["name"]: entry["value"] for entry in config}
    expected_builtin = manifest["toolchain"]["builtin_dll"]
    expected_configuration = manifest["toolchain"]["configuration"]
    if values.get("wine_builtin_dll") is not expected_builtin:
        fail(
            f"{label} PE artifact builtin identity mismatch: "
            f"{values.get('wine_builtin_dll')!r}"
        )
    if values.get("buildtype") != expected_configuration:
        fail(
            f"{label} PE artifact configuration mismatch: "
            f"{values.get('buildtype')!r}"
        )
    machines = read_json(
        build_dir / "meson-info/intro-machines.json",
        f"{label} machine identity",
    )
    machine_identity = validate_arch_identity(machines, arch, label)
    compilers = read_json(
        build_dir / "meson-info/intro-compilers.json",
        f"{label} compiler identity",
    )
    compiler = compiler_identity(compilers, label)
    expected_compiler_id = manifest["toolchain"]["compiler_id"]
    expected_linker_id = manifest["toolchain"]["linker_id"]
    for language, identity in compiler.items():
        if identity["id"] != expected_compiler_id:
            fail(f"{label} host {language} compiler is not {expected_compiler_id}")
        if identity["linker_id"] != expected_linker_id:
            fail(f"{label} host {language} linker is not {expected_linker_id}")
        if manifest["toolchain"]["compiler_family"] == "llvm-mingw" and not all(
            "w64-mingw32" in executable for executable in identity["exelist"]
        ):
            fail(f"{label} host {language} compiler is not llvm-mingw")
    return {
        "buildtype": values.get("buildtype"),
        "wine_builtin_dll": values.get("wine_builtin_dll"),
        "machines": machine_identity,
        "compilers": compiler,
    }


def require_matching_artifact_identities(candidate: dict,
                                         baseline: dict) -> None:
    if candidate != baseline:
        fail("candidate/baseline PE build identities differ")


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
    # The promoted all-family owner has one non-template append envelope.
    "appendRecord": "ZN14D3D9DeviceImpl12appendRecordE",
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
        if len(matches) != 1:
            fail(f"ambiguous hot symbol for {label}: {[row[1] for row in matches]}")
        selected[label] = matches[0][1]
    return selected


LINKER_FILL_MNEMONICS = {"int3", "nop", "nopl", "nopw"}


def disassembly_text_metrics(text: str, symbol: str) -> dict:
    rows = []
    instruction_re = re.compile(
        r"^\s*[0-9A-Fa-f]+:\s+((?:[0-9A-Fa-f]{2}\s+)+)(.*)$"
    )
    for line in text.splitlines():
        match = instruction_re.match(line)
        if not match:
            continue
        mnemonic = match.group(2).lstrip().split(None, 1)[0]
        rows.append({
            "bytes": len(re.findall(r"[0-9A-Fa-f]{2}", match.group(1))),
            "mnemonic": mnemonic,
            "assembly": match.group(2),
        })
    # llvm-objdump disassembles through the next symbol boundary. COFF/lld
    # fills only the trailing gap with INT3/NOP families. Trim that suffix
    # from the byte metric, but keep identical mnemonics when they occur
    # inside the function so a real code-generation change cannot hide.
    byte_rows = list(rows)
    while byte_rows and byte_rows[-1]["mnemonic"] in LINKER_FILL_MNEMONICS:
        byte_rows.pop()
    byte_count = sum(row["bytes"] for row in byte_rows)
    instruction_count = sum(
        row["mnemonic"] not in LINKER_FILL_MNEMONICS for row in rows
    )
    direct_call_count = sum(
        bool(re.search(r"\bcall\w*\s+(?!\*)", row["assembly"]))
        for row in rows
    )
    if instruction_count == 0 or byte_count == 0:
        fail(f"disassembly produced no instructions for {symbol}")
    return {
        "bytes": byte_count,
        "instructions": instruction_count,
        "direct_calls": direct_call_count,
    }


def disassembly_metrics(dll: Path, symbol: str) -> dict:
    text = run_tool(["llvm-objdump", f"--disassemble-symbols={symbol}", str(dll)])
    return disassembly_text_metrics(text, symbol)


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


def require_zero_hot_metrics(metrics: list[dict]) -> None:
    for row in metrics:
        if any(value != 0 for value in row["delta"].values()):
            fail(f"hot codegen delta is nonzero: {row['symbol']}: {row['delta']}")


def object_owner(line: str) -> str:
    match = re.search(r":([^:\s]+\.cpp\.obj):\s", line)
    if not match:
        fail(f"cannot parse PE archive owner: {line}")
    return Path(match.group(1)).name


def expected_object_owner(source: str) -> str:
    return Path(source).name + ".obj"


def archive_owner_text_check(manifest: dict, text: str) -> dict:
    real_markers = {
        "query_interface": "ZN14D3D9DeviceImpl14QueryInterface",
        "vtable": "ZTV14D3D9DeviceImpl",
        "rtti": "ZTI14D3D9DeviceImpl",
    }
    owners = {}
    for key, marker in real_markers.items():
        lines = []
        for line in text.splitlines():
            symbol = line.rsplit(None, 1)[-1] if line.split() else ""
            if ".refptr." in symbol:
                continue
            normalized = symbol.lstrip("_").split("@", 1)[0]
            if normalized.startswith(marker):
                lines.append(line)
        if len(lines) != 1:
            fail(f"expected exactly one real {key} owner, found {len(lines)}")
        owner = object_owner(lines[0])
        expected = expected_object_owner(manifest["owners"][key])
        if owner != expected:
            fail(f"{key} owner is {owner}, expected {expected}")
        owners[key] = owner

    refptr_marker = "ZTV14D3D9DeviceImpl"
    actual_refptr_owners = sorted({
        object_owner(line)
        for line in text.splitlines()
        if ".refptr." in line and refptr_marker in line
    })
    expected_refptr_owners = sorted(
        expected_object_owner(source)
        for source in manifest["owners"]["vtable_refptr"]
    )
    if actual_refptr_owners != expected_refptr_owners:
        fail(
            "D3D9DeviceImpl vtable .refptr owners changed: "
            f"{actual_refptr_owners!r} != {expected_refptr_owners!r}"
        )
    owners["vtable_refptr"] = actual_refptr_owners
    return owners


def archive_owner_check(manifest: dict, build_dir: Path) -> dict:
    archive = build_dir / "src/d3d9/libdxmt9_pe_core.a"
    if not archive.exists():
        fail(f"missing PE core archive: {archive}")
    text = run_tool(["llvm-nm", "--defined-only", "--print-file-name", str(archive)])
    return archive_owner_text_check(manifest, text)


def artifact_audit(manifest: dict, build_dir: Path, baseline_dir: Path, arch: str) -> dict:
    candidate_identity = artifact_identity(
        manifest, build_dir, arch, "candidate")
    baseline_identity = artifact_identity(
        manifest, baseline_dir, arch, "baseline")
    require_matching_artifact_identities(candidate_identity, baseline_identity)
    candidate = artifact_dll(build_dir)
    baseline = artifact_dll(baseline_dir)
    metrics = measured_hot_metrics(manifest, candidate, baseline)
    by_symbol = {row["symbol"]: row for row in metrics}
    require_zero_hot_metrics(metrics)
    fast_proof = []
    for symbol in manifest["hot"]["fast_path_symbols"]:
        row = by_symbol[symbol]
        fast_proof.append({"symbol": symbol, "delta": row["delta"],
                           "status": "zero-delta"})
    return {
        "arch": arch,
        "build_dir": str(build_dir),
        "baseline_dir": str(baseline_dir),
        "builtin": True,
        "identity": candidate_identity,
        "export_check": export_pairs(manifest, candidate),
        "abi_owner_check": archive_owner_check(manifest, build_dir),
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
