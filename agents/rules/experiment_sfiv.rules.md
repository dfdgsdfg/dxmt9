---
description: Reproducible Street Fighter IV D3D9 benchmark lane and evidence rules
paths:
  - "experiments/launchers/app-d3d9-sfiv-benchmark.sh"
  - "experiments/output/app-d3d9-sfiv-benchmark-*/**"
alwaysApply: false
---

# Street Fighter IV Experiment Rules

This rule covers the external Street Fighter IV Benchmark as a D3D9 workload.
It is a catalogue experiment, not a Wine conformance oracle. Wine runtime and
WSI eligibility are defined by [`test_wild.rules.md`](test_wild.rules.md); this
file fixes the SFIV-specific run and measurement contract.

## Canonical invocation

Use only the consolidated runner. The launcher sets the benchmark switch and
the runner owns prefix bootstrap, DLL staging, logging, capture, and
`result.json`:

```sh
DXMT_EXPERIMENT_PROFILE=perf \
  scripts/run_python.sh scripts/run_apps/run_experiment.py run \
  app-d3d9-sfiv-benchmark --output-suffix sfiv-perf-20260826
```

The binary is the externally-installed
`experiments/apps_3rd/app-d3d9-sfiv-benchmark/.../StreetFighterIV_Benchmark.exe`
payload, exposed through the catalogue prefix's `D:` junction. Use
`--binary /absolute/path/to/StreetFighterIV_Benchmark.exe` only for an
intentional alternate install and keep the resulting path in the artifact.
Do not add a hardcoded Wine root or resurrect the removed per-app wrapper.

The catalogue default is the Sikarugir-Engines
`sikarugir-cx-24.0.7` manifest entry. Confirm the manifest identity,
`result.json:wine`, `result.json:wsi.layer_acquisition`, and the staged-artifact
hashes. Keep the desktop unlocked and use the same Wine root for both sides of
an A/B comparison. Follow [`test_wild.rules.md`](test_wild.rules.md) for the
runtime probe and the small bridge sanity test.

## Profile and lane shape

SFIV has one catalogue benchmark lane: the launcher invokes `-benchmark`.
There is no `DXMT_3DMARK*_LANE` selector and no scene-by-scene CLI preset.
`result.json:benchmark_lane` is consequently absent for SFIV; do not infer a
3DMark lane from a SFIV log.

Use the `perf` profile for FPS:

```sh
DXMT_EXPERIMENT_PROFILE=perf \
DXMT9_PERF_FRAME_SAMPLING=1 \
  scripts/run_python.sh scripts/run_apps/run_experiment.py run \
  app-d3d9-sfiv-benchmark --output-suffix sfiv-frame-sampling-20260826
```

The profile records effective validation, logging, perf-counter, and
WINEDEBUG settings in `result.json:profile`. A `debug` run is for diagnosis,
not for a performance baseline. The catalogue timeout is allowed because the
benchmark can leave a final UI/result state after its useful frame interval;
report `timed_out`, process elapsed time, and the actual sampled interval
instead of hiding the timeout.

## FPS extraction and validity

SFIV does not expose an authoritative `.3dr` result through this launcher.
Treat positive `wall_ms` `[dxmt9-perf-frame]` records in `dxmt9.log` as the
renderer-side observation and compute:

```text
sampled FPS = number of positive-wall samples × 1000
              ÷ sum of their wall_ms
```

Exclude the bootstrap `frame=0 wall_ms=0` record and any incomplete/zero-wall
records. Keep the exact sample count and wall sum in the report. The runner's
`result.json:performance.fps` is only a process-elapsed estimate and normally
does not contain the SFIV benchmark FPS; it must not be substituted silently.

For each run, retain and inspect:

```sh
jq '{status,returncode,timed_out,performance,profile,wine,wsi,
    staged_build,failures,dxmt9_perf_counters,dxmt9_pe_recorder_counters}' \
  experiments/output/app-d3d9-sfiv-benchmark-<suffix>/result.json
rg -n '\[dxmt9-perf-frame\]|gpu_command_buffer_errors|DXMT_ASSERT|validation error' \
  experiments/output/app-d3d9-sfiv-benchmark-<suffix>/dxmt9.log
```

A valid performance point has a recorded `perf` profile, successful WSI
acquisition/ABI handshake, non-black output when capture is requested, zero
GPU command-buffer errors, zero command-chunk rejects, and no pipeline/library
failure that skips draws. A process timeout is not by itself a renderer
failure when the catalogue allows it, but a run without a usable positive-wall
sample interval is not an FPS result.

## Comparing SFIV runs

Use identical binary, Wine manifest entry, display settings, renderer mode,
profile, build/staging identity, and sampling policy. Prefer interleaved or
paired runs when testing a change; one warm-up observation and one candidate
does not establish a win. Report both sampled average and, when available, the
median steady-frame FPS because a long shader or present tail can pull the
average down while the frame body remains unchanged. A single-run delta is a
spot check only; do not label it a regression or improvement without repeated
runs and a stated noise band.

SFIV is currently a useful producer/pacing and full-screen shader workload,
not proof of 3DMark scene behavior. Its benchmark result can be CPU/pacing-tail
dominated even when individual Metal command buffers are short. When
investigating that distinction, pair the normal frame-sampling run with an
explicitly documented no-gputrace diagnostic; do not enable heavy encoder,
shader-dump, or GPU-capture instrumentation on the baseline itself.

## Artifact and `.3dr` policy

The run directory is
`experiments/output/app-d3d9-sfiv-benchmark-<suffix>/`. Preserve
`result.json`, `dxmt9.log`, `actual.png`/`actual.bmp` when captured, and the
staged-build identity together. The generic runner's future `.3dr` discovery
must remain scoped to products that request a result file; SFIV must not be
declared to have a benchmark-owned `.3dr` merely because another workload's
file is present in its prefix or working directory.
