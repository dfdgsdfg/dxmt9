#!/usr/bin/env python3
"""Regression tests for 3DMark05 perf probe shell wrappers."""

from __future__ import annotations

import json
import importlib.util
import csv
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
RUN_WRAPPER = REPO_ROOT / "scripts" / "tools" / "run_3dmark05_perf_probe.sh"
FINALIZER = REPO_ROOT / "scripts" / "tools" / "finalize_3dmark05_perf_probe.sh"
CAPTURE_LAYER_WRAPPER = REPO_ROOT / "scripts" / "tools" / "run_with_wine_metal_capture_layer.sh"
SYSTEM_TRACE_SIDECAR = REPO_ROOT / "scripts" / "tools" / "run_3dmark05_system_trace_sidecar.sh"
SUMMARIZER = REPO_ROOT / "scripts" / "tools" / "summarize_3dmark05_perf.py"
XCODE_SUMMARIZER = REPO_ROOT / "scripts" / "tools" / "summarize_xcode_encoder_counters.py"
RUN_EXPERIMENT = REPO_ROOT / "scripts" / "run_apps" / "run_experiment.py"
DIRECT_WRAPPER = REPO_ROOT / "scripts" / "run_apps" / "run_app-d3d9-3dmark05-verify_direct.sh"
LAUNCHER = REPO_ROOT / "experiments" / "launchers" / "app-d3d9-3dmark05.sh"
RUN_WITH_TIMEOUT = REPO_ROOT / "scripts" / "tools" / "run_with_timeout.py"


def load_xcode_summarizer():
    spec = importlib.util.spec_from_file_location("summarize_xcode_encoder_counters", XCODE_SUMMARIZER)
    assert spec is not None
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def write_result(path: Path) -> None:
    path.mkdir(parents=True, exist_ok=True)
    path.joinpath("result.json").write_text(
        json.dumps({"dxmt9_perf_counters": {}}),
        encoding="utf-8",
    )


PACING_COMPARE_FLAGS = (
    "--require-completion-present-wait-decrease",
    "--require-completion-wait-with-enqueue-increase",
    "--require-completion-wait-without-enqueue-decrease",
    "--require-completion-present-wait-with-enqueue-increase",
    "--require-completion-present-wait-without-enqueue-decrease",
    "--require-encode-ready-depth-gt1-increase",
    "--require-commit-chunk-replay-cpu-per-present-decrease",
    "--require-queue-draw-submission-cpu-per-present-decrease",
    "--require-snapshot-cpu-per-present-decrease",
    "--require-snapshot-cache-lookup-cpu-per-present-decrease",
    "--require-encode-chunk-cpu-per-present-decrease",
    "--require-no-enqueue-commit-entry-to-publish-decrease",
    "--require-no-enqueue-publish-to-encode-dequeue-decrease",
    "--require-no-enqueue-encode-dequeue-to-commit-decrease",
    "--require-no-enqueue-wait-to-next-enqueue-decrease",
    "--require-no-enqueue-before-publish-closure-decrease",
    "--require-no-enqueue-before-publish-inter-replay-gap-decrease",
    "--require-command-buffers-per-present-not-increase",
    "--require-render-passes-per-present-not-increase",
    "--require-render-pass-carry-promotion-gates",
    "--require-encoder-final-end-reason-not-increase",
    "--require-encoder-color-load-not-increase",
    "--require-encoder-depth-load-not-increase",
    "--require-tile-preservation-not-increase",
)

UNIFORM_OWNER_COMPARE_FLAGS = (
    "--require-snapshot-cache-uniform-build-cpu-per-present-decrease",
    "--require-snapshot-cache-uniform-hash-cpu-per-present-decrease",
    "--require-batch-miss-uniform-build-cpu-per-present-decrease",
    "--require-batch-miss-uniform-hash-cpu-per-present-decrease",
    "--require-batch-miss-vs-const-hash-cpu-per-present-decrease",
    "--require-batch-miss-ps-const-hash-cpu-per-present-decrease",
    "--require-batch-miss-nonconst-hash-cpu-per-present-decrease",
    "--require-snapshot-uniform-copy-cpu-per-present-decrease",
    "--require-submit-draw-run-batch-append-uniform-cpu-per-present-decrease",
    "--require-draw-uniform-payload-lookup-cpu-per-present-decrease",
    "--require-draw-uniform-payload-append-copy-cpu-per-present-decrease",
)

UNIFORM_COMPACT_COMPARE_FLAGS = (
    "--require-uniform-compact-saved-bytes-present",
)

STATE_ELISION_COMPARE_FLAGS = (
    "--require-snapshot-state-elided-present",
    "--require-discarded-state-not-increase",
)

CARRIER_COMPARE_FLAGS = (
    "--require-submission-carrier-bytes-per-record-decrease",
    "--require-submission-carrier-uniform-storage-per-record-decrease",
)

ARGBUF_OWNER_COMPARE_FLAGS = (
    "--require-argbuf-setup-cpu-per-present-decrease",
    "--require-argbuf-open-cpu-per-present-decrease",
    "--require-argbuf-cbuf-update-cpu-per-present-decrease",
    "--require-argbuf-cbuf-update-vs-cpu-per-present-decrease",
)


class ThreeDMark05ProbeScriptTests(unittest.TestCase):
    def write_capture_layer_wine_root(self, path: Path) -> None:
        bin_dir = path / "bin"
        bin_dir.mkdir(parents=True, exist_ok=True)
        originals = {
            "wine.real": "original wine.real\n",
            "wine-preloader": "original wine-preloader\n",
        }
        captures = {
            "wine.capture.real": "capture wine.real\nMetalCaptureEnabled\n",
            "wine.capture.real-preloader": "capture wine-preloader\nMetalCaptureEnabled\n",
        }
        for name, text in {**originals, **captures}.items():
            file = bin_dir / name
            file.write_text(text, encoding="utf-8")
            file.chmod(0o755)

    def write_system_trace_fake_wrapper(
        self,
        path: Path,
        *,
        output_dir: Path,
        trace_dir: Path,
        actual_marker: Path,
        session_locked: str = "no",
        gputrace: str = "disabled",
        measure_index_reuse: str = "1",
        actual_sleep_sec: str = "0",
    ) -> None:
        path.write_text(
            f"""#!/usr/bin/env bash
set -euo pipefail
dry_run=0
for arg in "$@"; do
  if [[ "$arg" == "--dry-run" ]]; then
    dry_run=1
  fi
done
if (( dry_run )); then
  cat <<'OUT'
run_id: app-d3d9-3dmark05-fake-sidecar
output_dir: {output_dir}
trace_dir: {trace_dir}
summary: {output_dir}/3dmark05-perf-summary.md
indexed_probe_draws: {output_dir}/3dmark05-perf-indexed-probe-draws.csv
metal_system_trace: {trace_dir}/metal-system.trace
metal_gpu_intervals_xml: {trace_dir}/analysis/metal-gpu-intervals.xml
xctrace_gpu_intervals_summary: {trace_dir}/analysis/xctrace-metal-gpu-intervals-summary.md
measure_index_reuse: {measure_index_reuse}
session_locked: {session_locked}
gputrace: {gputrace}
OUT
  exit 0
fi
mkdir -p "$(dirname -- {actual_marker})"
printf '%s\n' "$*" > {actual_marker}
sleep {actual_sleep_sec}
exit 0
""",
            encoding="utf-8",
        )
        path.chmod(0o755)

    def write_system_trace_flipping_fake_wrapper(
        self,
        path: Path,
        *,
        output_dir: Path,
        trace_dir: Path,
        actual_marker: Path,
        counter_file: Path,
        locked_dry_runs: int = 1,
        actual_sleep_sec: str = "0",
    ) -> None:
        path.write_text(
            f"""#!/usr/bin/env bash
set -euo pipefail
dry_run=0
for arg in "$@"; do
  if [[ "$arg" == "--dry-run" ]]; then
    dry_run=1
  fi
done
if (( dry_run )); then
  count=0
  if [[ -f {counter_file} ]]; then
    count=$(cat {counter_file})
  fi
  count=$((count + 1))
  mkdir -p "$(dirname -- {counter_file})"
  echo "$count" > {counter_file}
  locked=yes
  if (( count > {locked_dry_runs} )); then
    locked=no
  fi
  cat <<OUT
run_id: app-d3d9-3dmark05-fake-sidecar
output_dir: {output_dir}
trace_dir: {trace_dir}
summary: {output_dir}/3dmark05-perf-summary.md
indexed_probe_draws: {output_dir}/3dmark05-perf-indexed-probe-draws.csv
metal_system_trace: {trace_dir}/metal-system.trace
metal_gpu_intervals_xml: {trace_dir}/analysis/metal-gpu-intervals.xml
xctrace_gpu_intervals_summary: {trace_dir}/analysis/xctrace-metal-gpu-intervals-summary.md
measure_index_reuse: 1
session_locked: $locked
gputrace: disabled
OUT
  exit 0
fi
mkdir -p "$(dirname -- {actual_marker})"
printf '%s\n' "$*" > {actual_marker}
sleep {actual_sleep_sec}
exit 0
""",
            encoding="utf-8",
        )
        path.chmod(0o755)

    def write_fake_osascript(self, path: Path) -> None:
        path.write_text(
            """#!/usr/bin/env bash
set -euo pipefail
cat >/dev/null
printf '%s\n' "${FAKE_OSASCRIPT_OUTPUT:-status=fail reason=missing-output}"
exit "${FAKE_OSASCRIPT_STATUS:-0}"
""",
            encoding="utf-8",
        )
        path.chmod(0o755)

    def write_fake_devtools_security(self, path: Path) -> None:
        path.write_text(
            """#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "${FAKE_DEVTOOLS_SECURITY_OUTPUT:-Developer mode is currently enabled.}"
exit "${FAKE_DEVTOOLS_SECURITY_STATUS:-0}"
""",
            encoding="utf-8",
        )
        path.chmod(0o755)

    def write_system_trace_fake_xctrace(self, path: Path, marker: Path) -> None:
        path.write_text(
            f"""#!/usr/bin/env bash
set -euo pipefail
if [[ "${{1:-}}" != "record" ]]; then
  echo "unexpected xctrace command: $*" >&2
  exit 2
fi
output=
while (($#)); do
  case "$1" in
    --output)
      output=$2
      shift 2
      ;;
    *)
      shift
      ;;
  esac
done
[[ -n "$output" ]] || exit 2
mkdir -p "$(dirname -- {marker})"
echo "$output" > {marker}
mkdir -p "$output"
""",
            encoding="utf-8",
        )
        path.chmod(0o755)

    def write_system_trace_fake_xctrace_with_optional_cpu_failures(
        self,
        path: Path,
        marker: Path,
    ) -> None:
        path.write_text(
            f"""#!/usr/bin/env bash
set -euo pipefail
cmd=${{1:-}}
shift || true
case "$cmd" in
  record)
    output=
    while (($#)); do
      case "$1" in
        --output)
          output=$2
          shift 2
          ;;
        *)
          shift
          ;;
      esac
    done
    [[ -n "$output" ]] || exit 2
    mkdir -p "$(dirname -- {marker})"
    echo "$output" > {marker}
    mkdir -p "$output"
    ;;
  export)
    output=
    xpath=
    while (($#)); do
      case "$1" in
        --output)
          output=$2
          shift 2
          ;;
        --xpath)
          xpath=$2
          shift 2
          ;;
        *)
          shift
          ;;
      esac
    done
    [[ -n "$output" ]] || exit 2
    mkdir -p "$(dirname -- "$output")"
    case "$xpath" in
      *metal-gpu-intervals*)
        cat > "$output" <<'XML'
<trace-toc>
  <run>
    <data>
      <table schema="metal-gpu-intervals">
        <row>
          <formatted-label>RenderPass[seq=60,enc=2,rt=0x1,depth=0x2]</formatted-label>
          <duration>10 ms</duration>
          <gpu-channel-name>Vertex</gpu-channel-name>
        </row>
      </table>
    </data>
  </run>
</trace-toc>
XML
        ;;
      *time-profile*)
        cat > "$output" <<'XML'
<trace-query-result>
  <node>
    <row>
      <thread id="th1" fmt="3DMark05.exe (0xabc) (3DMark05.exe, pid: 42)"/>
      <process id="p1" fmt="3DMark05.exe (42)"/>
      <thread-state id="running" fmt="Running">Running</thread-state>
      <weight id="w1" fmt="1.00 ms">1000000</weight>
      <tagged-backtrace id="bt1">
        <backtrace>
          <frame id="f1" name="d3d9_frame_dispatch">
            <binary id="b1" name="3DMark05.exe"/>
          </frame>
        </backtrace>
      </tagged-backtrace>
    </row>
  </node>
</trace-query-result>
XML
        ;;
      *time-sample*|*thread-info*)
        echo "schema not available: $xpath" >&2
        exit 3
        ;;
      *)
        echo "unexpected export xpath: $xpath" >&2
        exit 2
        ;;
    esac
    ;;
  *)
    echo "unexpected xctrace command: $cmd $*" >&2
    exit 2
    ;;
esac
""",
            encoding="utf-8",
        )
        path.chmod(0o755)

    def write_minimal_system_trace_encoder_csv(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(
                handle,
                fieldnames=[
                    "seq",
                    "encoder",
                    "draw_calls",
                    "primitive_count",
                    "vertex_count",
                    "route_depth_only_draws",
                    "route_depth_only_primitives",
                    "route_depth_only_vertices",
                    "route_programmable_textured_draws",
                    "route_programmable_textured_primitives",
                    "route_programmable_textured_vertices",
                    "route_programmable_color_draws",
                    "route_programmable_color_primitives",
                    "route_programmable_color_vertices",
                    "route_alpha_blend_primitives",
                    "route_alpha_test_primitives",
                ],
            )
            writer.writeheader()
            writer.writerow({
                "seq": "60",
                "encoder": "2",
                "draw_calls": "100",
                "primitive_count": "333333",
                "vertex_count": "1000000",
                "route_depth_only_draws": "100",
                "route_depth_only_primitives": "333333",
                "route_depth_only_vertices": "1000000",
                "route_programmable_textured_draws": "0",
                "route_programmable_textured_primitives": "0",
                "route_programmable_textured_vertices": "0",
                "route_programmable_color_draws": "0",
                "route_programmable_color_primitives": "0",
                "route_programmable_color_vertices": "0",
                "route_alpha_blend_primitives": "0",
                "route_alpha_test_primitives": "0",
            })

    def run_script(
        self,
        script: Path,
        *args: str,
        env: dict[str, str] | None = None,
    ) -> subprocess.CompletedProcess[str]:
        process_env = os.environ.copy()
        if env:
            process_env.update(env)
        return subprocess.run(
            [str(script), *args],
            cwd=REPO_ROOT,
            env=process_env,
            text=True,
            capture_output=True,
            check=False,
        )

    def test_wrapper_defaults_timeout_for_gputrace(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "timeout-default-gputrace",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        cmd_line = next(line for line in result.stdout.splitlines() if line.startswith("cmd:"))
        self.assertIn("--timeout 420", cmd_line)

    def test_wrapper_defaults_timeout_for_no_gputrace(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "timeout-default-no-gputrace",
            "--no-gputrace",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        cmd_line = next(line for line in result.stdout.splitlines() if line.startswith("cmd:"))
        self.assertIn("--timeout 120", cmd_line)

    def test_wrapper_dry_run_prints_top_level_watchdog_timeout(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "watchdog-timeout",
            "--no-gputrace",
            "--timeout",
            "10",
            "--dry-run",
            env={"DXMT_3DMARK05_PROBE_TIMEOUT_SLACK": "7"},
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("runner_timeout_sec: 10", result.stdout)
        self.assertIn("watchdog_timeout_sec: 10+70+7", result.stdout)
        self.assertIn("capture_delay_sec: 70 (catalogue default)", result.stdout)
        cmd_line = next(line for line in result.stdout.splitlines() if line.startswith("cmd:"))
        self.assertIn("--timeout 10", cmd_line)

    def test_wrapper_dry_run_includes_capture_delay_override(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "capture-delay",
            "--no-gputrace",
            "--capture-delay-sec",
            "50",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("capture_delay_sec: 50", result.stdout)
        self.assertIn("watchdog_timeout_sec: 120+50+45", result.stdout)
        cmd_line = next(line for line in result.stdout.splitlines() if line.startswith("cmd:"))
        self.assertIn("--capture-delay-sec 50", cmd_line)

    def test_wrapper_rejects_invalid_capture_delay_override(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--capture-delay-sec",
            "bad",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("--capture-delay-sec must be non-negative numeric seconds", result.stderr)

    def test_wrapper_dry_run_includes_internal_capture_range(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "capture-range",
            "--no-gputrace",
            "--capture-range",
            "820:900:20",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("capture_range: 820:900:20", result.stdout)
        self.assertIn(
            "traces/app-d3d9-3dmark05-capture-range/analysis/captures",
            result.stdout,
        )
        self.assertIn("DXMT_CAPTURE_RANGE=820:900:20", result.stdout)
        self.assertIn(
            f"DXMT_EXPERIMENT_CAPTURE_DIR={REPO_ROOT}/"
            "traces/app-d3d9-3dmark05-capture-range/analysis/captures",
            result.stdout,
        )

    def test_wrapper_dry_run_includes_internal_capture_frames_and_dir(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "capture-frames",
            "--no-gputrace",
            "--capture-frames",
            "820,840,860",
            "--capture-dir",
            "traces/custom-captures",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("capture_frames: 820,840,860", result.stdout)
        self.assertIn(
            "DXMT_CAPTURE_FRAMES=820\\,840\\,860",
            result.stdout,
        )
        self.assertIn(
            f"DXMT_EXPERIMENT_CAPTURE_DIR={REPO_ROOT}/traces/custom-captures",
            result.stdout,
        )

    def test_wrapper_rejects_invalid_internal_capture_range(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--capture-range",
            "820-900",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("--capture-range must be START:END[:STEP]", result.stderr)

    def test_wrapper_rejects_invalid_internal_capture_frames(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--capture-frames",
            "820,bad",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("--capture-frames must be", result.stderr)

    def test_wrapper_rejects_invalid_top_level_watchdog_slack(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--dry-run",
            env={"DXMT_3DMARK05_PROBE_TIMEOUT_SLACK": "bad"},
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("DXMT_3DMARK05_PROBE_TIMEOUT_SLACK must be", result.stderr)

    def test_run_with_timeout_kills_long_child(self) -> None:
        result = self.run_script(
            RUN_WITH_TIMEOUT,
            "--timeout",
            "0.2",
            "--grace",
            "0.2",
            "--label",
            "test-watchdog",
            "--",
            "python3",
            "-c",
            "import time; time.sleep(5)",
        )

        self.assertEqual(result.returncode, 124)
        self.assertIn("[test-watchdog] timeout after", result.stderr)

    def test_wrapper_dry_run_prints_index_cache_runtime_report_path(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "index-cache-runtime-path",
            "--no-gputrace",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(
            "index_cache_runtime_report:",
            result.stdout,
        )
        self.assertIn(
            "3dmark05-index-cache-runtime-summary.md",
            result.stdout,
        )

    def test_wrapper_dry_run_prints_trace_artifacts_manifest_path(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "trace-artifacts-path",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(
            "trace_artifacts_json: "
            f"{REPO_ROOT}/experiments/output/"
            "app-d3d9-3dmark05-trace-artifacts-path/3dmark05-trace-artifacts.json",
            result.stdout,
        )

    def test_wrapper_direct_run_uses_catalogue_prefix(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "direct-prefix",
            "--no-gputrace",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        env_line = next(line for line in result.stdout.splitlines() if line.startswith("env:"))
        self.assertIn("DXMT_3DMARK05_DIRECT=1", env_line)
        self.assertIn(
            "DXMT_3DMARK05_PREFIX="
            f"{REPO_ROOT}/experiments/prefixs/app-d3d9-3dmark05",
            env_line,
        )
        self.assertIn(
            "DXMT_3DMARK05_WINESERVER="
            f"{REPO_ROOT}/experiments/wine/sikarugir-cx-24.0.7/bin/wineserver",
            env_line,
        )

    def test_wrapper_runs_wineserver_cleanup_after_watchdog(self) -> None:
        text = RUN_WRAPPER.read_text(encoding="utf-8")

        self.assertIn("cleanup_3dmark05_probe_wineserver", text)
        self.assertIn('WINEPREFIX="$probe_prefix" "$probe_wineserver" -k', text)
        self.assertIn(
            "cleanup_3dmark05_probe_wineserver\n\nsummary_cmd=(",
            text,
        )
        self.assertIn('"${summary_cmd[@]}"', text)

    def test_wrapper_rejects_disabled_timeout(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--timeout",
            "0",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("--timeout must be positive numeric seconds", result.stderr)

    def test_wrapper_rejects_locked_session_before_actual_probe(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fake_bin = root / "bin"
            fake_bin.mkdir()
            fake_ioreg = fake_bin / "ioreg"
            fake_ioreg.write_text(
                """#!/usr/bin/env bash
cat <<'OUT'
"CGSSessionScreenIsLocked"=Yes
OUT
""",
                encoding="utf-8",
            )
            fake_ioreg.chmod(0o755)
            suffix = f"locked-session-{root.name}"
            output_dir = REPO_ROOT / "experiments" / "output" / f"app-d3d9-3dmark05-{suffix}"
            trace_dir = REPO_ROOT / "traces" / f"app-d3d9-3dmark05-{suffix}"

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                suffix,
                "--no-gputrace",
                "--timeout",
                "120",
                env={"PATH": f"{fake_bin}{os.pathsep}{os.environ.get('PATH', '')}"},
            )

            self.assertEqual(result.returncode, 2)
            self.assertIn("session_locked: yes", result.stdout)
            self.assertIn("macOS session is locked", result.stderr)
            self.assertFalse(output_dir.exists())
            self.assertFalse(trace_dir.exists())

    def test_wrapper_wait_unlocked_times_out_before_actual_probe(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fake_bin = root / "bin"
            fake_bin.mkdir()
            fake_ioreg = fake_bin / "ioreg"
            fake_ioreg.write_text(
                """#!/usr/bin/env bash
cat <<'OUT'
"CGSSessionScreenIsLocked"=Yes
OUT
""",
                encoding="utf-8",
            )
            fake_ioreg.chmod(0o755)
            suffix = f"wait-locked-session-{root.name}"
            output_dir = REPO_ROOT / "experiments" / "output" / f"app-d3d9-3dmark05-{suffix}"
            trace_dir = REPO_ROOT / "traces" / f"app-d3d9-3dmark05-{suffix}"

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                suffix,
                "--no-gputrace",
                "--timeout",
                "120",
                "--wait-unlocked-sec",
                "1",
                "--wait-unlocked-interval-sec",
                "1",
                env={"PATH": f"{fake_bin}{os.pathsep}{os.environ.get('PATH', '')}"},
            )

            self.assertEqual(result.returncode, 2)
            self.assertIn("session_locked: yes", result.stdout)
            self.assertIn("wait_unlocked_sec: 1", result.stdout)
            self.assertIn("session_locked_after_wait: yes", result.stdout)
            self.assertIn("waiting for macOS session unlock: 0s/1s", result.stderr)
            self.assertIn("after waiting 1s", result.stderr)
            self.assertFalse(output_dir.exists())
            self.assertFalse(trace_dir.exists())

    def test_wrapper_dry_run_prints_wait_unlocked_plan_without_waiting(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fake_bin = root / "bin"
            fake_bin.mkdir()
            fake_ioreg = fake_bin / "ioreg"
            fake_ioreg.write_text(
                """#!/usr/bin/env bash
cat <<'OUT'
"CGSSessionScreenIsLocked"=Yes
OUT
""",
                encoding="utf-8",
            )
            fake_ioreg.chmod(0o755)

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                f"wait-dry-run-{root.name}",
                "--no-gputrace",
                "--wait-unlocked-sec",
                "9",
                "--wait-unlocked-interval-sec",
                "3",
                "--dry-run",
                env={"PATH": f"{fake_bin}{os.pathsep}{os.environ.get('PATH', '')}"},
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("session_locked: yes", result.stdout)
            self.assertIn("wait_unlocked_sec: 9", result.stdout)
            self.assertIn("wait_unlocked_interval_sec: 3", result.stdout)
            self.assertNotIn("waiting for macOS session unlock", result.stderr)

    def test_wrapper_rejects_invalid_wait_unlocked_values(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--wait-unlocked-sec",
            "1.5",
            "--dry-run",
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("--wait-unlocked-sec must be non-negative integer seconds", result.stderr)

        result = self.run_script(
            RUN_WRAPPER,
            "--wait-unlocked-interval-sec",
            "0",
            "--dry-run",
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("--wait-unlocked-interval-sec must be a positive integer", result.stderr)

    def test_wrapper_dry_run_prints_keep_frontmost_plan(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "keep-frontmost",
            "--no-gputrace",
            "--keep-frontmost",
            "--keep-frontmost-interval-sec",
            "0.5",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("keep_frontmost: 1", result.stdout)
        self.assertIn("keep_frontmost_process: 3DMark05.exe", result.stdout)
        self.assertIn("keep_frontmost_interval_sec: 0.5", result.stdout)

    def test_wrapper_rejects_invalid_keep_frontmost_values(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--keep-frontmost",
            "--keep-frontmost-interval-sec",
            "0",
            "--dry-run",
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("--keep-frontmost-interval-sec must be positive numeric seconds", result.stderr)

        result = self.run_script(
            RUN_WRAPPER,
            "--keep-frontmost",
            "--keep-frontmost-process",
            '3DMark05.exe"',
            "--dry-run",
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("--keep-frontmost-process must be", result.stderr)

    def test_wrapper_stops_keep_frontmost_loop_after_watchdog(self) -> None:
        text = RUN_WRAPPER.read_text(encoding="utf-8")

        self.assertIn("start_3dmark05_frontmost_loop\n(", text)
        self.assertIn(
            ") || run_status=$?\nstop_3dmark05_frontmost_loop\ntrap - EXIT\ncleanup_3dmark05_probe_wineserver",
            text,
        )

    def test_catalogue_runner_rejects_disabled_timeout_for_3dmark05(self) -> None:
        result = self.run_script(
            RUN_EXPERIMENT,
            "run",
            "app-d3d9-3dmark05",
            "--timeout",
            "0",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("app-d3d9-3dmark05: --timeout must be positive", result.stderr)

    def test_direct_wrapper_defaults_timeout(self) -> None:
        result = self.run_script(
            DIRECT_WRAPPER,
            env={"DXMT_3DMARK05_DIRECT_DRY_RUN": "1"},
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("timeout: 120s", result.stdout)
        cmd_line = next(line for line in result.stdout.splitlines() if line.startswith("cmd:"))
        self.assertIn("experiments/launchers/app-d3d9-3dmark05.sh", cmd_line)

    def test_direct_wrapper_rejects_disabled_timeout(self) -> None:
        result = self.run_script(
            DIRECT_WRAPPER,
            env={
                "DXMT_3DMARK05_DIRECT_DRY_RUN": "1",
                "DXMT_3DMARK05_DIRECT_TIMEOUT": "0",
            },
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("DXMT_3DMARK05_DIRECT_TIMEOUT must be positive numeric seconds", result.stderr)

    def test_direct_launcher_self_timeout_dry_run(self) -> None:
        result = self.run_script(
            LAUNCHER,
            env={
                "DXMT_3DMARK05_DIRECT_DRY_RUN": "1",
                "DXMT_3DMARK05_LAUNCHER_TIMEOUT": "12",
            },
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("launcher_timeout: 12s", result.stdout)
        self.assertIn("DXMT_3DMARK05_SELF_SUPERVISED=1", result.stdout)

    def test_direct_launcher_rejects_disabled_self_timeout(self) -> None:
        result = self.run_script(
            LAUNCHER,
            env={
                "DXMT_3DMARK05_DIRECT_DRY_RUN": "1",
                "DXMT_3DMARK05_LAUNCHER_TIMEOUT": "0",
            },
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("DXMT_3DMARK05_LAUNCHER_TIMEOUT must be positive numeric seconds", result.stderr)

    def test_direct_launcher_traps_timeout_cleanup(self) -> None:
        text = LAUNCHER.read_text(encoding="utf-8")

        self.assertIn("dxmt_3dmark05_auto_enter_pid=$!", text)
        self.assertIn("DXMT_3DMARK05_LAUNCHER_TIMEOUT", text)
        self.assertIn("DXMT_3DMARK05_SELF_SUPERVISED", text)
        self.assertIn("trap 'cleanup_app_d3d9_3dmark05_direct 143' TERM", text)
        self.assertIn('WINEPREFIX="$prefix" "$wine_server" -k', text)

    def test_wrapper_rejects_run_level_gate_without_baseline_output(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--require-draw-run-records-increase",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("run-level comparison gates require", result.stderr)

    def test_finalizer_rejects_run_level_gate_without_baseline_output(self) -> None:
        result = self.run_script(
            FINALIZER,
            "--suffix",
            "missing-baseline",
            "--require-draw-run-records-increase",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("run-level comparison gates require", result.stderr)

    def test_wrapper_rejects_xcode_compare_gate_without_baseline_joined(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--require-top-unexplained-buffer-write-decrease",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("Xcode comparison gates require", result.stderr)

    def test_wrapper_rejects_tvb_mechanism_gate_without_baseline_joined(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--require-tvb-mechanism-proof",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("Xcode comparison gates require", result.stderr)

    def test_finalizer_rejects_xcode_compare_gate_without_baseline_joined(self) -> None:
        result = self.run_script(
            FINALIZER,
            "--suffix",
            "missing-baseline",
            "--max-top-unexplained-buffer-write-ratio",
            "0.25",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("Xcode comparison gates require", result.stderr)

    def test_finalizer_rejects_tvb_mechanism_gate_without_baseline_joined(self) -> None:
        result = self.run_script(
            FINALIZER,
            "--suffix",
            "missing-tvb-baseline",
            "--require-tvb-mechanism-proof",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("Xcode comparison gates require", result.stderr)

    def test_wrapper_rejects_non_target_gate_without_baseline_joined(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--target-row-key",
            "50/1",
            "--max-non-target-gpu-regression-ms",
            "1.0",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("Xcode comparison gates require", result.stderr)

    def test_finalizer_rejects_non_target_gate_without_baseline_joined(self) -> None:
        result = self.run_script(
            FINALIZER,
            "--suffix",
            "missing-baseline",
            "--target-row-key",
            "50/1",
            "--max-non-target-vs-buffer-write-regression-mib",
            "16",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("Xcode comparison gates require", result.stderr)

    def test_wrapper_rejects_missing_baseline_output_path(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--compare-baseline-output",
            "does-not-exist",
            "--require-draw-run-records-increase",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("missing baseline result.json", result.stderr)

    def test_finalizer_rejects_missing_baseline_output_path(self) -> None:
        result = self.run_script(
            FINALIZER,
            "--suffix",
            "missing-baseline",
            "--baseline-output",
            "does-not-exist",
            "--require-draw-run-records-increase",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("missing baseline result.json", result.stderr)

    def test_wrapper_rejects_missing_baseline_joined_path(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--baseline-joined",
            "does-not-exist.csv",
            "--require-top-gpu-decrease",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("missing baseline joined CSV", result.stderr)

    def test_wrapper_current_uniform_compact_gate_does_not_require_baseline(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "current-uniform-compact-gate",
            "--no-gputrace",
            "--require-current-uniform-compact-saved-bytes-present",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(
            "require_current_uniform_compact_saved_bytes_present: 1",
            result.stdout,
        )
        self.assertNotIn("run-level comparison gates require", result.stderr)

    def test_wrapper_forwards_current_uniform_compact_gate_to_finalizer(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "forward-current-uniform-compact-gate",
            "--require-current-uniform-compact-saved-bytes-present",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        finalize_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("finalize_cmd_after_xcode_export:")
        )
        self.assertIn(
            "--require-current-uniform-compact-saved-bytes-present",
            finalize_line,
        )

    def test_finalizer_current_uniform_compact_gate_forwards_to_summary(self) -> None:
        result = self.run_script(
            FINALIZER,
            "--suffix",
            "current-uniform-compact-gate",
            "--require-current-uniform-compact-saved-bytes-present",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        summary_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("summary_cmd:")
        )
        self.assertIn(
            "--require-uniform-compact-saved-bytes-present",
            summary_line,
        )

    def test_wrapper_dry_run_low_space_warning_does_not_interleave_commands(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "dry-run-order",
            "--min-free-mb",
            "999999999",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stderr, "")
        lines = result.stdout.splitlines()
        finalize_lines = [
            line for line in lines if line.startswith("finalize_cmd_after_xcode_export:")
        ]
        self.assertEqual(len(finalize_lines), 1)
        self.assertNotIn("dry-run:", finalize_lines[0])
        dry_run_index = lines.index(
            "dry-run: free space is below the launch guard; cleanup candidates follow"
        )
        finalize_index = lines.index(finalize_lines[0])
        self.assertGreater(dry_run_index, finalize_index)
        self.assertIn("large trace run directories:", result.stdout)
        self.assertIn("large output run directories:", result.stdout)
        self.assertIn("large ignored/manual-review candidates:", result.stdout)
        self.assertIn("cleanup note: remove only obsolete run ids", result.stdout)

    def test_wrapper_rejects_low_gputrace_free_space_guard_without_override(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "unsafe-low-gputrace-space",
            "--min-free-mb",
            "256",
            env={"DXMT_3DMARK05_REQUIRE_UNLOCKED": "0"},
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("refusing low free-space gputrace launch guard", result.stderr)
        self.assertIn("DXMT_3DMARK05_ALLOW_LOW_TRACE_FREE_MB=1", result.stderr)

    def test_wrapper_forwards_unexplained_write_gates_to_finalizer(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "forward-unexplained",
                "--baseline-joined",
                str(baseline_joined),
                "--require-top-unexplained-buffer-write-decrease",
                "--max-top-unexplained-buffer-write-ratio",
                "0.50",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        finalize_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("finalize_cmd_after_xcode_export:")
        )
        self.assertIn("--require-top-unexplained-buffer-write-decrease", finalize_line)
        self.assertIn("--max-top-unexplained-buffer-write-ratio", finalize_line)
        self.assertIn("0.50", finalize_line)

    def test_wrapper_forwards_frame_shape_gates_to_finalizer(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "forward-frame-shape",
                "--baseline-joined",
                str(baseline_joined),
                "--require-top-row-key-match",
                "--max-top-draw-call-delta-ratio",
                "0.05",
                "--max-top-vertex-count-delta-ratio",
                "0.05",
                "--max-top-triangle-delta-ratio",
                "0.05",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        finalize_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("finalize_cmd_after_xcode_export:")
        )
        self.assertIn("--require-top-row-key-match", finalize_line)
        self.assertIn("--max-top-draw-call-delta-ratio", finalize_line)
        self.assertIn("--max-top-vertex-count-delta-ratio", finalize_line)
        self.assertIn("--max-top-triangle-delta-ratio", finalize_line)
        self.assertIn("0.05", finalize_line)

    def test_wrapper_forwards_non_target_hot_row_gates_to_finalizer(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "forward-non-target",
                "--baseline-joined",
                str(baseline_joined),
                "--target-row-key",
                "50/1",
                "--target-row-key",
                "50/3",
                "--max-non-target-gpu-regression-ms",
                "1.0",
                "--max-non-target-vs-buffer-write-regression-mib",
                "16",
                "--max-non-target-vs-invocations-regression-ratio",
                "0.05",
                "--max-non-target-draw-call-delta-ratio",
                "0.05",
                "--max-non-target-vertex-count-delta-ratio",
                "0.05",
                "--max-non-target-triangle-delta-ratio",
                "0.05",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        finalize_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("finalize_cmd_after_xcode_export:")
        )
        self.assertIn("--target-row-key", finalize_line)
        self.assertIn("50/1", finalize_line)
        self.assertIn("50/3", finalize_line)
        self.assertIn("--max-non-target-gpu-regression-ms", finalize_line)
        self.assertIn("1.0", finalize_line)
        self.assertIn("--max-non-target-vs-buffer-write-regression-mib", finalize_line)
        self.assertIn("16", finalize_line)
        self.assertIn(
            "--max-non-target-vs-invocations-regression-ratio",
            finalize_line,
        )
        self.assertIn("0.05", finalize_line)
        self.assertIn("--max-non-target-draw-call-delta-ratio", finalize_line)
        self.assertIn("--max-non-target-vertex-count-delta-ratio", finalize_line)
        self.assertIn("--max-non-target-triangle-delta-ratio", finalize_line)

    def test_wrapper_forwards_target_row_apply_gates_to_finalizer(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "forward-target",
                "--baseline-joined",
                str(baseline_joined),
                "--target-row-key",
                "50/0",
                "--target-row-key",
                "50/1",
                "--require-target-index-cache-miss32-decrease",
                "--require-target-index-cache-opt-miss32-decrease",
                "--require-target-reordered-index-cache-hits",
                "--require-target-vs-buffer-write-decrease",
                "--require-target-vs-invocations-decrease",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        finalize_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("finalize_cmd_after_xcode_export:")
        )
        self.assertIn("--target-row-key", finalize_line)
        self.assertIn("50/0", finalize_line)
        self.assertIn("50/1", finalize_line)
        self.assertIn("--require-target-index-cache-miss32-decrease", finalize_line)
        self.assertIn("--require-target-index-cache-opt-miss32-decrease", finalize_line)
        self.assertIn("--require-target-reordered-index-cache-hits", finalize_line)
        self.assertIn("--require-target-vs-buffer-write-decrease", finalize_line)
        self.assertIn("--require-target-vs-invocations-decrease", finalize_line)

    def test_wrapper_expands_cache_opt_apply_proof_preset_to_finalizer(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "cache-opt-apply-proof",
                "--baseline-joined",
                str(baseline_joined),
                "--target-row-key",
                "50/0",
                "--require-cache-opt-apply-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        finalize_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("finalize_cmd_after_xcode_export:")
        )
        self.assertIn("--require-stable-frame-proof", finalize_line)
        self.assertIn("--target-row-key", finalize_line)
        self.assertIn("50/0", finalize_line)
        self.assertIn("--require-target-index-cache-miss32-decrease", finalize_line)
        self.assertIn("--require-target-vs-buffer-write-decrease", finalize_line)
        self.assertIn("--require-target-vs-invocations-decrease", finalize_line)

    def test_wrapper_rejects_cache_opt_apply_proof_without_target_row(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "cache-opt-apply-proof-missing-row",
                "--baseline-joined",
                str(baseline_joined),
                "--require-cache-opt-apply-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "--require-cache-opt-apply-proof requires at least one --target-row-key",
            result.stderr,
        )

    def test_wrapper_rejects_unsafe_nonopaque_cache_apply_proof_without_semantic_gate(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "unsafe-cache-opt-apply-proof-missing-semantic",
                "--baseline-joined",
                str(baseline_joined),
                "--target-row-key",
                "50/2",
                "--probe-apply-index-cache-opt-candidate",
                "--probe-apply-index-cache-opt-candidate-unsafe-nonopaque",
                "--require-cache-opt-apply-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "--require-semantic-image-proof requires --semantic-image-policy",
            result.stderr,
        )

    def test_wrapper_forwards_unsafe_nonopaque_cache_apply_semantic_proof(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            baseline_joined = root / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")
            before = root / "before.ppm"
            after = root / "after.ppm"
            before.write_text("P3\n1 1\n255\n0 0 0\n", encoding="ascii")
            after.write_text("P3\n1 1\n255\n0 0 0\n", encoding="ascii")

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "unsafe-cache-opt-apply-proof",
                "--baseline-joined",
                str(baseline_joined),
                "--target-row-key",
                "50/2",
                "--semantic-image-policy",
                "exact",
                "--semantic-image-before",
                str(before),
                "--semantic-image-after",
                str(after),
                "--probe-apply-index-cache-opt-candidate",
                "--probe-apply-index-cache-opt-candidate-unsafe-nonopaque",
                "--require-cache-opt-apply-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        finalize_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("finalize_cmd_after_xcode_export:")
        )
        self.assertIn("--require-semantic-image-proof", finalize_line)
        self.assertIn("--require-target-index-cache-miss32-decrease", finalize_line)
        self.assertIn("--require-target-vs-buffer-write-decrease", finalize_line)
        self.assertIn("--require-target-vs-invocations-decrease", finalize_line)
        self.assertIn("--semantic-image-policy", finalize_line)
        self.assertIn("exact", finalize_line)

    def test_wrapper_rejects_screen_blend_cache_proof_without_semantic_gate(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "screen-blend-cache-proof-missing-semantic",
                "--baseline-joined",
                str(baseline_joined),
                "--target-row-key",
                "50/2",
                "--require-screen-blend-cache-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "--require-screen-blend-cache-proof requires --semantic-image-policy",
            result.stderr,
        )

    def test_wrapper_rejects_opaque_depth_index_cache_proof_without_opt_flag(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "opaque-depth-cache-proof-missing-opt",
                "--baseline-joined",
                str(baseline_joined),
                "--target-row-key",
                "50/1",
                "--require-opaque-depth-index-cache-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "--require-opaque-depth-index-cache-proof requires --optimize-opaque-depth-index-cache",
            result.stderr,
        )

    def test_wrapper_rejects_opaque_depth_index_cache_proof_without_target_row(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "opaque-depth-cache-proof-missing-target-row",
                "--baseline-joined",
                str(baseline_joined),
                "--optimize-opaque-depth-index-cache",
                "--require-opaque-depth-index-cache-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "--require-opaque-depth-index-cache-proof requires at least one --target-row-key",
            result.stderr,
        )

    def test_wrapper_forwards_opaque_depth_index_cache_proof(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "opaque-depth-cache-proof",
                "--baseline-joined",
                str(baseline_joined),
                "--target-row-key",
                "50/0",
                "--target-row-key",
                "50/1",
                "--optimize-opaque-depth-index-cache",
                "--require-opaque-depth-index-cache-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE=1", result.stdout)
        self.assertIn("DXMT9_MEASURE_INDEX_REUSE=1", result.stdout)
        self.assertIn("DXMT9_MEASURE_INDEX_CACHE_OPT_CANDIDATE=1", result.stdout)
        finalize_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("finalize_cmd_after_xcode_export:")
        )
        self.assertIn("--require-opaque-depth-index-cache-proof", finalize_line)
        self.assertIn("--require-stable-frame-proof", finalize_line)
        self.assertNotIn("--require-target-index-cache-miss32-decrease", finalize_line)
        self.assertIn("--require-target-index-cache-opt-miss32-decrease", finalize_line)
        self.assertIn("--require-target-reordered-index-cache-hits", finalize_line)
        self.assertIn("--require-target-vs-buffer-write-decrease", finalize_line)
        self.assertIn("--require-target-vs-invocations-decrease", finalize_line)
        self.assertIn("--target-row-key 50/0", finalize_line)
        self.assertIn("--target-row-key 50/1", finalize_line)

    def test_wrapper_rejects_screen_blend_cache_proof_without_cache_opt(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            baseline_joined = root / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")
            before = root / "before.ppm"
            after = root / "after.ppm"
            before.write_text("P3\n1 1\n255\n0 0 0\n", encoding="ascii")
            after.write_text("P3\n1 1\n255\n0 0 0\n", encoding="ascii")

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "screen-blend-cache-proof-missing-cache-opt",
                "--baseline-joined",
                str(baseline_joined),
                "--target-row-key",
                "50/2",
                "--semantic-image-policy",
                "lsb1",
                "--semantic-image-before",
                str(before),
                "--semantic-image-after",
                str(after),
                "--require-screen-blend-cache-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "--require-screen-blend-cache-proof requires --optimize-screen-blend-index-cache",
            result.stderr,
        )

    def test_wrapper_rejects_screen_blend_cache_proof_without_target_row(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            baseline_joined = root / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")
            before = root / "before.ppm"
            after = root / "after.ppm"
            before.write_text("P3\n1 1\n255\n0 0 0\n", encoding="ascii")
            after.write_text("P3\n1 1\n255\n0 0 0\n", encoding="ascii")

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "screen-blend-cache-proof-missing-target-row",
                "--baseline-joined",
                str(baseline_joined),
                "--semantic-image-policy",
                "lsb1",
                "--semantic-image-before",
                str(before),
                "--semantic-image-after",
                str(after),
                "--optimize-screen-blend-index-cache",
                "--require-screen-blend-cache-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "--require-screen-blend-cache-proof requires at least one --target-row-key",
            result.stderr,
        )

    def test_wrapper_forwards_screen_blend_cache_proof_with_semantic_gate(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            baseline_joined = root / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")
            before = root / "before.ppm"
            after = root / "after.ppm"
            before.write_text("P3\n1 1\n255\n0 0 0\n", encoding="ascii")
            after.write_text("P3\n1 1\n255\n0 0 0\n", encoding="ascii")

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "screen-blend-cache-proof",
                "--baseline-joined",
                str(baseline_joined),
                "--target-row-key",
                "50/2",
                "--semantic-image-policy",
                "lsb1",
                "--semantic-image-before",
                str(before),
                "--semantic-image-after",
                str(after),
                "--optimize-screen-blend-index-cache",
                "--require-screen-blend-cache-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_CACHE=1", result.stdout)
        self.assertIn("DXMT9_MEASURE_INDEX_REUSE=1", result.stdout)
        self.assertIn("DXMT9_MEASURE_INDEX_CACHE_OPT_CANDIDATE=1", result.stdout)
        finalize_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("finalize_cmd_after_xcode_export:")
        )
        self.assertIn("--require-screen-blend-cache-proof", finalize_line)
        self.assertIn("--require-stable-frame-proof", finalize_line)
        self.assertNotIn("--require-target-index-cache-miss32-decrease", finalize_line)
        self.assertIn("--require-target-index-cache-opt-miss32-decrease", finalize_line)
        self.assertIn("--require-target-reordered-index-cache-hits", finalize_line)
        self.assertIn("--require-target-vs-buffer-write-decrease", finalize_line)
        self.assertIn("--require-target-vs-invocations-decrease", finalize_line)
        self.assertIn("--semantic-image-policy", finalize_line)
        self.assertIn("lsb1", finalize_line)

    def test_wrapper_forwards_stable_frame_proof_preset_to_finalizer(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "forward-stable-proof",
                "--baseline-joined",
                str(baseline_joined),
                "--require-stable-frame-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        finalize_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("finalize_cmd_after_xcode_export:")
        )
        self.assertIn("--require-stable-frame-proof", finalize_line)
        self.assertIn("--require-result-json", finalize_line)
        self.assertIn("--require-top-pso-attribution", finalize_line)
        self.assertIn("--require-xcode-counter-coverage", finalize_line)
        self.assertIn("--require-dxmt-join-coverage", finalize_line)
        self.assertIn("--max-top-draw-call-delta-ratio", finalize_line)
        self.assertIn("--max-top-vertex-count-delta-ratio", finalize_line)
        self.assertIn("--max-top-triangle-delta-ratio", finalize_line)
        self.assertIn("0.05", finalize_line)

    def test_wrapper_allows_partial_stable_frame_proof_without_result_json_gate(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "forward-partial-stable-proof",
                "--baseline-joined",
                str(baseline_joined),
                "--require-stable-frame-proof",
                "--allow-partial-stable-frame-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        finalize_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("finalize_cmd_after_xcode_export:")
        )
        self.assertIn("--require-stable-frame-proof", finalize_line)
        self.assertIn("--allow-partial-stable-frame-proof", finalize_line)
        self.assertNotIn("--require-result-json", finalize_line)

    def test_wrapper_forwards_tvb_mechanism_proof_to_finalizer(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "forward-tvb-mechanism",
                "--baseline-joined",
                str(baseline_joined),
                "--require-tvb-mechanism-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        finalize_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("finalize_cmd_after_xcode_export:")
        )
        self.assertIn("--require-tvb-mechanism-proof", finalize_line)

    def test_wrapper_forwards_top_and_hot_share_to_finalizer(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "forward-hot-set",
            "--top",
            "4",
            "--hot-gpu-share",
            "98",
            "--min-free-mb",
            "0",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        finalize_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("finalize_cmd_after_xcode_export:")
        )
        self.assertIn("--top 4", finalize_line)
        self.assertIn("--hot-gpu-share 98", finalize_line)

    def test_wrapper_forwards_result_json_gate_to_finalizer(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "forward-result-json",
            "--require-result-json",
            "--min-free-mb",
            "0",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        finalize_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("finalize_cmd_after_xcode_export:")
        )
        self.assertIn("--require-result-json", finalize_line)

    def test_wrapper_forwards_semantic_image_gate_to_finalizer(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "forward-semantic-image",
            "--semantic-image-policy",
            "lsb1",
            "--semantic-image-before",
            "before.ppm",
            "--semantic-image-after",
            "after.ppm",
            "--semantic-image-min-active-pct",
            "2",
            "--min-free-mb",
            "0",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        finalize_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("finalize_cmd_after_xcode_export:")
        )
        self.assertIn("--semantic-image-policy lsb1", finalize_line)
        self.assertIn("--semantic-image-before before.ppm", finalize_line)
        self.assertIn("--semantic-image-after after.ppm", finalize_line)
        self.assertIn("--semantic-image-min-active-pct 2", finalize_line)

    def test_wrapper_rejects_incomplete_semantic_image_gate(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "bad-semantic-image",
            "--semantic-image-policy",
            "exact",
            "--semantic-image-before",
            "before.ppm",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "--semantic-image-policy requires --semantic-image-before and --semantic-image-after",
            result.stderr,
        )

    def test_wrapper_dry_run_includes_sparse_const_split_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--split-sparse-const-records",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_SPLIT_SPARSE_CONST_RECORDS=1", result.stdout)

    def test_wrapper_dry_run_includes_vertex_temp_trim_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--trim-vertex-temps",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_TRIM_VERTEX_TEMPS=1", result.stdout)

    def test_wrapper_dry_run_includes_scoped_varying_trim_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--trim-unused-varyings",
            "--trim-unused-varyings-vs-hashes",
            "0x61be862718e1d00c",
            "--trim-unused-varyings-ps-hashes",
            "0xfbeb0f02c65a9526",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_TRIM_UNUSED_VARYINGS=1", result.stdout)
        self.assertIn("DXMT9_TRIM_UNUSED_VARYINGS_VS_HASHES=0x61be862718e1d00c", result.stdout)
        self.assertIn("DXMT9_TRIM_UNUSED_VARYINGS_PS_HASHES=0xfbeb0f02c65a9526", result.stdout)

    def test_wrapper_rejects_scoped_varying_trim_without_trim_flag(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--trim-unused-varyings-vs-hashes",
            "0x61be862718e1d00c",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("require --trim-unused-varyings", result.stderr)

    def test_wrapper_dry_run_includes_vsout_point_size_probe_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--drop-vsout-point-size",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_PROBE_DROP_VSOUT_POINT_SIZE=1", result.stdout)

    def test_wrapper_dry_run_includes_half_vsout_probe_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--probe-half-vsout",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_PROBE_HALF_VSOUT=1", result.stdout)

    def test_wrapper_dry_run_includes_fragmentless_keep_vsout_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--probe-fragmentless-depth-only-row",
            "60/0",
            "--probe-fragmentless-depth-only-keep-vsout",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_PROBE_FRAGMENTLESS_DEPTH_ONLY_ROW=60/0", result.stdout)
        self.assertIn("DXMT9_PROBE_FRAGMENTLESS_DEPTH_ONLY_KEEP_VSOUT=1", result.stdout)

    def test_wrapper_dry_run_includes_force_fragment_color_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--force-fragment-color",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT_DEBUG_FORCE_FRAGMENT_COLOR=1", result.stdout)

    def test_wrapper_gputrace_dry_run_auto_scopes_encoder_breakdown_to_frame(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--frame",
            "50",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_PERF_ENCODER_BREAKDOWN=1", result.stdout)
        self.assertIn("DXMT9_PERF_ENCODER_BREAKDOWN_SEQ=50", result.stdout)

    def test_wrapper_gputrace_dry_run_can_keep_all_frame_encoder_breakdown(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--frame",
            "50",
            "--encoder-breakdown-all-frames",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_PERF_ENCODER_BREAKDOWN=1", result.stdout)
        self.assertNotIn("DXMT9_PERF_ENCODER_BREAKDOWN_SEQ=", result.stdout)

    def test_wrapper_dry_run_supports_encoder_breakdown_seq_range(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--frame",
            "50",
            "--encoder-breakdown-seq-range",
            "1000:1700",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_PERF_ENCODER_BREAKDOWN=1", result.stdout)
        self.assertIn("DXMT9_PERF_ENCODER_BREAKDOWN_SEQ_MIN=1000", result.stdout)
        self.assertIn("DXMT9_PERF_ENCODER_BREAKDOWN_SEQ_MAX=1700", result.stdout)
        self.assertNotIn("DXMT9_PERF_ENCODER_BREAKDOWN_SEQ=50", result.stdout)

    def test_wrapper_rejects_exact_and_range_encoder_breakdown(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--frame",
            "50",
            "--encoder-breakdown-seq",
            "50",
            "--encoder-breakdown-seq-range",
            "1000:1700",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("mutually exclusive", result.stderr)

    def test_wrapper_no_encoder_breakdown_for_no_gputrace_smoke(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--frame",
            "50",
            "--no-gputrace",
            "--no-encoder-breakdown",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("DXMT9_PERF_ENCODER_BREAKDOWN=1", result.stdout)
        self.assertNotIn("DXMT9_PERF_ENCODER_BREAKDOWN_SEQ=", result.stdout)

    def test_wrapper_dry_run_includes_framegraph_dag_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "dag-dry-run",
            "--no-gputrace",
            "--dump-framegraph-dag",
            "--framegraph-dag-frame",
            "50",
            "--framegraph-dag-frame-radius",
            "2",
            "--framegraph-dag-formats",
            "json,mermaid",
            "--framegraph-dag-optimize",
            "passcoalesce",
            "--framegraph-dag-draws",
            "--dry-run",
        )

        dag_dir = REPO_ROOT / "traces" / "app-d3d9-3dmark05-dag-dry-run" / "analysis" / "dag"
        analysis_dir = dag_dir.parent
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"framegraph_dag_dir: {dag_dir}", result.stdout)
        self.assertIn("framegraph_dag_frame: 50", result.stdout)
        self.assertIn("framegraph_dag_frame_radius: 2", result.stdout)
        self.assertIn("framegraph_dag_formats: json,mermaid", result.stdout)
        self.assertIn("framegraph_dag_optimize: passcoalesce", result.stdout)
        self.assertIn("framegraph_dag_draws: 1", result.stdout)
        self.assertIn(f"framegraph_dag_summary: {analysis_dir / 'framegraph-dag-summary.md'}", result.stdout)
        self.assertIn(f"framegraph_dag_candidates_csv: {analysis_dir / 'framegraph-dag-candidates.csv'}", result.stdout)
        self.assertIn(f"framegraph_dag_preopt_summary: {analysis_dir / 'framegraph-dag-preopt-summary.md'}", result.stdout)
        self.assertIn(f"framegraph_dag_preopt_candidates_csv: {analysis_dir / 'framegraph-dag-preopt-candidates.csv'}", result.stdout)
        self.assertIn(f"framegraph_dag_postopt_summary: {analysis_dir / 'framegraph-dag-postopt-summary.md'}", result.stdout)
        self.assertIn(f"framegraph_dag_postopt_candidates_csv: {analysis_dir / 'framegraph-dag-postopt-candidates.csv'}", result.stdout)
        self.assertIn(f"DXMT9_RENDERER_DUMP_DAG={dag_dir}", result.stdout)
        self.assertIn("DXMT9_RENDERER_DUMP_DAG_FRAME=50", result.stdout)
        self.assertIn("DXMT9_RENDERER_DUMP_DAG_FRAME_RADIUS=2", result.stdout)
        self.assertIn("DXMT9_RENDERER_DUMP_DAG_FORMATS=json\\,mermaid", result.stdout)
        self.assertIn("DXMT9_RENDERER_DUMP_DAG_OPTIMIZE=passcoalesce", result.stdout)
        self.assertIn("DXMT9_RENDERER_DUMP_DAG_DRAWS=1", result.stdout)

    def test_wrapper_framegraph_dag_defaults_to_capture_frame(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "dag-defaults",
            "--frame",
            "60",
            "--no-gputrace",
            "--dump-framegraph-dag",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("framegraph_dag_frame: 60", result.stdout)
        self.assertIn("framegraph_dag_frame_radius: 0", result.stdout)
        self.assertIn("framegraph_dag_formats: json,mermaid", result.stdout)
        self.assertIn("DXMT9_RENDERER_DUMP_DAG_FRAME=60", result.stdout)
        self.assertIn("DXMT9_RENDERER_DUMP_DAG_FRAME_RADIUS=0", result.stdout)

    def test_wrapper_rejects_invalid_framegraph_dag_frame(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--framegraph-dag-frame",
            "bad",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("--framegraph-dag-frame must be a positive integer", result.stderr)

    def test_wrapper_rejects_no_encoder_breakdown_with_gputrace(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--frame",
            "50",
            "--no-encoder-breakdown",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("--no-encoder-breakdown requires --no-gputrace", result.stderr)

    def test_wrapper_no_gputrace_index_diagnostics_auto_scope_to_frame(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--frame",
            "50",
            "--no-gputrace",
            "--probe-apply-index-cache-opt-candidate",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_PERF_ENCODER_BREAKDOWN=1", result.stdout)
        self.assertIn("DXMT9_PERF_ENCODER_BREAKDOWN_SEQ=50", result.stdout)

    def test_wrapper_no_gputrace_index_diagnostics_can_keep_all_frames(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--frame",
            "50",
            "--no-gputrace",
            "--measure-index-reuse",
            "--encoder-breakdown-all-frames",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_PERF_ENCODER_BREAKDOWN=1", result.stdout)
        self.assertNotIn("DXMT9_PERF_ENCODER_BREAKDOWN_SEQ=", result.stdout)

    def test_wrapper_dry_run_includes_index_reuse_measure_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--measure-index-reuse",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_MEASURE_INDEX_REUSE=1", result.stdout)

    def test_wrapper_dry_run_prints_xctrace_sidecar_commands(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "xctrace-sidecar",
            "--no-gputrace",
            "--measure-index-reuse",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("measure_index_reuse: 1", result.stdout)
        self.assertIn("metal_system_trace:", result.stdout)
        self.assertIn("metal_gpu_intervals_xml:", result.stdout)
        export_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("xctrace_system_trace_export_cmd:")
        )
        summary_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("xctrace_system_trace_summary_cmd:")
        )
        self.assertIn("xcrun xctrace export", export_line)
        self.assertIn("metal-system.trace", export_line)
        self.assertIn("summarize_xctrace_metal_intervals.py", summary_line)
        self.assertIn("--indexed-probe-draws", summary_line)
        self.assertIn("--require-xctrace-render-rows", summary_line)
        self.assertIn("--min-dxmt-join-coverage", summary_line)
        self.assertIn("--require-route-verdicts", summary_line)
        self.assertIn("3dmark05-perf-indexed-probe-draws.csv", summary_line)
        self.assertIn("xctrace-metal-gpu-intervals-summary.md", summary_line)

    def test_wrapper_dry_run_includes_index_cache_opt_candidate_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--measure-index-cache-opt-candidate",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_MEASURE_INDEX_REUSE=1", result.stdout)
        self.assertIn("DXMT9_MEASURE_INDEX_CACHE_OPT_CANDIDATE=1", result.stdout)

    def test_wrapper_default_enables_promoted_pair_but_not_mutating_probes(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT_EXPERIMENT_PROFILE=perf", result.stdout)
        self.assertIn("DXMT_DISABLE_AUTO_EXPAND_INDEXED=1", result.stdout)
        # The promoted offload+index-cache pair matches the engine default
        # (on); the env vars are the off switch.
        self.assertIn("DXMT9_OFFLOAD_COMMIT_REPLAY=1", result.stdout)
        self.assertIn("DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE=1", result.stdout)
        # Diagnostic mutating probes stay off without their explicit flags.
        self.assertNotIn("DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_CACHE=1", result.stdout)
        self.assertNotIn("DXMT9_PROBE_APPLY_INDEX_CACHE_OPT_CANDIDATE=1", result.stdout)
        self.assertNotIn("DXMT9_MEASURE_INDEX_CACHE_OPT_CANDIDATE=1", result.stdout)

    def test_wrapper_env_opt_out_disables_promoted_pair(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--dry-run",
            env={
                "DXMT9_OFFLOAD_COMMIT_REPLAY": "0",
                "DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE": "0",
            },
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_OFFLOAD_COMMIT_REPLAY=0", result.stdout)
        self.assertIn("DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE=0", result.stdout)

    def test_wrapper_dry_run_includes_apply_index_cache_opt_candidate_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--probe-apply-index-cache-opt-candidate",
            "--probe-apply-index-cache-opt-candidate-min-gain-pct",
            "12",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_MEASURE_INDEX_REUSE=1", result.stdout)
        self.assertIn("DXMT9_MEASURE_INDEX_CACHE_OPT_CANDIDATE=1", result.stdout)
        self.assertIn("DXMT9_PROBE_APPLY_INDEX_CACHE_OPT_CANDIDATE=1", result.stdout)
        self.assertIn(
            "DXMT9_PROBE_APPLY_INDEX_CACHE_OPT_CANDIDATE_MIN_GAIN_PCT=12",
            result.stdout,
        )

    def test_wrapper_dry_run_includes_opaque_depth_index_cache_opt_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--optimize-opaque-depth-index-cache",
            "--optimize-opaque-depth-index-cache-min-gain-pct",
            "12",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE=1", result.stdout)
        self.assertIn(
            "DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE_MIN_GAIN_PCT=12",
            result.stdout,
        )
        self.assertNotIn("DXMT9_PROBE_APPLY_INDEX_CACHE_OPT_CANDIDATE=1", result.stdout)
        self.assertNotIn("DXMT9_MEASURE_INDEX_CACHE_OPT_CANDIDATE=1", result.stdout)

    def test_wrapper_rejects_invalid_opaque_depth_index_cache_min_gain(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--optimize-opaque-depth-index-cache-min-gain-pct",
            "bad",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "--optimize-opaque-depth-index-cache-min-gain-pct must be",
            result.stderr,
        )

    def test_wrapper_dry_run_includes_screen_blend_index_cache_opt_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--optimize-screen-blend-index-cache",
            "--optimize-screen-blend-index-cache-min-gain-pct",
            "11",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_CACHE=1", result.stdout)
        self.assertIn(
            "DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_CACHE_MIN_GAIN_PCT=11",
            result.stdout,
        )
        self.assertIn(
            "warning: --optimize-screen-blend-index-cache is mechanism/profiling-only until a same-input semantic proof is attached.",
            result.stdout,
        )
        self.assertIn(
            "warning: add --semantic-image-policy exact|lsb1",
            result.stdout,
        )
        self.assertNotIn("DXMT9_PROBE_APPLY_INDEX_CACHE_OPT_CANDIDATE=1", result.stdout)
        self.assertNotIn("DXMT9_MEASURE_INDEX_CACHE_OPT_CANDIDATE=1", result.stdout)

    def test_wrapper_rejects_invalid_screen_blend_index_cache_min_gain(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--optimize-screen-blend-index-cache-min-gain-pct",
            "bad",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "--optimize-screen-blend-index-cache-min-gain-pct must be",
            result.stderr,
        )

    def test_wrapper_dry_run_includes_unsafe_nonopaque_apply_index_cache_opt_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--probe-apply-index-cache-opt-candidate",
            "--probe-apply-index-cache-opt-candidate-unsafe-nonopaque",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_PROBE_APPLY_INDEX_CACHE_OPT_CANDIDATE=1", result.stdout)
        self.assertIn(
            "DXMT9_PROBE_APPLY_INDEX_CACHE_OPT_CANDIDATE_UNSAFE_NONOPAQUE=1",
            result.stdout,
        )
        self.assertIn(
            "warning: --probe-apply-index-cache-opt-candidate-unsafe-nonopaque",
            result.stdout,
        )

    def test_wrapper_dry_run_includes_indexed_geometry_dump_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "geometry-dump-dry-run",
            "--no-gputrace",
            "--dump-indexed-geometry",
            "--dump-indexed-geometry-cbufs",
            "--dump-indexed-geometry-max-draws",
            "3",
            "--dump-indexed-geometry-vs",
            "0x7836c3b4c98a465b",
            "--dump-indexed-geometry-ps",
            "0x11cc89f85cc54054",
            "--dump-indexed-geometry-texture0",
            "0x200000100000081",
            "--dump-indexed-geometry-texture0-width",
            "512",
            "--dump-indexed-geometry-texture0-height",
            "64",
            "--dump-indexed-geometry-texture0-format",
            "2",
            "--probe-reverse-indexed-triangles-rows",
            "60/0,60/1",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("geometry_dump_dir:", result.stdout)
        self.assertIn("DXMT9_MEASURE_INDEX_REUSE=1", result.stdout)
        self.assertIn(
            "DXMT9_DUMP_INDEXED_GEOMETRY_DIR=",
            result.stdout,
        )
        self.assertIn(
            "traces/app-d3d9-3dmark05-geometry-dump-dry-run/analysis/geometry",
            result.stdout,
        )
        self.assertIn("DXMT9_DUMP_INDEXED_GEOMETRY_MAX_DRAWS=3", result.stdout)
        self.assertIn("DXMT9_DUMP_INDEXED_GEOMETRY_CBUFS=1", result.stdout)
        self.assertIn(
            "DXMT9_DUMP_INDEXED_GEOMETRY_VS=0x7836c3b4c98a465b",
            result.stdout,
        )
        self.assertIn(
            "DXMT9_DUMP_INDEXED_GEOMETRY_PS=0x11cc89f85cc54054",
            result.stdout,
        )
        self.assertIn(
            "DXMT9_DUMP_INDEXED_GEOMETRY_TEXTURE0=0x200000100000081",
            result.stdout,
        )
        self.assertIn("DXMT9_DUMP_INDEXED_GEOMETRY_TEXTURE0_WIDTH=512", result.stdout)
        self.assertIn("DXMT9_DUMP_INDEXED_GEOMETRY_TEXTURE0_HEIGHT=64", result.stdout)
        self.assertIn("DXMT9_DUMP_INDEXED_GEOMETRY_TEXTURE0_FORMAT=2", result.stdout)
        self.assertIn(
            "DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES_ROWS=60/0\\,60/1",
            result.stdout,
        )

    def test_wrapper_dry_run_includes_depth_attachment_dump_env(self) -> None:
        depth_path = REPO_ROOT / "traces/depth-sidecar/analysis/depth.bin"

        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "depth-sidecar",
            "--frame",
            "50",
            "--no-gputrace",
            "--dump-depth-attachment-handle",
            "0x300000100000001",
            "--dump-depth-attachment-seq",
            "50",
            "--dump-depth-attachment-enc",
            "2",
            "--dump-depth-attachment-path",
            "traces/depth-sidecar/analysis/depth.bin",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"depth_attachment_dump: {depth_path}", result.stdout)
        self.assertIn(
            "DXMT9_DUMP_DEPTH_ATTACHMENT_HANDLE=0x300000100000001",
            result.stdout,
        )
        self.assertIn(f"DXMT9_DUMP_DEPTH_ATTACHMENT_PATH={depth_path}", result.stdout)
        self.assertIn("DXMT9_DUMP_DEPTH_ATTACHMENT_SEQ=50", result.stdout)
        self.assertIn("DXMT9_DUMP_DEPTH_ATTACHMENT_ENC=2", result.stdout)

    def test_wrapper_dry_run_defaults_depth_attachment_dump_path(self) -> None:
        depth_path = (
            REPO_ROOT
            / "traces/app-d3d9-3dmark05-depth-default/analysis/frame50-depth.bin"
        )

        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "depth-default",
            "--frame",
            "50",
            "--no-gputrace",
            "--dump-depth-attachment-handle",
            "0x300000100000001",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"depth_attachment_dump: {depth_path}", result.stdout)
        self.assertIn(f"DXMT9_DUMP_DEPTH_ATTACHMENT_PATH={depth_path}", result.stdout)

    def test_wrapper_dry_run_includes_color_attachment_dump_env(self) -> None:
        color_path = REPO_ROOT / "traces/color-sidecar/analysis/color.bin"

        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "color-sidecar",
            "--frame",
            "50",
            "--no-gputrace",
            "--dump-color-attachment-handle",
            "0x30000900000000b",
            "--dump-color-attachment-seq",
            "50",
            "--dump-color-attachment-enc",
            "2",
            "--dump-color-attachment-path",
            "traces/color-sidecar/analysis/color.bin",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"color_attachment_dump: {color_path}", result.stdout)
        self.assertIn(
            "DXMT9_DUMP_COLOR_ATTACHMENT_HANDLE=0x30000900000000b",
            result.stdout,
        )
        self.assertIn(f"DXMT9_DUMP_COLOR_ATTACHMENT_PATH={color_path}", result.stdout)
        self.assertIn("DXMT9_DUMP_COLOR_ATTACHMENT_SEQ=50", result.stdout)
        self.assertIn("DXMT9_DUMP_COLOR_ATTACHMENT_ENC=2", result.stdout)

    def test_wrapper_dry_run_defaults_color_attachment_dump_path(self) -> None:
        color_path = (
            REPO_ROOT
            / "traces/app-d3d9-3dmark05-color-default/analysis/frame50-color.bin"
        )

        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "color-default",
            "--frame",
            "50",
            "--no-gputrace",
            "--dump-color-attachment-handle",
            "0x30000900000000b",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"color_attachment_dump: {color_path}", result.stdout)
        self.assertIn(f"DXMT9_DUMP_COLOR_ATTACHMENT_PATH={color_path}", result.stdout)

    def test_wrapper_dry_run_defaults_color_attachment_index_without_handle(self) -> None:
        color_path = (
            REPO_ROOT
            / "traces/app-d3d9-3dmark05-color-index-default/analysis/frame50-color.bin"
        )

        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "color-index-default",
            "--frame",
            "50",
            "--no-gputrace",
            "--dump-color-attachment-seq",
            "50",
            "--dump-color-attachment-enc",
            "2",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"color_attachment_dump: {color_path}", result.stdout)
        self.assertIn("DXMT9_DUMP_COLOR_ATTACHMENT_INDEX=0", result.stdout)
        self.assertIn("DXMT9_DUMP_COLOR_ATTACHMENT_SEQ=50", result.stdout)
        self.assertIn("DXMT9_DUMP_COLOR_ATTACHMENT_ENC=2", result.stdout)

    def test_wrapper_dry_run_includes_color_attachment_after_draw_env(self) -> None:
        color_path = REPO_ROOT / "traces/color-after-draw/analysis/color.bin"

        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "color-after-draw",
            "--frame",
            "517",
            "--no-gputrace",
            "--dump-color-attachment-seq",
            "517",
            "--dump-color-attachment-enc",
            "2",
            "--dump-color-attachment-after-draw",
            "--dump-color-attachment-draw",
            "272",
            "--dump-color-attachment-texture0",
            "0x200000100000077",
            "--dump-color-attachment-path",
            "traces/color-after-draw/analysis/color.bin",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"color_attachment_dump: {color_path}", result.stdout)
        self.assertIn(f"DXMT9_DUMP_COLOR_ATTACHMENT_PATH={color_path}", result.stdout)
        self.assertIn("DXMT9_DUMP_COLOR_ATTACHMENT_INDEX=0", result.stdout)
        self.assertIn("DXMT9_DUMP_COLOR_ATTACHMENT_SEQ=517", result.stdout)
        self.assertIn("DXMT9_DUMP_COLOR_ATTACHMENT_ENC=2", result.stdout)
        self.assertIn("DXMT9_DUMP_COLOR_ATTACHMENT_AFTER_DRAW=1", result.stdout)
        self.assertIn("DXMT9_DUMP_COLOR_ATTACHMENT_DRAW=272", result.stdout)
        self.assertIn(
            "DXMT9_DUMP_COLOR_ATTACHMENT_TEXTURE0=0x200000100000077",
            result.stdout,
        )

    def test_wrapper_dry_run_includes_color_attachment_after_draws_dir_env(self) -> None:
        color_dir = REPO_ROOT / "traces/color-history/analysis/color-history"

        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "color-history",
            "--frame",
            "517",
            "--no-gputrace",
            "--dump-color-attachment-seq",
            "517",
            "--dump-color-attachment-enc",
            "2",
            "--dump-color-attachment-after-draw",
            "--dump-color-attachment-draws",
            "272,273,274",
            "--dump-color-attachment-texture0",
            "0x200000100000077",
            "--dump-color-attachment-dir",
            "traces/color-history/analysis/color-history",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"color_attachment_dump_dir: {color_dir}", result.stdout)
        self.assertIn(f"DXMT9_DUMP_COLOR_ATTACHMENT_DIR={color_dir}", result.stdout)
        self.assertIn("DXMT9_DUMP_COLOR_ATTACHMENT_INDEX=0", result.stdout)
        self.assertIn("DXMT9_DUMP_COLOR_ATTACHMENT_SEQ=517", result.stdout)
        self.assertIn("DXMT9_DUMP_COLOR_ATTACHMENT_ENC=2", result.stdout)
        self.assertIn("DXMT9_DUMP_COLOR_ATTACHMENT_AFTER_DRAW=1", result.stdout)
        self.assertIn("DXMT9_DUMP_COLOR_ATTACHMENT_DRAWS=272\\,273\\,274", result.stdout)
        self.assertIn(
            "DXMT9_DUMP_COLOR_ATTACHMENT_TEXTURE0=0x200000100000077",
            result.stdout,
        )

    def test_wrapper_dry_run_includes_color_attachment_command_index_range_env(self) -> None:
        color_dir = REPO_ROOT / "traces/color-command-history/analysis/color-history"

        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "color-command-history",
            "--frame",
            "517",
            "--no-gputrace",
            "--dump-color-attachment-seq",
            "517",
            "--dump-color-attachment-after-draw",
            "--dump-color-attachment-command-index-min",
            "293",
            "--dump-color-attachment-command-index-max",
            "330",
            "--dump-color-attachment-dir",
            "traces/color-command-history/analysis/color-history",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"color_attachment_dump_dir: {color_dir}", result.stdout)
        self.assertIn(f"DXMT9_DUMP_COLOR_ATTACHMENT_DIR={color_dir}", result.stdout)
        self.assertIn("DXMT9_DUMP_COLOR_ATTACHMENT_INDEX=0", result.stdout)
        self.assertIn("DXMT9_DUMP_COLOR_ATTACHMENT_SEQ=517", result.stdout)
        self.assertIn("DXMT9_DUMP_COLOR_ATTACHMENT_AFTER_DRAW=1", result.stdout)
        self.assertIn("DXMT9_DUMP_COLOR_ATTACHMENT_COMMAND_INDEX_MIN=293", result.stdout)
        self.assertIn("DXMT9_DUMP_COLOR_ATTACHMENT_COMMAND_INDEX_MAX=330", result.stdout)

    def test_wrapper_dry_run_includes_color_attachment_texture0s_dir_env(self) -> None:
        color_dir = REPO_ROOT / "traces/color-texture-history/analysis/color-history"

        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "color-texture-history",
            "--frame",
            "517",
            "--no-gputrace",
            "--dump-color-attachment-seq",
            "517",
            "--dump-color-attachment-after-draw",
            "--dump-color-attachment-texture0s",
            "0x200000100000077,0x200000100000080",
            "--dump-color-attachment-dir",
            "traces/color-texture-history/analysis/color-history",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"color_attachment_dump_dir: {color_dir}", result.stdout)
        self.assertIn(f"DXMT9_DUMP_COLOR_ATTACHMENT_DIR={color_dir}", result.stdout)
        self.assertIn("DXMT9_DUMP_COLOR_ATTACHMENT_INDEX=0", result.stdout)
        self.assertIn("DXMT9_DUMP_COLOR_ATTACHMENT_SEQ=517", result.stdout)
        self.assertIn("DXMT9_PERF_ENCODER_BREAKDOWN_SEQ=517", result.stdout)
        self.assertIn("DXMT9_DUMP_COLOR_ATTACHMENT_AFTER_DRAW=1", result.stdout)
        self.assertIn(
            "DXMT9_DUMP_COLOR_ATTACHMENT_TEXTURE0S=0x200000100000077\\,0x200000100000080",
            result.stdout,
        )

    def test_wrapper_dry_run_includes_draw_texture_dump_env(self) -> None:
        texture_dir = REPO_ROOT / "traces/texture-sidecar/analysis/textures"

        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "texture-sidecar",
            "--frame",
            "60",
            "--no-gputrace",
            "--dump-draw-texture-handles",
            "0x20000010000008d,0x200000100000072",
            "--dump-draw-texture-seq",
            "60",
            "--dump-draw-texture-enc",
            "2",
            "--dump-draw-texture-dir",
            "traces/texture-sidecar/analysis/textures",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"draw_texture_dump_dir: {texture_dir}", result.stdout)
        self.assertIn(
            "DXMT9_DUMP_DRAW_TEXTURE_HANDLES=0x20000010000008d\\,0x200000100000072",
            result.stdout,
        )
        self.assertIn(f"DXMT9_DUMP_DRAW_TEXTURE_DIR={texture_dir}", result.stdout)
        self.assertIn("DXMT9_DUMP_DRAW_TEXTURE_SEQ=60", result.stdout)
        self.assertIn("DXMT9_DUMP_DRAW_TEXTURE_ENC=2", result.stdout)

    def test_wrapper_dry_run_defaults_draw_texture_dump_dir(self) -> None:
        texture_dir = (
            REPO_ROOT
            / "traces/app-d3d9-3dmark05-texture-default/analysis/textures"
        )

        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "texture-default",
            "--frame",
            "60",
            "--no-gputrace",
            "--dump-draw-texture-handles",
            "0x20000010000008d",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"draw_texture_dump_dir: {texture_dir}", result.stdout)
        self.assertIn(f"DXMT9_DUMP_DRAW_TEXTURE_DIR={texture_dir}", result.stdout)

    def test_wrapper_dry_run_includes_draw_texture_descriptor_dump_env(self) -> None:
        texture_dir = (
            REPO_ROOT
            / "traces/app-d3d9-3dmark05-texture-desc/analysis/textures"
        )

        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "texture-desc",
            "--frame",
            "506",
            "--no-gputrace",
            "--dump-draw-texture0-width",
            "512",
            "--dump-draw-texture0-height",
            "64",
            "--dump-draw-texture0-format",
            "2",
            "--dump-draw-texture-seq",
            "506",
            "--dump-draw-texture-enc",
            "4",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"draw_texture_dump_dir: {texture_dir}", result.stdout)
        self.assertIn(f"DXMT9_DUMP_DRAW_TEXTURE_DIR={texture_dir}", result.stdout)
        self.assertIn("DXMT9_DUMP_DRAW_TEXTURE0_WIDTH=512", result.stdout)
        self.assertIn("DXMT9_DUMP_DRAW_TEXTURE0_HEIGHT=64", result.stdout)
        self.assertIn("DXMT9_DUMP_DRAW_TEXTURE0_FORMAT=2", result.stdout)
        self.assertIn("DXMT9_DUMP_DRAW_TEXTURE_SEQ=506", result.stdout)
        self.assertIn("DXMT9_DUMP_DRAW_TEXTURE_ENC=4", result.stdout)

    def test_wrapper_dry_run_includes_draw_texture0_any_env(self) -> None:
        texture_dir = (
            REPO_ROOT
            / "traces/app-d3d9-3dmark05-texture-any/analysis/textures"
        )

        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "texture-any",
            "--frame",
            "506",
            "--no-gputrace",
            "--dump-draw-texture0-any",
            "--dump-draw-texture-seq",
            "506",
            "--dump-draw-texture-enc",
            "4",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"draw_texture_dump_dir: {texture_dir}", result.stdout)
        self.assertIn(f"DXMT9_DUMP_DRAW_TEXTURE_DIR={texture_dir}", result.stdout)
        self.assertIn("DXMT9_DUMP_DRAW_TEXTURE0_ANY=1", result.stdout)
        self.assertIn("DXMT9_DUMP_DRAW_TEXTURE_SEQ=506", result.stdout)
        self.assertIn("DXMT9_DUMP_DRAW_TEXTURE_ENC=4", result.stdout)

    def test_wrapper_rejects_draw_texture_dir_without_handles(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--dump-draw-texture-dir",
            "traces/texture-sidecar/analysis/textures",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "--dump-draw-texture-seq/enc/dir require --dump-draw-texture-handles or --dump-draw-texture0-* filter",
            result.stderr,
            result.stderr,
        )

    def test_wrapper_rejects_depth_attachment_path_without_handle(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--dump-depth-attachment-path",
            "traces/depth-sidecar/analysis/depth.bin",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "--dump-depth-attachment-seq/enc/path require --dump-depth-attachment-handle",
            result.stderr,
        )

    def test_wrapper_rejects_color_attachment_path_without_selector(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--dump-color-attachment-path",
            "traces/color-sidecar/analysis/color.bin",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "--dump-color-attachment-path requires --dump-color-attachment-handle, --dump-color-attachment-index, --dump-color-attachment-seq/enc, --dump-color-attachment-draw, --dump-color-attachment-draws, --dump-color-attachment-command-index, --dump-color-attachment-command-index-min/max, --dump-color-attachment-texture0, or --dump-color-attachment-texture0s",
            result.stderr,
        )

    def test_wrapper_rejects_color_attachment_after_draw_without_draw_or_texture(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--dump-color-attachment-after-draw",
            "--dump-color-attachment-seq",
            "517",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "--dump-color-attachment-after-draw requires --dump-color-attachment-draw, --dump-color-attachment-draws, --dump-color-attachment-command-index, --dump-color-attachment-command-index-min/max, --dump-color-attachment-texture0, or --dump-color-attachment-texture0s",
            result.stderr,
        )

    def test_wrapper_rejects_color_attachment_draws_without_dir(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--dump-color-attachment-after-draw",
            "--dump-color-attachment-seq",
            "517",
            "--dump-color-attachment-draws",
            "272,273",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "--dump-color-attachment-draws requires --dump-color-attachment-dir or --dump-color-attachment-roi-summary-path",
            result.stderr,
        )

    def test_wrapper_rejects_color_attachment_command_index_range_without_dir(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--dump-color-attachment-after-draw",
            "--dump-color-attachment-seq",
            "517",
            "--dump-color-attachment-command-index-min",
            "293",
            "--dump-color-attachment-command-index-max",
            "330",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "--dump-color-attachment-command-index-min/max requires --dump-color-attachment-dir or --dump-color-attachment-roi-summary-path",
            result.stderr,
        )

    def test_wrapper_rejects_color_attachment_texture0s_without_dir(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--dump-color-attachment-after-draw",
            "--dump-color-attachment-seq",
            "517",
            "--dump-color-attachment-texture0s",
            "0x200000100000077,0x200000100000080",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "--dump-color-attachment-texture0s requires --dump-color-attachment-dir or --dump-color-attachment-roi-summary-path",
            result.stderr,
        )

    def test_wrapper_dry_run_includes_color_attachment_roi_summary_env(self) -> None:
        summary_path = (
            REPO_ROOT
            / "traces/color-roi-summary/analysis/color-roi-summary.csv"
        )

        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "color-roi-summary",
            "--frame",
            "820",
            "--no-gputrace",
            "--dump-color-attachment-seq",
            "820",
            "--dump-color-attachment-after-draw",
            "--dump-color-attachment-command-index-min",
            "0",
            "--dump-color-attachment-command-index-max",
            "260",
            "--dump-color-attachment-roi-summary-path",
            "traces/color-roi-summary/analysis/color-roi-summary.csv",
            "--dump-color-attachment-roi",
            "700,190,850,330:muzzle",
            "--dump-color-attachment-roi",
            "500,120,900,430:weapon",
            "--dump-color-attachment-bright-threshold",
            "210",
            "--dump-color-attachment-white-threshold",
            "235",
            "--dump-color-attachment-warm-red-threshold",
            "175",
            "--dump-color-attachment-warm-green-threshold",
            "105",
            "--dump-color-attachment-warm-blue-margin",
            "40",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"color_attachment_roi_summary: {summary_path}", result.stdout)
        self.assertIn("DXMT9_DUMP_COLOR_ATTACHMENT_INDEX=0", result.stdout)
        self.assertIn("DXMT9_DUMP_COLOR_ATTACHMENT_SEQ=820", result.stdout)
        self.assertIn("DXMT9_DUMP_COLOR_ATTACHMENT_AFTER_DRAW=1", result.stdout)
        self.assertIn("DXMT9_DUMP_COLOR_ATTACHMENT_COMMAND_INDEX_MIN=0", result.stdout)
        self.assertIn("DXMT9_DUMP_COLOR_ATTACHMENT_COMMAND_INDEX_MAX=260", result.stdout)
        self.assertIn(
            f"DXMT9_DUMP_COLOR_ATTACHMENT_ROI_SUMMARY_PATH={summary_path}",
            result.stdout,
        )
        self.assertIn("DXMT9_DUMP_COLOR_ATTACHMENT_WARM_RED_THRESHOLD=175", result.stdout)
        self.assertIn("DXMT9_DUMP_COLOR_ATTACHMENT_WARM_GREEN_THRESHOLD=105", result.stdout)
        self.assertIn("DXMT9_DUMP_COLOR_ATTACHMENT_WARM_BLUE_MARGIN=40", result.stdout)
        self.assertIn(
            "DXMT9_DUMP_COLOR_ATTACHMENT_ROIS=700\\,190\\,850\\,330:muzzle\\;500\\,120\\,900\\,430:weapon",
            result.stdout,
        )
        self.assertIn("DXMT9_DUMP_COLOR_ATTACHMENT_BRIGHT_THRESHOLD=210", result.stdout)
        self.assertIn("DXMT9_DUMP_COLOR_ATTACHMENT_WHITE_THRESHOLD=235", result.stdout)

    def test_wrapper_dry_run_includes_x8_shader_alpha_fill_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--suppress-x8-rt-pixel-format-view",
            "--x8-shader-alpha-fill",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_SUPPRESS_X8_RT_PIXEL_FORMAT_VIEW=1", result.stdout)
        self.assertIn("DXMT9_X8_SHADER_ALPHA_FILL=1", result.stdout)

    def test_summarizer_accepts_partial_log_without_result_json(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            output_dir = Path(tmp) / "run"
            output_dir.mkdir()
            output_dir.joinpath("dxmt9.log").write_text(
                "\n".join([
                    "[dxmt9-bridge-perf] bridge_factory=1 bridge_draw=2",
                    "[dxmt9-perf-encoder seq=60 encoder=2 draw_calls=3 "
                    "primitive_count=30 vertex_count=90 "
                    "route_depth_only_draws=1 route_depth_only_primitives=10 "
                    "route_depth_only_vertices=30 "
                    "route_programmable_textured_draws=2 "
                    "route_programmable_textured_primitives=20 "
                    "route_programmable_textured_vertices=60 "
                    "route_alpha_blend_primitives=20 "
                    "pso_state_samples=3 stream_handle_changes=4]",
                    "[dxmt9-perf-encoder-stream seq=60 encoder=2 stream=0 samples=3 "
                    "metal_binds=3]",
                    "[dxmt9-perf] present_encoded=5 draw_calls=7 "
                    "map_buffer_total_ms=0.250 completion_wait_ms=1.500",
                ]),
                encoding="utf-8",
            )

            result = subprocess.run(
                ["python3", str(SUMMARIZER), str(output_dir)],
                cwd=REPO_ROOT,
                text=True,
                capture_output=True,
                check=False,
            )

            summary = output_dir / "3dmark05-perf-summary.md"
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(summary.exists())
            text = summary.read_text(encoding="utf-8")
            self.assertIn("- Status: `partial-log`", text)
            self.assertIn("| `present_encoded` | `5` |", text)
            encoder_csv = output_dir / "3dmark05-perf-encoders.csv"
            self.assertTrue(encoder_csv.exists())
            with encoder_csv.open(newline="", encoding="utf-8") as f:
                rows = list(csv.DictReader(f))
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0]["route_depth_only_primitives"], "10")
            self.assertEqual(rows[0]["route_programmable_textured_primitives"], "20")
            self.assertEqual(rows[0]["route_alpha_blend_primitives"], "20")
            self.assertTrue(output_dir.joinpath("3dmark05-perf-encoder-streams.csv").exists())

    def test_finalizer_result_json_gate_rejects_partial_log(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            output_dir = Path(tmp) / "run"
            output_dir.mkdir()
            output_dir.joinpath("dxmt9.log").write_text(
                "[dxmt9-perf] present_encoded=5 draw_calls=7\n",
                encoding="utf-8",
            )

            result = self.run_script(
                FINALIZER,
                "--suffix",
                "partial-log",
                "--output-dir",
                str(output_dir),
                "--require-result-json",
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn("missing required result.json", result.stderr)

    def test_finalizer_partial_stable_frame_proof_accepts_partial_log(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            output_dir = Path(tmp) / "run"
            output_dir.mkdir()
            output_dir.joinpath("dxmt9.log").write_text(
                "[dxmt9-perf] present_encoded=5 draw_calls=7\n",
                encoding="utf-8",
            )
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                FINALIZER,
                "--suffix",
                "partial-stable-proof",
                "--output-dir",
                str(output_dir),
                "--baseline-joined",
                str(baseline_joined),
                "--require-stable-frame-proof",
                "--allow-partial-stable-frame-proof",
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn("warning: missing result.json; using dxmt9.log partial-run counters", result.stderr)
        self.assertIn("missing Xcode encoder counters CSV", result.stderr)
        self.assertNotIn("missing required result.json", result.stderr)

    def test_finalizer_result_json_gate_wins_over_partial_stable_frame_proof(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            output_dir = Path(tmp) / "run"
            output_dir.mkdir()
            output_dir.joinpath("dxmt9.log").write_text(
                "[dxmt9-perf] present_encoded=5 draw_calls=7\n",
                encoding="utf-8",
            )
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                FINALIZER,
                "--suffix",
                "partial-stable-proof",
                "--output-dir",
                str(output_dir),
                "--baseline-joined",
                str(baseline_joined),
                "--require-stable-frame-proof",
                "--allow-partial-stable-frame-proof",
                "--require-result-json",
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn("missing required result.json", result.stderr)

    def test_xcode_summarizer_joins_x8_shader_alpha_fill_fields(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            encoder_csv = Path(tmp) / "encoders.csv"
            encoder_csv.write_text(
                "\n".join([
                    "seq,encoder,x8_rt_texture_binding_samples,x8_shader_alpha_fill_samples,x8_shader_alpha_fill_mask_or",
                    "60,8,2,2,0x3",
                ]),
                encoding="utf-8",
            )

            summarizer = load_xcode_summarizer()
            dxmt = summarizer.load_dxmt_from_csv(encoder_csv)
            joined = summarizer.join_dxmt({"seq": 60, "enc": 8}, dxmt)

        self.assertEqual(joined["dxmt_x8_rt_texture_binding_samples"], 2)
        self.assertEqual(joined["dxmt_x8_shader_alpha_fill_samples"], 2)
        self.assertEqual(joined["dxmt_x8_shader_alpha_fill_mask_or"], "0x3")

    def test_xcode_summarizer_derives_index_reuse_fields(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            encoder_csv = Path(tmp) / "encoders.csv"
            encoder_csv.write_text(
                "\n".join([
                    "seq,encoder,indexed_vertex_reference_count,indexed_unique_vertex_estimate,indexed_vertex_reuse_samples,indexed_vertex_reuse_skipped,indexed_vertex_cache_miss_estimate_16,indexed_vertex_cache_miss_estimate_32,indexed_vertex_cache_miss_estimate_64",
                    "60,2,300,120,1,0,150,120,120",
                ]),
                encoding="utf-8",
            )

            summarizer = load_xcode_summarizer()
            dxmt = summarizer.load_dxmt_from_csv(encoder_csv)
            joined = {
                "seq": 60,
                "enc": 2,
                "buffer_write_mib": 1.0,
                "vs_buffer_write_mib": 1.0,
                "vs_invocations": 120.0,
            }
            joined = summarizer.join_dxmt(joined, dxmt)

        self.assertEqual(joined["dxmt_indexed_vertex_reference_count"], 300)
        self.assertEqual(joined["dxmt_indexed_unique_vertex_estimate"], 120)
        self.assertEqual(joined["dxmt_indexed_vertex_reuse_ratio"], 2.5)
        self.assertEqual(joined["dxmt_vs_invocations_per_indexed_unique_vertex"], 1.0)
        self.assertEqual(joined["dxmt_indexed_vertex_cache_miss_estimate_16"], 150)
        self.assertEqual(joined["dxmt_indexed_vertex_cache_miss_over_unique_16"], 1.25)
        self.assertEqual(joined["dxmt_vs_invocations_per_indexed_cache_miss_32"], 1.0)

    def test_xcode_summarizer_classifies_hidden_backend_storage(self) -> None:
        summarizer = load_xcode_summarizer()
        joined = {
            "buffer_write_mib": 225.0,
            "vs_buffer_write_mib": 224.0,
            "vs_buffer_bytes_per_vs_invocation": 1500.0,
            "vs_buffer_bytes_per_primitive": 2400.0,
            "tiled_vertex_buffer_mib": 10.0,
            "tiled_primitive_block_mib": 1.0,
            "vs_invocations": 1000.0,
            "dxmt_draw_calls": 10,
            "dxmt_vertex_count": 1000,
            "dxmt_primitive_count": 400,
            "dxmt_vsout_layout_last": "0xfff",
            "dxmt_argbuf_table_bytes": 1024,
            "dxmt_argbuf_cbuf_bytes": 1024,
            "dxmt_set_vertex_bytes_bytes": 0,
            "dxmt_transient_vertex_bytes": 0,
            "dxmt_transient_index_bytes": 0,
            "dxmt_stream0_stride_min": 24,
            "dxmt_stream0_stride_max": 24,
            "dxmt_stream_handle_changes": 10,
            "dxmt_ib_handle_changes": 10,
        }

        summarizer.derive_dxmt_attribution(joined)

        self.assertEqual(joined["dxmt_gpu_write_hint"], "gpu_vs_buffer_write")
        self.assertEqual(
            joined["dxmt_backend_storage_class"],
            "hidden_vertex_tiler_parameter_storage",
        )
        self.assertEqual(joined["dxmt_backend_storage_confidence"], "high")
        self.assertGreater(joined["dxmt_hidden_backend_write_ratio"], 0.90)

    def test_wrapper_dry_run_includes_vs_output_scratch_trim_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--trim-vs-output-scratch",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_TRIM_VS_OUTPUT_SCRATCH=1", result.stdout)

    def test_wrapper_dry_run_includes_render_state_ab_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--disable-cull",
            "--disable-scissor",
            "--probe-disable-alpha-blend",
            "--probe-disable-alpha-blend-texture0",
            "0x200000100000080",
            "--probe-disable-alpha-blend-texture0-width",
            "128",
            "--probe-disable-alpha-blend-texture0-height",
            "128",
            "--probe-disable-alpha-blend-texture0-format",
            "2",
            "--probe-disable-depth-write",
            "--probe-depth-func-always",
            "--force-cull-mode",
            "back",
            "--force-visible",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT_DISABLE_CULL=1", result.stdout)
        self.assertIn("DXMT_DISABLE_SCISSOR=1", result.stdout)
        self.assertIn("DXMT9_PROBE_DISABLE_ALPHA_BLEND=1", result.stdout)
        self.assertIn(
            "DXMT9_PROBE_DISABLE_ALPHA_BLEND_TEXTURE0=0x200000100000080",
            result.stdout,
        )
        self.assertIn(
            "DXMT9_PROBE_DISABLE_ALPHA_BLEND_TEXTURE0_WIDTH=128",
            result.stdout,
        )
        self.assertIn(
            "DXMT9_PROBE_DISABLE_ALPHA_BLEND_TEXTURE0_HEIGHT=128",
            result.stdout,
        )
        self.assertIn(
            "DXMT9_PROBE_DISABLE_ALPHA_BLEND_TEXTURE0_FORMAT=2",
            result.stdout,
        )
        self.assertIn("DXMT9_PROBE_DISABLE_DEPTH_WRITE=1", result.stdout)
        self.assertIn("DXMT9_PROBE_DEPTH_FUNC_ALWAYS=1", result.stdout)
        self.assertIn("DXMT_DEBUG_FORCE_CULL_MODE=back", result.stdout)
        self.assertIn("DXMT_DEBUG_FORCE_VISIBLE=1", result.stdout)

    def test_wrapper_dry_run_includes_scoped_depth_func_texture_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--probe-depth-func-always-row",
            "506/4",
            "--probe-depth-func-always-class",
            "screen-blend",
            "--probe-depth-func-always-texture0",
            "0x200000100000075",
            "--probe-depth-func-always-texture0-width",
            "512",
            "--probe-depth-func-always-texture0-height",
            "64",
            "--probe-depth-func-always-texture0-format",
            "2",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_PROBE_DEPTH_FUNC_ALWAYS=1", result.stdout)
        self.assertIn("DXMT9_PROBE_DEPTH_FUNC_ALWAYS_ROW=506/4", result.stdout)
        self.assertIn("DXMT9_PROBE_DEPTH_FUNC_ALWAYS_CLASS=screen-blend", result.stdout)
        self.assertIn("DXMT9_PROBE_DEPTH_FUNC_ALWAYS_TEXTURE0=0x200000100000075", result.stdout)
        self.assertIn("DXMT9_PROBE_DEPTH_FUNC_ALWAYS_TEXTURE0_WIDTH=512", result.stdout)
        self.assertIn("DXMT9_PROBE_DEPTH_FUNC_ALWAYS_TEXTURE0_HEIGHT=64", result.stdout)
        self.assertIn("DXMT9_PROBE_DEPTH_FUNC_ALWAYS_TEXTURE0_FORMAT=2", result.stdout)

    def test_wrapper_dry_run_includes_visibility_scout_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--suffix",
            "visibility-scout-dry-run",
            "--no-gputrace",
            "--visibility-scout-row",
            "60/2",
            "--visibility-scout-draw-indices",
            "36..37",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_VISIBILITY_SCOUT=1", result.stdout)
        self.assertIn("DXMT9_VISIBILITY_SCOUT_ROW=60/2", result.stdout)
        self.assertIn(
            "traces/app-d3d9-3dmark05-visibility-scout-dry-run/analysis/"
            "frame60-visibility-scout.csv",
            result.stdout,
        )
        self.assertIn("visibility_scout_draw_indices: 36..37", result.stdout)
        self.assertIn(
            "traces/app-d3d9-3dmark05-visibility-scout-dry-run/analysis/"
            "frame60-visibility-scout-summary.md",
            result.stdout,
        )
        self.assertIn(
            "traces/app-d3d9-3dmark05-visibility-scout-dry-run/analysis/"
            "frame60-visibility-scout-summary.csv",
            result.stdout,
        )

    def test_wrapper_dry_run_includes_scoped_force_texture_white_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--probe-force-texture-white-row",
            "50/2",
            "--probe-force-texture-white-classes",
            "depth-read,screen-blend,textured",
            "--probe-force-texture-white-texture0",
            "0x20000010000007f",
            "--probe-force-texture-white-texture0-width",
            "1024",
            "--probe-force-texture-white-texture0-height",
            "256",
            "--probe-force-texture-white-texture0-format",
            "31",
            "--probe-indexed-triangle-encoder-draw-min",
            "177",
            "--probe-indexed-triangle-encoder-draw-max",
            "186",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_PROBE_FORCE_TEXTURE_WHITE_ROW=50/2", result.stdout)
        self.assertIn(
            r"DXMT9_PROBE_FORCE_TEXTURE_WHITE_CLASSES=depth-read\,screen-blend\,textured",
            result.stdout,
        )
        self.assertIn(
            "DXMT9_PROBE_FORCE_TEXTURE_WHITE_TEXTURE0=0x20000010000007f",
            result.stdout,
        )
        self.assertIn(
            "DXMT9_PROBE_FORCE_TEXTURE_WHITE_TEXTURE0_WIDTH=1024",
            result.stdout,
        )
        self.assertIn(
            "DXMT9_PROBE_FORCE_TEXTURE_WHITE_TEXTURE0_HEIGHT=256",
            result.stdout,
        )
        self.assertIn(
            "DXMT9_PROBE_FORCE_TEXTURE_WHITE_TEXTURE0_FORMAT=31",
            result.stdout,
        )
        self.assertIn("DXMT9_PROBE_INDEXED_TRIANGLE_ENCODER_DRAW_MIN=177", result.stdout)
        self.assertIn("DXMT9_PROBE_INDEXED_TRIANGLE_ENCODER_DRAW_MAX=186", result.stdout)
        self.assertIn(
            "warning: --probe-force-texture-white is diagnostic only",
            result.stdout,
        )

    def test_wrapper_dry_run_includes_draw_local_force_texture_white_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--probe-force-texture-white-row",
            "520/2",
            "--probe-force-texture-white-texture0",
            "0x200000100000077",
            "--probe-force-texture-white-draw-ordinal",
            "325857",
            "--probe-force-texture-white-draw-ordinal-min",
            "325800",
            "--probe-force-texture-white-draw-ordinal-max",
            "325900",
            "--probe-force-texture-white-command-index",
            "7",
            "--probe-force-texture-white-command-index-min",
            "6",
            "--probe-force-texture-white-command-index-max",
            "8",
            "--probe-force-texture-white-command-draw-index",
            "4",
            "--probe-force-texture-white-command-draw-index-min",
            "3",
            "--probe-force-texture-white-command-draw-index-max",
            "5",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_PROBE_FORCE_TEXTURE_WHITE_ROW=520/2", result.stdout)
        self.assertIn(
            "DXMT9_PROBE_FORCE_TEXTURE_WHITE_TEXTURE0=0x200000100000077",
            result.stdout,
        )
        self.assertIn("DXMT9_PROBE_FORCE_TEXTURE_WHITE_DRAW_ORDINALS=325857", result.stdout)
        self.assertIn("DXMT9_PROBE_FORCE_TEXTURE_WHITE_DRAW_ORDINAL_MIN=325800", result.stdout)
        self.assertIn("DXMT9_PROBE_FORCE_TEXTURE_WHITE_DRAW_ORDINAL_MAX=325900", result.stdout)
        self.assertIn("DXMT9_PROBE_FORCE_TEXTURE_WHITE_COMMAND_INDEXES=7", result.stdout)
        self.assertIn("DXMT9_PROBE_FORCE_TEXTURE_WHITE_COMMAND_INDEX_MIN=6", result.stdout)
        self.assertIn("DXMT9_PROBE_FORCE_TEXTURE_WHITE_COMMAND_INDEX_MAX=8", result.stdout)
        self.assertIn(
            "DXMT9_PROBE_FORCE_TEXTURE_WHITE_COMMAND_DRAW_INDEXES=4",
            result.stdout,
        )
        self.assertIn(
            "DXMT9_PROBE_FORCE_TEXTURE_WHITE_COMMAND_DRAW_INDEX_MIN=3",
            result.stdout,
        )
        self.assertIn(
            "DXMT9_PROBE_FORCE_TEXTURE_WHITE_COMMAND_DRAW_INDEX_MAX=5",
            result.stdout,
        )

    def test_wrapper_dry_run_includes_effect_draw_trace_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--frame-sampling",
            "--effect-draw-trace-seq",
            "1037",
            "--effect-draw-trace-seq-min",
            "1000",
            "--effect-draw-trace-seq-max",
            "1100",
            "--effect-draw-trace-enc",
            "2",
            "--effect-draw-trace-texture0",
            "0x20000010000007f",
            "--effect-draw-trace-texture0-width",
            "1024",
            "--effect-draw-trace-texture0-height",
            "256",
            "--effect-draw-trace-texture0-format",
            "31",
            "--effect-draw-trace-primitive-type",
            "0",
            "--effect-draw-trace-point-sprite",
            "--effect-draw-trace-include-non-alpha",
            "--effect-draw-trace-include-untextured",
            "--effect-draw-trace-geometry-max-refs",
            "96",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_PERF_FRAME_SAMPLING=1", result.stdout)
        self.assertIn("DXMT9_EFFECT_DRAW_TRACE=1", result.stdout)
        self.assertIn("DXMT9_EFFECT_DRAW_TRACE_SEQ=1037", result.stdout)
        self.assertIn("DXMT9_EFFECT_DRAW_TRACE_SEQ_MIN=1000", result.stdout)
        self.assertIn("DXMT9_EFFECT_DRAW_TRACE_SEQ_MAX=1100", result.stdout)
        self.assertIn("DXMT9_EFFECT_DRAW_TRACE_ENC=2", result.stdout)
        self.assertIn("DXMT9_EFFECT_DRAW_TRACE_TEXTURE0=0x20000010000007f", result.stdout)
        self.assertIn("DXMT9_EFFECT_DRAW_TRACE_TEXTURE0_WIDTH=1024", result.stdout)
        self.assertIn("DXMT9_EFFECT_DRAW_TRACE_TEXTURE0_HEIGHT=256", result.stdout)
        self.assertIn("DXMT9_EFFECT_DRAW_TRACE_TEXTURE0_FORMAT=31", result.stdout)
        self.assertIn("DXMT9_EFFECT_DRAW_TRACE_PRIMITIVE_TYPE=0", result.stdout)
        self.assertIn("DXMT9_EFFECT_DRAW_TRACE_POINT_SPRITE=1", result.stdout)
        self.assertIn("DXMT9_EFFECT_DRAW_TRACE_INCLUDE_NON_ALPHA=1", result.stdout)
        self.assertIn("DXMT9_EFFECT_DRAW_TRACE_INCLUDE_UNTEXTURED=1", result.stdout)
        self.assertIn("DXMT9_EFFECT_DRAW_TRACE_GEOMETRY=1", result.stdout)
        self.assertIn("DXMT9_EFFECT_DRAW_TRACE_GEOMETRY_MAX_REFS=96", result.stdout)

    def test_wrapper_rejects_retired_draw_packet_actual_change_flag(self) -> None:
        # --probe-draw-packet-actual-change was retired with the fat packet: it
        # compared a draw packet's declared state bits against the unix-side
        # DeviceState, and there is no packet delta any more. Pinned as REJECTED
        # rather than deleted, so the flag cannot quietly come back as a no-op.
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--probe-draw-packet-actual-change",
            "--dry-run",
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unknown argument", result.stderr + result.stdout)

    def test_wrapper_dry_run_includes_vs_const_setter_range_probe_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--probe-vs-const-setter-range",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_PERF_VS_CONST_SETTER_RANGE=1", result.stdout)

    def test_wrapper_dry_run_includes_force_expand_indexed_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--force-expand-indexed",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT_FORCE_EXPAND_INDEXED=1", result.stdout)

    def test_wrapper_dry_run_includes_scoped_force_expand_indexed_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--probe-force-expand-indexed-row",
            "50/2",
            "--probe-force-expand-indexed-classes",
            "depth-read,screen-blend,textured",
            "--probe-indexed-triangle-encoder-draw-min",
            "71",
            "--probe-indexed-triangle-encoder-draw-max",
            "188",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_PROBE_FORCE_EXPAND_INDEXED_ROW=50/2", result.stdout)
        self.assertIn(
            r"DXMT9_PROBE_FORCE_EXPAND_INDEXED_CLASSES=depth-read\,screen-blend\,textured",
            result.stdout,
        )
        self.assertIn("DXMT9_PROBE_INDEXED_TRIANGLE_ENCODER_DRAW_MIN=71", result.stdout)
        self.assertIn("DXMT9_PROBE_INDEXED_TRIANGLE_ENCODER_DRAW_MAX=188", result.stdout)
        self.assertIn(
            "warning: --probe-force-expand-indexed is diagnostic only",
            result.stdout,
        )

    def test_wrapper_dry_run_includes_reverse_nonopaque_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--probe-reverse-nonopaque-indexed-triangles",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_PROBE_REVERSE_NONOPAQUE_INDEXED_TRIANGLES=1", result.stdout)

    def test_wrapper_dry_run_includes_indexed_triangle_encoder_draw_range_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--probe-sort-indexed-triangles-by-min-index",
            "--probe-reverse-indexed-triangles-row",
            "60/2",
            "--probe-indexed-triangle-encoder-draw-min",
            "71",
            "--probe-indexed-triangle-encoder-draw-max",
            "188",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_PROBE_SORT_INDEXED_TRIANGLES_BY_MIN_INDEX=1", result.stdout)
        self.assertIn("DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES_ROW=60/2", result.stdout)
        self.assertIn("DXMT9_PROBE_INDEXED_TRIANGLE_ENCODER_DRAW_MIN=71", result.stdout)
        self.assertIn("DXMT9_PROBE_INDEXED_TRIANGLE_ENCODER_DRAW_MAX=188", result.stdout)

    def test_wrapper_dry_run_includes_indexed_triangle_encoder_draw_exclude_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--probe-apply-index-cache-opt-candidate",
            "--probe-reverse-indexed-triangles-row",
            "50/2",
            "--probe-indexed-triangle-encoder-draw-min",
            "14",
            "--probe-indexed-triangle-encoder-draw-max",
            "32",
            "--probe-indexed-triangle-encoder-draw-exclude",
            "18,21",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_PROBE_APPLY_INDEX_CACHE_OPT_CANDIDATE=1", result.stdout)
        self.assertIn("DXMT9_PROBE_INDEXED_TRIANGLE_ENCODER_DRAW_MIN=14", result.stdout)
        self.assertIn("DXMT9_PROBE_INDEXED_TRIANGLE_ENCODER_DRAW_MAX=32", result.stdout)
        self.assertIn(
            r"DXMT9_PROBE_INDEXED_TRIANGLE_ENCODER_DRAW_EXCLUDE=18\,21",
            result.stdout,
        )

    def test_wrapper_dry_run_includes_no_alpha_blend_class_filter(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--probe-optimize-indexed-triangles-vertex-cache",
            "--probe-reverse-indexed-triangles-row",
            "50/2",
            "--probe-reverse-indexed-triangles-classes",
            "depth-read,no-alpha-blend,no-scissor,textured",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_PROBE_OPTIMIZE_INDEXED_TRIANGLES_VERTEX_CACHE=1", result.stdout)
        self.assertIn("DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES_ROW=50/2", result.stdout)
        self.assertIn(
            "DXMT9_PROBE_REVERSE_INDEXED_TRIANGLES_CLASSES=depth-read\\,no-alpha-blend\\,no-scissor\\,textured",
            result.stdout,
        )

    def test_wrapper_dry_run_includes_scissor_rect_probe_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--probe-scissor-rect",
            "0,0,190,553",
            "--probe-scissor-rect-row",
            "60/4",
            "--probe-scissor-rect-classes",
            "large4096,alpha-blend,scissor",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT9_PROBE_SCISSOR_RECT=0\\,0\\,190\\,553", result.stdout)
        self.assertIn("DXMT9_PROBE_SCISSOR_RECT_ROW=60/4", result.stdout)
        self.assertIn(
            "DXMT9_PROBE_SCISSOR_RECT_CLASSES=large4096\\,alpha-blend\\,scissor",
            result.stdout,
        )

    def test_wrapper_rejects_invalid_force_cull_mode(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--force-cull-mode",
            "sideways",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("--force-cull-mode must be one of", result.stderr)

    def test_wrapper_dry_run_omits_metal_capture_layer_env_for_gputrace_by_default(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT_METAL_CAPTURE_FRAME=60", result.stdout)
        self.assertIn("DXMT_METAL_CAPTURE_PATH=", result.stdout)
        self.assertNotIn("MTL_CAPTURE_ENABLED=1", result.stdout)

    def test_wrapper_dry_run_includes_metal_capture_layer_env_when_opted_in(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--dry-run",
            env={"DXMT_3DMARK05_SET_MTL_CAPTURE_ENABLED": "1"},
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("MTL_CAPTURE_ENABLED=1", result.stdout)

    def test_wrapper_rejects_file_gputrace_without_capture_layer_preflight(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            wine_root = Path(tmp) / "wine"
            bin_dir = wine_root / "bin"
            bin_dir.mkdir(parents=True)
            for name in ("wine.real", "wine-preloader"):
                path = bin_dir / name
                path.write_text("plain wine binary\n", encoding="utf-8")
                path.chmod(0o755)

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                f"file-capture-layer-preflight-{wine_root.name}",
                "--timeout",
                "120",
                "--min-free-mb",
                "0",
                env={
                    "DXMT_3DMARK05_WINE_ROOT": str(wine_root),
                    "DXMT_3DMARK05_REQUIRE_UNLOCKED": "0",
                    "DXMT_3DMARK05_ALLOW_LOW_TRACE_FREE_MB": "1",
                },
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn("file_capture_layer_preflight: status=fail", result.stdout)
        self.assertIn("wine-binaries-lack-metal-capture-enabled", result.stdout)
        self.assertIn("Metal file capture requires Apple's capture layer", result.stderr)
        self.assertIn("--with-wine-capture-layer", result.stderr)
        self.assertIn("DXMT_3DMARK05_ALLOW_NO_FILE_CAPTURE_LAYER=1", result.stderr)

    def test_wrapper_dry_run_can_wrap_with_wine_capture_layer(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            wine_root = Path(tmp) / "wine"
            self.write_capture_layer_wine_root(wine_root)

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "capture-layer-wrapper-dry",
                "--with-wine-capture-layer",
                "--min-free-mb",
                "0",
                "--dry-run",
                env={"DXMT_3DMARK05_WINE_ROOT": str(wine_root)},
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("wine_capture_layer_wrapper: enabled", result.stdout)
        self.assertIn("file_capture_layer_preflight: status=pass", result.stdout)
        self.assertIn("reason=wine-capture-layer-wrapper", result.stdout)
        cmd_line = next(line for line in result.stdout.splitlines() if line.startswith("cmd:"))
        self.assertIn("run_with_wine_metal_capture_layer.sh", cmd_line)
        self.assertIn("--allow-3dmark05", cmd_line)
        self.assertIn("run_experiment.py", cmd_line)
        self.assertNotIn("MTL_CAPTURE_ENABLED=1", result.stdout)

    def test_wrapper_capture_layer_preflight_checks_capture_copies(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            wine_root = Path(tmp) / "wine"
            bin_dir = wine_root / "bin"
            bin_dir.mkdir(parents=True)
            for name in ("wine.real", "wine-preloader"):
                path = bin_dir / name
                path.write_text("plain wine binary\n", encoding="utf-8")
                path.chmod(0o755)

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                f"capture-layer-wrapper-preflight-{wine_root.name}",
                "--with-wine-capture-layer",
                "--timeout",
                "120",
                "--min-free-mb",
                "0",
                env={
                    "DXMT_3DMARK05_WINE_ROOT": str(wine_root),
                    "DXMT_3DMARK05_REQUIRE_UNLOCKED": "0",
                    "DXMT_3DMARK05_ALLOW_LOW_TRACE_FREE_MB": "1",
                },
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn("file_capture_layer_preflight: status=fail", result.stdout)
        self.assertIn("capture-real-lacks-metal-capture-enabled", result.stdout)
        self.assertIn("capture-enabled Wine copies are unavailable", result.stderr)

    def test_wrapper_dry_run_reports_capture_layer_wrapper_preflight_failure(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            wine_root = Path(tmp) / "wine"
            bin_dir = wine_root / "bin"
            bin_dir.mkdir(parents=True)
            for name in ("wine.real", "wine-preloader"):
                path = bin_dir / name
                path.write_text("plain wine binary\n", encoding="utf-8")
                path.chmod(0o755)

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "capture-layer-wrapper-dry-fail",
                "--with-wine-capture-layer",
                "--min-free-mb",
                "0",
                "--dry-run",
                env={"DXMT_3DMARK05_WINE_ROOT": str(wine_root)},
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("file_capture_layer_preflight: status=fail", result.stdout)
        self.assertIn("capture-real-lacks-metal-capture-enabled", result.stdout)
        self.assertIn(
            "dry-run: file capture layer preflight would fail for --with-wine-capture-layer",
            result.stdout,
        )

    def test_wrapper_dry_run_forwards_metal_capture_destination(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--dry-run",
            env={"DXMT_3DMARK05_METAL_CAPTURE_DESTINATION": "developerTools"},
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT_METAL_CAPTURE_DESTINATION=developerTools", result.stdout)
        self.assertIn("metal_capture_destination: developerTools", result.stdout)
        self.assertIn("gputrace: developerTools", result.stdout)
        self.assertIn("xcode_developer_tools_capture_preflight:", result.stdout)
        self.assertIn("choose a frame known to be reached", result.stdout)

    def test_wrapper_dry_run_accepts_xcode_developer_tools_capture_flag(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--xcode-developer-tools-capture",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("DXMT_METAL_CAPTURE_DESTINATION=developerTools", result.stdout)
        self.assertIn("metal_capture_destination: developerTools", result.stdout)
        self.assertIn("no direct file expected", result.stdout)
        self.assertIn("attach Xcode to the real Wine child", result.stdout)
        self.assertNotIn("MTL_CAPTURE_ENABLED=1", result.stdout)

    def test_wrapper_dry_run_marks_required_xcode_attach_preflight(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--xcode-developer-tools-capture",
            "--require-xcode-attach-preflight",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("xcode_developer_tools_capture_preflight_required: 1", result.stdout)

    def test_wrapper_rejects_xcode_attach_preflight_without_developer_tools_capture(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--require-xcode-attach-preflight",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "--require-xcode-attach-preflight requires --xcode-developer-tools-capture",
            result.stderr,
        )

    def test_wrapper_xcode_attach_preflight_only_passes_with_fake_osascript(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            fake_osascript = Path(tmp) / "fake-osascript.sh"
            fake_devtools = Path(tmp) / "fake-devtools-security.sh"
            self.write_fake_osascript(fake_osascript)
            self.write_fake_devtools_security(fake_devtools)

            result = self.run_script(
                RUN_WRAPPER,
                "--xcode-attach-preflight-only",
                env={
                    "DXMT_3DMARK05_OSASCRIPT_BIN": str(fake_osascript),
                    "DXMT_3DMARK05_DEVTOOLS_SECURITY_BIN": str(fake_devtools),
                    "FAKE_OSASCRIPT_OUTPUT": (
                        "status=pass reason=attach-by-pid-enabled "
                        "attach_by_pid_found=true attach_by_pid_enabled=true"
                    ),
                },
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("xcode_attach_preflight: status=pass", result.stdout)

    def test_wrapper_xcode_attach_preflight_only_fails_with_fake_osascript(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            fake_osascript = Path(tmp) / "fake-osascript.sh"
            fake_devtools = Path(tmp) / "fake-devtools-security.sh"
            self.write_fake_osascript(fake_osascript)
            self.write_fake_devtools_security(fake_devtools)

            result = self.run_script(
                RUN_WRAPPER,
                "--xcode-attach-preflight-only",
                env={
                    "DXMT_3DMARK05_OSASCRIPT_BIN": str(fake_osascript),
                    "DXMT_3DMARK05_DEVTOOLS_SECURITY_BIN": str(fake_devtools),
                    "FAKE_OSASCRIPT_OUTPUT": (
                        "status=fail reason=process-list-loading "
                        "attach_by_pid_found=true attach_by_pid_enabled=false "
                        "attach_process_first_item=Getting Process List..."
                    ),
                },
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn("xcode_attach_preflight: status=fail", result.stdout)

    def test_wrapper_xcode_attach_preflight_reports_disabled_developer_mode(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            fake_osascript = Path(tmp) / "fake-osascript.sh"
            fake_devtools = Path(tmp) / "fake-devtools-security.sh"
            self.write_fake_osascript(fake_osascript)
            self.write_fake_devtools_security(fake_devtools)

            result = self.run_script(
                RUN_WRAPPER,
                "--xcode-attach-preflight-only",
                env={
                    "DXMT_3DMARK05_OSASCRIPT_BIN": str(fake_osascript),
                    "DXMT_3DMARK05_DEVTOOLS_SECURITY_BIN": str(fake_devtools),
                    "FAKE_DEVTOOLS_SECURITY_OUTPUT": "Developer mode is currently disabled.",
                    "FAKE_OSASCRIPT_OUTPUT": (
                        "status=pass reason=attach-by-pid-enabled "
                        "attach_by_pid_found=true attach_by_pid_enabled=true"
                    ),
                },
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn("reason=developer-mode-disabled", result.stdout)
        self.assertNotIn("attach-by-pid-enabled", result.stdout)

    def test_wrapper_required_xcode_attach_preflight_fails_before_launch(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            fake_osascript = Path(tmp) / "fake-osascript.sh"
            fake_devtools = Path(tmp) / "fake-devtools-security.sh"
            self.write_fake_osascript(fake_osascript)
            self.write_fake_devtools_security(fake_devtools)

            result = self.run_script(
                RUN_WRAPPER,
                "--xcode-developer-tools-capture",
                "--require-xcode-attach-preflight",
                env={
                    "DXMT_3DMARK05_REQUIRE_UNLOCKED": "0",
                    "DXMT_3DMARK05_OSASCRIPT_BIN": str(fake_osascript),
                    "DXMT_3DMARK05_DEVTOOLS_SECURITY_BIN": str(fake_devtools),
                    "FAKE_OSASCRIPT_OUTPUT": (
                        "status=fail reason=process-list-loading "
                        "attach_by_pid_found=true attach_by_pid_enabled=false "
                        "attach_process_first_item=Getting Process List..."
                    ),
                },
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn("Xcode attach preflight failed", result.stderr)
        self.assertIn("xcode_attach_preflight: status=fail", result.stdout)

    def test_wrapper_rejects_invalid_metal_capture_destination(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--metal-capture-destination",
            "bad-destination",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("--metal-capture-destination must be one of", result.stderr)

    def test_wrapper_no_gputrace_ignores_invalid_capture_destination_env(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--dry-run",
            env={"DXMT_3DMARK05_METAL_CAPTURE_DESTINATION": "bad-destination"},
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("gputrace: disabled", result.stdout)

    def test_wrapper_developer_tools_capture_checks_logs_instead_of_file(self) -> None:
        text = RUN_WRAPPER.read_text(encoding="utf-8")

        self.assertIn("capture_destination_is_developer_tools", text)
        self.assertIn("destination=1 started", text)
        self.assertIn("destination=1 stopped", text)
        self.assertIn("Metal developerTools capture was requested", text)

    def test_wrapper_dry_run_omits_metal_capture_layer_env_without_gputrace(self) -> None:
        result = self.run_script(
            RUN_WRAPPER,
            "--no-gputrace",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("MTL_CAPTURE_ENABLED=1", result.stdout)

    def test_capture_layer_wrapper_rejects_3dmark05_by_default(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            wine_root = Path(tmp) / "wine"
            self.write_capture_layer_wine_root(wine_root)

            result = self.run_script(
                CAPTURE_LAYER_WRAPPER,
                "--wine-root",
                str(wine_root),
                "--",
                "/bin/echo",
                "app-d3d9-3dmark05",
            )

            self.assertEqual(result.returncode, 2)
            self.assertIn("refusing 3DMark05 capture-layer run", result.stderr)
            self.assertEqual(
                (wine_root / "bin" / "wine.real").read_text(encoding="utf-8"),
                "original wine.real\n",
            )
            self.assertEqual(
                (wine_root / "bin" / "wine-preloader").read_text(encoding="utf-8"),
                "original wine-preloader\n",
            )

    def test_capture_layer_wrapper_allows_explicit_3dmark05_diagnostic(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            wine_root = Path(tmp) / "wine"
            self.write_capture_layer_wine_root(wine_root)
            wine_real = wine_root / "bin" / "wine.real"
            wine_preloader = wine_root / "bin" / "wine-preloader"
            original_real_inode = wine_real.stat().st_ino
            original_preloader_inode = wine_preloader.stat().st_ino
            marker = Path(tmp) / "patched-state.json"

            result = self.run_script(
                CAPTURE_LAYER_WRAPPER,
                "--wine-root",
                str(wine_root),
                "--allow-3dmark05",
                "--",
                sys.executable,
                "-c",
                (
                    "import json, pathlib, sys; "
                    "wine_root = pathlib.Path(sys.argv[1]); "
                    "marker = pathlib.Path(sys.argv[2]); "
                    "real = wine_root / 'bin' / 'wine.real'; "
                    "preloader = wine_root / 'bin' / 'wine-preloader'; "
                    "marker.write_text(json.dumps({"
                    "'real_text': real.read_text(), "
                    "'preloader_text': preloader.read_text(), "
                    "'real_inode': real.stat().st_ino, "
                    "'preloader_inode': preloader.stat().st_ino"
                    "}))"
                ),
                str(wine_root),
                str(marker),
                "app-d3d9-3dmark05",
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("run_with_wine_metal_capture_layer: patched", result.stderr)
            self.assertIn("run_with_wine_metal_capture_layer: restored", result.stderr)
            patched_state = json.loads(marker.read_text(encoding="utf-8"))
            self.assertEqual(patched_state["real_text"], "capture wine.real\nMetalCaptureEnabled\n")
            self.assertEqual(
                patched_state["preloader_text"],
                "capture wine-preloader\nMetalCaptureEnabled\n",
            )
            self.assertNotEqual(patched_state["real_inode"], original_real_inode)
            self.assertNotEqual(patched_state["preloader_inode"], original_preloader_inode)
            self.assertEqual(
                wine_real.read_text(encoding="utf-8"),
                "original wine.real\n",
            )
            self.assertEqual(
                wine_preloader.read_text(encoding="utf-8"),
                "original wine-preloader\n",
            )

    def test_system_trace_sidecar_rejects_locked_before_actual_probe(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fake_wrapper = root / "fake-probe.sh"
            actual_marker = root / "actual-called"
            trace_dir = root / "trace"
            self.write_system_trace_fake_wrapper(
                fake_wrapper,
                output_dir=root / "out",
                trace_dir=trace_dir,
                actual_marker=actual_marker,
                session_locked="yes",
            )

            result = self.run_script(
                SYSTEM_TRACE_SIDECAR,
                "--wrapper",
                str(fake_wrapper),
                "--",
                "--suffix",
                "fake-sidecar",
            )

            self.assertEqual(result.returncode, 2)
            self.assertIn("macOS session is locked", result.stderr)
            self.assertFalse(actual_marker.exists())
            self.assertFalse(trace_dir.exists())

    def test_system_trace_sidecar_wait_unlocked_times_out_without_artifacts(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fake_wrapper = root / "fake-probe.sh"
            actual_marker = root / "actual-called"
            trace_dir = root / "trace"
            self.write_system_trace_fake_wrapper(
                fake_wrapper,
                output_dir=root / "out",
                trace_dir=trace_dir,
                actual_marker=actual_marker,
                session_locked="yes",
            )

            result = self.run_script(
                SYSTEM_TRACE_SIDECAR,
                "--wrapper",
                str(fake_wrapper),
                "--wait-unlocked-sec",
                "1",
                "--wait-unlocked-interval-sec",
                "1",
                "--",
                "--suffix",
                "fake-sidecar",
            )

            self.assertEqual(result.returncode, 2)
            self.assertIn("after waiting 1s", result.stderr)
            self.assertFalse(actual_marker.exists())
            self.assertFalse(trace_dir.exists())

    def test_system_trace_sidecar_dry_run_prints_record_plan(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fake_wrapper = root / "fake-probe.sh"
            actual_marker = root / "actual-called"
            trace_dir = root / "trace"
            self.write_system_trace_fake_wrapper(
                fake_wrapper,
                output_dir=root / "out",
                trace_dir=trace_dir,
                actual_marker=actual_marker,
            )

            result = self.run_script(
                SYSTEM_TRACE_SIDECAR,
                "--wrapper",
                str(fake_wrapper),
                "--xctrace-bin",
                "/bin/false",
                "--record-delay-sec",
                "1",
                "--time-limit-sec",
                "2",
                "--summary-top",
                "7",
                "--dry-run",
                "--",
                "--suffix",
                "fake-sidecar",
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("system_trace_record_cmd:", result.stdout)
            self.assertIn("system_trace_summary_cmd:", result.stdout)
            self.assertIn("--require-xctrace-render-rows", result.stdout)
            self.assertIn("--min-dxmt-join-coverage", result.stdout)
            self.assertIn("--require-route-verdicts", result.stdout)
            self.assertIn("--time-limit 2s", result.stdout)
            self.assertIn("system_trace_encoder_breakdown: all_frames", result.stdout)
            self.assertIn("system_trace_summary_top: 7", result.stdout)
            self.assertIn("system_trace_free_space_mb:", result.stdout)
            self.assertIn("system_trace_min_free_space_mb:", result.stdout)
            self.assertFalse(actual_marker.exists())
            self.assertFalse(trace_dir.exists())

    def test_system_trace_sidecar_rejects_low_free_space_before_launch(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fake_wrapper = root / "fake-probe.sh"
            actual_marker = root / "actual-called"
            trace_dir = root / "trace"
            self.write_system_trace_fake_wrapper(
                fake_wrapper,
                output_dir=root / "out",
                trace_dir=trace_dir,
                actual_marker=actual_marker,
            )

            result = self.run_script(
                SYSTEM_TRACE_SIDECAR,
                "--wrapper",
                str(fake_wrapper),
                "--xctrace-bin",
                "/bin/false",
                "--min-free-mb",
                "999999999",
                "--",
                "--suffix",
                "fake-sidecar",
            )

            self.assertEqual(result.returncode, 2)
            self.assertIn(
                "insufficient free space for 3DMark05 system trace sidecar",
                result.stderr,
            )
            self.assertFalse(actual_marker.exists())
            self.assertFalse(trace_dir.exists())

    def test_system_trace_sidecar_dry_run_prints_cpu_summary_plan(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fake_wrapper = root / "fake-probe.sh"
            actual_marker = root / "actual-called"
            trace_dir = root / "trace"
            self.write_system_trace_fake_wrapper(
                fake_wrapper,
                output_dir=root / "out",
                trace_dir=trace_dir,
                actual_marker=actual_marker,
            )

            result = self.run_script(
                SYSTEM_TRACE_SIDECAR,
                "--wrapper",
                str(fake_wrapper),
                "--xctrace-bin",
                "/bin/false",
                "--export-cpu-summary",
                "--dry-run",
                "--",
                "--suffix",
                "fake-sidecar",
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("system_trace_cpu_summary: enabled", result.stdout)
            self.assertIn("system_trace_cpu_p4_positive_required: no", result.stdout)
            self.assertIn("system_trace_cpu_summary_cmd:", result.stdout)
            self.assertIn("summarize_xctrace_cpu_threads.py", result.stdout)
            self.assertIn("time-profile.xml", result.stdout)
            self.assertIn("xctrace-cpu-thread-summary.md", result.stdout)
            self.assertIn("--output-verdict-json", result.stdout)
            self.assertIn("xctrace-cpu-thread-verdict.json", result.stdout)
            self.assertFalse(actual_marker.exists())
            self.assertFalse(trace_dir.exists())

    def test_system_trace_sidecar_rejects_cpu_p4_gate_without_cpu_summary(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fake_wrapper = root / "fake-probe.sh"
            actual_marker = root / "actual-called"
            trace_dir = root / "trace"
            self.write_system_trace_fake_wrapper(
                fake_wrapper,
                output_dir=root / "out",
                trace_dir=trace_dir,
                actual_marker=actual_marker,
            )

            result = self.run_script(
                SYSTEM_TRACE_SIDECAR,
                "--wrapper",
                str(fake_wrapper),
                "--require-cpu-p4-positive",
                "--dry-run",
                "--",
                "--suffix",
                "fake-sidecar",
            )

            self.assertEqual(result.returncode, 2)
            self.assertIn(
                "--require-cpu-p4-positive requires --export-cpu-summary",
                result.stderr,
            )
            self.assertFalse(actual_marker.exists())
            self.assertFalse(trace_dir.exists())

    def test_system_trace_sidecar_rejects_cpu_producer_regex_without_cpu_summary(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fake_wrapper = root / "fake-probe.sh"
            actual_marker = root / "actual-called"
            trace_dir = root / "trace"
            self.write_system_trace_fake_wrapper(
                fake_wrapper,
                output_dir=root / "out",
                trace_dir=trace_dir,
                actual_marker=actual_marker,
            )

            result = self.run_script(
                SYSTEM_TRACE_SIDECAR,
                "--wrapper",
                str(fake_wrapper),
                "--cpu-producer-thread-regex",
                "0x1234",
                "--dry-run",
                "--",
                "--suffix",
                "fake-sidecar",
            )

            self.assertEqual(result.returncode, 2)
            self.assertIn(
                "--cpu-producer-thread-regex requires --export-cpu-summary",
                result.stderr,
            )
            self.assertFalse(actual_marker.exists())
            self.assertFalse(trace_dir.exists())

    def test_system_trace_sidecar_rejects_cpu_producer_from_pe_log_without_cpu_summary(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fake_wrapper = root / "fake-probe.sh"
            actual_marker = root / "actual-called"
            trace_dir = root / "trace"
            self.write_system_trace_fake_wrapper(
                fake_wrapper,
                output_dir=root / "out",
                trace_dir=trace_dir,
                actual_marker=actual_marker,
            )

            result = self.run_script(
                SYSTEM_TRACE_SIDECAR,
                "--wrapper",
                str(fake_wrapper),
                "--cpu-producer-from-pe-log",
                "--dry-run",
                "--",
                "--suffix",
                "fake-sidecar",
            )

            self.assertEqual(result.returncode, 2)
            self.assertIn(
                "--cpu-producer-from-pe-log requires --export-cpu-summary",
                result.stderr,
            )
            self.assertFalse(actual_marker.exists())
            self.assertFalse(trace_dir.exists())

    def test_system_trace_sidecar_dry_run_prints_cpu_p4_gate_plan(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fake_wrapper = root / "fake-probe.sh"
            actual_marker = root / "actual-called"
            trace_dir = root / "trace"
            self.write_system_trace_fake_wrapper(
                fake_wrapper,
                output_dir=root / "out",
                trace_dir=trace_dir,
                actual_marker=actual_marker,
            )

            result = self.run_script(
                SYSTEM_TRACE_SIDECAR,
                "--wrapper",
                str(fake_wrapper),
                "--xctrace-bin",
                "/bin/false",
                "--export-cpu-summary",
                "--cpu-producer-from-pe-log",
                "--require-cpu-p4-positive",
                "--dry-run",
                "--",
                "--suffix",
                "fake-sidecar",
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("system_trace_cpu_summary: enabled", result.stdout)
            self.assertIn("system_trace_cpu_producer_from_pe_log: yes", result.stdout)
            self.assertIn("system_trace_cpu_p4_positive_required: yes", result.stdout)
            self.assertIn("system_trace_probe_env:", result.stdout)
            self.assertIn("DXMT9_PE_RECORDER_STATS=1", result.stdout)
            self.assertIn("DXMT_LOG_LEVEL=info", result.stdout)
            self.assertIn("system_trace_cpu_producer_pe_log:", result.stdout)
            self.assertIn(str(root / "out" / "3dmark05-direct.log"), result.stdout)
            self.assertIn("system_trace_cpu_summary_cmd:", result.stdout)
            self.assertIn("--producer-thread-regex-from-pe-log", result.stdout)
            self.assertIn(
                "--producer-thread-regex-from-pe-log " + str(root / "out" / "3dmark05-direct.log"),
                result.stdout,
            )
            self.assertFalse(actual_marker.exists())
            self.assertFalse(trace_dir.exists())

    def test_system_trace_sidecar_rejects_scoped_encoder_breakdown(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fake_wrapper = root / "fake-probe.sh"
            actual_marker = root / "actual-called"
            trace_dir = root / "trace"
            self.write_system_trace_fake_wrapper(
                fake_wrapper,
                output_dir=root / "out",
                trace_dir=trace_dir,
                actual_marker=actual_marker,
            )

            result = self.run_script(
                SYSTEM_TRACE_SIDECAR,
                "--wrapper",
                str(fake_wrapper),
                "--",
                "--suffix",
                "fake-sidecar",
                "--encoder-breakdown-seq",
                "60",
            )

            self.assertEqual(result.returncode, 2)
            self.assertIn("requires all-frame or range encoder breakdown", result.stderr)
            self.assertFalse(actual_marker.exists())
            self.assertFalse(trace_dir.exists())

    def test_system_trace_sidecar_forwards_encoder_breakdown_seq_range(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fake_wrapper = root / "fake-probe.sh"
            fake_xctrace = root / "fake-xctrace.sh"
            actual_marker = root / "actual-called"
            xctrace_marker = root / "xctrace-called"
            output_dir = root / "out"
            trace_dir = root / "trace"
            self.write_system_trace_fake_wrapper(
                fake_wrapper,
                output_dir=output_dir,
                trace_dir=trace_dir,
                actual_marker=actual_marker,
                actual_sleep_sec="1",
            )
            self.write_system_trace_fake_xctrace(fake_xctrace, xctrace_marker)

            result = self.run_script(
                SYSTEM_TRACE_SIDECAR,
                "--wrapper",
                str(fake_wrapper),
                "--xctrace-bin",
                str(fake_xctrace),
                "--record-delay-sec",
                "0",
                "--time-limit-sec",
                "1",
                "--min-free-mb",
                "0",
                "--encoder-breakdown-seq-range",
                "1000:1700",
                "--skip-export-summary",
                "--",
                "--suffix",
                "fake-sidecar",
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            actual_args = actual_marker.read_text(encoding="utf-8")
            self.assertIn("--encoder-breakdown-seq-range 1000:1700", actual_args)
            self.assertNotIn("--encoder-breakdown-all-frames", actual_args)
            self.assertIn(
                "system_trace_encoder_breakdown: range 1000:1700",
                result.stdout,
            )

    def test_system_trace_sidecar_waits_then_records_when_unlocked(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fake_wrapper = root / "fake-probe.sh"
            fake_xctrace = root / "fake-xctrace.sh"
            actual_marker = root / "actual-called"
            xctrace_marker = root / "xctrace-called"
            counter_file = root / "dry-run-count"
            output_dir = root / "out"
            trace_dir = root / "trace"
            self.write_system_trace_flipping_fake_wrapper(
                fake_wrapper,
                output_dir=output_dir,
                trace_dir=trace_dir,
                actual_marker=actual_marker,
                counter_file=counter_file,
                locked_dry_runs=1,
                actual_sleep_sec="1",
            )
            self.write_system_trace_fake_xctrace(fake_xctrace, xctrace_marker)

            result = self.run_script(
                SYSTEM_TRACE_SIDECAR,
                "--wrapper",
                str(fake_wrapper),
                "--xctrace-bin",
                str(fake_xctrace),
                "--record-delay-sec",
                "0",
                "--time-limit-sec",
                "1",
                "--wait-unlocked-sec",
                "2",
                "--wait-unlocked-interval-sec",
                "1",
                "--min-free-mb",
                "0",
                "--skip-export-summary",
                "--",
                "--suffix",
                "fake-sidecar",
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(counter_file.read_text(encoding="utf-8").strip(), "2")
            self.assertTrue(actual_marker.exists())
            self.assertIn(
                "--encoder-breakdown-all-frames",
                actual_marker.read_text(encoding="utf-8"),
            )
            self.assertEqual(
                xctrace_marker.read_text(encoding="utf-8").strip(),
                str(trace_dir / "metal-system.trace"),
            )
            self.assertTrue((trace_dir / "metal-system.trace").is_dir())

    def test_system_trace_sidecar_actual_records_when_unlocked(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fake_wrapper = root / "fake-probe.sh"
            fake_xctrace = root / "fake-xctrace.sh"
            actual_marker = root / "actual-called"
            xctrace_marker = root / "xctrace-called"
            output_dir = root / "out"
            trace_dir = root / "trace"
            self.write_system_trace_fake_wrapper(
                fake_wrapper,
                output_dir=output_dir,
                trace_dir=trace_dir,
                actual_marker=actual_marker,
                actual_sleep_sec="1",
            )
            self.write_system_trace_fake_xctrace(fake_xctrace, xctrace_marker)

            result = self.run_script(
                SYSTEM_TRACE_SIDECAR,
                "--wrapper",
                str(fake_wrapper),
                "--xctrace-bin",
                str(fake_xctrace),
                "--record-delay-sec",
                "0",
                "--time-limit-sec",
                "1",
                "--min-free-mb",
                "0",
                "--skip-export-summary",
                "--",
                "--suffix",
                "fake-sidecar",
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(actual_marker.exists())
            self.assertIn(
                "--encoder-breakdown-all-frames",
                actual_marker.read_text(encoding="utf-8"),
            )
            self.assertEqual(
                xctrace_marker.read_text(encoding="utf-8").strip(),
                str(trace_dir / "metal-system.trace"),
            )
            self.assertTrue((trace_dir / "metal-system.trace").is_dir())
            self.assertTrue(
                (trace_dir / "analysis" / "system-trace-preflight.log").exists()
            )
            self.assertIn("wrote metal system trace:", result.stdout)

    def test_system_trace_sidecar_cpu_summary_allows_missing_optional_tables(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fake_wrapper = root / "fake-probe.sh"
            fake_xctrace = root / "fake-xctrace.sh"
            actual_marker = root / "actual-called"
            xctrace_marker = root / "xctrace-called"
            output_dir = root / "out"
            trace_dir = root / "trace"
            analysis_dir = trace_dir / "analysis"
            self.write_system_trace_fake_wrapper(
                fake_wrapper,
                output_dir=output_dir,
                trace_dir=trace_dir,
                actual_marker=actual_marker,
                actual_sleep_sec="1",
            )
            self.write_minimal_system_trace_encoder_csv(
                output_dir / "3dmark05-perf-encoders.csv"
            )
            self.write_system_trace_fake_xctrace_with_optional_cpu_failures(
                fake_xctrace,
                xctrace_marker,
            )

            result = self.run_script(
                SYSTEM_TRACE_SIDECAR,
                "--wrapper",
                str(fake_wrapper),
                "--xctrace-bin",
                str(fake_xctrace),
                "--record-delay-sec",
                "0",
                "--time-limit-sec",
                "1",
                "--min-free-mb",
                "0",
                "--export-cpu-summary",
                "--",
                "--suffix",
                "fake-sidecar",
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue((analysis_dir / "xctrace-cpu-thread-summary.md").exists())
            self.assertTrue((analysis_dir / "xctrace-cpu-thread-verdict.json").exists())
            self.assertFalse((analysis_dir / "time-sample.xml").exists())
            self.assertFalse((analysis_dir / "thread-info.xml").exists())
            cpu_export_log = (analysis_dir / "system-trace-cpu-export.log").read_text(
                encoding="utf-8"
            )
            self.assertIn(
                "optional CPU table missing or failed to export: time-sample",
                cpu_export_log,
            )
            self.assertIn(
                "optional CPU table missing or failed to export: thread-info",
                cpu_export_log,
            )
            verdict = json.loads(
                (analysis_dir / "xctrace-cpu-thread-verdict.json").read_text(
                    encoding="utf-8"
                )
            )
            self.assertEqual(verdict["status"], "producer-stack-negative-inconclusive")
            self.assertIn("system_trace_cpu_summary_verdict:", result.stdout)
            verdict_line = next(
                line
                for line in result.stdout.splitlines()
                if line.startswith("system_trace_cpu_summary_verdict:")
            )
            self.assertIn("holder_status=holder-not-sampled", verdict_line)
            self.assertIn("main_thread_holder_hits=0", verdict_line)
            self.assertIn("nonproducer_holder_hits=0", verdict_line)

    def test_wrapper_forwards_const_upload_gates_to_finalizer(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_output = Path(tmp) / "baseline-output"
            write_result(baseline_output)

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "forward-const-gates",
                "--compare-baseline-output",
                str(baseline_output),
                "--require-const-upload-break-bytes-decrease",
                "--require-draw-submission-batch-present",
                "--max-const-upload-break-count-ratio",
                "1.10",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        finalize_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("finalize_cmd_after_xcode_export:")
        )
        self.assertIn("--require-const-upload-break-bytes-decrease", finalize_line)
        self.assertIn("--require-draw-submission-batch-present", finalize_line)
        self.assertIn("--max-const-upload-break-count-ratio", finalize_line)
        self.assertIn("1.10", finalize_line)

    def test_wrapper_forwards_pacing_compare_gates_to_counter_compare(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_output = Path(tmp) / "baseline-output"
            write_result(baseline_output)

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "forward-pacing-counter-gates",
                "--no-gputrace",
                "--compare-baseline-output",
                str(baseline_output),
                *PACING_COMPARE_FLAGS,
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        compare_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("counter_compare_cmd:")
        )
        for flag in PACING_COMPARE_FLAGS:
            self.assertIn(flag, compare_line)

    def test_wrapper_forwards_pacing_compare_gates_to_finalizer(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_output = Path(tmp) / "baseline-output"
            write_result(baseline_output)

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "forward-pacing-finalizer-gates",
                "--compare-baseline-output",
                str(baseline_output),
                *PACING_COMPARE_FLAGS,
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        finalize_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("finalize_cmd_after_xcode_export:")
        )
        for flag in PACING_COMPARE_FLAGS:
            self.assertIn(flag, finalize_line)

    def test_wrapper_forwards_uniform_owner_compare_gates_to_counter_compare(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_output = Path(tmp) / "baseline-output"
            write_result(baseline_output)

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "forward-uniform-owner-counter-gates",
                "--no-gputrace",
                "--compare-baseline-output",
                str(baseline_output),
                *UNIFORM_OWNER_COMPARE_FLAGS,
                *UNIFORM_COMPACT_COMPARE_FLAGS,
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        compare_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("counter_compare_cmd:")
        )
        for flag in UNIFORM_OWNER_COMPARE_FLAGS:
            self.assertIn(flag, compare_line)
        for flag in UNIFORM_COMPACT_COMPARE_FLAGS:
            self.assertIn(flag, compare_line)

    def test_wrapper_forwards_uniform_owner_compare_gates_to_finalizer(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_output = Path(tmp) / "baseline-output"
            write_result(baseline_output)

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "forward-uniform-owner-finalizer-gates",
                "--compare-baseline-output",
                str(baseline_output),
                *UNIFORM_OWNER_COMPARE_FLAGS,
                *UNIFORM_COMPACT_COMPARE_FLAGS,
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        finalize_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("finalize_cmd_after_xcode_export:")
        )
        for flag in UNIFORM_OWNER_COMPARE_FLAGS:
            self.assertIn(flag, finalize_line)
        for flag in UNIFORM_COMPACT_COMPARE_FLAGS:
            self.assertIn(flag, finalize_line)

    def test_wrapper_forwards_state_elision_compare_gates_to_counter_compare(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_output = Path(tmp) / "baseline-output"
            write_result(baseline_output)

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "forward-state-elision-counter-gates",
                "--no-gputrace",
                "--compare-baseline-output",
                str(baseline_output),
                *STATE_ELISION_COMPARE_FLAGS,
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        compare_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("counter_compare_cmd:")
        )
        for flag in STATE_ELISION_COMPARE_FLAGS:
            self.assertIn(flag, compare_line)

    def test_wrapper_forwards_state_elision_compare_gates_to_finalizer(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_output = Path(tmp) / "baseline-output"
            write_result(baseline_output)

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "forward-state-elision-finalizer-gates",
                "--compare-baseline-output",
                str(baseline_output),
                *STATE_ELISION_COMPARE_FLAGS,
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        finalize_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("finalize_cmd_after_xcode_export:")
        )
        for flag in STATE_ELISION_COMPARE_FLAGS:
            self.assertIn(flag, finalize_line)

    def test_wrapper_forwards_carrier_compare_gates_to_counter_compare(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_output = Path(tmp) / "baseline-output"
            write_result(baseline_output)

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "forward-carrier-counter-gates",
                "--no-gputrace",
                "--compare-baseline-output",
                str(baseline_output),
                *CARRIER_COMPARE_FLAGS,
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        compare_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("counter_compare_cmd:")
        )
        for flag in CARRIER_COMPARE_FLAGS:
            self.assertIn(flag, compare_line)

    def test_wrapper_forwards_carrier_compare_gates_to_finalizer(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_output = Path(tmp) / "baseline-output"
            write_result(baseline_output)

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "forward-carrier-finalizer-gates",
                "--compare-baseline-output",
                str(baseline_output),
                *CARRIER_COMPARE_FLAGS,
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        finalize_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("finalize_cmd_after_xcode_export:")
        )
        for flag in CARRIER_COMPARE_FLAGS:
            self.assertIn(flag, finalize_line)

    def test_wrapper_forwards_argbuf_owner_compare_gates_to_counter_compare(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_output = Path(tmp) / "baseline-output"
            write_result(baseline_output)

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "forward-argbuf-owner-counter-gates",
                "--no-gputrace",
                "--compare-baseline-output",
                str(baseline_output),
                *ARGBUF_OWNER_COMPARE_FLAGS,
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        compare_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("counter_compare_cmd:")
        )
        for flag in ARGBUF_OWNER_COMPARE_FLAGS:
            self.assertIn(flag, compare_line)

    def test_wrapper_forwards_argbuf_owner_compare_gates_to_finalizer(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_output = Path(tmp) / "baseline-output"
            write_result(baseline_output)

            result = self.run_script(
                RUN_WRAPPER,
                "--suffix",
                "forward-argbuf-owner-finalizer-gates",
                "--compare-baseline-output",
                str(baseline_output),
                *ARGBUF_OWNER_COMPARE_FLAGS,
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        finalize_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("finalize_cmd_after_xcode_export:")
        )
        for flag in ARGBUF_OWNER_COMPARE_FLAGS:
            self.assertIn(flag, finalize_line)

    def test_finalizer_forwards_unexplained_write_gates_to_compare_script(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                FINALIZER,
                "--suffix",
                "forward-unexplained",
                "--baseline-joined",
                str(baseline_joined),
                "--require-top-unexplained-buffer-write-decrease",
                "--max-top-unexplained-buffer-write-ratio",
                "0.50",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        compare_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("xcode_compare_cmd:")
        )
        summary_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("xcode_summary_cmd:")
        )
        self.assertIn("--dxmt-streams-csv", summary_line)
        self.assertIn("--require-top-unexplained-buffer-write-decrease", compare_line)
        self.assertIn("--max-top-unexplained-buffer-write-ratio", compare_line)
        self.assertIn("0.50", compare_line)

    def test_finalizer_builds_index_cache_runtime_summary_command(self) -> None:
        result = self.run_script(
            FINALIZER,
            "--suffix",
            "index-cache-runtime",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        command_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("index_cache_runtime_cmd:")
        )
        self.assertIn("summarize_index_cache_runtime.py", command_line)
        self.assertIn("3dmark05-perf-encoders.csv", command_line)
        self.assertIn("3dmark05-perf-indexed-probe-draws.csv", command_line)
        self.assertIn("frame60-index-cache-runtime-summary.md", command_line)

    def test_finalizer_builds_indexed_class_proxy_command(self) -> None:
        result = self.run_script(
            FINALIZER,
            "--suffix",
            "class-proxy",
            "--class-proxy-top",
            "5",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        command_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("class_proxy_cmd:")
        )
        self.assertIn("analyze_indexed_probe_classes.py", command_line)
        self.assertIn("3dmark05-perf-indexed-probe-draws.csv", command_line)
        self.assertIn("--group row-state-class", command_line)
        self.assertIn("--joined-summary", command_line)
        self.assertIn("frame60-xcode-dxmt-joined-summary.csv", command_line)
        self.assertIn("--top 5", command_line)
        self.assertIn("frame60-indexed-state-class-xcode-proxy.md", command_line)
        self.assertIn("frame60-indexed-state-class-xcode-proxy.csv", command_line)

    def test_finalizer_builds_semantic_image_compare_command(self) -> None:
        result = self.run_script(
            FINALIZER,
            "--suffix",
            "semantic-image",
            "--semantic-image-policy",
            "exact",
            "--semantic-image-before",
            "before.ppm",
            "--semantic-image-after",
            "after.ppm",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        compare_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("semantic_image_compare_cmd:")
        )
        self.assertIn("scripts/tools/compare_experiment_images.py", compare_line)
        self.assertIn("--policy exact", compare_line)
        self.assertIn("--min-before-active-pct 1", compare_line)
        self.assertIn("--min-after-active-pct 1", compare_line)
        self.assertIn("frame60-semantic-image-policy-exact-compare.md", compare_line)
        self.assertIn("frame60-semantic-image-policy-exact-compare.csv", compare_line)
        self.assertIn("frame60-semantic-image-policy-exact-compare.png", compare_line)

    def test_finalizer_forwards_frame_shape_gates_to_compare_script(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                FINALIZER,
                "--suffix",
                "forward-frame-shape",
                "--baseline-joined",
                str(baseline_joined),
                "--require-top-row-key-match",
                "--max-top-draw-call-delta-ratio",
                "0.05",
                "--max-top-vertex-count-delta-ratio",
                "0.05",
                "--max-top-triangle-delta-ratio",
                "0.05",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        compare_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("xcode_compare_cmd:")
        )
        self.assertIn("--require-top-row-key-match", compare_line)
        self.assertIn("--max-top-draw-call-delta-ratio", compare_line)
        self.assertIn("--max-top-vertex-count-delta-ratio", compare_line)
        self.assertIn("--max-top-triangle-delta-ratio", compare_line)
        self.assertIn("0.05", compare_line)

    def test_finalizer_forwards_tvb_mechanism_proof_to_compare_script(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                FINALIZER,
                "--suffix",
                "forward-tvb-mechanism",
                "--baseline-joined",
                str(baseline_joined),
                "--require-tvb-mechanism-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        compare_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("xcode_compare_cmd:")
        )
        self.assertIn("--require-tvb-mechanism-proof", compare_line)

    def test_finalizer_forwards_non_target_hot_row_gates_to_compare_script(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                FINALIZER,
                "--suffix",
                "forward-non-target",
                "--baseline-joined",
                str(baseline_joined),
                "--target-row-key",
                "50/1",
                "--target-row-key",
                "50/3",
                "--max-non-target-gpu-regression-ms",
                "1.0",
                "--max-non-target-vs-buffer-write-regression-mib",
                "16",
                "--max-non-target-vs-invocations-regression-ratio",
                "0.05",
                "--max-non-target-draw-call-delta-ratio",
                "0.05",
                "--max-non-target-vertex-count-delta-ratio",
                "0.05",
                "--max-non-target-triangle-delta-ratio",
                "0.05",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        compare_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("xcode_compare_cmd:")
        )
        self.assertIn("--target-row-key", compare_line)
        self.assertIn("50/1", compare_line)
        self.assertIn("50/3", compare_line)
        self.assertIn("--max-non-target-gpu-regression-ms", compare_line)
        self.assertIn("1.0", compare_line)
        self.assertIn("--max-non-target-vs-buffer-write-regression-mib", compare_line)
        self.assertIn("16", compare_line)
        self.assertIn(
            "--max-non-target-vs-invocations-regression-ratio",
            compare_line,
        )
        self.assertIn("0.05", compare_line)
        self.assertIn("--max-non-target-draw-call-delta-ratio", compare_line)
        self.assertIn("--max-non-target-vertex-count-delta-ratio", compare_line)
        self.assertIn("--max-non-target-triangle-delta-ratio", compare_line)

    def test_finalizer_forwards_target_row_apply_gates_to_compare_script(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                FINALIZER,
                "--suffix",
                "forward-target",
                "--baseline-joined",
                str(baseline_joined),
                "--target-row-key",
                "50/0",
                "--target-row-key",
                "50/1",
                "--require-target-index-cache-miss32-decrease",
                "--require-target-index-cache-opt-miss32-decrease",
                "--require-target-reordered-index-cache-hits",
                "--require-target-vs-buffer-write-decrease",
                "--require-target-vs-invocations-decrease",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        compare_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("xcode_compare_cmd:")
        )
        self.assertIn("--target-row-key", compare_line)
        self.assertIn("50/0", compare_line)
        self.assertIn("50/1", compare_line)
        self.assertIn("--require-target-index-cache-miss32-decrease", compare_line)
        self.assertIn("--require-target-index-cache-opt-miss32-decrease", compare_line)
        self.assertIn("--require-target-reordered-index-cache-hits", compare_line)
        self.assertIn("--require-target-vs-buffer-write-decrease", compare_line)
        self.assertIn("--require-target-vs-invocations-decrease", compare_line)

    def test_finalizer_expands_cache_opt_apply_proof_preset_to_compare_script(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                FINALIZER,
                "--suffix",
                "cache-opt-apply-proof",
                "--baseline-joined",
                str(baseline_joined),
                "--target-row-key",
                "50/1",
                "--require-cache-opt-apply-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        compare_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("xcode_compare_cmd:")
        )
        self.assertIn("--require-stable-frame-proof", compare_line)
        self.assertIn("--target-row-key", compare_line)
        self.assertIn("50/1", compare_line)
        self.assertIn("--require-target-index-cache-miss32-decrease", compare_line)
        self.assertIn("--require-target-vs-buffer-write-decrease", compare_line)
        self.assertIn("--require-target-vs-invocations-decrease", compare_line)

    def test_finalizer_rejects_cache_opt_apply_proof_without_target_row(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                FINALIZER,
                "--suffix",
                "cache-opt-apply-proof-missing-row",
                "--baseline-joined",
                str(baseline_joined),
                "--require-cache-opt-apply-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "--require-cache-opt-apply-proof requires at least one --target-row-key",
            result.stderr,
        )

    def test_finalizer_rejects_semantic_image_proof_without_semantic_gate(self) -> None:
        result = self.run_script(
            FINALIZER,
            "--suffix",
            "semantic-proof-missing-image",
            "--require-semantic-image-proof",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "--require-semantic-image-proof requires --semantic-image-policy",
            result.stderr,
        )

    def test_finalizer_accepts_semantic_image_proof_with_semantic_gate(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            before = root / "before.ppm"
            after = root / "after.ppm"
            before.write_text("P3\n1 1\n255\n0 0 0\n", encoding="ascii")
            after.write_text("P3\n1 1\n255\n0 0 0\n", encoding="ascii")

            result = self.run_script(
                FINALIZER,
                "--suffix",
                "semantic-proof",
                "--semantic-image-policy",
                "exact",
                "--semantic-image-before",
                str(before),
                "--semantic-image-after",
                str(after),
                "--require-semantic-image-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        semantic_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("semantic_image_compare_cmd:")
        )
        self.assertIn("--policy exact", semantic_line)

    def test_finalizer_rejects_screen_blend_cache_proof_without_semantic_gate(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                FINALIZER,
                "--suffix",
                "screen-blend-cache-proof-missing-semantic",
                "--baseline-joined",
                str(baseline_joined),
                "--target-row-key",
                "50/2",
                "--require-screen-blend-cache-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "--require-screen-blend-cache-proof requires --semantic-image-policy",
            result.stderr,
        )

    def test_finalizer_rejects_opaque_depth_index_cache_proof_without_target_row(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                FINALIZER,
                "--suffix",
                "opaque-depth-cache-proof-missing-target-row",
                "--baseline-joined",
                str(baseline_joined),
                "--require-opaque-depth-index-cache-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "--require-opaque-depth-index-cache-proof requires at least one --target-row-key",
            result.stderr,
        )

    def test_finalizer_expands_opaque_depth_index_cache_proof(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                FINALIZER,
                "--suffix",
                "opaque-depth-cache-proof",
                "--baseline-joined",
                str(baseline_joined),
                "--target-row-key",
                "50/0",
                "--target-row-key",
                "50/1",
                "--require-opaque-depth-index-cache-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        compare_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("xcode_compare_cmd:")
        )
        self.assertIn("--require-stable-frame-proof", compare_line)
        self.assertNotIn("--require-target-index-cache-miss32-decrease", compare_line)
        self.assertIn("--require-target-index-cache-opt-miss32-decrease", compare_line)
        self.assertIn("--require-target-reordered-index-cache-hits", compare_line)
        self.assertIn("--require-target-vs-buffer-write-decrease", compare_line)
        self.assertIn("--require-target-vs-invocations-decrease", compare_line)
        self.assertIn("--target-row-key 50/0", compare_line)
        self.assertIn("--target-row-key 50/1", compare_line)

    def test_finalizer_rejects_screen_blend_cache_proof_without_target_row(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            baseline_joined = root / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")
            before = root / "before.ppm"
            after = root / "after.ppm"
            before.write_text("P3\n1 1\n255\n0 0 0\n", encoding="ascii")
            after.write_text("P3\n1 1\n255\n0 0 0\n", encoding="ascii")

            result = self.run_script(
                FINALIZER,
                "--suffix",
                "screen-blend-cache-proof-missing-target-row",
                "--baseline-joined",
                str(baseline_joined),
                "--semantic-image-policy",
                "lsb1",
                "--semantic-image-before",
                str(before),
                "--semantic-image-after",
                str(after),
                "--require-screen-blend-cache-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn(
            "--require-screen-blend-cache-proof requires at least one --target-row-key",
            result.stderr,
        )

    def test_finalizer_expands_screen_blend_cache_proof_and_semantic_gate(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            baseline_joined = root / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")
            before = root / "before.ppm"
            after = root / "after.ppm"
            before.write_text("P3\n1 1\n255\n0 0 0\n", encoding="ascii")
            after.write_text("P3\n1 1\n255\n0 0 0\n", encoding="ascii")

            result = self.run_script(
                FINALIZER,
                "--suffix",
                "screen-blend-cache-proof",
                "--baseline-joined",
                str(baseline_joined),
                "--target-row-key",
                "50/2",
                "--semantic-image-policy",
                "lsb1",
                "--semantic-image-before",
                str(before),
                "--semantic-image-after",
                str(after),
                "--require-screen-blend-cache-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        compare_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("xcode_compare_cmd:")
        )
        semantic_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("semantic_image_compare_cmd:")
        )
        self.assertIn("--require-stable-frame-proof", compare_line)
        self.assertNotIn("--require-target-index-cache-miss32-decrease", compare_line)
        self.assertIn("--require-target-index-cache-opt-miss32-decrease", compare_line)
        self.assertIn("--require-target-reordered-index-cache-hits", compare_line)
        self.assertIn("--require-target-vs-buffer-write-decrease", compare_line)
        self.assertIn("--require-target-vs-invocations-decrease", compare_line)
        self.assertIn("--target-row-key", compare_line)
        self.assertIn("50/2", compare_line)
        self.assertIn("--policy lsb1", semantic_line)

    def test_finalizer_expands_stable_frame_proof_preset(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_joined = Path(tmp) / "baseline-joined.csv"
            baseline_joined.write_text("gpu_ms\n", encoding="utf-8")

            result = self.run_script(
                FINALIZER,
                "--suffix",
                "stable-proof",
                "--baseline-joined",
                str(baseline_joined),
                "--require-stable-frame-proof",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        compare_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("xcode_compare_cmd:")
        )
        summary_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("xcode_summary_cmd:")
        )
        self.assertIn("--require-stable-frame-proof", compare_line)
        self.assertIn("--require-xcode-counter-coverage", summary_line)
        self.assertIn("--require-dxmt-join-coverage", summary_line)
        self.assertIn("--require-top-pso-attribution", summary_line)
        self.assertIn("--max-top-draw-call-delta-ratio", compare_line)
        self.assertIn("--max-top-vertex-count-delta-ratio", compare_line)
        self.assertIn("--max-top-triangle-delta-ratio", compare_line)
        self.assertIn("0.05", compare_line)

    def test_finalizer_forwards_const_upload_gates_to_compare_script(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_output = Path(tmp) / "baseline-output"
            write_result(baseline_output)

            result = self.run_script(
                FINALIZER,
                "--suffix",
                "forward-const-gates",
                "--baseline-output",
                str(baseline_output),
                "--require-const-upload-break-bytes-decrease",
                "--require-draw-submission-batch-present",
                "--max-const-upload-break-count-ratio",
                "1.10",
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        compare_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("perf_compare_cmd:")
        )
        self.assertIn("--require-const-upload-break-bytes-decrease", compare_line)
        self.assertIn("--require-draw-submission-batch-present", compare_line)
        self.assertIn("--max-const-upload-break-count-ratio", compare_line)
        self.assertIn("1.10", compare_line)

    def test_finalizer_forwards_pacing_compare_gates_to_compare_script(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_output = Path(tmp) / "baseline-output"
            write_result(baseline_output)

            result = self.run_script(
                FINALIZER,
                "--suffix",
                "forward-pacing-finalizer-gates",
                "--baseline-output",
                str(baseline_output),
                *PACING_COMPARE_FLAGS,
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        compare_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("perf_compare_cmd:")
        )
        for flag in PACING_COMPARE_FLAGS:
            self.assertIn(flag, compare_line)

    def test_finalizer_forwards_uniform_owner_compare_gates_to_compare_script(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_output = Path(tmp) / "baseline-output"
            write_result(baseline_output)

            result = self.run_script(
                FINALIZER,
                "--suffix",
                "forward-uniform-owner-finalizer-gates",
                "--baseline-output",
                str(baseline_output),
                *UNIFORM_OWNER_COMPARE_FLAGS,
                *UNIFORM_COMPACT_COMPARE_FLAGS,
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        compare_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("perf_compare_cmd:")
        )
        for flag in UNIFORM_OWNER_COMPARE_FLAGS:
            self.assertIn(flag, compare_line)
        for flag in UNIFORM_COMPACT_COMPARE_FLAGS:
            self.assertIn(flag, compare_line)

    def test_finalizer_forwards_state_elision_compare_gates_to_compare_script(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_output = Path(tmp) / "baseline-output"
            write_result(baseline_output)

            result = self.run_script(
                FINALIZER,
                "--suffix",
                "forward-state-elision-finalizer-gates",
                "--baseline-output",
                str(baseline_output),
                *STATE_ELISION_COMPARE_FLAGS,
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        compare_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("perf_compare_cmd:")
        )
        for flag in STATE_ELISION_COMPARE_FLAGS:
            self.assertIn(flag, compare_line)

    def test_finalizer_forwards_carrier_compare_gates_to_compare_script(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_output = Path(tmp) / "baseline-output"
            write_result(baseline_output)

            result = self.run_script(
                FINALIZER,
                "--suffix",
                "forward-carrier-finalizer-gates",
                "--baseline-output",
                str(baseline_output),
                *CARRIER_COMPARE_FLAGS,
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        compare_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("perf_compare_cmd:")
        )
        for flag in CARRIER_COMPARE_FLAGS:
            self.assertIn(flag, compare_line)

    def test_finalizer_forwards_argbuf_owner_compare_gates_to_compare_script(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            baseline_output = Path(tmp) / "baseline-output"
            write_result(baseline_output)

            result = self.run_script(
                FINALIZER,
                "--suffix",
                "forward-argbuf-owner-finalizer-gates",
                "--baseline-output",
                str(baseline_output),
                *ARGBUF_OWNER_COMPARE_FLAGS,
                "--dry-run",
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        compare_line = next(
            line for line in result.stdout.splitlines()
            if line.startswith("perf_compare_cmd:")
        )
        for flag in ARGBUF_OWNER_COMPARE_FLAGS:
            self.assertIn(flag, compare_line)

    def test_finalizer_rejects_missing_baseline_joined_path(self) -> None:
        result = self.run_script(
            FINALIZER,
            "--suffix",
            "missing-baseline",
            "--baseline-joined",
            "does-not-exist.csv",
            "--require-top-gpu-decrease",
            "--dry-run",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("missing baseline joined CSV", result.stderr)


if __name__ == "__main__":
    unittest.main()
