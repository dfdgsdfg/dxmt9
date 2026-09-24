"""Sequential fixed-capture A/A, A/B, B/A and nested-timer control.

This records elapsed scope sums, not thread CPU or pipeline critical-path time.
Run only when builds and other benchmarks have stopped.
"""
import argparse
import hashlib
import json
import pathlib
import statistics
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--source-plan-sha256", required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    provider = pathlib.Path("build-x86_64-builtin/tools/dxmt9-render-tape-provider").resolve()
    digest = hashlib.sha256(provider.read_bytes()).hexdigest()
    launcher = pathlib.Path("scripts/run_python.sh").resolve()
    runner = pathlib.Path("scripts/tools/run_dxmt9_render_tape.py").resolve()
    import os
    # Keep all inherited renderer settings in the runner's receipt, and require
    # them to remain identical except for these two explicitly controlled knobs.
    env = dict(os.environ, DXMT_PERF_COUNTERS="1")
    baseline_input = None
    baseline_images = None
    baseline_pixels = None
    reports = {}
    for label, enabled, timing in (
        ("aa0", "0", "1"), ("aa1", "0", "1"),
        ("ab1", "1", "1"), ("ba1", "1", "1"),
        ("ba0", "0", "1"), ("timer_off1", "1", "0"),
    ):
        output = args.output / label
        child_env = dict(env, DXMT9_PREPARED_SERIAL_READS=enabled,
                         DXMT9_PREPARED_BUILD_TIMING=timing)
        command = [str(launcher), str(runner), "experiment", str(args.bundle.resolve()),
                   "--provider", str(provider), "--validator", "build/tools/dxmt9-render-tape",
                   "--output-dir", str(output), "--warmup", "2", "--repeat", "16",
                   "--reset", "cold-device", "--replay-route", "production-slot",
                   "--source-boundary-mode", "capture-source", "--source-plan-sha256",
                   args.source_plan_sha256, "--partition-mode", "identity",
                   "--require-path", "--timeout-seconds", "300"]
        print(label, "starting", flush=True)
        completed = subprocess.run(command, env=child_env, text=True, capture_output=True)
        (args.output / (label + ".stdout")).write_text(completed.stdout)
        (args.output / (label + ".stderr")).write_text(completed.stderr)
        if completed.returncode:
            raise RuntimeError(f"{label} failed; partial evidence retained")
        receipt = json.loads((output / "result.json").read_text())
        assert receipt["accepted"] and receipt["readback_artifacts_valid"]
        assert receipt["loaded_images_stable"] and not receipt["diagnostic"]
        if baseline_images is None:
            baseline_images = receipt["result"]["loaded_images"]
        assert receipt["result"]["loaded_images"] == baseline_images
        assert receipt["provider"]["sha256"] == digest
        assert hashlib.sha256(provider.read_bytes()).hexdigest() == digest
        if baseline_input is None:
            baseline_input = receipt["input_components"]
        assert receipt["input_components"] == baseline_input
        samples = []
        for iteration in receipt["result"]["iterations"]:
            assert iteration["reset_ok"]
            for artifact in iteration["readback_artifacts"].values():
                data = (output / "readback" / artifact["path"]).read_bytes()
                assert len(data) == artifact["bytes"]
                assert hashlib.sha256(data).hexdigest() == artifact["sha256"]
                if baseline_pixels is None:
                    baseline_pixels = data
                assert data == baseline_pixels, "pixel drift"
            scopes = iteration["cpu_scope_wall_ns"]
            assert (scopes["prepared_reads"] > 0) == (enabled == "1")
            # H359 fuses marking into preparation; do not label this as the
            # historical isolated build scope in the output or comparisons.
            prepared_scope = scopes.get("prepared_build_and_mark", scopes.get("prepared_build", 0))
            assert (prepared_scope > 0) == (enabled == timing == "1")
            if not iteration["warmup"]:
                samples.append(dict(scopes, service=iteration["service_ns"],
                                    chunk_replay=iteration["phases_wall_ns"]["chunk_replay"]))
        assert len(samples) == 16
        reports[label] = {
            "selector": enabled, "timing": timing, "samples": samples,
            "median_ms": {k: statistics.median(s[k] for s in samples) / 1e6
                          for k in samples[0] if k not in ("prepared_reads", "prepared_read_fallbacks")},
            "prepared_reads": sorted({s["prepared_reads"] for s in samples}),
        }
        (args.output / "summary.json").write_text(json.dumps({
            "scope": "elapsed wall; nested and concurrent scopes are not additive",
            "provider_sha256": digest, "source_plan_sha256": args.source_plan_sha256,
            "reports": reports,
        }, indent=2) + "\n")
        print(label, reports[label]["median_ms"], flush=True)


if __name__ == "__main__":
    main()
