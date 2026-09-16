---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "CPU supply: upstream saturation and conditional replay headroom"
type: evidence
status: diagnostic-attribution
updated: 2026-09-16
source: experiments/output/cpu-supply-r633-20260916.noindex/qualified-v2; experiments/output/cpu-supply-r633-20260916.noindex/v2-native-tests.log; experiments/output/cpu-supply-r633-20260916.noindex/v2-unix-tests.log; experiments/output/cpu-supply-r633-20260916.noindex/tla-full.log
related: present-pacing-lock-scope-attribution.311.md; overview.md; ../../../specs/backend/encode-scheduling/gap.md
---

# CPU supply critical path and conditional replay headroom

The four qualified diagnostic windows point to **upstream application-thread
supply, not replay saturation**, as the first CPU throughput constraint in
05/06 GT2. App CPU occupies 0.94–0.96 of one core; replay occupies 0.39–0.54
and encode 0.41–0.56 of separate cores. Replay spends substantial elapsed time
waiting for input, predominantly while the FIFO is empty. Neither reserved-head
blocking nor ready dispatch accounts for most of that wait.

This identifies a thread/pipeline boundary, **not PE-exclusive cost**. The app
thread contains game code, Wine, PE recording and synchronous Unix work. It
does not establish that another PE micro-optimization can recover a large FPS
gain, nor that eliminating replay has an unconditional near-zero benefit.
No rendering/scheduling default or README FPS value changes.

## Build, recipe and evidence qualification

The measured build is `4547f030` plus the uncommitted CPU-supply observer
overlay, not bare HEAD. `qualified-v2/campaign.json` records the tracked patch
digest, new-source digests, all five image digests, commands, explicit knobs,
runtime and run order. The Unix provider SHA256 is
`7940d1667e32efb2c8698768e90eaf9c0eb1c482af2b975e5b2d59e34263b030`.
`audit.json` rechecks each run's staged image receipts and the unchanged build
images after the campaign. `tracked-overlay.patch` retains the measured
tracked-source overlay.

All six runs use the consolidated experiment runner, Sikarugir cx24.0.7 and
the perf profile with frame sampling and periodic health counters. 05 uses
1024x768; 06 uses 1280x720. Replay offload is ON; CPU-ready Tape, segmented
transport, inline constant delta, lock-scope refinement, precise uniform
invalidation and opaque-depth index optimization are explicitly OFF. Per
product, order is trace → control → trace. Control disables only the new
supply observer; it is **not a globally uninstrumented run**. No project
build, test or other benchmark from this task overlaps the campaign. Host
process snapshots are retained; external host activity is not proved absent.

Run directories are `experiments/output/app-d3d9-3dmark{05,06}-` followed by
the suffix in this table. Scene FPS uses consecutive positive-wall frame
samples with `draw_calls > 100`; 05 additionally preserves benchmark-owned
`.3dr` results, with size/SHA checks. Their timing populations differ.

| Suffix | Scene frames | Scene wall (s) | Frame-sampled FPS | 05 `.3dr` FPS | Uncovered health-tail frames |
|---|---:|---:|---:|---:|---:|
| `cpu-supply-r633d-05-gt2-1-trace` | 1924 | 64.552 | 29.806 | 30.808 | 0 |
| `cpu-supply-r633d-05-gt2-2-control` | 1867 | 64.602 | 28.900 | 29.884 | 0 |
| `cpu-supply-r633d-05-gt2-3-trace` | 1810 | 64.632 | 28.005 | 28.969 | 0 |
| `cpu-supply-r633d-06-gt2-1-trace` | 1153 | 64.092 | 17.990 | not emitted | 8 |
| `cpu-supply-r633d-06-gt2-2-control` | 1173 | 64.042 | 18.316 | not emitted | 44 |
| `cpu-supply-r633d-06-gt2-3-trace` | 1174 | 64.058 | 18.327 | not emitted | 3 |

All official run results pass. 06 completes its approximately 64-second scene
before the expected result-GUI timeout. Eight checked rejection/GPU/pipeline
health counters are zero at their covered snapshots; the table does not hide
the unobserved 06 tail. All diagnostic windows precede those snapshots. Six
saved screenshots show the forest/firefly scene rather than a black screen or
menu. These are spot visual checks, not full-scene temporal/pixel equivalence.

The trace/control scene-FPS mean differences are +0.02% (05) and -0.86% (06).
Diagnostic-window differences are -2.44% and -0.26%. The two trace runs differ
materially themselves, and there is only one control per product. These are
**not noise-qualified observer overhead estimates**. The observer performs
libproc calls, timestamps and atomics, including inside existing locks.

## What is measured

`DXMT9_CPU_SUPPLY_TRACE` adds a default-off bounded event stream without
changing ordering, wake, admission, lifetime or wire/Slot representation.
V2 uses native-TID `PROC_PIDTHREADID64INFO` user+system CPU nanoseconds,
independently calibrated against Mach thread accounting. Thread CPU is not
inferred from `*_cpu_ms` counters that actually measure elapsed scopes.

The accepted scope is ordinary replay-worker → final Slot → dedicated encode
worker. Arena/session/batched-encode paths are not qualified. Unix import
starts inside `dxmt9c_device_commit_chunk_impl`: PE serialization/seal and
segmented pre-import are outside that scope. Replay includes mutations,
StateBlocks, item release and queue reacquisition, not only draw construction.

05 records encoded Presents 300–600 and analyzes 310→590; 06 records
1200–1500 and analyzes 1210→1490. Each analysis covers 280 frame intervals.
The analyzer requires matching frame-log coverage and rejects menu/loading
frames. 06 starts its scene at global frame 955/971/989 across these runs;
the selected windows are all real scene work but **not exact scene-time or
immutable-source matches across runs**. Do not attribute small differences
between these windows to a code change or observer cost.

### CPU and elapsed supply (two trace runs per product)

CPU occupancy is CPU time divided by that thread's sampled wall envelope.
Each envelope covers more than 99.95% of the selected wall interval. These
are different threads operating concurrently; rows are **not additive**.

| Metric | 05 GT2 | 06 GT2 |
|---|---:|---:|
| Window frame wall (ms/P) | 36.20–38.83 | 65.63–69.68 |
| App CPU (cores) | 0.941–0.952 | 0.963–0.963 |
| Replay CPU (cores) | 0.534–0.541 | 0.388–0.398 |
| Encode CPU (cores) | 0.554–0.560 | 0.415–0.423 |
| App CPU (ms/P) | 34.05–36.93 | 63.18–67.07 |
| Replay CPU envelope (ms/P) | 19.33–21.00 | 25.45–27.70 |
| Encode CPU envelope (ms/P) | 20.04–21.73 | 27.23–29.46 |
| Unix import scope, CPU (ms/P) | 0.816–0.911 | 1.084–1.200 |
| Unix import scope, elapsed (ms/P) | 1.674–1.854 | 2.612–2.777 |
| Replay service, elapsed (ms/P) | 19.62–21.23 | 25.46–27.75 |
| Replay input wait, elapsed (ms/P) | 16.49–17.48 | 40.04–41.79 |
| FIFO empty (wall %) | 80.48–80.74 | 84.18–85.07 |
| FIFO ready (wall %) | 19.26–19.51 | 14.92–15.81 |
| FIFO reserved head (wall %) | 0.0061–0.0068 | 0.0058–0.0095 |
| Input wait while empty (ms/P) | 16.30–17.32 | 39.73–41.53 |
| Input wait while already ready (ms/P) | 0.163–0.188 | 0.264–0.310 |
| Input wait on reserved head (ms/P) | 0.0024–0.0025 | 0.0041–0.0063 |
| Encode gap before next publication (ms/P) | 15.97–17.04 | 38.21–39.88 |
| Encode ready/dispatch/tail gap (ms/P) | 0.077–0.082 | 0.095–0.117 |

Queue occupancy includes time replay is busy with a previously popped item;
empty FIFO is not identical to idle replay. The wait/state intersection above
is the stronger evidence: most measured input wait really occurs with no
consumable item. Scope sums include complete scopes only (279 complete encode
scopes within 280 frame intervals); they are not exact full-window CPU totals.
Encode gaps are elapsed intervals between successive encode scopes, not
separately measured GPU waits. The app thread can concurrently be preparing
the next frame while the current one is replayed/encoded.

Control cumulative counters additionally report zero replay push-backpressure
and zero queue writer/commit waits. Drain-fence waits are 224.073ms (05) and
139.057ms (06) over their covered populations. These are whole-run counters,
not the selected-window CPU envelope. GPU-CB totals and completion-thread wait
must not be added to CPU stage times or used alone to claim a GPU ceiling.

## Replay half/zero: conditional sensitivity, not an FPS ceiling

For each trace, the analyzer keeps **observed producer arrivals, dispatch gaps,
encode service and GPU feedback fixed** and scales replay elapsed service plus
within-replay publication offsets. Actual raw/queue identities join to replay;
publication sequence joins to encode. All 301 publications in each trace link
to an enclosing replay job. Factor one exactly reconstructs observed Presents.

| Counterfactual | 05 median Present advance | 05 p95 | 06 median Present advance | 06 p95 |
|---|---:|---:|---:|---:|
| Replay service × 0.5 | 0.607–0.645ms | 2.455–2.513ms | 0.799–0.858ms | 3.198–3.251ms |
| Replay service × 0 | 1.192–1.254ms | 4.045–4.145ms | 1.568–1.654ms | 4.052–4.135ms |

The modeled 280-frame throughput difference is below 0.004% for all four
traces, essentially an endpoint effect with fixed input cadence. **It is not
a measured FPS gain, a noise-resolved estimate or an unconditional optimization
ceiling.** Reducing most of a non-limiting stage can move frame latency without
moving the rate at which the producer supplies frames.

The model does not allow the producer to arrive earlier when replay releases
locks/CPU/cache pressure, does not change GPU/allocator feedback and scales
elapsed replay service rather than only removable CPU work. A saturated
prepared-input experiment must still execute actual replay → encode → GPU
completion; CPU-only controlled-completion Tape timings cannot close that gap.
The present evidence therefore closes a bounded supply attribution, **not the
full end-to-end replay speedup bound**.

## Validation and excluded attempts

Native tests pass for supply analysis (15 cases after final coverage hardening),
the production observer/FIFO fixture, replay offload queue and native CPU-clock
calibration (`experiments/output/cpu-supply-r633-20260916.noindex/final-native-tests.log`).
The x86 observer/queue/install-name gates pass. Full TLA regression
passes 196 configurations: 59 normal, four progress and 133 required
counterexamples. These are semantic regressions, not a proof of FPS economics.

Final analysis hardening rejects missing interior stage endpoints, uncovered
interior Presents and between-scope CPU counter rollback. Reanalysis of all
four final traces passes with unchanged numbers in
`qualified-v2/{05,06}-{1,3}-reviewed-analysis.json`. Original analysis results
and `analyze-v2-campaign.py` are preserved; the campaign's analyzer hash names
that earlier version, not the final hardening. Runtime images did not change.

The two focus-interrupted initial controls (`cpu-supply-r633-05-gt2-1-control`
and `cpu-supply-r633b-05-gt2-1-control`) remain excluded. The intermediate
`r633c`/`retry-focus-2` campaign also remains excluded: its V1 POSIX CPU clock
was grossly underscaled under this Rosetta runtime, and its initial 06
300–600 window was startup/menu work. V1 transcripts are rejected rather than
silently multiplied into qualified CPU evidence. The failed external sampler's
metadata-only `peer-06-2.jsonl` contains no usable peer CPU observations.

## Next decision

1. Attribute the nearly saturated **app thread** on the current image to game,
   Wine, PE and synchronous bridge/Unix stacks. Do not reuse historical module
   percentages as current exclusive CPU budgets.
2. If replay optimization remains a candidate, measure a same-source saturated
   actual replay+encode lane and a real workload intervention. Observe producer
   arrivals and lock feedback, not only replay's internal timer reduction.
3. Prioritize removing an intermediate plan/snapshot/Slot representation only
   after pricing its contribution to that constraint. Replay CPU reductions
   remain useful for utilization/latency even when they do not raise FPS.
