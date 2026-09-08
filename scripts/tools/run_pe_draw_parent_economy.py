#!/usr/bin/env python3
"""Run and qualify one public-COM draw-parent matrix through Wine."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from scripts.wine.resolve import load_manifest, validate_wsi_spawn
from summarize_pe_draw_parent_economy import parse_text

WINE_MANIFEST = ROOT / "experiments/wine/manifest.toml"
BUILDS = ("build", "build-win32-x86-builtin", "build-win32-x64-builtin", "build-x86_64-builtin")
SOURCE_ROOTS = ("src", "include", "cross")


def canonical_images() -> dict[str, Path]:
    return {
        "i386-d3d9": ROOT / "build-win32-x86-builtin/src/win32/d3d9.dll",
        "x86_64-d3d9": ROOT / "build-win32-x64-builtin/src/win32/d3d9.dll",
        "i386-winemetal": ROOT / "build-win32-x86-builtin/src/winemetal/winemetal_dxmt9.dll",
        "x86_64-winemetal": ROOT / "build-win32-x64-builtin/src/winemetal/winemetal_dxmt9.dll",
        "unix-winemetal": ROOT / "build-x86_64-builtin/src/winemetal/unix/winemetal_dxmt9.so",
    }


def resolve_wine(wine_id: str, wine_root: Path | None):
    entries = load_manifest(WINE_MANIFEST)
    for entry in entries:
        if entry.id == wine_id:
            if wine_root is not None and wine_root.resolve() != entry.path.resolve():
                raise ValueError("Wine root override does not match the selected manifest identity")
            validate_wsi_spawn(entry)
            return entry
    raise ValueError("canonical Wine manifest entry is unavailable")


def pe_public_binding(path: Path, arch: str) -> dict[str, int]:
    """Resolve COM slots 81/82 against the selected PE image and symbols."""
    data = path.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    machine = struct.unpack_from("<H", data, pe + 4)[0]
    expected_machine = {"x86": 0x14C, "x64": 0x8664}[arch]
    if machine != expected_machine or data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("selected d3d9 image has wrong executable machine")
    width = 4 if arch == "x86" else 8
    image_base = struct.unpack_from("<I" if width == 4 else "<Q", data,
                                    pe + 24 + (28 if width == 4 else 24))[0]
    section_count = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]

    def section_offset(rva: int, size: int, executable: bool = False) -> int:
        for index in range(section_count):
            at = pe + 24 + optional_size + index * 40
            virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from("<IIII", data, at + 8)
            flags = struct.unpack_from("<I", data, at + 36)[0]
            if virtual_address <= rva and rva + size <= virtual_address + raw_size:
                offset = raw_offset + rva - virtual_address
                if offset + size > len(data) or (executable and not flags & 0x20000000):
                    raise ValueError("invalid or non-executable PE range")
                return offset
        raise ValueError("RVA is outside backed PE sections")

    def read_rva(rva: int) -> int:
        offset = section_offset(rva, width)
        return int.from_bytes(data[offset:offset + width], "little")

    nm = shutil.which("llvm-nm")
    if nm is None:
        raise ValueError("llvm-nm is required for public binding qualification")
    lines = subprocess.check_output([nm, "--defined-only", str(path)], text=True).splitlines()

    def symbols(pattern: str) -> list[int]:
        return [int(fields[0], 16) for line in lines
                if len(fields := line.split()) == 3 and re.fullmatch(pattern, fields[2])]

    table = symbols(r"_+ZTV14D3D9DeviceImpl")
    primitive = symbols(r"_+ZN14D3D9DeviceImpl13DrawPrimitiveE17_D3DPRIMITIVETYPEjj(?:@16)?")
    indexed = symbols(r"_+ZN14D3D9DeviceImpl20DrawIndexedPrimitiveE17_D3DPRIMITIVETYPEijjjj(?:@28)?")
    if len(table) != 1 or len(primitive) != 1 or len(indexed) != 1:
        raise ValueError("public draw symbols or vtable are ambiguous")
    vtable = table[0] - image_base
    primitive_rva, indexed_rva = primitive[0] - image_base, indexed[0] - image_base
    section_offset(primitive_rva, 1, executable=True)
    section_offset(indexed_rva, 1, executable=True)
    if read_rva(vtable + 83 * width) != primitive[0] or read_rva(vtable + 84 * width) != indexed[0]:
        raise ValueError("public COM slots do not resolve to linked draw symbols")
    return {"machine": machine, "primitive_rva": primitive_rva, "indexed_rva": indexed_rva}


def identity(path: Path) -> dict[str, object]:
    data = path.read_bytes()
    return {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def source_receipt() -> dict[str, dict[str, object]]:
    names = subprocess.check_output(["git", "ls-files", *SOURCE_ROOTS], cwd=ROOT, text=True).splitlines()
    return {name: identity(ROOT / name) for name in names if (ROOT / name).is_file()}


def build_receipt() -> dict[str, dict[str, dict[str, object]]]:
    result = {}
    for name in BUILDS:
        result[name] = {}
        for role, relative in (("options", f"{name}/meson-info/intro-buildoptions.json"),
                               ("coredata", f"{name}/meson-private/coredata.dat")):
            path = ROOT / relative
            if not path.is_file():
                raise ValueError(f"missing build receipt input: {path}")
            result[name][role] = {"path": str(path), "identity": identity(path)}
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", choices=("x86", "x64"), required=True)
    parser.add_argument("--mode", choices=("trace", "timing"), default="timing")
    parser.add_argument("--group-detail", action="store_true",
                        help="Retain each group and separately time its drain tail; diagnostic, not aggregate timing parity.")
    parser.add_argument("--tail-detail", action="store_true",
                        help="Also split EndScene, Issue and GetData wall scopes; implies --group-detail.")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--groups", type=int)
    parser.add_argument("--warm-groups", type=int)
    parser.add_argument("--fixture", type=Path)
    parser.add_argument("--fixture-source", type=Path)
    parser.add_argument("--pe-build-dir", type=Path)
    parser.add_argument("--exe-dir", type=Path)
    parser.add_argument("--winemetal-so", type=Path)
    parser.add_argument("--wine-root", type=Path,
                        help="Optional Wine root override; otherwise resolve --wine-id from manifest.")
    parser.add_argument("--wine-id", default="sikarugir-cx-24.0.7")
    parser.add_argument("--prefix", type=Path, default=ROOT / "experiments/prefixs/conf-d3d9-triangle")
    args = parser.parse_args()
    args.group_detail = args.group_detail or args.tail_detail
    groups = args.groups if args.groups is not None else (2 if args.mode == "trace" else 512)
    warm_groups = args.warm_groups if args.warm_groups is not None else (1 if args.mode == "trace" else 8)
    if not 1 <= groups <= 8192 or not 0 <= warm_groups <= 128:
        raise SystemExit("groups/warm-groups outside bounded range")
    canonical_build = (ROOT / f"build-win32-{args.arch}-builtin").resolve()
    build = (args.pe_build_dir or canonical_build).resolve()
    if build != canonical_build:
        raise SystemExit("--pe-build-dir must be the canonical architecture build root for source association")
    exe_dir = (args.exe_dir or build / "tests/conformance/d3d9").resolve()
    fixture = (args.fixture or exe_dir / f"d3d9_draw_parent_economy_{args.arch}.exe").resolve()
    fixture_source = (args.fixture_source or ROOT / "tests/conformance/d3d9/d3d9_draw_parent_economy.cpp").resolve()
    provider = (args.winemetal_so or ROOT / "build-x86_64-builtin/src/winemetal/unix/winemetal_dxmt9.so").resolve()
    wine_entry = resolve_wine(args.wine_id, args.wine_root)
    wine_root = wine_entry.path.resolve()
    wine = wine_entry.wine_bin().resolve()
    if not fixture.is_file() or not fixture_source.is_file() or not provider.is_file():
        raise SystemExit("missing fixture, fixture source, or unix provider")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    case = output / "case.toml"
    case.write_text(f'[[case]]\nexecutable = "{fixture.name}"\nfunction = "runDrawParentEconomy"\n')
    pe_images = {"d3d9.dll": build / "src/win32/d3d9.dll",
                 "winemetal_dxmt9.dll": build / "src/winemetal/winemetal_dxmt9.dll",
                 "winemetal_dxmt9.so": provider}
    for path in (fixture, fixture_source, *pe_images.values()):
        if not path.is_file():
            raise SystemExit(f"missing run input: {path}")
    product_inputs = canonical_images()
    for path in product_inputs.values():
        if not path.is_file():
            raise SystemExit(f"missing canonical production image: {path}")
    if provider != product_inputs["unix-winemetal"]:
        raise SystemExit("--winemetal-so must be the canonical Unix provider for source association")
    selected_binding = pe_public_binding(pe_images["d3d9.dll"], args.arch)
    source_before, builds_before = source_receipt(), build_receipt()
    fixture_source_before = identity(fixture_source)
    runtime_paths = {
        "d3d9.dll": wine_root / "lib/wine" / ("i386-windows" if args.arch == "x86" else "x86_64-windows") / "d3d9.dll",
        "winemetal_dxmt9.dll": wine_root / "lib/wine" / ("i386-windows" if args.arch == "x86" else "x86_64-windows") / "winemetal_dxmt9.dll",
        "winemetal_dxmt9.so": wine_root / "lib/wine/x86_64-unix/winemetal_dxmt9.so",
        "ntdll.so": wine_root / "lib/wine/x86_64-unix/ntdll.so",
        "winemac.so": wine_root / "lib/wine/x86_64-unix/winemac.so",
    }
    wine_before = {name: identity(runtime_paths[name]) for name in ("ntdll.so", "winemac.so")}
    explicit = {"DXMT_LOG_LEVEL": "info" if args.mode == "trace" else "warn",
                "DXMT_LOG_PATH": "none", "WINEDEBUG": "-all", "DXMT_PERF_COUNTERS": "0",
                "DXMT9_OFFLOAD_COMMIT_REPLAY": "1", "DXMT9_PE_INLINE_CONST_DELTA": "0",
                "DXMT9_PE_SEGMENTED_TRANSPORT": "0", "DXMT9_PE_CHUNK_MAX_RECORDS": "256",
                "DXMT9_PE_CHUNK_MAX_BYTES": "1310720", "DXMT9_CPU_READY_TAPE": "0",
                "DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE": "0", "DXMT9_PE_STATS_DECIMATION": "0",
                "DXMT9_TEST_DRAW_PARENT_GROUPS": str(groups), "DXMT9_TEST_DRAW_PARENT_WARM_GROUPS": str(warm_groups),
                "DXMT9_TEST_DRAW_PARENT_GROUP_DETAIL": "1" if args.group_detail else "0",
                "DXMT9_TEST_DRAW_PARENT_TAIL_DETAIL": "1" if args.tail_detail else "0",
                "DXMT9_TEST_DRAW_PARENT_TRACE": "1" if args.mode == "trace" else "0"}
    if args.mode == "trace":
        explicit["DXMT9_PE_RECORDER_CHUNK_LOG"] = "1"
    env = {key: os.environ[key] for key in ("PATH", "HOME", "TMPDIR", "LANG", "LC_ALL", "TZ") if key in os.environ}
    env.update(explicit)
    command = ["bash", "scripts/run_python.sh", "scripts/tools/run_d3d9_conformance.py",
               "--start", "0", "--end", "0", "--no-retry-timeouts", "--no-archive", "--verbose",
               "--aux-timeout", "600", "--pe-arch", args.arch, "--pe-dll-dir", str(build),
               "--exe", str(fixture), "--exe-dir", str(exe_dir), "--aux-exe", fixture.name,
               "--winemetal-so", str(provider), "--prefix", str(args.prefix.resolve()),
               "--wine-id", args.wine_id, "--wine", str(wine),
               "--manifest", str(case), "--output", str(output / "verdicts.json")]
    recipe = {"schema": "dxmt9-pe-draw-parent-economy-recipe-v1", "arch": args.arch,
              "head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
              "mode": args.mode, "groups": groups, "warm_groups": warm_groups, "iterations": 32,
              "group_detail": args.group_detail,
              "tail_detail": args.tail_detail,
              "command": command, "env": explicit, "fixture": {"path": str(fixture), **identity(fixture)},
              "fixture_source": fixture_source_before, "images": {str(path): identity(path) for path in pe_images.values()},
              "production_images": {name: {"path": str(path), "identity": identity(path)}
                                    for name, path in product_inputs.items()},
              "binding": selected_binding,
              "wine_runtime_before": wine_before,
              "llvm_nm": {"path": shutil.which("llvm-nm"), "identity": identity(Path(shutil.which("llvm-nm")))},
              "source_before": source_before, "builds_before": builds_before,
              "runner": identity(ROOT / "scripts/tools/run_d3d9_conformance.py"),
              "driver": identity(Path(__file__)), "parser": identity(Path(__file__).with_name("summarize_pe_draw_parent_economy.py")),
              "manifest": identity(ROOT / "experiments/wine/manifest.toml"), "wine": {"id": args.wine_id, "binary": identity(wine)},
              "effective_environment_sha256": hashlib.sha256(json.dumps(env, sort_keys=True).encode()).hexdigest(),
              "scope": "Public COM draw-parent QPC rows; no private entry, PE-exclusive CPU, FPS, or neutrality claim."}
    (output / "recipe.json").write_text(json.dumps(recipe, indent=2) + "\n")
    started = time.monotonic()
    with (output / "run.log").open("x") as stream:
        result = subprocess.run(command, cwd=ROOT, env=env, stdout=stream, stderr=subprocess.STDOUT)
    qualification = {"schema": "dxmt9-pe-draw-parent-economy-qualification-v1", "qualified": False,
                     "returncode": result.returncode, "elapsed_seconds": time.monotonic() - started}
    try:
        if result.returncode != 0:
            raise ValueError("canonical conformance runner failed")
        verdicts = json.loads((output / "verdicts.json").read_text())
        if verdicts != {"runDrawParentEconomy": "pass"}:
            raise ValueError("unexpected fixture verdict")
        parsed = parse_text((output / "run.log").read_text(), expected_arch=args.arch,
                            expected_trace=args.mode == "trace", expected_groups=groups,
                            expected_group_detail=args.group_detail,
                            expected_tail_detail=args.tail_detail)
        if parsed["binding"]["primitive_rva"] != selected_binding["primitive_rva"] or \
                parsed["binding"]["indexed_rva"] != selected_binding["indexed_rva"]:
            raise ValueError("fixture binding does not match linked public COM slots")
        staged_path = output / "verdicts.json.staged-build.json"
        staged = json.loads(staged_path.read_text())["artifacts"]
        if (len(staged) != 3 or {item["source"] for item in staged.values()} != set(recipe["images"])
                or set(staged) != {str(runtime_paths[name]) for name in pe_images}):
            raise ValueError("expected three product staged entries")
        staged_full = {}
        for destination, detail in staged.items():
            source = Path(detail["source"])
            expected = recipe["images"][str(source)]
            if detail["bytes"] != expected["bytes"] or detail["sha256"] != expected["sha256"][:16]:
                raise ValueError("staged receipt mismatch")
            if identity(source) != expected or identity(Path(destination)) != expected:
                raise ValueError("staged product image changed")
            staged_full[destination] = identity(Path(destination))
        runtime = {name: identity(path) for name, path in runtime_paths.items()}
        if any(runtime[name] != value for name, value in wine_before.items()):
            raise ValueError("Wine Unix runtime changed during run")
        production_after = {name: identity(Path(value["path"]))
                            for name, value in recipe["production_images"].items()}
        if any(production_after[name] != value["identity"]
               for name, value in recipe["production_images"].items()):
            raise ValueError("production image changed during run")
        source_after, builds_after = source_receipt(), build_receipt()
        if source_after != source_before or builds_after != builds_before:
            raise ValueError("source or build receipt changed during run")
        fixture_identity = {key: recipe["fixture"][key] for key in ("bytes", "sha256")}
        if identity(fixture) != fixture_identity:
            raise ValueError("fixture changed during run")
        if identity(fixture_source) != fixture_source_before:
            raise ValueError("fixture source changed during run")
        if identity(Path(__file__)) != recipe["driver"] or \
                identity(Path(__file__).with_name("summarize_pe_draw_parent_economy.py")) != recipe["parser"] or \
                identity(ROOT / "scripts/tools/run_d3d9_conformance.py") != recipe["runner"] or \
                identity(WINE_MANIFEST) != recipe["manifest"] or identity(wine) != recipe["wine"]["binary"]:
            raise ValueError("driver, parser, runner, manifest, or Wine identity changed")
        qualification.update(qualified=True, verdicts=verdicts, parsed=parsed,
                             staged=staged, staged_full=staged_full, runtime_images=runtime,
                             production_images_after=production_after, source_after=source_after, builds_after=builds_after,
                             staging_receipt=identity(staged_path), recipe=identity(output / "recipe.json"),
                             run_log=identity(output / "run.log"), driver=recipe["driver"], parser=recipe["parser"])
    except (OSError, KeyError, ValueError) as error:
        qualification["failure"] = str(error)
    qualification["artifacts"] = {path.name: identity(path) for path in output.iterdir() if path.is_file()}
    (output / "qualification.json").write_text(json.dumps(qualification, indent=2) + "\n")
    print(f"{'QUALIFIED' if qualification['qualified'] else 'REJECTED'} {output}", flush=True)
    return 0 if qualification["qualified"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
