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
META_V1_RE = re.compile(r"^draw_parent_meta schema=1 arch=(x86|x64) qpc_frequency=(\d+) "
                        r"groups=(\d+) warm_groups=(\d+) iterations=(\d+) trace=(0|1) "
                        r"pid=(\d+) tid=(\d+)$")
META_V2_RE = re.compile(r"^draw_parent_meta schema=2 arch=(x86|x64) qpc_frequency=(\d+) "
                        r"groups=(\d+) warm_groups=(\d+) iterations=(\d+) trace=(0|1) "
                        r"group_detail=1 pid=(\d+) tid=(\d+)$")
META_V3_RE = re.compile(r"^draw_parent_meta schema=3 arch=(x86|x64) qpc_frequency=(\d+) "
                        r"groups=(\d+) warm_groups=(\d+) iterations=(\d+) trace=(0|1) "
                        r"group_detail=1 tail_detail=1 pid=(\d+) tid=(\d+)$")
META_V4_RE = re.compile(r"^draw_parent_meta schema=4 arch=(x86|x64) qpc_frequency=(\d+) "
                        r"groups=(\d+) warm_groups=(\d+) iterations=(\d+) trace=(0|1) "
                        r"group_detail=1 tail_detail=1 completion_window=(\d+) "
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
GROUP_RE = re.compile(r"^draw_parent_group case=(\w+) path=(\w+) group=(\d+) "
                      r"measured=(0|1) qpc_ticks=(\d+) tail_qpc_ticks=(\d+) "
                      r"tail_getdata_calls=(\d+)$")
TAIL_RE = re.compile(r"^draw_parent_tail case=(\w+) path=(\w+) group=(\d+) "
                     r"end_scene_qpc_ticks=(\d+) issue_qpc_ticks=(\d+) "
                     r"getdata_qpc_ticks=(\d+) other_qpc_ticks=(\d+)$")
EXPECTED = {(case, path) for case in CASES for path in PATHS}
KNOWN_HEALTH = ("draw_parent_meta", "draw_parent_binding", "draw_parent_row",
                "draw_parent_group", "draw_parent_tail", "draw_parent_trace",
                "draw_parent_release", "draw_parent_complete")


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


def distribution(values: list[int]) -> dict[str, int | None]:
    if not values:
        return {"min": None, "p50": None, "p90": None, "p99": None, "max": None,
                "sum": 0, "count": 0}
    ordered = sorted(values)
    def nearest(percent: int) -> int:
        return ordered[(percent * len(ordered) + 99) // 100 - 1]
    total = sum(ordered)
    if total > UINT64_MAX:
        raise ValueError("distribution sum overflow")
    return {"min": ordered[0], "p50": nearest(50), "p90": nearest(90),
            "p99": nearest(99), "max": ordered[-1], "sum": total,
            "count": len(ordered)}


def bounded_sum(values: list[int], what: str) -> int:
    total = sum(values)
    if total > UINT64_MAX:
        raise ValueError(f"{what} sum overflow")
    return total


def completion_boundary(ordinal: int, warm_groups: int, groups: int,
                        window: int) -> bool:
    """Return whether a phase-local group must Issue/GetData-complete."""
    if ordinal < warm_groups:
        local_ordinal, phase_groups = ordinal + 1, warm_groups
    else:
        local_ordinal, phase_groups = ordinal - warm_groups + 1, groups
    return local_ordinal % window == 0 or local_ordinal == phase_groups


def parse_text(text: str, *, expected_arch: str | None = None,
               expected_trace: bool | None = None,
               expected_groups: int | None = None,
               expected_group_detail: bool | None = None,
               expected_tail_detail: bool | None = None,
               expected_completion_window: int | None = None) -> dict:
    lines = owned_lines(text)
    meta_v1 = [META_V1_RE.fullmatch(line) for line in lines]
    meta_v2 = [META_V2_RE.fullmatch(line) for line in lines]
    meta_v3 = [META_V3_RE.fullmatch(line) for line in lines]
    meta_v4 = [META_V4_RE.fullmatch(line) for line in lines]
    meta_v1 = [match for match in meta_v1 if match]
    meta_v2 = [match for match in meta_v2 if match]
    meta_v3 = [match for match in meta_v3 if match]
    meta_v4 = [match for match in meta_v4 if match]
    if (len(meta_v1) + len(meta_v2) + len(meta_v3) + len(meta_v4) != 1
            or sum(line.startswith("draw_parent_meta ") for line in lines) != 1):
        raise ValueError("exactly one valid meta record required")
    schema = 4 if meta_v4 else (3 if meta_v3 else (2 if meta_v2 else 1))
    group_detail = schema >= 2
    tail_detail = schema >= 3
    completion_window = 1
    if schema == 4:
        meta_match = meta_v4[0]
        (arch, frequency, groups, warm_groups, iterations, trace,
         completion_window, pid, tid) = meta_match.groups()
    else:
        meta_match = meta_v3[0] if tail_detail else (meta_v2[0] if group_detail else meta_v1[0])
        arch, frequency, groups, warm_groups, iterations, trace, pid, tid = meta_match.groups()
    frequency, groups, warm_groups, iterations, trace, pid, tid = (
        uint(frequency, positive=True), uint(groups), uint(warm_groups), uint(iterations),
        int(trace), uint(pid, positive=True), uint(tid, positive=True))
    completion_window = uint(str(completion_window))
    if expected_arch is not None and arch != expected_arch:
        raise ValueError("architecture mismatch")
    if expected_trace is not None and bool(trace) != expected_trace:
        raise ValueError("trace mode mismatch")
    if expected_groups is not None and groups != expected_groups:
        raise ValueError("group count mismatch")
    if expected_group_detail is not None and group_detail != expected_group_detail:
        raise ValueError("group-detail mode mismatch")
    if expected_tail_detail is not None and tail_detail != expected_tail_detail:
        raise ValueError("tail-detail mode mismatch")
    if expected_completion_window is not None and completion_window != expected_completion_window:
        raise ValueError("completion-window mismatch")
    if schema == 4 and not 2 <= completion_window <= 64:
        raise ValueError("invalid bounded completion window")
    if schema < 4 and completion_window != 1:
        raise ValueError("legacy schema completion window mismatch")
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
    groups_by_key = {}
    group_sequence = []
    group_indices = []
    tails_by_key = {}
    tail_sequence = []
    pending_tail = None
    traces = {}
    trace_sequence = []
    footer = release = 0
    complete_index = None
    for index, line in enumerate(lines):
        if not line:
            raise ValueError("blank record")
        if footer:
            raise ValueError("record after completion")
        if pending_tail is not None and not TAIL_RE.fullmatch(line):
            raise ValueError("tail detail must immediately follow its group")
        match = TAIL_RE.fullmatch(line)
        if match:
            if not tail_detail or pending_tail is None or rows:
                raise ValueError("unexpected tail detail")
            case, path, ordinal, end_scene, issue, getdata, other = match.groups()
            ordinal = uint(ordinal)
            values = [uint(value) for value in (end_scene, issue, getdata, other)]
            key = (case, path, ordinal)
            if key != pending_tail or key in tails_by_key:
                raise ValueError("invalid or duplicate tail detail identity")
            group = groups_by_key.get(key)
            if group is None or bounded_sum(values, "tail component") != group["tail_qpc_ticks"]:
                raise ValueError("tail detail sum does not match group")
            if schema == 4:
                draining = completion_boundary(ordinal, warm_groups, groups, completion_window)
                if not draining and (group["tail_getdata_calls"] != 0
                                     or values[1] != 0 or values[2] != 0):
                    raise ValueError("non-draining group has completion work")
            tails_by_key[key] = {"case": case, "path": path, "group": ordinal,
                                 "end_scene_qpc_ticks": values[0],
                                 "issue_qpc_ticks": values[1],
                                 "getdata_qpc_ticks": values[2],
                                 "other_qpc_ticks": values[3]}
            tail_sequence.append(key)
            pending_tail = None
            continue
        match = GROUP_RE.fullmatch(line)
        if match:
            if not group_detail:
                raise ValueError("group detail is not valid in schema 1")
            if rows:
                raise ValueError("group detail must precede all row records")
            case, path, ordinal, measured, qpc, tail, polls = match.groups()
            ordinal, measured = uint(ordinal), int(measured)
            qpc, tail, polls = uint(qpc), uint(tail), uint(polls, positive=schema < 4)
            key = (case, path)
            if key not in EXPECTED or ordinal >= warm_groups + groups:
                raise ValueError("invalid group-detail identity")
            if measured != int(ordinal >= warm_groups):
                raise ValueError("invalid measured group flag")
            if schema == 4:
                draining = completion_boundary(ordinal, warm_groups, groups, completion_window)
                if draining != (polls != 0):
                    raise ValueError("group completion boundary mismatch")
            group_key = (case, path, ordinal)
            if group_key in groups_by_key or key in rows:
                raise ValueError("duplicate or post-row group detail")
            groups_by_key[group_key] = {"case": case, "path": path, "group": ordinal,
                                        "measured": bool(measured), "qpc_ticks": qpc,
                                        "tail_qpc_ticks": tail,
                                        "tail_getdata_calls": polls}
            group_sequence.append(group_key)
            group_indices.append(index)
            if tail_detail:
                pending_tail = group_key
            continue
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
            if group_detail:
                detail = [groups_by_key.get((case, path, ordinal))
                          for ordinal in range(warm_groups + groups)]
                if any(item is None for item in detail):
                    raise ValueError("row is missing group detail")
                measured_sum = bounded_sum(
                    [item["qpc_ticks"] for item in detail if item["measured"]],
                    "measured group QPC")
                if measured_sum != qpc:
                    raise ValueError("measured group QPC sum does not match row")
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
        complete_line = f"draw_parent_complete schema={schema} rows=27 status=pass"
        if line == complete_line:
            if release != 1 or set(rows) != EXPECTED:
                raise ValueError("completion before release or rows")
            footer += 1
            complete_index = index
            continue
        if line.startswith("draw_parent_meta ") or line.startswith("draw_parent_binding "):
            continue
        raise ValueError(f"unexpected record: {line[:80]}")
    if (set(rows) != EXPECTED or footer != 1 or release != 1
            or complete_index != len(lines) - 1 or pending_tail is not None):
        raise ValueError("incomplete draw-parent receipt")
    if group_detail:
        expected_group_keys = {(case, path, ordinal)
                               for case, path in EXPECTED
                               for ordinal in range(warm_groups + groups)}
        if set(groups_by_key) != expected_group_keys:
            raise ValueError("group-detail set mismatch")
        expected_group_sequence = [(case, path, ordinal)
                                   for case in CASES for path in PATHS
                                   for ordinal in range(warm_groups + groups)]
        if group_sequence != expected_group_sequence:
            raise ValueError("group-detail sequence mismatch")
        if trace and (not traces or min(group_indices) <= max(traces.values())):
            raise ValueError("traced group samples must follow the complete trace loop")
        if tail_detail:
            if set(tails_by_key) != expected_group_keys or tail_sequence != expected_group_sequence:
                raise ValueError("tail-detail set or sequence mismatch")
    elif tails_by_key:
        raise ValueError("tail detail without group detail")
    elif groups_by_key:
        raise ValueError("group detail in schema 1")
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
        for case, path in sorted(EXPECTED):
            for group in range(warm_groups + groups):
                order = [traces[(case, path, group, phase)]
                         for phase in ("begin", "timed_begin", "timed_end", "end")]
                if order != sorted(order):
                    raise ValueError("unbalanced trace markers")
                if any("pe_recorder_chunk" in line for line in lines[order[1] + 1:order[2]]):
                    raise ValueError("chunk flush inside timed interval")
    elif traces or any(line.startswith("pe_recorder_chunk") for line in lines):
        raise ValueError("trace records in timing mode")
    report = {"schema": f"dxmt9-pe-draw-parent-economy-v{schema}", "meta": {
                "arch": arch, "qpc_frequency": frequency, "groups": groups,
                "warm_groups": warm_groups, "iterations": iterations, "trace": bool(trace),
                "pid": pid, "tid": tid},
            "binding": {"module": module, "machine": int(machine, 16),
                        "primitive_rva": int(primitive, 16), "indexed_rva": int(indexed, 16)},
            "rows": [rows[key] for key in sorted(rows)],
            "scope": "Raw QPC rows; CPU process time includes setup, warm groups, drains and oracle; no exclusive PE or FPS claim."}
    if group_detail:
        detail_rows = [groups_by_key[key] for key in expected_group_sequence]
        report["group_rows"] = detail_rows
        report["meta"]["group_detail"] = True
        report["group_distributions"] = {}
        for case, path in sorted(EXPECTED):
            owned = [item for item in detail_rows
                     if item["case"] == case and item["path"] == path]
            phases = {}
            for category, predicate in (("warm", lambda item: not item["measured"]),
                                         ("measured", lambda item: item["measured"])):
                selected = [item for item in owned if predicate(item)]
                phases[category] = {
                    "qpc_ticks": distribution([item["qpc_ticks"] for item in selected]),
                    "tail_qpc_ticks": distribution([item["tail_qpc_ticks"] for item in selected]),
                    "tail_getdata_calls": distribution([item["tail_getdata_calls"] for item in selected]),
                }
            report["group_distributions"][f"{case}/{path}"] = phases
        if tail_detail:
            detail_tails = [tails_by_key[key] for key in expected_group_sequence]
            report["tail_rows"] = detail_tails
            report["meta"]["tail_detail"] = True
            report["tail_distributions"] = {}
            for case, path in sorted(EXPECTED):
                owned = [item for item in detail_tails
                         if item["case"] == case and item["path"] == path]
                phases = {}
                for phase, measured in (("warm", False), ("measured", True)):
                    selected = [item for item in owned
                                if (item["group"] >= warm_groups) == measured]
                    phases[phase] = {
                        field: distribution([item[field] for item in selected])
                        for field in ("end_scene_qpc_ticks", "issue_qpc_ticks",
                                      "getdata_qpc_ticks", "other_qpc_ticks")
                    }
                report["tail_distributions"][f"{case}/{path}"] = phases
        if schema == 4:
            report["meta"]["completion_window"] = completion_window
            report["completion_summary"] = {}
            for case, path in sorted(EXPECTED):
                owned = [item for item in detail_rows
                         if item["case"] == case and item["path"] == path]
                phases = {}
                for phase, measured in (("warm", False), ("measured", True)):
                    selected = [item for item in owned
                                if (item["group"] >= warm_groups) == measured]
                    completed = [item for item in selected
                                 if completion_boundary(item["group"], warm_groups,
                                                        groups, completion_window)]
                    phases[phase] = {
                        "count": len(completed),
                        "tail_qpc_ticks_sum": bounded_sum(
                            [item["tail_qpc_ticks"] for item in completed],
                            "completion tail QPC"),
                    }
                report["completion_summary"][f"{case}/{path}"] = phases
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--arch", choices=("x86", "x64"))
    parser.add_argument("--trace", type=int, choices=(0, 1))
    parser.add_argument("--groups", type=int)
    parser.add_argument("--group-detail", type=int, choices=(0, 1))
    parser.add_argument("--tail-detail", type=int, choices=(0, 1))
    parser.add_argument("--completion-window", type=int)
    args = parser.parse_args()
    try:
        data = args.log.read_bytes()
        report = parse_text(data.decode("utf-8"), expected_arch=args.arch,
                            expected_trace=None if args.trace is None else bool(args.trace),
                            expected_groups=args.groups,
                            expected_group_detail=None if args.group_detail is None else bool(args.group_detail),
                            expected_tail_detail=None if args.tail_detail is None else bool(args.tail_detail),
                            expected_completion_window=args.completion_window)
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
