#!/usr/bin/env python3
"""Run a command in a new process group with TERM/KILL timeout cleanup."""

from __future__ import annotations

import argparse
import os
import signal
import subprocess
import sys


def positive_float(value: str) -> float:
    parsed = float(value)
    if parsed <= 0.0:
        raise argparse.ArgumentTypeError("must be positive")
    return parsed


def non_negative_float(value: str) -> float:
    parsed = float(value)
    if parsed < 0.0:
        raise argparse.ArgumentTypeError("must be non-negative")
    return parsed


def terminate_process_group(process: subprocess.Popen[object], grace_sec: float) -> None:
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        process.wait(timeout=grace_sec)
        return
    except subprocess.TimeoutExpired:
        pass

    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        return
    process.wait(timeout=grace_sec)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--timeout", type=positive_float, required=True)
    parser.add_argument("--slack", type=non_negative_float, default=0.0)
    parser.add_argument("--grace", type=positive_float, default=5.0)
    parser.add_argument("--label", default="run-with-timeout")
    parser.add_argument(
        "--timeout-exit-code",
        type=int,
        default=124,
        help="exit code to return after a timeout, default 124",
    )
    parser.add_argument("command", nargs=argparse.REMAINDER)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    command = list(args.command)
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        print("run_with_timeout.py: missing command", file=sys.stderr)
        return 2

    effective_timeout = args.timeout + args.slack
    process = subprocess.Popen(command, start_new_session=True)
    try:
        return process.wait(timeout=effective_timeout)
    except subprocess.TimeoutExpired:
        print(
            f"[{args.label}] timeout after {effective_timeout:g}s "
            f"(base={args.timeout:g}s slack={args.slack:g}s); "
            "terminating process group",
            file=sys.stderr,
            flush=True,
        )
        terminate_process_group(process, args.grace)
        return args.timeout_exit_code
    except KeyboardInterrupt:
        print(
            f"[{args.label}] interrupted; terminating process group",
            file=sys.stderr,
            flush=True,
        )
        terminate_process_group(process, args.grace)
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
