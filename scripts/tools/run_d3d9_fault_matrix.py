#!/usr/bin/env python3
"""Run one typed PE fault per fresh canonical D3D9 conformance process.

This helper is deliberately an orchestration layer: it does not contain a
Wine fixture or duplicate the conformance runner.  Each invocation delegates
to ``run_d3d9_conformance.py`` with one fault selector, a singleton chunk, and
timeout replay disabled.  Pass runner paths and staging options with
``--runner-arg``; use ``--dry-run`` to inspect the bounded command matrix
without launching Wine.  Each point gets a separate JSON output under
``tmp/d3d9-fault-matrix`` by default.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_RUNNER = REPO_ROOT / "scripts/tools/run_d3d9_conformance.py"

RECORDER_FAULT_POINTS = (
    "capacity_pre_reserve",
    # Keep both sides of the typed acquire seam visible: zero fails before
    # the first unique retain, one allows a first retain then fails on the
    # next unique handle. Additional bounded N values can be passed with
    # --fault retain_acquire=N.
    "retain_acquire=0",
    "retain_acquire=1",
    "bridge_pre",
    "bridge_entered",
    "capture_disposition",
    "capture_throw",
    "reset",
    "teardown",
)
RECORDER_FAULT_POINT_NAMES = tuple(
    selector.split("=", 1)[0] for selector in RECORDER_FAULT_POINTS
)
STATEBLOCK_FAULT_POINTS = (
    "capture_pre",
    "apply_pre",
    "end_pre",
    "alloc_pre",
    "bridge_pre",
    "capture_entered",
    "apply_entered",
    "end_entered",
    "bridge_entered",
)


def _point_name(selector: str) -> str:
    name = selector.split("=", 1)[0]
    if name not in RECORDER_FAULT_POINT_NAMES + STATEBLOCK_FAULT_POINTS:
        raise ValueError(f"unknown fault point: {name}")
    if "," in selector:
        raise ValueError("one fault point is required per process")
    return name


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fault-kind", choices=("recorder", "stateblock"),
                        default="recorder")
    parser.add_argument("--fault", action="append", dest="faults",
                        help="Point to run (repeatable; default: every point).")
    parser.add_argument("--runner", type=Path, default=DEFAULT_RUNNER,
                        help="Canonical conformance runner path.")
    parser.add_argument("--pe-arch", choices=("auto", "x64", "x86"),
                        default="auto",
                        help="PE fixture architecture (default: auto).")
    parser.add_argument("--output-dir", type=Path,
                        default=REPO_ROOT / "tmp/d3d9-fault-matrix",
                        help="Directory for one JSON result per fault point.")
    parser.add_argument("--start", type=int, default=0,
                        help="Main conformance index (default: 0).")
    parser.add_argument("--end", type=int, default=0,
                        help="Exclusive main conformance index (default: 0).")
    parser.add_argument("--stateblock-exe", default="d3d9_stateblock_x64.exe",
                        help="StateBlock auxiliary executable name.")
    parser.add_argument("--aux-exe", action="append", dest="aux_exes",
                        default=[],
                        help="Auxiliary executable to run (repeatable).")
    parser.add_argument("--recorder-exe", default=None,
                        help="Recorder fault fixture executable name; defaults "
                             "to the configured PE architecture.")
    parser.add_argument("--runner-arg", action="append", default=[],
                        help="Additional argument passed to the canonical runner.")
    parser.add_argument("--dry-run", action="store_true",
                        help="Print commands without launching Wine.")
    return parser.parse_args(argv)


def matrix_points(kind: str, selectors: list[str] | None) -> list[str]:
    allowed = RECORDER_FAULT_POINTS if kind == "recorder" else STATEBLOCK_FAULT_POINTS
    points = selectors or list(allowed)
    for selector in points:
        name = _point_name(selector)
        allowed_names = (RECORDER_FAULT_POINT_NAMES if kind == "recorder"
                         else STATEBLOCK_FAULT_POINTS)
        if name not in allowed_names:
            raise ValueError(f"{name} is not a {kind} fault point")
    return points


def command_for_point(args: argparse.Namespace, selector: str) -> list[str]:
    if args.fault_kind == "recorder":
        fault_flag = "--pe-recorder-fault"
    else:
        fault_flag = "--stateblock-fault"

    command = [
        sys.executable,
        str(args.runner),
        "--chunk-size", "1",
        "--start", str(args.start),
        "--end", str(args.end),
        "--no-retry-timeouts",
        "--no-archive",
        "--pe-arch", args.pe_arch,
        "--output",
        str(args.output_dir / (args.fault_kind + "-" +
                               re.sub(r"[^A-Za-z0-9_.-]", "_", selector) +
                               ".json")),
        fault_flag,
        selector,
    ]
    if args.fault_kind == "recorder":
        aux_exes = args.aux_exes
        if not aux_exes:
            default_arch = "x86" if getattr(args, "pe_arch", "auto") == "x86" else "x64"
            aux_exes = [args.recorder_exe or
                        f"d3d9_recorder_fault_{default_arch}.exe"]
        for exe in aux_exes:
            command.extend(("--aux-exe", exe))
    else:
        # An empty main range keeps this lane to one fresh auxiliary Wine
        # process, where the clean-room StateBlock fixture consumes the fault.
        command[command.index("--start") + 1] = "0"
        command[command.index("--end") + 1] = "0"
        aux_exes = args.aux_exes or [args.stateblock_exe]
        for exe in aux_exes:
            command.extend(("--aux-exe", exe))
    command.extend(args.runner_arg)
    return command


def result_file_passes(path: Path) -> bool:
    try:
        payload = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError):
        return False
    return (isinstance(payload, dict) and bool(payload) and
            all(verdict == "pass" for verdict in payload.values()))


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        points = matrix_points(args.fault_kind, args.faults)
        commands = [command_for_point(args, point) for point in points]
    except ValueError as exc:
        raise SystemExit(str(exc)) from exc

    failures = 0
    for command in commands:
        print("[fault-matrix]", " ".join(command), file=sys.stderr)
        if args.dry_run:
            continue
        result = subprocess.run(command, cwd=REPO_ROOT, check=False)
        output_index = command.index("--output") + 1
        output = Path(command[output_index])
        if result.returncode or not result_file_passes(output):
            failures += 1
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
