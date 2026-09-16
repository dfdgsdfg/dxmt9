"""R-BACK-2.15.4: exercise real snapshot work in fresh observer-mode processes."""

import os
import re
import subprocess
import sys


def run(executable, mode, perf=True):
    env = dict(os.environ)
    env["DXMT_PERF_COUNTERS"] = "1" if perf else "0"
    env.pop("DXMT9_PERF_UNIFORM_HASH_OBSERVER_MODE", None)
    if mode is not None:
        env["DXMT9_PERF_UNIFORM_HASH_OBSERVER_MODE"] = mode
    result = subprocess.run([executable], env=env, capture_output=True,
                            text=True, timeout=60)
    if result.returncode:
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        raise AssertionError(f"semantic fixture failed: {mode}")
    lines = [line for line in result.stderr.splitlines()
             if line.startswith("[dxmt9-perf] ")]
    if not perf:
        assert not lines, "disabled perf must not publish"
        return {}
    assert len(lines) == 1
    return {key: float(value) for key, value in re.findall(
        r"\b([a-z][a-z0-9_]+)=([0-9.]+)", lines[0])}


def main():
    executable = sys.argv[1]
    rows = {mode: run(executable, mode)
            for mode in (None, "full", "counts", "parent", "invalid")}
    reference = rows["full"]
    prefix = "d3d9_snapshot_uniform_build_"
    parent = prefix + "hash_cpu_ms"
    child_times = [key for key in reference if key.startswith(prefix)
                   and "hash" in key and key.endswith("_cpu_ms") and key != parent]
    child_counts = [key for key in reference if key.startswith(prefix)
                    and "hash" in key and not key.endswith("_ms")]
    assert child_times and child_counts
    assert reference[prefix + "calls"] > 0, "must enter production cache"
    assert reference[prefix + "vs_const_hash_bytes"] > 0
    stable = {key: value for key, value in reference.items()
              if key.startswith(("d3d9_snapshot_", "draw_uniform_"))
              and not key.endswith("_ms") and key not in child_counts
              and "_uniform_build_" not in key}
    for mode, row in rows.items():
        assert parent in row, "parent scope must remain published"
        assert row[prefix + "calls"] == reference[prefix + "calls"]
        assert {key: row[key] for key in stable} == stable
        assert all(row[key] == (0 if mode == "parent" else reference[key])
                   for key in child_counts), f"child count gate: {mode}"
        if mode in ("counts", "parent"):
            assert all(row[key] == 0 for key in child_times), mode
    # Disabled-path semantics execute too; the selector cannot activate perf.
    for mode in ("full", "counts", "parent"):
        run(executable, mode, perf=False)
    print("uniform-hash observer modes: semantic fixtures and counter gates pass")


if __name__ == "__main__":
    main()
