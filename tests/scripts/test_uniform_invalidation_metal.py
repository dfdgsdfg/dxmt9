"""Compare actual pixels and require execution of the optimized cache branch."""
import hashlib
import os
import re
import subprocess
import sys


def main():
    results = []
    for mode in (0, 1):
        env = os.environ | {
            "DXMT_PERF_COUNTERS": "1",
            "DXMT9_REPLAY_PRECISE_UNIFORM_INVALIDATION": str(mode),
            "DXMT9_PERF_UNIFORM_DEPENDENCY_PROBE": "0",
        }
        result = subprocess.run([sys.argv[1]], env=env, capture_output=True,
                                text=True, timeout=60)
        if result.returncode == 77:
            if results:
                raise RuntimeError("Metal became unavailable between OFF/ON runs")
            print(result.stdout)
            return 77
        if result.returncode:
            sys.stderr.write(result.stderr)
            raise RuntimeError(f"pixel child mode={mode} exited {result.returncode}")
        frames = [line for line in result.stdout.splitlines() if line.startswith("frame=")]
        assert len(frames) == 30, f"mode={mode}: missing frame readbacks"
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
        print(f"mode={mode} frames={len(frames)} reuse_full/nonconst/build={counts}")
        results.append((frames, counts))
    assert results[0][0] == results[1][0], "OFF/ON pixel bytes differ"
    assert results[1][1][0] >= results[0][1][0] + 9, "nine irrelevant RS edits must reuse"
    assert results[0][1][2] >= results[1][1][2] + 9, "nine full rebuilds must disappear"
    digest = hashlib.sha256("\n".join(results[0][0]).encode()).hexdigest()
    print(f"30 exact OFF/ON frames accepted; pixel transcript sha256={digest}")


if __name__ == "__main__":
    sys.exit(main())
