# Baselines — the reference captures every other experiment compares against

> Part of the 3DMark05 GT1 GPU-bottleneck investigation. Root map: [[overview-3dmark05-gt1]].

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
wrong-path counter interpretation separate from the Xcode GPU proof. Almost
every A/B delta elsewhere is measured against [[baselines-frame50.01]],
[[baselines-frame60.01]], or the refreshed [[baselines-frame60.02]].

## Hypotheses & verdicts

| # | Hypothesis | Verdict | Evidence |
|---|-----------|---------|----------|
| H1 | The captured GT1 frame is GPU-bound and the cost is concentrated in a few render encoders | accepted | [[baselines-frame120.01]] |
| H2 | The dominant counters are LLC/MMU/buffer-write, not ALU or texture | accepted | [[baselines-frame120.01]], [[baselines-frame60.01]], [[baselines-frame60.02]] |
| H3 | The big VS-buffer-write bucket is not explained by dxmt CPU writers (~0.2-0.4 MiB) or visible VSOut width (184 B) | accepted | [[baselines-frame50.01]], [[baselines-frame60.01]], [[baselines-frame60.02]] |
| H4 | The frame50 runtime shape is stable across code changes (usable as a fixed A/B denominator) | accepted | [[baselines-frame50.02]], [[baselines-frame50.03]] |
| H5 | A no-gputrace timeout-finalized run is a valid counter sample (not a wall-clock FPS sample) | accepted | [[baselines-frame50.03]] |
| H6 | The new top-level watchdog + Wine cleanup path preserves baseline counter shape | accepted | [[baselines-frame50.04]] |
| H7 | A time-based GT1 `actual.png` alone can prove visual correctness after optimization changes | rejected | [[baselines-visual-capture.01]] |
| H8 | The `v0.0.1` tag is the known-good visual correctness / alignment anchor for GT1 regression triage, and screenshot diffs against it are useful corruption finders | accepted | [[baselines-visual-capture.02]] |
| H9 | `MTL_CAPTURE_ENABLED=1` is required for standard 3DMark05 gputrace probes | rejected | [[baselines-gputrace-capture.01]] (`MTL_CAPTURE_ENABLED=1` alone reproduced black-screen startup with draw/present counters at zero; omitting it restores normal GT1, but file/simple `developerTools` capture still need an inserted layer; external `.app` plist does not reach Wine's temp child, and tmp embedded-plist roots are not runtime-equivalent yet) |
| H10 | Current visual-coupling frame60 smoke shows skipped/error/overflow/hazard-split work as the obvious muzzle/glow perf owner | rejected-current-smoke; post-`01:05` oracle refresh stays flat (`draw_skipped_no_pipeline=0`, `gpu_command_buffer_errors=0`, `map_buffer_wait_ms=0`, `queue_sequence_wait_ms=0`), while RT/depth/clear/present pass churn remains open | [[baselines-frame60.03]] |

## Verification methods

- **`scripts/tools/run_3dmark05_perf_probe.sh`** — the standard wrapper for
  every capture; `--frame N` scopes encoder breakdown / capture to a frame,
  `--encoder-breakdown-seq N` bounds the breakdown log, `--top N` limits rows.
- **`--no-gputrace`** — scout mode: emits `result.json` + perf counters without
  the expensive `.gputrace`/Xcode export; proves runtime-shape stability cheaply
  ([[baselines-frame50.02]], [[baselines-frame50.03]]).
- **Timeout policy** — `--timeout` is mandatory and positive (120s no-gputrace,
  420s gputrace); 3DMark05 hangs on the final frame, so the wrapper
  timeout-finalizes (`timed_out=true`, `returncode=143`/`-15`). A
  timeout-finalized run with expected artifacts is a valid counter sample;
  `process_elapsed_sec` is NOT an FPS metric.
- **`scripts/tools/finalize_3dmark05_perf_probe.sh`** — after Xcode exports
  encoder counters, joins them with dxmt per-encoder attribution into
  `frame<N>-xcode-dxmt-joined-summary.csv` + bottleneck report; gates
  `--require-xcode-counter-coverage`, `--require-dxmt-join-coverage`,
  `--require-top-pso-attribution`, `--require-shader-dump-matches` certify the
  baseline ([[baselines-frame60.01]], [[baselines-frame60.02]]).

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
  Visual["baselines-visual-capture.01\ntime-based screenshot caveat\nnot a visual oracle"]
  VisualAnchor["baselines-visual-capture.02\nv0.0.1 visual correctness anchor\ndiff-assisted triage"]
  CaptureEnv["baselines-gputrace-capture.01\nMTL_CAPTURE_ENABLED black-screen\nwrapper omits it by default"]
  F60["baselines-frame60.01\nframe60 validation\n34.02ms / top3 98.41%\nVS write 1627.4MiB"]
  F60post["baselines-frame60.02\npost-visualfix frame60\n33.614ms / top3 98.12%\nhidden 1597.8MiB"]
  F60vc["baselines-frame60.03\nvisual-coupling no-gputrace\nskips/errors/overflows 0\nhazard split 0\nRT/depth churn open"]

  RunLevel -->|context-for| F120
  F120 -->|same-shape, narrowed-to| F50
  F120 -->|same-shape, narrowed-to| F60
  F60 -->|refreshed-after-visual-fix| F60post
  F60post -->|runtime wrong-path scout| F60vc
  F50san -->|shape-stable, superseded-by| F50
  F50 -->|refreshed-by| F50to
  F50to -->|supervised-timeout-refresh| F50wd
  F50wd -->|visual-smoke-caveat| Visual
  Visual -->|anchor-refined-by| VisualAnchor
  VisualAnchor -->|visual-alignment anchor for| F60post
  CaptureEnv -->|standard gputrace launch hygiene for| F60post
  F60vc -->|feeds correctness gate for| Backend

  F120 -->|feeds| Store["[[render-pass-store]]\n+ bottleneck shape"]
  F50 -->|baseline-for| IdxCache["[[index-cache-locality]]\nopaque / screen-blend proofs"]
  F60post -->|baseline-for| VSOut["[[vsout-layout]]"]
  F60post -->|baseline-for| Backend["[[backend-shape-classifiers]]"]
  F60post -->|baseline-for| Churn["[[state-churn-encode]]"]

  class F120,F50,F50san,F50to,F50wd,F60,F60post,F60vc,RunLevel,VisualAnchor accepted
  class Visual,CaptureEnv rejected
  class Store,IdxCache,VSOut,Backend,Churn open
```

## Headline numbers

| Baseline | Date | Total GPU | Top-3 share | Key write/owner figure |
|---|---|---:|---:|---|
| [[baselines-frame120.01]] | 2026-05-31 | `33.611ms` | `33.075ms` / `98.40%` | LLC/MMU/buffer-write dominate; same RT/depth pair twice `24.643ms` / `73.32%` |
| [[baselines-runlevel.01]] | undated | — | — | `present_encoded=1260`, `draw_calls=915070`, tile preservation `167.74GB`, stream/IB deltas `796k`/`753k` |
| [[baselines-frame50.01]] | 2026-06-04 | `35.024ms` | `34.390ms` / `98.19%` | VS write `1627.372MiB`; hidden backend `1597.615MiB` (`98.2%`); dxmt CPU `0.444MiB`; `7.9x`/`33.1x` |
| [[baselines-frame50.02]] | 2026-06-04 | — (HUD FPS 9) | — | no-gputrace; rows `50/0..3` match prior samples; `gpu_command_buffer_time_ms=4151.436` |
| [[baselines-frame50.03]] | 2026-06-05 | — | — | no-gputrace timeout scout; `present_encoded=1440`, `gpu_command_buffer_time_ms=4193.474` |
| [[baselines-frame50.04]] | 2026-06-06 | — | — | watchdog-cleanup no-gputrace scout; `present_encoded=1440`, `gpu_command_buffer_time_ms=4207.759`, `completion_wait_ms=31071.820`; shape flat vs baseline |
| [[baselines-frame60.01]] | 2026-06-01 | `34.02ms` | `33.481ms` / `98.41%` | VS write `1627.414MiB`; dxmt CPU `0.444MiB`; unexplained `1627.642MiB`; `7.9x` |
| [[baselines-frame60.02]] | 2026-06-06 | `33.614ms` | `32.984ms` / `98.12%` | post-visualfix refresh; VS write `1627.332MiB`; hidden backend `1597.755MiB`; dxmt CPU `0.202MiB`; `7.9x` |
| [[baselines-frame60.03]] | 2026-06-07/08 | — | — | visual-coupling no-gputrace scouts; initial run and post-`01:05` oracle refresh both keep `present_encoded=1680`, `draw_skipped_no_pipeline=0`, `gpu_command_buffer_errors=0`, `map_buffer_wait_ms=0`, `queue_sequence_wait_ms=0`; refresh split reasons are RT/depth `13,163`, clear `4,895`, present `1,673`; render-pass preservation remains `~120.1MiB/present`; sampled average `15.753fps`, steady late frames `~23fps` |
| [[baselines-visual-capture.02]] | 2026-06-06 | — | — | `v0.0.1` is the known-good visual correctness / alignment anchor; same 40s screenshot can still drift (`Frame 351` vs `483`), but PNG diff is useful for broad texture/color/geometry, black/translucent vertex, UV, and cbuf-identity triage |
| [[baselines-gputrace-capture.01]] | 2026-06-06 | — | — | capture workflow fix: `MTL_CAPTURE_ENABLED=1` alone can black-screen 3DMark05 startup with draw/present `0`; no-layer runs restore normal GT1 (`present_encoded=1680`) but file `.gputrace`, simple Xcode-open `developerTools`, external `.app` plist, and current tmp embedded-plist roots are still not valid capture routes |

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
[[hidden-backend-storage]]. The post-visualfix [[baselines-frame60.02]]
refresh confirms this shape after the latest visual/cbuf identity path:
`33.614ms` total GPU, top-3 `98.12%`, VS write `1627.3MiB`, and hidden
backend `1597.8MiB`. The no-gputrace [[baselines-frame50.02]] /
[[baselines-frame50.03]] / [[baselines-frame50.04]] scouts prove the runtime shape is stable enough to
treat frame50/frame60 as fixed A/B denominators.

What is settled: the baseline numbers and the capture/finalize methodology
(wrapper, `--frame`, `--no-gputrace`, timeout policy, finalizer join + gates).
The capture workflow now also rejects `MTL_CAPTURE_ENABLED=1` as a default
3DMark05 env because it can black-screen startup before any draw/present calls.
Normal no-layer runs still cannot emit a frame capture unless the process has
an inserted capture layer; simply opening Xcode and selecting the
`developerTools` destination is not sufficient. A simple external `.app`
`MetalCaptureEnabled` plist also does not reach Wine's temp child, and current
tmp embedded-plist Wine roots fail 3DMark05 before capture even without the
capture key ([[baselines-gputrace-capture.01]]).
What stays open lives in the consuming domains, not here — almost every A/B
delta elsewhere is measured against [[baselines-frame50.01]] (frame50 locality
proofs), [[baselines-frame60.01]] (historical VSOut / backend-shape /
state-churn probes), or [[baselines-frame60.02]] (post-visualfix refresh).

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

- [[overview-3dmark05-gt1]] — root map, priority DAG, and where these baselines sit in it.
- [[hidden-backend-storage]] — the recurring hidden-VS-write fingerprint every
  baseline exhibits.
- [[render-pass-store]] — fed by frame120's same-RT/depth re-entry and the
  run-level tile-preservation counters.
- [[index-cache-locality]] — opaque-depth and screen-blend frame50 A/B proofs
  measured against [[baselines-frame50.01]].
