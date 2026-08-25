#!/usr/bin/env python3
"""Chunked Sikarugir-CX runner for the D3D9 conformance suites.

Drives every conformance executable referenced by
`tests/conformance/d3d9/MANIFEST.toml` under the Sikarugir-CX 24.0.7
Wine runtime and captures per-test pass/fail/skip/timeout state into
a single JSON snapshot.

Two execution shapes:

* `dxmt9-d3d9-conformance.exe` (and any future binary built from the
  same `RUN [N:name] ... PASS/FAIL/SKIP [name]` dispatch table) uses
  the chunked argv flow with `start=N end=M` selectors. Chunk size N
  keeps per-chunk wall time bounded; timed-out chunks are re-run as
  singletons.
* Auxiliary executables (`d3d9_*_x64.exe`, `dxmt9-d3d9-device-lifetime.exe`)
  do not accept a per-test selector — they call a small fixed sequence
  of test functions and report only a per-process exit code plus
  `SKIP:<name>:` and `FAIL:LINE: …` lines. They are launched once each
  and the cases they own (enumerated from MANIFEST.toml) get a verdict
  inferred from exit code + parsed `SKIP:<name>:` markers:
  - exit 0          -> all owned cases pass
  - exit 77         -> all owned cases pass, except any case whose
                       function appears in a `SKIP:<name>:` line, which
                       is marked skip
  - non-zero / 1    -> all owned cases fail (the binary does not
                       attribute its FAIL lines to a specific function,
                       so we cannot reliably split the verdict — record
                       the conservative aggregate fail)
  - timeout         -> all owned cases timeout

Output:
  tmp/d3d9-conformance-results.json
    {"<function_name>": "pass"|"fail"|"skip"|"timeout", ...}
  experiments/output/conformance-<UTC>/
    results.json + staged-build.json + run.json -- a timestamped bundle
    that survives the next run, so a document can cite a verdict.
    Disable with --no-archive.

The function name in the JSON matches the binary's internal table for
the main conformance exe (strip the leading "test_") and the function
field declared in MANIFEST.toml for auxiliary exes.
"""
from __future__ import annotations

import argparse
import hashlib
import pathlib
import tempfile
import json
import os
import re
import shutil
import subprocess
import sys
import time
import tomllib
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from scripts.wine.resolve import (  # noqa: E402
    ManifestError,
    WineEntry,
    load_manifest,
    validate_wsi_spawn,
)

DEFAULT_EXE = REPO_ROOT / "build-win32-x64-builtin/tests/conformance/d3d9/dxmt9-d3d9-conformance.exe"
DEFAULT_EXE_DIR = DEFAULT_EXE.parent
DEFAULT_WINE = REPO_ROOT / "experiments/wine/sikarugir-cx-24.0.7/bin/wine"
DEFAULT_PREFIX = REPO_ROOT / "tmp/conformance-prefix"
DEFAULT_OUTPUT = REPO_ROOT / "tmp/d3d9-conformance-results.json"
DEFAULT_ARCHIVE_ROOT = REPO_ROOT / "experiments/output"
DEFAULT_MANIFEST = REPO_ROOT / "tests/conformance/d3d9/MANIFEST.toml"
DEFAULT_WINE_MANIFEST = REPO_ROOT / "experiments/wine/manifest.toml"
DEFAULT_WINEMETAL_SO = REPO_ROOT / "build-x86_64-builtin/src/winemetal/unix/winemetal_dxmt9.so"
TEST_SOURCE = REPO_ROOT / "tests/conformance/d3d9/d3d9_conformance.c"

# Executables that follow the same `RUN [N:name] ... PASS/FAIL/SKIP [name]`
# protocol and `start=N end=M` argv selector as the main conformance exe.
# Anything else is treated as an auxiliary executable (single launch,
# exit-code-driven verdict). Currently only the main exe; extend when a
# new exe is wired into the same dispatch shape.
CHUNKED_EXECUTABLES: set[str] = {"dxmt9-d3d9-conformance.exe"}

# Tests known to hang or time out in isolation. The chunk runner
# applies these as a-priori "skip" verdicts so the chunk wall-time
# budget is not consumed by a known-bad case. Empty by default —
# the chunk-timeout + singleton-replay logic already classifies
# transient hangs as "timeout".
KNOWN_HANGS: set[str] = set()

RUN_RE = re.compile(r"^RUN\s+\[(\d+):([^\]]+)\]\s*$")
RESULT_RE = re.compile(r"^(PASS|FAIL|SKIP)\s+\[([^\]]+)\]\s*$")


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--chunk-size", type=int, default=4,
                   help="Tests per chunk (default: 4).")
    p.add_argument("--start", type=int, default=0,
                   help="First test index (inclusive, default: 0).")
    p.add_argument("--end", type=int, default=None,
                   help="Last test index (exclusive, default: binary's total).")
    p.add_argument("--timeout", type=float, default=60.0,
                   help="Per-chunk timeout in seconds (default: 60).")
    p.add_argument("--singleton-timeout", type=float, default=30.0,
                   help="Per-singleton retry timeout in seconds (default: 30).")
    p.add_argument("--exe", type=Path, default=DEFAULT_EXE,
                   help=f"Main chunked conformance exe path (default: {DEFAULT_EXE.relative_to(REPO_ROOT)}).")
    p.add_argument("--exe-dir", type=Path, default=DEFAULT_EXE_DIR,
                   help="Directory holding auxiliary conformance .exes "
                        "(default: same directory as --exe).")
    p.add_argument("--pe-dll-dir", type=Path, default=None,
                   help="Builtin PE build root containing src/win32/d3d9.dll "
                        "and src/winemetal/winemetal_dxmt9.dll (default: infer from --exe).")
    p.add_argument("--pe-arch", choices=("auto", "x64", "x86"), default="auto",
                   help="Builtin PE staging lane (default: infer from d3d9.dll).")
    p.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST,
                   help=f"MANIFEST.toml describing all cases "
                        f"(default: {DEFAULT_MANIFEST.relative_to(REPO_ROOT)}).")
    p.add_argument("--winemetal-so", type=Path,
                   default=Path(os.environ.get("DXMT9_CONFORMANCE_WINEMETAL_SO",
                                               str(DEFAULT_WINEMETAL_SO))),
                   help="winemetal_dxmt9.so to stage next to the conformance exe "
                        f"(default: {DEFAULT_WINEMETAL_SO.relative_to(REPO_ROOT)}, "
                        "or DXMT9_CONFORMANCE_WINEMETAL_SO).")
    p.add_argument("--skip-aux", action="store_true",
                   help="Skip auxiliary executables; only run the main chunked exe.")
    p.add_argument("--aux-exe", action="append", default=[],
                   help="Run only this auxiliary executable (repeatable).")
    p.add_argument("--aux-timeout", type=float, default=120.0,
                   help="Per-auxiliary-exe timeout in seconds (default: 120).")
    p.add_argument("--wine", type=Path, default=DEFAULT_WINE,
                   help="Wine binary path.")
    p.add_argument("--prefix", type=Path, default=DEFAULT_PREFIX,
                   help="WINEPREFIX path.")
    p.add_argument("--wine-manifest", type=Path, default=DEFAULT_WINE_MANIFEST,
                   help="Wine root registry used to declare the WSI surface "
                        f"protocol (default: "
                        f"{DEFAULT_WINE_MANIFEST.relative_to(REPO_ROOT)}).")
    p.add_argument("--wine-id", default=None,
                   help="Manifest id describing --wine. Default: match the "
                        "wine root (--wine's grandparent) against the "
                        "manifest paths.")
    p.add_argument("--allow-unsupported-wsi-negative-test", action="store_true",
                   help="Run even though the resolved runtime declares no "
                        "usable WSI surface protocol. Device creation then "
                        "fail-closes with D3DERR_NOTAVAILABLE, so use this "
                        "only for a deliberate negative-compatibility run.")
    p.add_argument("--output", type=Path, default=DEFAULT_OUTPUT,
                   help="JSON output path.")
    p.add_argument("--archive-root", type=Path, default=DEFAULT_ARCHIVE_ROOT,
                   help="Directory receiving the timestamped run bundle "
                        "(default: experiments/output).")
    p.add_argument("--no-archive", action="store_true",
                   help="Skip the timestamped run bundle; --output alone is "
                        "then the only record, and the next run overwrites it.")
    p.add_argument("--retry-timeouts", action="store_true", default=True,
                   help="Replay timed-out chunks as singletons (default: on).")
    p.add_argument("--no-retry-timeouts", action="store_false",
                   dest="retry_timeouts",
                   help="Skip singleton replay of timed-out chunks.")
    p.add_argument("--verbose", "-v", action="store_true",
                   help="Echo each chunk's stdout to stderr as it runs.")
    return p.parse_args()


def discover_test_names() -> list[str]:
    """Read the conformance.c dispatch table to get the canonical test
    name list in binary index order.
    """
    src = TEST_SOURCE.read_text()
    # Match { "name", test_func } entries inside the static `tests[]` array.
    # The brace shape is permissive: dxmt9_conformance.c puts each entry on
    # its own line (sometimes wrapped) with `{"name", fn},`.
    pat = re.compile(r'\{\s*"([a-zA-Z0-9_]+)"\s*,', re.M)
    names = pat.findall(src)
    if not names:
        sys.exit(f"could not discover test names from {TEST_SOURCE}")
    return names


def _relative_or_absolute(path: Path) -> str:
    """Render `path` as repo-relative when possible, else absolute."""
    if path.is_absolute():
        try:
            return str(path.relative_to(REPO_ROOT))
        except ValueError:
            return str(path)
    return str(path)


def _describe_head() -> str:
    """Short HEAD description, or "" when git is unavailable.

    The verdicts are only interpretable against the code that produced them,
    and the staged-build sidecar records the binaries but not the source point.
    """
    try:
        proc = subprocess.run(
            ["git", "-C", str(REPO_ROOT), "describe", "--always", "--dirty"],
            capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.SubprocessError):
        return ""
    return proc.stdout.strip() if proc.returncode == 0 else ""


def load_manifest_cases(manifest_path: Path) -> dict[str, list[str]]:
    """Read MANIFEST.toml and return {executable_basename: [function, ...]}.

    The ordering inside each list reflects MANIFEST order so the runner
    log is stable across invocations.
    """
    with manifest_path.open("rb") as f:
        data = tomllib.load(f)
    cases = data.get("case") or []
    by_exe: dict[str, list[str]] = {}
    for case in cases:
        exe = case.get("executable")
        fn = case.get("function")
        if not isinstance(exe, str) or not isinstance(fn, str):
            continue
        by_exe.setdefault(exe, []).append(fn)
    return by_exe


SKIP_RE = re.compile(r"^SKIP:([A-Za-z0-9_]+):")


def run_aux_exe(args: argparse.Namespace, exe_path: Path,
                owned_cases: list[str], timeout: float
                ) -> tuple[dict[str, str], str, bool]:
    """Launch one auxiliary executable and infer verdicts for its owned cases.

    Returns (verdicts, captured_stdout_stderr, timed_out). The verdicts
    map every name in owned_cases to "pass" | "fail" | "skip" | "timeout"
    based on:
      - exit 0          -> all pass
      - exit 77         -> pass for everything not in the SKIP:<name>:
                           markers, skip for any name that does appear
      - non-zero / 1    -> conservative aggregate fail (aux exes do not
                           attribute their FAIL lines to a specific
                           test function name)
      - timeout         -> all timeout
    """
    cmd = [str(args.wine), str(exe_path)]
    env = build_env(args)
    timed_out = False
    rc: int | None = None
    try:
        proc = subprocess.run(
            cmd,
            env=env,
            cwd=str(REPO_ROOT),
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )
        stdout = proc.stdout + proc.stderr
        rc = proc.returncode
    except subprocess.TimeoutExpired as exc:
        timed_out = True
        stdout = (exc.stdout or b"").decode("utf-8", errors="replace") \
            + (exc.stderr or b"").decode("utf-8", errors="replace")

    verdicts: dict[str, str] = {}
    if timed_out:
        for name in owned_cases:
            verdicts[name] = "timeout"
        return verdicts, stdout, True

    skipped_names: set[str] = set()
    for line in stdout.splitlines():
        m = SKIP_RE.match(line)
        if m:
            skipped_names.add(m.group(1))

    # Map exit code to aggregate verdict.
    if rc == 0:
        for name in owned_cases:
            verdicts[name] = "pass"
    elif rc == 77:
        # Pass with skips. Any case whose function name appears in a
        # SKIP:<name>: marker is recorded as skip; the rest pass.
        for name in owned_cases:
            verdicts[name] = "skip" if name in skipped_names else "pass"
    else:
        # Aux exes don't attribute FAIL lines to specific functions —
        # record a conservative aggregate fail. A maintainer reviewing
        # the saved stdout can refine to per-case granularity.
        for name in owned_cases:
            verdicts[name] = "fail"
    return verdicts, stdout, False


def stage_app_local_unixlib(args: argparse.Namespace) -> None:
    """Stage winemetal_dxmt9.so beside app-local winemetal_dxmt9.dll when available."""
    src = args.winemetal_so
    if not src.exists():
        return
    dst = args.exe.parent / "winemetal_dxmt9.so"
    if src.resolve() != dst.resolve():
        shutil.copy2(src, dst)
    args.staged_winemetal_so = dst


# The builtin lane's PE DLLs are postprocessed to carry Wine's "Wine builtin DLL"
# signature. That makes Wine resolve them from ITS OWN dll directory no matter
# what path LoadLibrary was given, so the copy beside the exe and the copy in
# the prefix's system32 are both inert -- Wine loads
# $WINE_ROOT/lib/wine/<arch>-windows/d3d9.dll; the PE machine field selects
# i386-windows for the 32-bit staging lane.
#
# Nothing in this runner used to write that file. It was written by 3DMark
# wild-run staging (install_heroic_wine.sh), at unrelated times, from whichever
# tree that run used. On 2026-08-01 a conformance run was therefore silently
# testing a two-week-old build, and later one staged by an unrelated A/B --
# while every path anyone thought to md5-check matched the build output. A suite
# that cannot observe a code change cannot pass informatively, so this staging
# is a correctness precondition, not a convenience.
BUILTIN_PE_DLLS = ("d3d9.dll", "winemetal_dxmt9.dll")


def detect_pe_arch(path: Path) -> str:
    """Read the PE machine field so x86 runs stage into i386-windows."""
    data = path.read_bytes()
    if len(data) < 0x40 or data[:2] != b"MZ":
        sys.exit(f"cannot identify PE architecture: invalid DOS header {path}")
    pe_offset = int.from_bytes(data[0x3c:0x40], "little")
    if pe_offset + 6 > len(data) or data[pe_offset:pe_offset + 4] != b"PE\0\0":
        sys.exit(f"cannot identify PE architecture: invalid PE header {path}")
    machine = int.from_bytes(data[pe_offset + 4:pe_offset + 6], "little")
    if machine == 0x8664:
        return "x64"
    if machine == 0x014c:
        return "x86"
    sys.exit(f"unsupported PE machine 0x{machine:04x}: {path}")


def canonical_pe_dlls(args: argparse.Namespace) -> tuple[Path, Path, str]:
    """Return the canonical postprocessed PE pair and its machine lane.

    The conformance executables are copied into their test directory by some
    build targets, but those copies are not the builtin DLLs Wine resolves.
    Only the two artifacts under the build root's ``src`` tree are eligible
    for builtin staging.  ``--pe-dll-dir`` is deliberately a build-root
    selector rather than an arbitrary pair of files, keeping this operation
    bounded to Meson's canonical output layout.
    """
    build_root = getattr(args, "pe_dll_dir", None)
    if build_root is None:
        # DEFAULT_EXE is .../<build>/tests/conformance/d3d9/<exe>.  Resolve
        # ancestors rather than relying on a fixed number of parents so an
        # explicitly supplied --exe remains safe when its layout differs.
        exe = args.exe.resolve()
        candidates = []
        for parent in exe.parents:
            if ((parent / "src/win32/d3d9.dll").is_file() and
                    (parent / "src/winemetal/winemetal_dxmt9.dll").is_file()):
                candidates.append(parent)
        if len(candidates) != 1:
            detail = "no canonical PE build root found" if not candidates \
                else "ambiguous canonical PE build roots: " + \
                ", ".join(str(path) for path in candidates)
            sys.exit(f"{detail}; pass --pe-dll-dir explicitly")
        build_root = candidates[0]
    else:
        build_root = build_root.resolve()

    d3d9 = build_root / "src/win32/d3d9.dll"
    winemetal = build_root / "src/winemetal/winemetal_dxmt9.dll"
    for dll in (d3d9, winemetal):
        if not dll.is_file():
            sys.exit(f"canonical builtin PE DLL missing: {dll}")
        stamp = Path(str(dll) + ".postproc")
        if not stamp.is_file():
            sys.exit(f"canonical builtin PE DLL has no postprocess stamp: {stamp}")
        if stamp.stat().st_mtime_ns < dll.stat().st_mtime_ns:
            sys.exit(f"postprocess stamp is older than DLL: {stamp} < {dll}")

    pe_arch = detect_pe_arch(d3d9)
    if detect_pe_arch(winemetal) != pe_arch:
        sys.exit(f"canonical builtin PE DLL architecture mismatch: {d3d9} vs {winemetal}")
    requested_arch = getattr(args, "pe_arch", "auto")
    if requested_arch != "auto" and requested_arch != pe_arch:
        sys.exit(f"--pe-arch={requested_arch} does not match canonical DLL machine ({pe_arch})")
    return d3d9, winemetal, pe_arch


def stage_builtin_pe_dlls(args: argparse.Namespace) -> None:
    """Copy the built PE DLLs AND the unix provider into the Wine root.

    The unix side has the same trap as the PE side: Wine resolves winemetal_dxmt9.so
    from $WINE_ROOT/lib/wine/x86_64-unix/, so the copy this runner used to place
    next to the exe (and DXMT9_WINEMETAL_SO) is inert in the builtin lane. Until
    2026-08-02 nothing here wrote that file either, so conformance ran against
    whichever winemetal_dxmt9.so a 3DMark wild run last staged -- the PE half of this
    bug was fixed first and left the unix half live.  The unix provider remains
    x86_64-unix for both PE lanes under Sikarugir WoW64.
    """
    staged: dict[str, dict[str, object]] = {}
    d3d9_src, winemetal_src, pe_arch = canonical_pe_dlls(args)
    wine_dll_dir = args.wine.parent.parent / "lib" / "wine" / (
        "i386-windows" if pe_arch == "x86" else "x86_64-windows")
    if not wine_dll_dir.is_dir():
        sys.exit(f"wine dll dir not found: {wine_dll_dir}")
    sources = {"d3d9.dll": d3d9_src, "winemetal_dxmt9.dll": winemetal_src}
    for name in BUILTIN_PE_DLLS:
        src = sources[name]
        dst = wine_dll_dir / name
        shutil.copy2(src, dst)
        data = dst.read_bytes()
        if data != src.read_bytes():
            sys.exit(f"staging {name} into the wine root did not take effect: {dst}")
        staged[str(dst)] = {
            "source": str(src),
            "sha256": hashlib.sha256(data).hexdigest()[:16],
            "bytes": len(data),
        }

    unix_dir = args.wine.parent.parent / "lib" / "wine" / "x86_64-unix"
    if not unix_dir.is_dir():
        sys.exit(f"wine unix dir not found: {unix_dir}")
    so_src = args.winemetal_so
    if not so_src.is_file():
        sys.exit(f"cannot stage winemetal_dxmt9.so: missing {so_src}")
    # Same-directory temp + atomic rename, never an in-place overwrite. An
    # in-place cp over a Mach-O file inside the live Wine tree has reproduced
    # `SIGKILL (Code Signature Invalid)` even when codesign --verify passed
    # (see run_with_wine_metal_capture_layer.sh and metal_debugging.rules.md),
    # and any process with the old winemetal_dxmt9.so still mapped can be killed when
    # its pages are invalidated. The PE DLLs above are not Mach-O and are not
    # exposed to this.
    so_dst = unix_dir / "winemetal_dxmt9.so"
    with tempfile.NamedTemporaryFile(dir=unix_dir, delete=False) as tmp:
        tmp_path = pathlib.Path(tmp.name)
    shutil.copy2(so_src, tmp_path)
    os.replace(tmp_path, so_dst)
    so_data = so_dst.read_bytes()
    if so_data != so_src.read_bytes():
        sys.exit(f"staging winemetal_dxmt9.so into the wine root did not take effect: {so_dst}")
    staged[str(so_dst)] = {
        "source": str(so_src),
        "sha256": hashlib.sha256(so_data).hexdigest()[:16],
        "bytes": len(so_data),
    }

    args.staged_wine_dll_dir = wine_dll_dir
    args.staged_pe_arch = pe_arch
    args.staged_build = staged


def resolve_wine_identity(args: argparse.Namespace) -> WineEntry:
    """Resolve the manifest entry that describes `--wine`.

    The PE side gates windowed WSI layer acquisition on the harness declaring
    the runtime's surface protocol: `legacy-macdrv-symbols:<id>` qualifies only
    when `DXMT9_WINE_METAL_SURFACE_PROTOCOL` and `DXMT9_WINE_MANIFEST_ID` agree
    exactly (`legacyRuntimeQualified()` in src/d3d9/d3d9_pe_wsi.cpp). With the
    declaration absent, `dxmt9PeAcquireWsiBinding` fail-closes and every
    CreateDevice/CreateDeviceEx returns D3DERR_NOTAVAILABLE -- which does not
    show up as a suite-wide failure, because most cases skip a missing device
    and still report PASS. Only the handful that assert on the HRESULT fail.
    So resolving this is a precondition for the run meaning anything, not a
    convenience: see specs/d3d9/wsi/spec.md and the same block in
    scripts/run_apps/run_experiment.py.
    """
    entries = load_manifest(args.wine_manifest)
    if args.wine_id:
        for entry in entries:
            if entry.id == args.wine_id:
                return entry
        raise ManifestError(
            f"--wine-id={args.wine_id!r} not found in {args.wine_manifest}"
        )
    # `--wine` points at <root>/bin/wine; the manifest keys on <root>.
    root = args.wine.resolve().parent.parent
    for entry in entries:
        try:
            if entry.path.resolve() == root:
                return entry
        except OSError:
            continue
    raise ManifestError(
        f"wine root {root} is not registered in {args.wine_manifest}; "
        "pass --wine-id to name the manifest entry it corresponds to"
    )


def build_env(args: argparse.Namespace) -> dict[str, str]:
    env = os.environ.copy()
    # Harness-owned WSI declaration. Cleared first so an inherited value from
    # an unrelated runtime can never qualify this one (the PE gate compares the
    # protocol suffix against the id, so a stale pair is a real hazard).
    env.pop("DXMT9_WINE_METAL_SURFACE_PROTOCOL", None)
    env.pop("DXMT9_WINE_MANIFEST_ID", None)
    wine_entry: WineEntry | None = getattr(args, "wine_entry", None)
    if wine_entry is not None:
        env["DXMT9_WINE_METAL_SURFACE_PROTOCOL"] = (
            wine_entry.metal_surface_protocol
        )
        env["DXMT9_WINE_MANIFEST_ID"] = wine_entry.id
    # Wine requires an absolute WINEPREFIX; a relative path silently resolves
    # to the wrong/default prefix (without the staged dxmt9 trio), so the
    # conformance exe loads no dxmt9 and emits no verdicts -> false all-skip.
    env["WINEPREFIX"] = str(args.prefix.resolve())
    env["WINEDLLOVERRIDES"] = os.environ.get(
        "DXMT9_CONFORMANCE_DLLOVERRIDES", "d3d9,winemetal_dxmt9=n,b")
    if getattr(args, "staged_winemetal_so", None):
        env.setdefault("DXMT9_WINEMETAL_SO",
                       str(args.staged_winemetal_so.resolve()))
    env["DXMT9_PREWARM"] = "disabled"
    # Quiet down Wine FIXMEs that would otherwise drown our parse loop.
    env.setdefault("WINEDEBUG", "-all")
    return env


def run_chunk(args: argparse.Namespace, start: int, end: int,
              timeout: float) -> tuple[dict[str, str], str, bool, int | None]:
    """Return verdicts, output, timeout state, and process status for a chunk."""
    cmd = [
        str(args.wine),
        str(args.exe),
        f"start={start}",
        f"end={end}",
    ]
    env = build_env(args)
    timed_out = False
    try:
        proc = subprocess.run(
            cmd,
            env=env,
            cwd=str(REPO_ROOT),
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )
        stdout = proc.stdout + proc.stderr
        returncode = proc.returncode
    except subprocess.TimeoutExpired as exc:
        timed_out = True
        returncode = None
        stdout = (exc.stdout or b"").decode("utf-8", errors="replace") \
            + (exc.stderr or b"").decode("utf-8", errors="replace")

    verdicts: dict[str, str] = {}
    last_running: str | None = None
    for line in stdout.splitlines():
        m = RUN_RE.match(line)
        if m:
            last_running = m.group(2)
            continue
        m = RESULT_RE.match(line)
        if m:
            kind = m.group(1).lower()  # pass | fail | skip
            name = m.group(2)
            verdicts[name] = kind
            last_running = None
            continue
    # If chunk timed out mid-test, last_running has no result line.
    if timed_out and last_running is not None and last_running not in verdicts:
        verdicts[last_running] = "timeout"
    return verdicts, stdout, timed_out, returncode


def mark_failed_process_missing_results(
        chunk_names: list[str], results: dict[str, str], *,
        timed_out: bool, returncode: int | None) -> None:
    """Fail unreported tests when their Wine process exits unsuccessfully."""
    if timed_out or returncode in (None, 0):
        return
    for name in chunk_names:
        results.setdefault(name, "fail")


def main() -> int:
    args = parse_args()

    if not args.exe.exists():
        sys.exit(f"conformance exe not found: {args.exe}")
    if not args.wine.exists():
        sys.exit(f"wine binary not found: {args.wine}")
    if not args.prefix.exists():
        sys.exit(f"wine prefix not found: {args.prefix} — run wineboot first")

    args.wine_entry = None
    try:
        args.wine_entry = resolve_wine_identity(args)
        validate_wsi_spawn(
            args.wine_entry,
            allow_unsupported_negative_test=(
                args.allow_unsupported_wsi_negative_test),
        )
    except ManifestError as exc:
        if not args.allow_unsupported_wsi_negative_test:
            sys.exit(f"WSI gate: {exc}")
        print(f"[runner] WSI gate bypassed: {exc}", file=sys.stderr)
    if args.wine_entry is not None:
        print(f"[runner] wine identity: id={args.wine_entry.id} "
              f"metal_surface_protocol="
              f"{args.wine_entry.metal_surface_protocol}",
              file=sys.stderr)

    stage_app_local_unixlib(args)
    stage_builtin_pe_dlls(args)

    names = discover_test_names()
    total = len(names)
    end = args.end if args.end is not None else total
    if end > total:
        end = total
    if args.start < 0 or args.start > end:
        sys.exit(f"invalid range: start={args.start} end={end}")

    results: dict[str, str] = {}
    timed_out_chunks: list[tuple[int, int]] = []

    print(f"[runner] {total} total tests; running [{args.start}, {end}); "
          f"chunk_size={args.chunk_size}; per-chunk-timeout={args.timeout}s",
          file=sys.stderr)

    i = args.start
    chunk_idx = 0
    while i < end:
        j = min(i + args.chunk_size, end)
        chunk_idx += 1

        # Preassign known-hang names as skips so they don't soak the chunk.
        chunk_names = names[i:j]
        skip_names = [n for n in chunk_names if n in KNOWN_HANGS]
        for n in skip_names:
            results[n] = "skip"
        runnable = [n for n in chunk_names if n not in KNOWN_HANGS]

        if not runnable:
            print(f"[runner] chunk {chunk_idx} [{i}:{j}) — all known-hang, skipped",
                  file=sys.stderr)
            i = j
            continue

        verdicts, stdout, timed_out, returncode = run_chunk(
            args, i, j, args.timeout)
        for name, verdict in verdicts.items():
            results[name] = verdict
        mark_failed_process_missing_results(
            runnable, results, timed_out=timed_out, returncode=returncode)

        if args.verbose:
            sys.stderr.write(stdout)

        pass_n = sum(1 for n in chunk_names if results.get(n) == "pass")
        fail_n = sum(1 for n in chunk_names if results.get(n) == "fail")
        skip_n = sum(1 for n in chunk_names if results.get(n) == "skip")
        to_n = sum(1 for n in chunk_names if results.get(n) == "timeout")
        missing = [n for n in chunk_names if n not in results]
        if timed_out:
            flag = " TIMEOUT"
        elif returncode:
            flag = f" PROCESS-EXIT-{returncode}"
        else:
            flag = ""
        print(f"[runner] chunk {chunk_idx} [{i}:{j}){flag} "
              f"pass={pass_n} fail={fail_n} skip={skip_n} timeout={to_n} "
              f"missing={len(missing)}",
              file=sys.stderr)

        # Incremental snapshot: writes after every chunk so a killed run
        # still leaves observable progress on disk.
        snap = {n: results[n] for n in names[args.start:j] if n in results}
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("w") as f:
            json.dump(snap, f, indent=2, sort_keys=True)
            f.write("\n")

        if timed_out:
            timed_out_chunks.append((i, j))
            # Tests in this chunk that produced no result line are
            # untested-due-to-timeout. They will be retried singly below.
            for n in chunk_names:
                if n not in results:
                    results[n] = "timeout"

        i = j

    # Singleton replay of timed-out chunks: each test gets its own wine launch.
    if args.retry_timeouts and timed_out_chunks:
        print(f"[runner] replaying {len(timed_out_chunks)} timed-out chunks as singletons",
              file=sys.stderr)
        for (cs, ce) in timed_out_chunks:
            for k in range(cs, ce):
                name = names[k]
                if name in KNOWN_HANGS:
                    continue
                # Only retry if it was not produced as a definitive verdict the
                # first time around (timeout verdicts are eligible).
                if results.get(name) not in (None, "timeout"):
                    continue
                verdicts, _, sto, returncode = run_chunk(
                    args, k, k + 1, args.singleton_timeout)
                if sto and name not in verdicts:
                    results[name] = "timeout"
                elif name in verdicts:
                    results[name] = verdicts[name]
                elif returncode:
                    results[name] = "fail"
                else:
                    # Process exited cleanly but produced no result line —
                    # treat as skip rather than silently overwriting.
                    results.setdefault(name, "skip")
                print(f"[runner]   singleton[{k}] {name} -> {results[name]}",
                      file=sys.stderr)

    # Fill in any names that the binary never reported (out of selected range).
    # Names outside [args.start, end) are not represented in the JSON: the
    # JSON is a snapshot of the dispatched window only.
    selected = set(names[args.start:end])
    for n in selected:
        results.setdefault(n, "skip")

    # Only write entries for the dispatched window plus any extras the
    # binary unexpectedly reported.
    out: dict[str, str] = {n: results[n] for n in names[args.start:end]}
    # Include any "extras" (e.g. name drift between source and binary) for
    # forensic transparency.
    for n, v in results.items():
        if n not in out and n not in names:
            out[n] = v

    # --- Auxiliary executables ---------------------------------------------
    # MANIFEST.toml owns the (exe, function) mapping. For each exe that is
    # not the chunked main exe, launch once and infer verdicts from exit
    # code + parsed SKIP markers (see run_aux_exe docstring).
    aux_summary = {"pass": 0, "fail": 0, "skip": 0, "timeout": 0}
    if not args.skip_aux:
        if not args.manifest.exists():
            print(f"[runner] WARNING: manifest not found at {args.manifest} — "
                  f"skipping aux executables", file=sys.stderr)
        else:
            by_exe = load_manifest_cases(args.manifest)
            aux_exes = sorted(
                exe for exe in by_exe.keys() if exe not in CHUNKED_EXECUTABLES
            )
            if args.aux_exe:
                wanted = set(args.aux_exe)
                aux_exes = [exe for exe in aux_exes if exe in wanted]
            print(f"[runner] driving {len(aux_exes)} auxiliary executable(s)",
                  file=sys.stderr)
            for exe_name in aux_exes:
                exe_path = args.exe_dir / exe_name
                owned = by_exe[exe_name]
                if not exe_path.exists():
                    print(f"[runner]   aux {exe_name}: BINARY MISSING at "
                          f"{exe_path} — recording {len(owned)} case(s) as skip",
                          file=sys.stderr)
                    for name in owned:
                        out[name] = "skip"
                        aux_summary["skip"] += 1
                    continue
                verdicts, stdout, timed_out = run_aux_exe(
                    args, exe_path, owned, args.aux_timeout,
                )
                if args.verbose:
                    sys.stderr.write(stdout)
                # Merge into out + tally.
                ps = fs = ss = ts = 0
                for name, verdict in verdicts.items():
                    out[name] = verdict
                    aux_summary[verdict] = aux_summary.get(verdict, 0) + 1
                    if verdict == "pass":
                        ps += 1
                    elif verdict == "fail":
                        fs += 1
                    elif verdict == "skip":
                        ss += 1
                    elif verdict == "timeout":
                        ts += 1
                flag = " TIMEOUT" if timed_out else ""
                print(f"[runner]   aux {exe_name}{flag} ({len(owned)} case(s)) "
                      f"pass={ps} fail={fs} skip={ss} timeout={ts}",
                      file=sys.stderr)
                # Snapshot after every aux exe for crash-resilience.
                args.output.parent.mkdir(parents=True, exist_ok=True)
                with args.output.open("w") as f:
                    json.dump(out, f, indent=2, sort_keys=True)
                    f.write("\n")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w") as f:
        json.dump(out, f, indent=2, sort_keys=True)
        f.write("\n")

    # Sidecar, not a key in `out`: that file is a flat name -> verdict map other
    # tools parse, and a synthetic entry would look like a test. This records
    # WHICH binaries produced the verdicts -- the thing that was unknowable when
    # this suite was found running against a DLL staged by an unrelated wild run.
    staged = getattr(args, "staged_build", None)
    if staged:
        sidecar = args.output.with_suffix(args.output.suffix + ".staged-build.json")
        with sidecar.open("w") as f:
            json.dump({"artifacts": staged}, f, indent=2, sort_keys=True)
            f.write("\n")
        print(f"[runner] staged build recorded -> {sidecar}", file=sys.stderr)

    summary = {"pass": 0, "fail": 0, "skip": 0, "timeout": 0}
    for v in out.values():
        if v in summary:
            summary[v] += 1
        else:
            summary.setdefault(v, 0)
            summary[v] += 1

    # `--output` is one fixed path that every run overwrites, so a verdict was
    # uncitable the moment the next run started: a performance or spec document
    # that wanted to state "conformance was N/M at this commit" had nothing to
    # point `source:` at, and the claim had to be dropped. Land a timestamped
    # bundle under experiments/output/ -- the tree documents already cite -- so
    # a result survives as evidence.
    if not args.no_archive:
        stamp = time.strftime("%Y%m%d-%H%M%S", time.gmtime())
        archive = args.archive_root / f"conformance-{stamp}"
        try:
            archive.mkdir(parents=True, exist_ok=False)
            with (archive / "results.json").open("w") as f:
                json.dump(out, f, indent=2, sort_keys=True)
                f.write("\n")
            if staged:
                with (archive / "staged-build.json").open("w") as f:
                    json.dump({"artifacts": staged}, f, indent=2, sort_keys=True)
                    f.write("\n")
            with (archive / "run.json").open("w") as f:
                json.dump({
                    "argv": sys.argv[1:],
                    "utc": stamp,
                    "commit": _describe_head(),
                    "start": args.start,
                    "end": args.end,
                    "skip_aux": bool(args.skip_aux),
                    "chunk_size": args.chunk_size,
                    "wine": str(args.wine),
                    "wine_id": (
                        args.wine_entry.id if args.wine_entry else None),
                    "metal_surface_protocol": (
                        args.wine_entry.metal_surface_protocol
                        if args.wine_entry else None),
                    "prefix": str(args.prefix),
                    "summary": summary,
                    "aux_summary": aux_summary,
                }, f, indent=2, sort_keys=True)
                f.write("\n")
            print(f"[runner] archived run -> {_relative_or_absolute(archive)}",
                  file=sys.stderr)
        except OSError as exc:
            # Never fail a completed suite over bookkeeping; --output still holds
            # the verdicts.
            print(f"[runner] archive skipped ({exc})", file=sys.stderr)
    print(
        f"[runner] done: {len(out)} tests "
        f"pass={summary['pass']} fail={summary['fail']} "
        f"skip={summary['skip']} timeout={summary['timeout']} "
        f"(aux pass={aux_summary['pass']} fail={aux_summary['fail']} "
        f"skip={aux_summary['skip']} timeout={aux_summary['timeout']}) "
        f"-> {_relative_or_absolute(args.output)}",
        file=sys.stderr,
    )

    return 0


if __name__ == "__main__":
    sys.exit(main())
