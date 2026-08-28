---
description: Reproducible 3DMark05 and 3DMark06 wild-experiment lanes and evidence rules
paths:
  - "experiments/launchers/app-d3d9-3dmark*.sh"
  - "scripts/tools/run_3dmark05_perf_probe.sh"
  - "experiments/output/app-d3d9-3dmark*/**"
alwaysApply: false
---

# 3DMark Experiment Rules

This rule covers 3DMark05 and 3DMark06 runs against the external commercial
payloads. Runtime selection and WSI qualification are owned by
[`test_wild.rules.md`](test_wild.rules.md); this file records the benchmark
lane recipe and the evidence that makes a result usable.

## One runner and one runtime

Run catalogue entries through the consolidated runner. Do not invoke a bare
Wine binary or add a per-app wrapper:

```sh
scripts/run_python.sh scripts/run_apps/run_experiment.py list
scripts/run_python.sh scripts/run_apps/run_experiment.py run app-d3d9-3dmark05 \
  --output-suffix gt1-perf-20260826
scripts/run_python.sh scripts/run_apps/run_experiment.py run app-d3d9-3dmark06 \
  --output-suffix hdr-perf-20260826
```

The catalogue defaults to the Sikarugir-Engines `sikarugir-cx-24.0.7`
manifest entry. Verify `result.json:wine` and the staged-artifact hashes before
comparing runs. Do not substitute Heroic Wine 11.x, `*-DXMT`, Proton/Vulkan,
or CrossOver as a baseline; the accepted-runtime matrix and the small-runtime
sanity check are in [`test_wild.rules.md`](test_wild.rules.md).

The run must be made from a built, ABI-matched staging set. For a fresh
measurement, rebuild the canonical `build-x86_64-builtin`,
`build-win32-x64-builtin`, and `build-win32-x86-builtin` directories, then let
`run_experiment.py` stage them. Keep the macOS desktop unlocked. A stale or
partially staged PE/unix set invalidates the measurement even if the process
starts.

## Named lanes

`experiments/launchers/3dmark_lane_presets.sh` is the single source for named
selection. Set the product-specific lane variable; raw `*_ARGS` takes
precedence and is recorded as `custom`.

| Product | Canonical lanes | Selection |
|---|---|---|
| 3DMark05 | `gt1`, `gt2`, `gt3` | `-gt1`, `-gt2`, `-gt3` |
| 3DMark05 | `graphics`, `cpu1`, `cpu2`, `cpu`, `score`, `feature`, `batch`, `all` | Product suite switches |
| 3DMark06 | `gt1`, `gt2` | `-gt1`, `-gt2` |
| 3DMark06 | `sm2` | `-gt1 -gt2` |
| 3DMark06 | `hdr1`, `hdr2`, `hdr` | `-hdr1`, `-hdr2`, or both |
| 3DMark06 | `graphics`, `cpu1`, `cpu2`, `cpu`, `score`, `feature`, `batch`, `all` | Product suite switches |

Examples:

```sh
DXMT_EXPERIMENT_PROFILE=perf DXMT_3DMARK05_LANE=gt2 \
  scripts/run_python.sh scripts/run_apps/run_experiment.py run \
  app-d3d9-3dmark05 --output-suffix gt2-perf-20260826

DXMT_EXPERIMENT_PROFILE=perf DXMT_3DMARK06_LANE=hdr \
  scripts/run_python.sh scripts/run_apps/run_experiment.py run \
  app-d3d9-3dmark06 --output-suffix hdr-perf-20260826
```

The launcher appends `-nosplash -nosysteminfo -noscreens` to named lanes and
emits `[3dmark-lane]`. The runner copies that resolved product, name, and
source to `result.json:benchmark_lane`. A lane absent from that field is not a
qualified named-lane result.

3DMark06 per-test switches are a Professional Edition facility. An Advanced
Edition installation may accept the arguments but remain at its GUI selection;
record this explicitly and do not call the result a CLI-selected lane. Use
`DXMT_3DMARK06_DRY_RUN=1 bash experiments/launchers/app-d3d9-3dmark06.sh` to
inspect the resolved command without staging or launching.

Verified 2026-08-29 on this install: the UL-published legacy key
(`3DM06-YKL9-C7R6-73WW-AAPA-VHKW` from
https://benchmarks.ul.com/legacy-benchmarks) registers **Advanced Edition**
by writing the 3DMark05-style registry value
`HKLM\Software\Wow6432Node\Futuremark\3DMark06\KeyCode` — no UI entry needed;
the main window title flips to "3DMark06 - Advanced Edition". Per-test CLI
selectors remain ignored even when registered (Professional-only), so
unattended runs use `DXMT_3DMARK06_AUTORUN=1` (the CATALOGUE default for
`app-d3d9-3dmark06` via `launcher_env`; export `0` for a manual-GUI run):
the launcher waits for the
`3DMark06 - <edition>` main window inside the prefix via
`experiments/apps/tool-winctl`. For `gt1`, `gt2`, `hdr1`, and `hdr2`, it clears
the Select Tests tree, checks exactly the requested row, accepts the dialog,
and clicks the Run 3DMark button (control id 1). Other suite lanes retain the
GUI-persisted selection. The path is focus-free and works with the window
hidden behind macOS windows (`DXMT_3DMARK06_AUTORUN_SETTLE_SEC` tunes the
post-window-ready delay, default 3). Build the helper with
`scripts/build_apps/build_tool-winctl.sh` if the exe is missing.

CPU, feature, and batch lanes are diagnostic workloads. They do not establish
graphics-scene promotion or GPU-performance claims.

## Profiles, FPS, and validity

Use `DXMT_EXPERIMENT_PROFILE=perf` for measurements. The profile resolves to
`DXMT_VALIDATE=0`, warn-level logging, perf counters, and `WINEDEBUG=-all` and
is recorded in `result.json:profile`. Never compare a `debug` run with a
`perf` run; validation and debug logging materially alter throughput.

For observer-free FPS, prefer a benchmark-owned `.3dr` result when the product
and edition can produce one. For renderer-side diagnostics, set
`DXMT9_PERF_FRAME_SAMPLING=1` (and keep `DXMT_PERF_COUNTERS=1`) and calculate
the elapsed average from positive `wall_ms` samples:

```sh
scripts/run_python.sh scripts/tools/summarize_3dmark05_perf.py \
  experiments/output/app-d3d9-3dmark05-<suffix>
rg 'sampled_avg_fps' \
  experiments/output/app-d3d9-3dmark05-<suffix>/3dmark05-perf-summary.md
```

`result.json:performance.fps` is the catalogue process-elapsed estimate; it is
not interchangeable with a benchmark `.3dr` FPS or a sampled scene average.
Always report the source, sample/frame count, wall interval, product lane,
profile, resolution, and runtime alongside FPS. One run is a spot check, not a
noise-qualified A/B claim; repeat runs when making a performance decision.

Before accepting a run, check:

```sh
jq '{status,returncode,timed_out,profile,benchmark_lane,performance,
    wine,staged_build,failures,dxmt9_perf_counters}' \
  experiments/output/app-d3d9-3dmark06-<suffix>/result.json
```

For a correctness-qualified graphics result, require no black-screen/missing
capture failure, no command-chunk rejection, no GPU command-buffer errors, and
no pipeline-library failure or no-pipeline draw skip in the relevant counter
snapshot. A timeout is acceptable only where the catalogue explicitly permits
it and the useful benchmark interval, output, and counters are complete.

## `.3dr` result files

`DXMT_3DMARK05_RESULT_FILE` and `DXMT_3DMARK06_RESULT_FILE` append a final
result filename argument after the selected test switches. The benchmark writes
that file relative to `DXMT_EXPERIMENT_WORKDIR`, not automatically into the
catalogue output directory. A result filename alone is therefore not evidence
that a file was produced.

When the result variable is unset, the catalogue runner injects a run-unique
basename. It snapshots the bounded benchmark/prefix roots before launch,
discovers only regular `.3dr` files created or modified by this run, atomically
copies them to `<output>/benchmark-results/`, and records source, change class,
relative artifact path, byte size, modification time, and SHA-256 in
`result.json:benchmark_result_files`. It never attributes an unchanged old file
or follows a symlink. A non-emitting edition records `status=not_emitted` and
`missing_requested=true` without converting that absence into renderer
failure; a discovered file that cannot be copied is a capture failure.

3DMark05's `.3dr` result is the preferred observer-free score/FPS source for
the standard 1024x768 convention. 3DMark06 Advanced Edition does not reliably
produce observer-free per-test `.3dr` output through the command-line lane;
use its displayed result or the renderer-side frame-sampling artifact and mark
the selection limitation in the report.

## 3DMark05 probe wrapper

For a supervised 3DMark05 GPU investigation, use the specialized wrapper,
which adds timeout/watchdog, free-space, capture preflight, and postprocessing:

```sh
scripts/tools/run_3dmark05_perf_probe.sh --no-gputrace --frame-sampling \
  --lane gt1 --suffix gt1-scout-20260826
```

Run `--dry-run` first. Use `--no-gputrace` for a low-overhead FPS scout; use
gputrace/Xcode only when the question requires GPU counters. Keep diagnostic
flags such as encoder breakdown, index reuse, shader dumps, and DAG dumping
off for ordinary FPS baselines because they perturb the workload. Preserve
`result.json`, `dxmt9.log`, the generated summaries, and any `.3dr` artifact as
one run-id group.
