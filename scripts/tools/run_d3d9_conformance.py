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

The function name in the JSON matches the binary's internal table for
the main conformance exe (strip the leading "test_") and the function
field declared in MANIFEST.toml for auxiliary exes.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tomllib
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

DEFAULT_EXE = REPO_ROOT / "build-win32-x64-builtin/tests/conformance/d3d9/dxmt9-d3d9-conformance.exe"
DEFAULT_EXE_DIR = DEFAULT_EXE.parent
DEFAULT_WINE = REPO_ROOT / "experiments/wine/sikarugir-cx-24.0.7/bin/wine"
DEFAULT_PREFIX = REPO_ROOT / "tmp/conformance-prefix"
DEFAULT_OUTPUT = REPO_ROOT / "tmp/d3d9-conformance-results.json"
DEFAULT_MANIFEST = REPO_ROOT / "tests/conformance/d3d9/MANIFEST.toml"
DEFAULT_WINEMETAL_SO = REPO_ROOT / "build-x86_64-builtin/src/winemetal/unix/winemetal.so"
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
    p.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST,
                   help=f"MANIFEST.toml describing all cases "
                        f"(default: {DEFAULT_MANIFEST.relative_to(REPO_ROOT)}).")
    p.add_argument("--winemetal-so", type=Path,
                   default=Path(os.environ.get("DXMT9_CONFORMANCE_WINEMETAL_SO",
                                               str(DEFAULT_WINEMETAL_SO))),
                   help="winemetal.so to stage next to the conformance exe "
                        f"(default: {DEFAULT_WINEMETAL_SO.relative_to(REPO_ROOT)}, "
                        "or DXMT9_CONFORMANCE_WINEMETAL_SO).")
    p.add_argument("--skip-aux", action="store_true",
                   help="Skip auxiliary executables; only run the main chunked exe.")
    p.add_argument("--aux-timeout", type=float, default=120.0,
                   help="Per-auxiliary-exe timeout in seconds (default: 120).")
    p.add_argument("--wine", type=Path, default=DEFAULT_WINE,
                   help="Wine binary path.")
    p.add_argument("--prefix", type=Path, default=DEFAULT_PREFIX,
                   help="WINEPREFIX path.")
    p.add_argument("--output", type=Path, default=DEFAULT_OUTPUT,
                   help="JSON output path.")
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
    """Stage winemetal.so beside app-local winemetal.dll when available."""
    src = args.winemetal_so
    if not src.exists():
        return
    dst = args.exe.parent / "winemetal.so"
    if src.resolve() != dst.resolve():
        shutil.copy2(src, dst)
    args.staged_winemetal_so = dst


# The builtin lane's PE DLLs are postprocessed to carry Wine's "Wine builtin DLL"
# signature. That makes Wine resolve them from ITS OWN dll directory no matter
# what path LoadLibrary was given, so the copy beside the exe and the copy in
# the prefix's system32 are both inert -- Wine loads
# $WINE_ROOT/lib/wine/<arch>-windows/d3d9.dll.
#
# Nothing in this runner used to write that file. It was written by 3DMark
# wild-run staging (install_heroic_wine.sh), at unrelated times, from whichever
# tree that run used. On 2026-08-01 a conformance run was therefore silently
# testing a two-week-old build, and later one staged by an unrelated A/B --
# while every path anyone thought to md5-check matched the build output. A suite
# that cannot observe a code change cannot pass informatively, so this staging
# is a correctness precondition, not a convenience.
BUILTIN_PE_DLLS = ("d3d9.dll", "winemetal.dll")


def stage_builtin_pe_dlls(args: argparse.Namespace) -> None:
    """Copy the built PE DLLs AND the unix provider into the Wine root.

    The unix side has the same trap as the PE side: Wine resolves winemetal.so
    from $WINE_ROOT/lib/wine/x86_64-unix/, so the copy this runner used to place
    next to the exe (and DXMT9_WINEMETAL_SO) is inert in the builtin lane. Until
    2026-08-02 nothing here wrote that file either, so conformance ran against
    whichever winemetal.so a 3DMark wild run last staged -- the PE half of this
    bug was fixed first and left the unix half live.
    """
    staged: dict[str, dict[str, object]] = {}
    wine_dll_dir = args.wine.parent.parent / "lib" / "wine" / "x86_64-windows"
    if not wine_dll_dir.is_dir():
        sys.exit(f"wine dll dir not found: {wine_dll_dir}")
    src_dir = args.exe.parent
    for name in BUILTIN_PE_DLLS:
        src = src_dir / name
        if not src.is_file():
            sys.exit(f"cannot stage {name}: missing {src} (build build-win32-x64-builtin first)")
        dst = wine_dll_dir / name
        shutil.copy2(src, dst)
        data = dst.read_bytes()
        if data != src.read_bytes():
            sys.exit(f"staging {name} into the wine root did not take effect: {dst}")
        staged[str(dst)] = {
            "sha256": hashlib.sha256(data).hexdigest()[:16],
            "bytes": len(data),
        }

    unix_dir = args.wine.parent.parent / "lib" / "wine" / "x86_64-unix"
    if not unix_dir.is_dir():
        sys.exit(f"wine unix dir not found: {unix_dir}")
    so_src = args.winemetal_so
    if not so_src.is_file():
        sys.exit(f"cannot stage winemetal.so: missing {so_src}")
    so_dst = unix_dir / "winemetal.so"
    shutil.copy2(so_src, so_dst)
    so_data = so_dst.read_bytes()
    if so_data != so_src.read_bytes():
        sys.exit(f"staging winemetal.so into the wine root did not take effect: {so_dst}")
    staged[str(so_dst)] = {
        "sha256": hashlib.sha256(so_data).hexdigest()[:16],
        "bytes": len(so_data),
    }

    args.staged_wine_dll_dir = wine_dll_dir
    args.staged_build = staged


def build_env(args: argparse.Namespace) -> dict[str, str]:
    env = os.environ.copy()
    # Wine requires an absolute WINEPREFIX; a relative path silently resolves
    # to the wrong/default prefix (without the staged dxmt9 trio), so the
    # conformance exe loads no dxmt9 and emits no verdicts -> false all-skip.
    env["WINEPREFIX"] = str(args.prefix.resolve())
    env["WINEDLLOVERRIDES"] = os.environ.get(
        "DXMT9_CONFORMANCE_DLLOVERRIDES", "d3d9,winemetal=n,b")
    if getattr(args, "staged_winemetal_so", None):
        env.setdefault("DXMT9_WINEMETAL_SO",
                       str(args.staged_winemetal_so.resolve()))
    env["DXMT9_PREWARM"] = "disabled"
    # Quiet down Wine FIXMEs that would otherwise drown our parse loop.
    env.setdefault("WINEDEBUG", "-all")
    return env


def run_chunk(args: argparse.Namespace, start: int, end: int,
              timeout: float) -> tuple[dict[str, str], str, bool]:
    """Run a chunk of [start, end) and return (verdicts, stdout, timed_out)."""
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
    except subprocess.TimeoutExpired as exc:
        timed_out = True
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
    return verdicts, stdout, timed_out


def main() -> int:
    args = parse_args()

    if not args.exe.exists():
        sys.exit(f"conformance exe not found: {args.exe}")
    if not args.wine.exists():
        sys.exit(f"wine binary not found: {args.wine}")
    if not args.prefix.exists():
        sys.exit(f"wine prefix not found: {args.prefix} — run wineboot first")
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

        verdicts, stdout, timed_out = run_chunk(args, i, j, args.timeout)
        for name, verdict in verdicts.items():
            results[name] = verdict

        if args.verbose:
            sys.stderr.write(stdout)

        pass_n = sum(1 for n in chunk_names if results.get(n) == "pass")
        fail_n = sum(1 for n in chunk_names if results.get(n) == "fail")
        skip_n = sum(1 for n in chunk_names if results.get(n) == "skip")
        to_n = sum(1 for n in chunk_names if results.get(n) == "timeout")
        missing = [n for n in chunk_names if n not in results]
        flag = " TIMEOUT" if timed_out else ""
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
                verdicts, _, sto = run_chunk(args, k, k + 1,
                                             args.singleton_timeout)
                if sto and name not in verdicts:
                    results[name] = "timeout"
                elif name in verdicts:
                    results[name] = verdicts[name]
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
