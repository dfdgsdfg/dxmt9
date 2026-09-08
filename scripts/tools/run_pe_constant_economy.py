#!/usr/bin/env python3
"""Run and identity-qualify one actual-PE setter matrix via the canonical runner."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import time

from summarize_pe_constant_economy import parse_text

ROOT = Path(__file__).resolve().parents[2]


def identity(path: Path) -> dict:
    data = path.read_bytes()
    return dict(bytes=len(data), sha256=hashlib.sha256(data).hexdigest())


def check_pe_machine(path: Path, arch: str) -> None:
    data = path.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3c)[0]
    if data[pe:pe + 4] != b"PE\0\0" or struct.unpack_from("<H", data, pe + 4)[0] != {"x86": 0x14c, "x64": 0x8664}[arch]:
        raise ValueError(f"wrong PE architecture: {path}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", choices=("x86", "x64"), required=True)
    parser.add_argument("--output", type=Path, required=True, help="New result directory; never reused.")
    parser.add_argument("--pe-build-dir", type=Path, help="Runtime PE build root, independent of the fixture.")
    parser.add_argument("--exe-dir", type=Path, help="Directory containing the matching-architecture conformance executables.")
    parser.add_argument("--winemetal-so", type=Path, default=ROOT / "build-x86_64-builtin/src/winemetal/unix/winemetal_dxmt9.so")
    parser.add_argument("--prefix", type=Path, default=ROOT / "experiments/prefixs/conf-d3d9-triangle")
    args = parser.parse_args()
    build = (args.pe_build_dir or ROOT / f"build-win32-{args.arch}-builtin").resolve()
    exe_dir = (args.exe_dir or ROOT / f"build-win32-{args.arch}-builtin/tests/conformance/d3d9").resolve()
    provider = args.winemetal_so.resolve()
    fixture = exe_dir / f"d3d9_inline_constants_{args.arch}.exe"
    images = [build / "src/win32/d3d9.dll", build / "src/winemetal/winemetal_dxmt9.dll", provider]
    for path in [fixture, *images[:2]]:
        check_pe_machine(path, args.arch)
    expected = {str(path): identity(path) for path in images}
    fixture_id = identity(fixture)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    command = ["bash", str(ROOT / "scripts/run_python.sh"),
               str(ROOT / "scripts/tools/run_d3d9_conformance.py"),
               "--start", "0", "--end", "0", "--no-retry-timeouts", "--no-archive", "--verbose",
               "--aux-timeout", "240", "--pe-arch", args.arch, "--pe-dll-dir", str(build),
               "--exe", str(exe_dir / "dxmt9-d3d9-conformance.exe"), "--exe-dir", str(exe_dir),
               "--aux-exe", fixture.name, "--winemetal-so", str(provider),
               "--prefix", str(args.prefix.resolve()), "--wine-id", "sikarugir-cx-24.0.7",
               "--output", str(output / "verdicts.json")]
    recipe = dict(DXMT9_TEST_INLINE_CONSTANT_CASE="setter-economy", DXMT_LOG_LEVEL="warn",
                  DXMT_LOG_PATH="none", DXMT_PERF_COUNTERS="0", WINEDEBUG="-all",
                  DXMT9_PE_INLINE_CONST_DELTA="0", DXMT9_OFFLOAD_COMMIT_REPLAY="1",
                  DXMT9_PE_SEGMENTED_TRANSPORT="0", DXMT9_CPU_READY_TAPE="0",
                  DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE="0")
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("DXMT", "MTL_", "METAL_", "WINE", "DYLD_"))}
    env.update(recipe)
    runtime_diff = subprocess.check_output(["git", "diff", "HEAD", "--", "src", "include"], cwd=ROOT)
    receipt = dict(command=command, env=recipe, arch=args.arch, expected_images=expected,
                   fixture=dict(path=str(fixture), **fixture_id),
                   head=subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
                   runtime_diff_sha256=hashlib.sha256(runtime_diff).hexdigest(),
                   fixture_sources={name: identity(ROOT / "tests/conformance/d3d9" / name)
                                    for name in ("d3d9_inline_constants.cpp", "d3d9_constant_economy.cpp", "d3d9_constant_economy.hpp")},
                   driver=identity(Path(__file__).resolve()),
                   parser=identity(ROOT / "scripts/tools/summarize_pe_constant_economy.py"),
                   effective_runner_environment_sha256=hashlib.sha256(
                       json.dumps(env, sort_keys=True).encode()).hexdigest(),
                   conformance_runner=identity(ROOT / "scripts/tools/run_d3d9_conformance.py"),
                   wine_manifest=identity(ROOT / "experiments/wine/manifest.toml"),
                   scope="Builtin Sikarugir actual-PE warm setter matrix only. Generic aux verdicts do not prove unselected pixel/capture/lifecycle selectors.")
    (output / "recipe.json").write_text(json.dumps(receipt, indent=2) + "\n")
    started = time.monotonic()
    with (output / "run.log").open("w") as log:
        result = subprocess.run(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
    report = dict(qualified=False, returncode=result.returncode, elapsed_seconds=time.monotonic() - started)
    try:
        if result.returncode != 0:
            raise ValueError("conformance runner failed")
        report["measurement"] = parse_text((output / "run.log").read_text())
        staged = json.loads((output / "verdicts.json.staged-build.json").read_text())["artifacts"]
        if len(staged) != 3 or {item["source"] for item in staged.values()} != set(expected):
            raise ValueError("unexpected staged-image sources")
        for destination, item in staged.items():
            wanted = expected[item["source"]]
            if item["sha256"] != wanted["sha256"][:16] or item["bytes"] != wanted["bytes"]:
                raise ValueError("staging receipt mismatch")
            if identity(Path(destination)) != wanted or identity(Path(item["source"])) != wanted:
                raise ValueError("full staged/source image changed")
        if identity(fixture) != fixture_id:
            raise ValueError("fixture changed during run")
        provider_destination = next(Path(destination) for destination, item in staged.items()
                                    if item['source'] == str(provider))
        wine_runtime_images = {name: identity(provider_destination.parent / name)
                               for name in ('ntdll.so', 'winemac.so')}
        report.update(qualified=True, staged_receipt=staged,
                      wine_runtime_images=wine_runtime_images)
    except (OSError, KeyError, ValueError) as error:
        report["failure"] = str(error)
    report["artifacts"] = {path.name: identity(path) for path in output.iterdir() if path.is_file()}
    (output / "qualification.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"{'QUALIFIED' if report['qualified'] else 'REJECTED'} {output}", flush=True)
    if not report["qualified"]:
        print(report.get("failure", "unknown failure"))
    return 0 if report["qualified"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
