#!/usr/bin/env python3
import os
import re
import subprocess
import sys


COUNTER_RE = re.compile(r"\b([a-zA-Z0-9_]+)=([^\s]+)")
PERF_PREFIX = "[dxmt9-perf] "


def fail(message: str) -> int:
    print(f"assert_perf_counters.py: {message}", file=sys.stderr)
    return 1


def main() -> int:
    if len(sys.argv) < 2:
        return fail("usage: assert_perf_counters.py <executable>")

    env = os.environ.copy()
    env["DXMT_PERF_COUNTERS"] = "1"
    result = subprocess.run(
        sys.argv[1:],
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode != 0:
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        return result.returncode

    perf_lines = [
        line for line in result.stderr.splitlines() if line.startswith(PERF_PREFIX)
    ]
    if len(perf_lines) != 1:
        return fail(f"expected one {PERF_PREFIX!r} line, got {len(perf_lines)}")

    counters = dict(COUNTER_RE.findall(perf_lines[0]))
    expected = {
        "metal_buffers": "2",
        "metal_buffer_bytes": "768",
        "draw_geometry_samples": "1",
        "draw_geometry_ffp": "1",
        "draw_geometry_vs": "0",
        "draw_geometry_indexed": "1",
        "draw_geometry_index16": "1",
        "draw_geometry_index32": "0",
        "draw_geometry_direct": "0",
        "draw_geometry_up": "1",
        "draw_geometry_expanded": "0",
        "draw_geometry_nonzero_base_vertex": "1",
        "draw_geometry_nonzero_start_index": "1",
        "draw_geometry_nonzero_stream0_offset": "1",
        "draw_geometry_last_stream0_stride": "32",
        "draw_geometry_last_decl_hash": "0x1234",
        "uniform_vs_consts_calls": "0",
        "uniform_vs_consts_bytes": "0",
        "uniform_ps_consts_calls": "0",
        "uniform_ps_consts_bytes": "0",
        "uniform_ffp_vs_calls": "0",
        "uniform_ffp_vs_bytes": "0",
        "uniform_ffp_ps_calls": "0",
        "uniform_ffp_ps_bytes": "0",
        "uniform_volatile_pushes": "0",
    }
    for key, value in expected.items():
        actual = counters.get(key)
        if actual != value:
            return fail(f"expected {key}={value}, got {actual!r}")

    sys.stdout.write(result.stdout)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
