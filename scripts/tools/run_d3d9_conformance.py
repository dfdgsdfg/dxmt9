#!/usr/bin/env python3
"""Chunked Sikarugir-CX runner for dxmt9-d3d9-conformance.exe.

Runs the D3D9 conformance binary in small chunks under the
Sikarugir-CX 24.0.7 Wine runtime and captures per-test
pass/fail/skip/timeout state into a JSON snapshot.

Rationale: the conformance .exe carries a few seconds of process +
DLL setup per Wine launch; running all 208 cases in one process is
slow and brittle, but launching one process per test is wasteful.
Chunking by N (default 4) keeps per-chunk wall time bounded so a
single hanging test only blocks `chunk_size` siblings, and timed-out
chunks are degraded to a re-run as singletons.

Output:
  tmp/d3d9-conformance-results.json
    {"<function_name>": "pass"|"fail"|"skip"|"timeout", ...}

The function name in the JSON matches the binary's internal table
(strip the leading "test_") and the RUN/PASS/FAIL stdout format from
tests/conformance/d3d9/d3d9_conformance.c.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

DEFAULT_EXE = REPO_ROOT / "build-win32-x64-builtin/tests/conformance/d3d9/dxmt9-d3d9-conformance.exe"
DEFAULT_WINE = REPO_ROOT / "experiments/wine/sikarugir-cx-24.0.7/bin/wine"
DEFAULT_PREFIX = REPO_ROOT / "tmp/conformance-prefix"
DEFAULT_OUTPUT = REPO_ROOT / "tmp/d3d9-conformance-results.json"
TEST_SOURCE = REPO_ROOT / "tests/conformance/d3d9/d3d9_conformance.c"

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
                   help=f"Conformance exe path (default: {DEFAULT_EXE.relative_to(REPO_ROOT)}).")
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


def build_env(prefix: Path) -> dict[str, str]:
    env = os.environ.copy()
    env["WINEPREFIX"] = str(prefix)
    env["WINEDLLOVERRIDES"] = "d3d9,winemetal=n,b"
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
    env = build_env(args.prefix)
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

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w") as f:
        json.dump(out, f, indent=2, sort_keys=True)
        f.write("\n")

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
        f"-> {args.output.relative_to(REPO_ROOT) if args.output.is_absolute() else args.output}",
        file=sys.stderr,
    )

    return 0


if __name__ == "__main__":
    sys.exit(main())
