#!/usr/bin/env python3
"""Strict parser for the public-COM draw-parent economy fixture."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re

CASES = ("steady", "viewport", "viewport_scissor", "scalar", "stream_offset",
         "six_constants", "vs_f3_equal", "vs_f3_changed", "vs_f4_changed")
PATHS = ("setters_only", "primitive", "indexed")
SETTERS = {"steady": 0, "viewport": 1, "viewport_scissor": 2, "scalar": 4,
           "stream_offset": 1, "six_constants": 6, "vs_f3_equal": 1,
           "vs_f3_changed": 1, "vs_f4_changed": 1}
UINT64_MAX = (1 << 64) - 1
META_RE = re.compile(r"^draw_parent_meta schema=1 arch=(x86|x64) qpc_frequency=(\d+) "
                     r"groups=(\d+) warm_groups=(\d+) iterations=(\d+) trace=(0|1) "
                     r"pid=(\d+) tid=(\d+)$")
BINDING_RE = re.compile(r"^draw_parent_binding module=(.+) machine=(0x[0-9a-fA-F]+) "
                         r"primitive_rva=(0x[0-9a-fA-F]+) indexed_rva=(0x[0-9a-fA-F]+)$")
ROW_RE = re.compile(
    r"^draw_parent_row case=(\w+) path=(\w+) groups=(\d+) iterations=(\d+) "
    r"qpc_ticks=(\d+) emptyqpc_ticks=(\d+) calls=(\d+) hresult_failures=(\d+) "
    r"status=(0x[0-9a-fA-F]+) initial=(pass|fail) final=(pass|fail) inputs=(pass|fail) "
    r"pixel_center=(0x[0-9a-fA-F]+) pixel_corner=(0x[0-9a-fA-F]+) "
    r"cpu_scope=(\w+) cpu_includes=(\w+) cpu_user_start_100ns=(\d+) "
    r"cpu_user_end_100ns=(\d+) cpu_kernel_start_100ns=(\d+) cpu_kernel_end_100ns=(\d+) "
    r"pid=(\d+) tid=(\d+)$")
TRACE_RE = re.compile(r"^draw_parent_trace phase=(begin|timed_begin|timed_end|end) "
                      r"case=(\w+) path=(\w+) group=(\d+)$")
FOOTER_RE = re.compile(r"^draw_parent_complete schema=1 rows=27 status=pass$")
EXPECTED = {(case, path) for case in CASES for path in PATHS}
KNOWN_HEALTH = ("draw_parent_meta", "draw_parent_binding", "draw_parent_row",
                "draw_parent_trace", "draw_parent_release", "draw_parent_complete")


def owned_lines(text: str) -> list[str]:
    """Extract harness-owned records from logger-prefixed Wine output."""
    result = []
    for raw in text.splitlines():
        if ("FAIL:" in raw or "SKIP:" in raw or "runtime error" in raw.lower()
                or re.search(r"\b(?:err|error|fatal)\s*:|Unhandled exception", raw, re.IGNORECASE)):
            raise ValueError("fixture failure, skip, or runtime error")
        # Paths such as d3d9_draw_parent_economy_x86.exe are runner prose,
        # not owned records. Preserve prefixed log ordering without accepting
        # an unknown record or a second marker hidden on the same line.
        matches = list(re.finditer(r"(?<![\w/\\])(?:draw_parent_[A-Za-z0-9_]+|pe_recorder_chunk)", raw))
        if len(matches) > 1:
            raise ValueError("multiple owned records on one line")
        if not matches:
            continue
        match = matches[0]
        if match.group(0) not in (*KNOWN_HEALTH, "pe_recorder_chunk"):
            raise ValueError("unknown draw-parent record")
        result.append(raw[match.start():])
    return result


def uint(value: str, *, positive: bool = False) -> int:
    if not re.fullmatch(r"[0-9]+", value):
        raise ValueError(f"invalid unsigned value: {value}")
    result = int(value)
    if result > UINT64_MAX or (positive and result == 0):
        raise ValueError(f"invalid unsigned range: {value}")
    return result


def parse_text(text: str, *, expected_arch: str | None = None,
               expected_trace: bool | None = None,
               expected_groups: int | None = None) -> dict:
    lines = owned_lines(text)
    meta_match = [META_RE.fullmatch(line) for line in lines]
    meta_match = [match for match in meta_match if match]
    if len(meta_match) != 1 or sum(line.startswith("draw_parent_meta ") for line in lines) != 1:
        raise ValueError("exactly one valid meta record required")
    arch, frequency, groups, warm_groups, iterations, trace, pid, tid = meta_match[0].groups()
    frequency, groups, warm_groups, iterations, trace, pid, tid = (
        uint(frequency, positive=True), uint(groups), uint(warm_groups), uint(iterations),
        int(trace), uint(pid, positive=True), uint(tid, positive=True))
    if expected_arch is not None and arch != expected_arch:
        raise ValueError("architecture mismatch")
    if expected_trace is not None and bool(trace) != expected_trace:
        raise ValueError("trace mode mismatch")
    if expected_groups is not None and groups != expected_groups:
        raise ValueError("group count mismatch")
    if not 1 <= groups <= 8192 or not 0 <= warm_groups <= 128 or iterations != 32:
        raise ValueError("invalid bounded meta")

    binding = [BINDING_RE.fullmatch(line) for line in lines]
    binding = [match for match in binding if match]
    if len(binding) != 1 or sum(line.startswith("draw_parent_binding ") for line in lines) != 1:
        raise ValueError("exactly one valid binding record required")
    module, machine, primitive, indexed = binding[0].groups()
    if int(machine, 16) != {"x86": 0x14C, "x64": 0x8664}[arch]:
        raise ValueError("binding machine mismatch")
    if not module or uint(str(int(primitive, 16))) == 0 or uint(str(int(indexed, 16))) == 0:
        raise ValueError("invalid binding target")

    rows = {}
    traces = {}
    trace_sequence = []
    footer = release = 0
    complete_index = None
    for index, line in enumerate(lines):
        if not line:
            raise ValueError("blank record")
        if footer:
            raise ValueError("record after completion")
        match = ROW_RE.fullmatch(line)
        if match:
            (case, path, row_groups, row_iterations, qpc, empty, calls, failures,
             status, initial, final, inputs, center, corner, cpu_scope, cpu_includes,
             user_start, user_end, kernel_start, kernel_end, row_pid, row_tid) = match.groups()
            if (case, path) not in EXPECTED or (case, path) in rows:
                raise ValueError("unexpected or duplicate row")
            row_groups, row_iterations = uint(row_groups), uint(row_iterations)
            qpc, empty, calls, failures = uint(qpc, positive=True), uint(empty), uint(calls), uint(failures)
            row_pid, row_tid = uint(row_pid, positive=True), uint(row_tid, positive=True)
            expected_calls = groups * iterations * (SETTERS[case] + (path != "setters_only"))
            if row_groups != groups or row_iterations != iterations or calls != expected_calls:
                raise ValueError("row bounds or call count mismatch")
            if failures or int(status, 16) != 0 or any(value != "pass" for value in (initial, final, inputs)):
                raise ValueError("row result failure")
            if int(center, 16) != 0xFF00FF00 or int(corner, 16) != 0xFF000000:
                raise ValueError("pixel oracle mismatch")
            if cpu_scope != "process" or cpu_includes != "setup_warm_drain_oracle":
                raise ValueError("CPU scope mismatch")
            user_start, user_end, kernel_start, kernel_end = map(uint, (user_start, user_end, kernel_start, kernel_end))
            if user_end < user_start or kernel_end < kernel_start or row_pid != pid or row_tid != tid:
                raise ValueError("non-monotonic CPU or identity mismatch")
            rows[(case, path)] = {"case": case, "path": path, "groups": row_groups,
                                  "iterations": row_iterations, "qpc_ticks": qpc,
                                  "emptyqpc_ticks": empty, "calls": calls,
                                  "hresult_failures": failures, "status": status,
                                  "initial": initial, "final": final, "inputs": inputs,
                                  "pixel_center": center, "pixel_corner": corner,
                                  "cpu_scope": cpu_scope, "cpu_includes": cpu_includes,
                                  "cpu_user_start_100ns": user_start,
                                  "cpu_user_end_100ns": user_end,
                                  "cpu_kernel_start_100ns": kernel_start,
                                  "cpu_kernel_end_100ns": kernel_end,
                                  "pid": row_pid, "tid": row_tid,
                                  "cpu_user_100ns": user_end - user_start,
                                  "cpu_kernel_100ns": kernel_end - kernel_start}
            continue
        match = TRACE_RE.fullmatch(line)
        if match:
            phase, case, path, group = match.groups()
            group = uint(group)
            if (case, path) not in EXPECTED or group >= warm_groups + groups:
                raise ValueError("invalid trace marker")
            key = (case, path, group, phase)
            if key in traces:
                raise ValueError("duplicate trace marker")
            traces[key] = index
            trace_sequence.append((case, path, group, phase))
            continue
        if line.startswith("pe_recorder_chunk"):
            continue
        if line == "draw_parent_release refs=0":
            release += 1
            if release > 1:
                raise ValueError("duplicate release")
            if set(rows) != EXPECTED:
                raise ValueError("release before complete row matrix")
            continue
        if line == "draw_parent_complete schema=1 rows=27 status=pass":
            if release != 1 or set(rows) != EXPECTED:
                raise ValueError("completion before release or rows")
            footer += 1
            complete_index = index
            continue
        if line.startswith("draw_parent_meta ") or line.startswith("draw_parent_binding "):
            continue
        raise ValueError(f"unexpected record: {line[:80]}")
    if set(rows) != EXPECTED or footer != 1 or release != 1 or complete_index != len(lines) - 1:
        raise ValueError("incomplete draw-parent receipt")
    expected_markers = {(case, path, group, phase)
                        for case, path in EXPECTED for group in range(warm_groups + groups)
                        for phase in ("begin", "timed_begin", "timed_end", "end")}
    if trace:
        if set(traces) != expected_markers:
            raise ValueError("trace marker set mismatch")
        expected_sequence = [(case, path, group, phase)
                             for case in CASES for path in PATHS
                             for group in range(warm_groups + groups)
                             for phase in ("begin", "timed_begin", "timed_end", "end")]
        if trace_sequence != expected_sequence:
            raise ValueError("trace marker sequence mismatch")
        if not any(line.startswith("pe_recorder_chunk") for line in lines):
            raise ValueError("trace has no recorder chunk evidence outside timed pairs")
        for case, path in EXPECTED:
            for group in range(warm_groups + groups):
                order = [traces[(case, path, group, phase)]
                         for phase in ("begin", "timed_begin", "timed_end", "end")]
                if order != sorted(order):
                    raise ValueError("unbalanced trace markers")
                if any("pe_recorder_chunk" in line for line in lines[order[1] + 1:order[2]]):
                    raise ValueError("chunk flush inside timed interval")
    elif traces or any(line.startswith("pe_recorder_chunk") for line in lines):
        raise ValueError("trace records in timing mode")
    return {"schema": "dxmt9-pe-draw-parent-economy-v1", "meta": {
                "arch": arch, "qpc_frequency": frequency, "groups": groups,
                "warm_groups": warm_groups, "iterations": iterations, "trace": bool(trace),
                "pid": pid, "tid": tid},
            "binding": {"module": module, "machine": int(machine, 16),
                        "primitive_rva": int(primitive, 16), "indexed_rva": int(indexed, 16)},
            "rows": [rows[key] for key in sorted(rows)],
            "scope": "Raw QPC rows; CPU process time includes setup, warm groups, drains and oracle; no exclusive PE or FPS claim."}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--arch", choices=("x86", "x64"))
    parser.add_argument("--trace", type=int, choices=(0, 1))
    parser.add_argument("--groups", type=int)
    args = parser.parse_args()
    try:
        data = args.log.read_bytes()
        report = parse_text(data.decode("utf-8"), expected_arch=args.arch,
                            expected_trace=None if args.trace is None else bool(args.trace),
                            expected_groups=args.groups)
        report["source"] = {"path": str(args.log), "bytes": len(data),
                            "sha256": hashlib.sha256(data).hexdigest()}
    except (OSError, UnicodeError, ValueError) as error:
        parser.exit(1, f"draw-parent economy rejected: {error}\n")
    output = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.write_text(output, encoding="utf-8")
    else:
        print(output, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
