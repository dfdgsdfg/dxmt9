"""Compare actual pixels and require execution of the optimized cache branch."""
import hashlib
import os
import re
import subprocess
import sys


def main():
    def run_case(label, args, obsolete_selector):
        env = os.environ.copy()
        env.update({
            "DXMT_PERF_COUNTERS": "1",
            "DXMT9_PERF_UNIFORM_DEPENDENCY_PROBE": "0",
        })
        if obsolete_selector is None:
            env.pop("DXMT9_REPLAY_PRECISE_UNIFORM_INVALIDATION", None)
        else:
            env["DXMT9_REPLAY_PRECISE_UNIFORM_INVALIDATION"] = obsolete_selector
        result = subprocess.run([sys.argv[1], *args], env=env,
                                capture_output=True, text=True, timeout=60)
        if result.returncode == 77:
            if label != "reference":
                raise RuntimeError("Metal became unavailable between oracle runs")
            print(result.stdout)
            return 77
        if result.returncode:
            sys.stderr.write(result.stderr)
            raise RuntimeError(f"pixel child case={label} exited {result.returncode}")
        frames = [line for line in result.stdout.splitlines() if line.startswith("frame=")]
        assert len(frames) == 30, f"case={label}: missing frame readbacks"
        for frame in frames:
            assert len(bytes.fromhex(frame.split("pixels=", 1)[1])) == 64 * 64 * 4
        perf = [line for line in result.stderr.splitlines() if line.startswith("[dxmt9-perf] ")]
        assert perf, "missing path-execution evidence"
        counters = dict(re.findall(r"\b([a-zA-Z0-9_]+)=([^\s]+)", perf[-1]))
        for key in ("gpu_command_buffer_errors", "chunk_reject", "draw_skipped_no_pipeline",
                    "pipeline_build_fail_library"):
            assert int(counters[key]) == 0, (key, counters[key])
        assert int(counters["draw_calls"]) == 30, "all 30 draws must reach Metal"
        keys = ["d3d9_snapshot_cache_batch_miss_uniform_payload_" + suffix
                for suffix in ("reuse_full", "reuse_nonconst", "full_build")]
        counts = [int(counters[key]) for key in keys]
        assert counts[2] > 0, "fixture must build initial/conservatively invalidated payloads"
        print(f"case={label} frames={len(frames)} reuse_full/nonconst/build={counts}")
        return frames, counts

    # The serial reference is a test-only CLI mode that forces a generic
    # invalidation after setters. Production has no selector: run it once with
    # the selector unset and again with each retired value to prove both are
    # inert.
    reference = run_case("reference", ["--serial-reference"], None)
    if reference == 77:
        return 77
    production_unset = run_case("production-unset", [], None)
    production_zero = run_case("production-obsolete-0", [], "0")
    production_one = run_case("production-obsolete-1", [], "1")

    assert reference[0] == production_unset[0], "reference/production pixel bytes differ"
    assert production_unset[0] == production_zero[0] == production_one[0], \
        "retired selector values changed production pixels"
    assert production_unset[1] == production_zero[1] == production_one[1], \
        "retired selector values changed production cache paths"
    assert production_unset[1][0] >= reference[1][0] + 9, \
        "nine irrelevant RS edits must reuse full payloads"
    assert reference[1][2] >= production_unset[1][2] + 9, \
        "nine serial-reference full rebuilds must disappear"
    digest = hashlib.sha256("\n".join(production_unset[0]).encode()).hexdigest()
    assert digest == "ef83939770ce9b6e6728bd564b5fa8207f38a0113683d8d61df552ad3d955fd7", \
        f"unexpected pixel transcript sha256={digest}"
    print(f"30 exact reference/production frames accepted; pixel transcript sha256={digest}")


if __name__ == "__main__":
    sys.exit(main())
