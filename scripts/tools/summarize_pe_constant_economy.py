#!/usr/bin/env python3
"""Strict R-BENCH-6 actual-PE constant-setter receipt parser (not an FPS oracle)."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re


TAG = "[dxmt9-pe-const-economy]"
FAMILIES = ("vs_float", "ps_float", "vs_int", "ps_int", "vs_bool", "ps_bool")
EXPECTED = {(family, count, shape) for family in FAMILIES
            for count, shape in ((0, "equal"), (1, "equal"), (1, "last"), (1, "all"),
                                 (3, "equal"), (3, "last"), (3, "all"), (3, "middle"),
                                 (4, "equal"), (4, "last"), (4, "all"))}
NUMERIC = {"count", "calls", "warmup", "start", "thread_id", "process_id", "qpc_ticks",
           "qpc_frequency", "kernel_100ns", "user_100ns", "hr_or", "precheck", "postcheck", "source_unchanged"}
FIELDS = NUMERIC | {"family", "shape", "cpu_scope"}
FOOTER = "PASS:pe_const_economy cells=66"
TERMINAL = "inline_constants: setter economy passed (no pixel oracle)"
UINT64_MAX = (1 << 64) - 1


def uint64(value: str) -> int:
    if not re.fullmatch(r"[0-9]+", value) or len(value) > 20:
        raise ValueError(f"invalid unsigned value: {value!r}")
    number = int(value)
    if number > UINT64_MAX:
        raise ValueError("uint64 overflow")
    return number


def parse_text(text: str) -> dict:
    cells = {}
    footer = terminal = release = main_pass = 0
    thread_id = process_id = frequency = None
    for raw in text.splitlines():
        line = raw.strip()
        if "FAIL:" in line or "SKIP:" in line:
            raise ValueError("fixture failure or skip")
        if "PIXEL_SIGNATURE" in line or "real PE pixel oracle" in line:
            raise ValueError("pixel selector is not setter economy")
        if "[dxmt9-pe-const-economy" in line:
            if not line.startswith(TAG + " ") or footer:
                raise ValueError("malformed or post-footer cell")
            values = {}
            for item in line[len(TAG):].split():
                if item.count("=") != 1:
                    raise ValueError("malformed field")
                key, value = item.split("=")
                if key in values:
                    raise ValueError("duplicate field")
                values[key] = value
            if values.keys() != FIELDS:
                raise ValueError("wrong cell schema")
            if values["cpu_scope"] != "process":
                raise ValueError("unqualified CPU scope")
            values.update({key: uint64(values[key]) for key in NUMERIC})
            key = values["family"], values["count"], values["shape"]
            if key not in EXPECTED or key in cells:
                raise ValueError("unexpected or duplicate cell")
            for field, expected in (("calls", 50_000_000), ("warmup", 10_000),
                                    ("start", 8), ("hr_or", 0), ("precheck", 1),
                                    ("postcheck", 1), ("source_unchanged", 1)):
                if values[field] != expected:
                    raise ValueError(f"invalid {field}")
            if not 0 < values["thread_id"] <= 0xffffffff:
                raise ValueError("invalid thread identity")
            if not 0 < values["process_id"] <= 0xffffffff:
                raise ValueError("invalid process identity")
            if not 0 < values["qpc_ticks"] < (1 << 63) or not 0 < values["qpc_frequency"] < (1 << 63):
                raise ValueError("invalid QPC interval or frequency")
            total_cpu = values["kernel_100ns"] + values["user_100ns"]
            if not 0 < total_cpu <= UINT64_MAX:
                raise ValueError("invalid CPU interval")
            if thread_id is not None and (thread_id != values["thread_id"] or
                    process_id != values["process_id"] or frequency != values["qpc_frequency"]):
                raise ValueError("process, thread or clock frequency changed")
            thread_id, frequency = values["thread_id"], values["qpc_frequency"]
            process_id = values["process_id"]
            values["wall_ns_per_call"] = values["qpc_ticks"] * 1e9 / frequency / values["calls"]
            values["process_cpu_ns_per_call"] = total_cpu * 100 / values["calls"]
            cells[key] = values
        elif "PASS:pe_const_economy" in line:
            if line != FOOTER or footer or cells.keys() != EXPECTED:
                raise ValueError("incomplete or duplicate matrix footer")
            footer += 1
        elif line.startswith("REACHED:inline_constants:device_release="):
            if line != "REACHED:inline_constants:device_release=0" or not footer or release:
                raise ValueError("invalid device teardown")
            release += 1
        elif line == "PASS:inline_constants":
            if not release or main_pass:
                raise ValueError("invalid main completion")
            main_pass += 1
        elif line == TERMINAL:
            if not main_pass or terminal:
                raise ValueError("invalid terminal completion")
            terminal += 1
    if cells.keys() != EXPECTED or (footer, release, main_pass, terminal) != (1, 1, 1, 1):
        raise ValueError("missing cells or complete successful termination")
    return dict(schema="dxmt9-pe-constant-economy", cells=[cells[key] for key in sorted(cells)],
                thread_id=thread_id, process_id=process_id, qpc_frequency=frequency,
                cpu_scope="process",
                scope="Warm public setter plus harness loop and any process helper threads. No control subtraction, PE-/thread-exclusive attribution, dirty-consumption, whole-frame CPU, FPS or pixel claim.")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        data = args.log.read_bytes()
        report = parse_text(data.decode("utf-8"))
        report["source"] = dict(path=str(args.log), bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
    except (OSError, UnicodeError, ValueError) as error:
        parser.exit(1, f"constant economy rejected: {error}\n")
    output = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.write_text(output, encoding="utf-8")
    else:
        print(output, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
