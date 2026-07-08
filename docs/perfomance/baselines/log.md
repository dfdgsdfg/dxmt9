---
domain: baselines
workload: 3DMark05 GT1
title: "Baselines — the reference captures every other experiment compares against - Historical Log"
type: domain-log
status: historical
updated: 2026-07-08
source: docs/perfomance/baselines/index.md
related: docs/perfomance/baselines/index.md; docs/perfomance/baselines/overview.md
---

# Baselines — the reference captures every other experiment compares against - Historical Log

> Full historical detail moved from the former top-level `baselines.md` overview.
> Keep [overview](overview.md) current and compact; append long-running chronology,
> rejected paths, and detailed synthesis here only when it is not already captured in
> one-experiment leaf documents.

---

# Baselines — the reference captures every other experiment compares against

> Part of the 3DMark05 GT1 GPU-bottleneck investigation. Root map: [overview-3dmark05-gt1](../overview-3dmark05-gt1.md).

## Scope & question

This domain owns the reference 3DMark05 GT1 captures that define the bottleneck
shape and serve as the A/B denominators for every other domain. It holds four
capture regimes: the historical **frame120** Xcode snapshot that first showed
the shape, the current canonical **frame50** normal-source gputrace/Xcode
replay plus its no-gputrace sanity/timeout/watchdog-cleanup scouts, the
mid-investigation **frame60** validation capture with full finalizer
attribution gates, and the post-visualfix **frame60** refresh that confirms the
same owner after the latest correctness path. It also
keeps the whole-run counter shape that contextualizes the single-frame
captures. The latest frame60 no-gputrace visual-coupling scout keeps the
wrong-path counter interpretation separate from the Xcode GPU proof. The
latest low-overhead recovery scout separates transient/heavy-instrumentation
FPS-zero observations from the current normal renderer path. Almost
every A/B delta elsewhere is measured against [baselines-frame50.01](baselines-frame50.01.md),
[baselines-frame60.01](baselines-frame60.01.md), or the refreshed [baselines-frame60.02](baselines-frame60.02.md).

## Hypotheses & verdicts

| # | Hypothesis | Verdict | Evidence |
|---|-----------|---------|----------|
| H1 | The captured GT1 frame is GPU-bound and the cost is concentrated in a few render encoders | accepted | [baselines-frame120.01](baselines-frame120.01.md) |
| H2 | The dominant counters are LLC/MMU/buffer-write, not ALU or texture | accepted | [baselines-frame120.01](baselines-frame120.01.md), [baselines-frame60.01](baselines-frame60.01.md), [baselines-frame60.02](baselines-frame60.02.md) |
| H3 | The big VS-buffer-write bucket is not explained by dxmt CPU writers (~0.2-0.4 MiB) or visible VSOut width (184 B) | accepted | [baselines-frame50.01](baselines-frame50.01.md), [baselines-frame60.01](baselines-frame60.01.md), [baselines-frame60.02](baselines-frame60.02.md) |
| H4 | The frame50 runtime shape is stable across code changes (usable as a fixed A/B denominator) | accepted | [baselines-frame50.02](baselines-frame50.02.md), [baselines-frame50.03](baselines-frame50.03.md) |
| H5 | A no-gputrace timeout-finalized run is a valid counter sample (not a wall-clock FPS sample) | accepted | [baselines-frame50.03](baselines-frame50.03.md) |
| H6 | The new top-level watchdog + Wine cleanup path preserves baseline counter shape | accepted | [baselines-frame50.04](baselines-frame50.04.md) |
| H7 | A time-based GT1 `actual.png` alone can prove visual correctness after optimization changes | rejected | [baselines-visual-capture.01](baselines-visual-capture.01.md) |
| H8 | The `v0.0.1` tag was a useful early coherent screenshot-diff artifact, but the last known GT1 visual-safe code point is `v0.0.3`; screenshot diffs remain broad corruption finders, not raw pixel gates | superseded by `v0.0.3` | [baselines-visual-capture.02](baselines-visual-capture.02.md), [snapshot-cache-visual.01](../snapshot-cache/snapshot-cache-visual.01.md) |
| H9 | `MTL_CAPTURE_ENABLED=1` is required for standard 3DMark05 gputrace probes | rejected / diagnostic-only | [baselines-gputrace-capture.01](baselines-gputrace-capture.01.md), [baselines-gputrace-capture.02](baselines-gputrace-capture.02.md) (`MTL_CAPTURE_ENABLED=1` previously reproduced black-screen startup with draw/present counters at zero, so it is not a default perf-run env; after the fragment-function lifetime fix, the explicit capture-layer diagnostic route can produce a valid `frame60.gputrace` and Xcode counters) |
| H10 | Current visual-coupling frame60 smoke shows skipped/error/overflow/hazard-split work as the obvious muzzle/glow perf owner | rejected-current-smoke; post-`01:05` oracle refresh stays flat (`draw_skipped_no_pipeline=0`, `gpu_command_buffer_errors=0`, `map_buffer_wait_ms=0`, `queue_sequence_wait_ms=0`), while RT/depth/clear/present pass churn remains open | [baselines-frame60.03](baselines-frame60.03.md) |
| H11 | The 120s no-gputrace timeout policy still produces standard `result.json` evidence when the wrapper watchdog includes capture delay | accepted | [baselines-frame50.05](baselines-frame50.05.md) |
| H12 | The current renderer is stuck in an FPS-zero state after the latest sidecar/visual work | rejected-current | [baselines-frame60.04](baselines-frame60.04.md) (`--no-encoder-breakdown` scout p50/p95 FPS `18.081`/`26.648`, last sample `24.798fps`, visual state observed normal; heavy sidecar FPS tails remain instrumentation caveat) |
| H13 | Current `.gputrace` / System Trace preflight is operational for file/sidecar routes, while Xcode `developerTools` attach remains blocked | accepted current preflight plus file export | [baselines-gputrace-preflight.02](baselines-gputrace-preflight.02.md) now records `~168GiB` free, full Xcode, Developer Mode enabled, file `.gputrace` dry-run passing through `--with-wine-capture-layer`, System Trace sidecar dry-run passing the `4096MiB` guard, and a real `capture-layer-current-r2-20260619` file-route export finalized into Xcode/dxmt reports. The old `~605MiB < 2048MiB` disk block is historical only. The Xcode `developerTools` route is still unavailable because attach preflight stays at `process-list-loading` / `Getting Process List...`; use file `.gputrace` or System Trace until that preflight passes. |
| H14 | Current capture-layer file route can produce Xcode encoder counters again | accepted | [baselines-gputrace-capture.02](baselines-gputrace-capture.02.md) (`frame60.gputrace` written, performance export and encoder counters exported; latest current-worktree `--with-wine-capture-layer` wrapper run has `10` encoder rows, Xcode reports `35.919ms`, `10` render encoders, `396` draw calls, top-three `98.26%`) |
| H15 | A latest black-geometry / transparent-weapon report invalidates the current performance direction | rejected as a wall; accepted as a baseline gate | [snapshot-cache-visual.02](../snapshot-cache/snapshot-cache-visual.02.md) keeps `v0.0.3` as the visual-safe anchor. For the sampled black-foreground firefight window, H169 rejects full-cbuf as the owner and H172 shows the broad dark class in `v0.0.3`; separate weapon/lighting artifacts still need same-frame or draw-local proof before perf runs are promoted. |

## Verification methods

- **`scripts/tools/run_3dmark05_perf_probe.sh`** — the standard wrapper for
  every capture; `--frame N` scopes encoder breakdown / capture to a frame,
  `--encoder-breakdown-seq N` bounds the breakdown log, `--top N` limits rows.
- **`--no-gputrace`** — scout mode: emits `result.json` + perf counters without
  the expensive `.gputrace`/Xcode export; proves runtime-shape stability cheaply
  ([baselines-frame50.02](baselines-frame50.02.md), [baselines-frame50.03](baselines-frame50.03.md)).
- **Timeout policy** — `--timeout` is mandatory and positive (120s no-gputrace,
  420s gputrace); 3DMark05 hangs on the final frame, so `run_experiment.py`
  timeout-finalizes (`timed_out=true`, `returncode=143`/`-15`). The wrapper's
  top-level watchdog must include the effective capture delay before adding
  slack, otherwise it can kill the runner before `result.json` is written. A
  timeout-finalized run with expected artifacts is a valid counter sample;
  `process_elapsed_sec` is NOT an FPS metric.
- **`scripts/tools/finalize_3dmark05_perf_probe.sh`** — after Xcode exports
  encoder counters, joins them with dxmt per-encoder attribution into
  `frame<N>-xcode-dxmt-joined-summary.csv` + bottleneck report; gates
  `--require-xcode-counter-coverage`, `--require-dxmt-join-coverage`,
  `--require-top-pso-attribution`, `--require-shader-dump-matches` certify the
  baseline ([baselines-frame60.01](baselines-frame60.01.md), [baselines-frame60.02](baselines-frame60.02.md)).

## Experiment dependency graph

```mermaid
flowchart TD
  classDef accepted fill:#d6f5d6,stroke:#2b7a2b,color:#063
  classDef rejected fill:#f8d7da,stroke:#a33,color:#600
  classDef open fill:#fff3cd,stroke:#a80,color:#640

  RunLevel["baselines-runlevel.01\nwhole-run counters\n1260 present / 915k draws"]
  F120["baselines-frame120.01\nframe120 snapshot\n33.611ms / top3 98.40%\nsame RT+depth 73.32%"]
  F50["baselines-frame50.01\ncanonical frame50 replay\n35.024ms / top3 98.19%\nhidden 1597.6MiB"]
  F50san["baselines-frame50.02\nno-gputrace sanity\nFPS 9, shape stable"]
  F50to["baselines-frame50.03\ntimeout no-gputrace scout\n2026-06-05, gpu cb 4193ms"]
  F50wd["baselines-frame50.04\nwatchdog-cleanup scout\n2026-06-06, gpu cb 4208ms\nsame shape, no manual kill"]
  F50wd120["baselines-frame50.05\n120s capture-delay-aware watchdog\n2026-06-12, result.json preserved\nsame post-uniform shape"]
  Visual["baselines-visual-capture.01\ntime-based screenshot caveat\nnot a visual oracle"]
  VisualAnchorOld["baselines-visual-capture.02\nv0.0.1 historical screenshot diff\ntriage only"]
  VisualAnchor["snapshot-cache-visual.01\nv0.0.3 last visual-safe code point\nuniform ABI-prefix correctness"]
  CaptureEnv["baselines-gputrace-capture.01\nMTL_CAPTURE_ENABLED black-screen\nwrapper omits it by default"]
  CapturePreflight["baselines-gputrace-preflight.02\nfile route dry-run OK\nSystem Trace dry-run OK\ndeveloperTools attach blocked"]
  CaptureRecovered["baselines-gputrace-capture.02\ncapture layer recovered\nframe60 Xcode counters"]
  F60["baselines-frame60.01\nframe60 validation\n34.02ms / top3 98.41%\nVS write 1627.4MiB"]
  F60post["baselines-frame60.02\npost-visualfix frame60\n33.614ms / top3 98.12%\nhidden 1597.8MiB"]
  F60vc["baselines-frame60.03\nvisual-coupling no-gputrace\nskips/errors/overflows 0\nhazard split 0\nRT/depth churn open"]
  F60low["baselines-frame60.04\nlow-overhead recovery scout\np50 18.081fps / p95 26.648fps\nvisual normal"]

  RunLevel -->|context-for| F120
  F120 -->|same-shape, narrowed-to| F50
  F120 -->|same-shape, narrowed-to| F60
  F60 -->|refreshed-after-visual-fix| F60post
  F60post -->|runtime wrong-path scout| F60vc
  F60vc -->|current low-overhead recovery| F60low
  F50san -->|shape-stable, superseded-by| F50
  F50 -->|refreshed-by| F50to
  F50to -->|supervised-timeout-refresh| F50wd
  F50wd -->|120s policy refresh| F50wd120
  F50wd -->|visual-smoke-caveat| Visual
  Visual -->|historical-diff-triage| VisualAnchorOld
  VisualAnchorOld -->|superseded-by| VisualAnchor
  VisualAnchor -->|visual-alignment anchor for| F60post
  CaptureEnv -->|standard gputrace launch hygiene for| F60post
  CaptureEnv -->|preflight-refreshed-by| CapturePreflight
  CaptureEnv -->|superseded-for-diagnostic route by| CaptureRecovered
  CaptureRecovered -->|current Xcode counter proof for| F60post
  F60vc -->|feeds correctness gate for| Backend
  F60low -->|baseline FPS sanity for| Churn

  F120 -->|feeds| Store"[render-pass-store\n+ bottleneck shape"]
  F50 -->|baseline-for| IdxCache"[index-cache-locality\nopaque / screen-blend proofs"]
  F60post -->|baseline-for| VSOut"[vsout-layout"]
  F60post -->|baseline-for| Backend"[backend-shape-classifiers"]
  F60post -->|baseline-for| Churn"[state-churn-encode"]

  class F120,F50,F50san,F50to,F50wd,F50wd120,F60,F60post,F60vc,F60low,RunLevel,VisualAnchorOld,VisualAnchor,CapturePreflight,CaptureRecovered accepted
  class Visual,CaptureEnv rejected
  class Store,IdxCache,VSOut,Backend,Churn open
```

## Headline numbers

| Baseline | Date | Total GPU | Top-3 share | Key write/owner figure |
|---|---|---:|---:|---|
| [baselines-frame120.01](baselines-frame120.01.md) | 2026-05-31 | `33.611ms` | `33.075ms` / `98.40%` | LLC/MMU/buffer-write dominate; same RT/depth pair twice `24.643ms` / `73.32%` |
| [baselines-runlevel.01](baselines-runlevel.01.md) | undated | — | — | `present_encoded=1260`, `draw_calls=915070`, tile preservation `167.74GB`, stream/IB deltas `796k`/`753k` |
| [baselines-frame50.01](baselines-frame50.01.md) | 2026-06-04 | `35.024ms` | `34.390ms` / `98.19%` | VS write `1627.372MiB`; hidden backend `1597.615MiB` (`98.2%`); dxmt CPU `0.444MiB`; `7.9x`/`33.1x` |
| [baselines-frame50.02](baselines-frame50.02.md) | 2026-06-04 | — (HUD FPS 9) | — | no-gputrace; rows `50/0..3` match prior samples; `gpu_command_buffer_time_ms=4151.436` |
| [baselines-frame50.03](baselines-frame50.03.md) | 2026-06-05 | — | — | no-gputrace timeout scout; `present_encoded=1440`, `gpu_command_buffer_time_ms=4193.474` |
| [baselines-frame50.04](baselines-frame50.04.md) | 2026-06-06 | — | — | watchdog-cleanup no-gputrace scout; `present_encoded=1440`, `gpu_command_buffer_time_ms=4207.759`, `completion_wait_ms=31071.820`; shape flat vs baseline |
| [baselines-frame50.05](baselines-frame50.05.md) | 2026-06-12 | — | — | 120s capture-delay-aware watchdog scout; `present_encoded=1680`, `result.json` preserved, `gpu_command_buffer_time_ms=5190.021`, `completion_wait_ms=40226.532`, `encode_draw_cpu_ms=16521.072`, `d3d9_snapshot_draw_submission_cpu_ms=6487.666`; shape flat vs [snapshot-cache-snapshot.10](../snapshot-cache/snapshot-cache-snapshot.10.md) |
| [baselines-frame60.01](baselines-frame60.01.md) | 2026-06-01 | `34.02ms` | `33.481ms` / `98.41%` | VS write `1627.414MiB`; dxmt CPU `0.444MiB`; unexplained `1627.642MiB`; `7.9x` |
| [baselines-frame60.02](baselines-frame60.02.md) | 2026-06-06 | `33.614ms` | `32.984ms` / `98.12%` | post-visualfix refresh; VS write `1627.332MiB`; hidden backend `1597.755MiB`; dxmt CPU `0.202MiB`; `7.9x` |
| [baselines-frame60.03](baselines-frame60.03.md) | 2026-06-07/08 | — | — | visual-coupling no-gputrace scouts; initial run and post-`01:05` oracle refresh both keep `present_encoded=1680`, `draw_skipped_no_pipeline=0`, `gpu_command_buffer_errors=0`, `map_buffer_wait_ms=0`, `queue_sequence_wait_ms=0`; refresh split reasons are RT/depth `13,163`, clear `4,895`, present `1,673`; render-pass preservation remains `~120.1MiB/present`; sampled average `15.753fps`, steady late frames `~23fps` |
| [baselines-frame60.04](baselines-frame60.04.md) | 2026-06-13 | — | — | low-overhead no-gputrace recovery scout after FPS-zero observation; `--no-encoder-breakdown`, `1,807` frame samples, p50/p95/max FPS `18.081`/`26.648`/`30.351`, last sample `24.798fps`, `present_boundary_wait_ms` max `0.000`; visual state observed normal |
| [baselines-visual-capture.02](baselines-visual-capture.02.md) | 2026-06-06 | — | — | `v0.0.1` was an early coherent screenshot-diff artifact; the last known visual-safe code point is `v0.0.3`, while the same 40s screenshot caveat still applies (`Frame 351` vs `483`) and PNG diff remains useful for broad texture/color/geometry, black/translucent vertex, UV, and cbuf-identity triage |
| [baselines-gputrace-capture.01](baselines-gputrace-capture.01.md) | 2026-06-06 | — | — | capture workflow fix: `MTL_CAPTURE_ENABLED=1` alone can black-screen 3DMark05 startup with draw/present `0`; no-layer runs restore normal GT1 (`present_encoded=1680`) but file `.gputrace`, simple Xcode-open `developerTools`, external `.app` plist, and tmp embedded-plist roots were not valid 3DMark05 capture routes; original-name Wine patching worked for synthetic Wine/D3D9 `.gputrace` but also black-screened 3DMark05 before draw/present; superseded for the explicit file route by [baselines-gputrace-capture.02](baselines-gputrace-capture.02.md) |
| [baselines-gputrace-preflight.02](baselines-gputrace-preflight.02.md) | 2026-06-19 | — | — | file `.gputrace` dry-run passes with `--with-wine-capture-layer` and `~168GiB` free; real `capture-layer-current-r2-20260619` export produced performance and encoder-counter files; System Trace sidecar dry-run passes the `4096MiB` guard; Xcode `developerTools` attach remains blocked by `process-list-loading` / `Getting Process List...`. The old `~605MiB < 2048MiB` disk block is retained only as historical context |
| [baselines-gputrace-capture.02](baselines-gputrace-capture.02.md) | 2026-06-16/19 | `37.206ms`, `38.092ms`, `37.709ms`, `37.492ms`, `36.183ms`, then `35.919ms` current-post-compact refresh | `36.596ms` / `98.36%`, `37.457ms` / `98.33%`, `37.115ms` / `98.42%`, `36.892ms` / `98.40%`, `35.577ms` / `98.33%`, then `35.296ms` / `98.26%` | capture-layer file route recovered after retaining the fragment `WMT::Function` and replacing Wine binaries via same-directory temp-file `mv`; the top-level `--with-wine-capture-layer` path writes `frame60.gputrace`, Xcode performance export, and `frame60-counters-xcode.csv`; latest current-post-compact top-three VS buffer write `1779.230MiB`, hidden backend estimate `1749.866MiB`, partial render count `0`, visual smoke normal |

## Results synthesis

The investigation has **four capture regimes**: the **frame120 historical
shape** that first revealed a GPU-bound frame whose cost concentrates in three
render encoders dominated by LLC/MMU/buffer-write counters (with two passes
re-entering the same RT/depth pair for `73.32%`); the **frame50 current
canonical** replay (`35.024ms`, hidden backend estimate `1597.6MiB` = `98.2%`
of VS write); and the **frame60 mid-investigation validation** (`34.02ms`, VS
write `1627.4MiB`, fully gated dxmt + shader attribution); plus the
**post-visualfix frame60 refresh** (`33.614ms`, hidden backend `1597.8MiB`).
All four show the
**same top-3-encoder / hidden-VS-write shape**: top-3 ≈ total buffer write, dxmt
CPU writers explain ≈ `0.4 MiB`, and the visible `184B` MSL VSOut width is
`7.9x` too small to account for the bucket — the recurring fingerprint of
[hidden-backend-storage](../hidden-backend-storage/index.md). The post-visualfix [baselines-frame60.02](baselines-frame60.02.md)
refresh confirms this shape after the latest visual/cbuf identity path:
`33.614ms` total GPU, top-3 `98.12%`, VS write `1627.3MiB`, and hidden
backend `1597.8MiB`. The no-gputrace [baselines-frame50.02](baselines-frame50.02.md) /
[baselines-frame50.03](baselines-frame50.03.md) / [baselines-frame50.04](baselines-frame50.04.md) /
[baselines-frame50.05](baselines-frame50.05.md) scouts prove the runtime shape is stable enough to
treat frame50/frame60 as fixed A/B denominators. The latest
[baselines-frame60.04](baselines-frame60.04.md) scout rejects the current "FPS zero" interpretation
for the low-overhead renderer path: with encoder breakdown disabled, the run
returns to the established `~18fps` median / `~26fps` p95 envelope and has no
present-boundary tail. Treat heavy System Trace / all-frame attribution FPS
tails as instrumentation caveats unless a matching low-overhead scout
reproduces them.

What is settled: the baseline numbers and the capture/finalize methodology
(wrapper, `--frame`, `--no-gputrace`, timeout policy, finalizer join + gates).
The capture workflow still rejects `MTL_CAPTURE_ENABLED=1` as a default
3DMark05 perf-run env because historical attempts black-screened startup before
any draw/present calls. The current status is narrower and better:
[baselines-gputrace-capture.02](baselines-gputrace-capture.02.md) proves the explicit capture-layer diagnostic
route can now write `frame60.gputrace` and Xcode encoder counters after the
fragment `WMT::Function` lifetime fix. The latest current-worktree refresh
repeats the route through the integrated `--with-wine-capture-layer` wrapper
with `35.919ms` total GPU, top-three `98.26%`, and `1779.230MiB` top-three VS
write, so the route is valid for Xcode-counter evidence. Still pair it with
no-gputrace visual/FPS scouts before making wall-clock claims. Simple Xcode-open
`developerTools` capture and external `.app` plist routes remain separate
workflow questions from the file capture route.
What stays open lives in the consuming domains, not here - almost every A/B
delta elsewhere is measured against [baselines-frame50.01](baselines-frame50.01.md) (frame50 locality
proofs), [baselines-frame60.01](baselines-frame60.01.md) (historical VSOut / backend-shape /
state-churn probes), or [baselines-frame60.02](baselines-frame60.02.md) (post-visualfix refresh).
[baselines-gputrace-preflight.02](baselines-gputrace-preflight.02.md) is not a new baseline sample; it records
whether the capture routes can be launched. The current file `.gputrace` and
System Trace sidecar dry-runs pass, but Xcode `developerTools` attach remains
blocked by `process-list-loading`. The current authoritative Xcode counter path
therefore remains the file capture-layer route in
[baselines-gputrace-capture.02](baselines-gputrace-capture.02.md), not an attached-Xcode `developerTools` run.

## How to run
Every experiment here is a 3DMark05 GT1 run via the standard wrapper. A baseline
is a plain frame capture with no behavior-changing flags. Use `--no-gputrace` for
a cheap runtime-shape scout, or capture a `.gputrace` and finalize for the
authoritative Xcode/dxmt joined baseline:

```sh
# Cheap scout: result.json + perf counters, no Xcode export
bash scripts/tools/run_3dmark05_perf_probe.sh --suffix baseline --frame 50 \
  --no-gputrace --timeout 120

# Authoritative baseline: capture .gputrace, then after Xcode export
bash scripts/tools/run_3dmark05_perf_probe.sh --suffix baseline --frame 50 --timeout 420
bash scripts/tools/finalize_3dmark05_perf_probe.sh --suffix baseline --frame 50 \
  --require-xcode-counter-coverage --require-dxmt-join-coverage --require-top-pso-attribution
```

The exact per-experiment flags (frame ids, `--encoder-breakdown-seq`, `--top`)
live in each leaf's `**Method.**` field. See
`agents/rules/environment_variables.rules.md` for env-var meanings and
`agents/rules/metal_debugging.rules.md` for the full workflow.

## Cross-references

- [overview-3dmark05-gt1](../overview-3dmark05-gt1.md) — root map, priority DAG, and where these baselines sit in it.
- [hidden-backend-storage](../hidden-backend-storage/index.md) — the recurring hidden-VS-write fingerprint every
  baseline exhibits.
- [render-pass-store](../render-pass-store/index.md) — fed by frame120's same-RT/depth re-entry and the
  run-level tile-preservation counters.
- [index-cache-locality](../index-cache-locality/index.md) — opaque-depth and screen-blend frame50 A/B proofs
  measured against [baselines-frame50.01](baselines-frame50.01.md).

## Root 3DMark05 Map Detail Migration - 2026-07-08

Detail migrated from the former long-form root [3DMark05 overview](../overview-3dmark05-gt1.md) so that `baselines` owns its detailed synthesis while the root overview stays cross-domain only.

### From Central finding (read this first)

The top-3 render encoders dominate every captured frame (~98% of GPU
time) and write a large **"VS Buffer Device Memory Bytes Written"**
bucket (~1.6 GiB at frame60, ~1.0–2.2 GiB depending on capture). That
bucket is **not** explained by:

Earlier `frame60` `.gputrace` attempts were blocked by capture-layer mechanics, not
by a dxmt9 draw/pipeline failure. File and `developerTools` destinations both
reported `startCapture failed` / `Capture layer is not inserted`; simply leaving
Xcode at the welcome screen did not insert the capture layer. A deliberate
`MTL_CAPTURE_ENABLED=1` smoke
(`app-d3d9-3dmark05-splitpayload-frame60-mtlcapture-r1`) produced a black
`actual.png`, no `result.json`, zero encoder rows, and no draw/present counters,
so that mode is not a valid performance sample for this app. Do not use it to
compare FPS or GPU time; either attach/launch through a real Xcode capture-layer
path, or keep using no-gputrace visual/perf scouts until a `.gputrace` can be
generated without changing the visual path.
A current-head phase43 recheck reproduced the same split after the latest
draw-state/resource-retention CPU work. The file destination
(`app-d3d9-3dmark05-phase43-frame60-gputrace-r1-20260613`) rendered normally
with `status=pass`, `present_encoded=1680`, and no timeout, but failed capture
with `destination=2 destination_supported=0` / `Capture layer is not inserted`.
The Xcode-open `developerTools` rerun
(`app-d3d9-3dmark05-phase43-frame60-xcode-devtools-r1-20260613`) also rendered
normally (`present_encoded=1740`) but failed with
`destination=1 destination_supported=0`. Therefore this branch still has no new
Xcode encoder-counter proof; treat both runs as normal-rendering counter samples
and capture-workflow negatives, not `.gputrace` evidence.
A sidecar Instruments capture does work without changing the visual path:
`app-d3d9-3dmark05-phase43-xctrace-system-r1-20260613` ran the supervised
no-gputrace wrapper and then recorded a 15s `Metal System Trace` with
`xctrace --all-processes`. Exported `metal-gpu-intervals` joined `3590/3590`
dxmt encoder rows by `RenderPass[seq=...,enc=...]`, covering
`seq=1394..1593`. The captured stage sum was `9303.143ms`, split
`8495.658ms` vertex and `807.485ms` fragment (`91.32%` vertex share). The top
rows were all `/11` large-geometry encoders (`16.299..21.459ms` stage sum) with
1.5M-1.86M vertices each, which reinforces the vertex-heavy bottleneck shape.
This still is not Xcode replay-counter proof: exported counter/shader-profiler
schemas were empty for the needed fields, so `VS Buffer Device Memory Bytes
Written` remains unavailable until a real `.gputrace` replay/export succeeds.
An in-place embedded-plist retry
(`app-d3d9-3dmark05-captureplist-frame60-gputrace-r1`) patched
`MetalCaptureEnabled=true` into copied `wine.capture.real` and
`wine.capture.real-preloader` binaries, then launched the normal Wine tree
without `MTL_CAPTURE_ENABLED=1`. This avoided the black-screen startup path and
rendered normally: `actual.png` at HUD `Time 0:58.77` / `Frame 1025` contains
visible circular white/yellow rifle/effect bloom in the wide infantry scene, and
the partial summary reports `present_encoded=1680`, `draw_calls=1234243`,
`draw_skipped_no_pipeline=0`, `gpu_command_buffer_errors=0`,
`map_buffer_wait_ms=0.000`, and `queue_sequence_wait_ms=0.000`. However,
`MTLCaptureManager` still logged `Capture layer is not inserted` at frame60 and
no `frame60.gputrace` was produced. Treat this as a cleaner negative capture
sample: embedded plist copies can be visually/runtime-safe, but they still do
not prove that the Wine temp child process has an inserted Metal capture layer.
The follow-up original-name replacement wrapper
(`scripts/tools/run_with_wine_metal_capture_layer.sh`) did prove that the temp
Wine launcher can receive `MetalCaptureEnabled`: the same mechanism wrote a
valid synthetic `perf-d3d9-present-loop` `.gputrace`, and the 3DMark05 temp
`/var/folders/.../winetemp.../wine.real` showed `Info.plist entries=13` with
`MetalCaptureEnabled`. That 2026-06-13 route nevertheless black-screened before
D3D9 draw/present (`bridge_draw=0`, `bridge_present=0`) and wrote no
`frame60.gputrace`.


### From Frame shape

Frame120 (historical bottleneck-shape capture, [baselines-frame120.01](baselines-frame120.01.md)):
total **33.611 ms** GPU, top-3 encoders **33.075 ms / 98.4%**; the same
RT/depth pair returns after another pass and accounts for **24.643 ms /
73.3%**. Passes are LLC/MMU/buffer-write limited, **not** ALU- or
texture-read-bound. Run-level: ~14673 passes preserve **167.73 GB** of
tile contents; draw-run submits = **580** against 913714 draws (≈99.94%
fail to batch), broken by const-upload (659938) and stream/IB state
deltas (793059 / 750041).
