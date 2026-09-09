---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "Replay baseline after PE hot-path work"
type: evidence
status: measured-runtime
updated: 2026-09-09
source: experiments/output/replay-baseline-H303-20260909/analysis
related: present-pacing-pe-qualified-pin-output.301.md; present-pacing-pe-completion-window.302.md
---

# Replay baseline

HEAD `1f3f0358`, unchanged production code. Replay service time is material,
but these runs do not establish replay as a saturated FPS limiter. The first
follow-up should separate compatibility state application/materialization and
final-Slot append, not assume that rewriting the planner or adding workers
will improve throughput. No optimization or README FPS promotion is made.

## Recipe and evidence boundaries

Four sequential runs: 05 perf, 06 perf, 05 diagnostic, 06 diagnostic. All use
Sikarugir `sikarugir-cx-24.0.7`, PE32/WoW64, named `gt2` presets, offload
replay on, CPU-ready Tape off, inline constant delta off, segmented transport
off, and opaque-depth index optimization off. Direct-slot replay retains its
default policy and correctness fallback. 05 uses 1024x768; 06 uses 1280x720.
Three canonical runtime builds completed with no work before measurement;
no build, test suite or second benchmark was launched during the runs.

Perf means the existing frame-sampled perf profile, not zero instrumentation.
Diagnostic adds `DXMT9_PERF_REPLAY_EMISSION_PLAN_SPLIT=1`,
`DXMT9_PERF_QUEUE_MUTEX_SPLIT=1` and Info logging. Per-record capacity timing,
copy ledger, lifecycle shape observer and GPU tracing remain off. The ignored
driver derives from H301; inherited observer-off prose in its recipes is not
the diagnostic classification. Frozen effective environment and the summary's
explicit `mode` are authoritative.

`analysis/run_baseline.py` freezes source, build options/coredata, all five
product images, Wine runtime, tools and host process snapshots before each
launch. It verifies the actual staged Wine-root images and unchanged source,
build, runtime and tools afterwards. `analysis/analyze.py` rechecks artifact
hashes and records its own and each measurement receipt's identity in
`analysis/replay-baseline-summary.json`. Raw logs, result JSON, CSV, screenshots
and recipes remain in the linked run directories. Host snapshots are not
continuous proof of a quiet host.

FPS uses all consecutive positive-wall, draw-heavy scene frames. Replay
counters are cumulative, emitted periodically. Their differences use the
largest fully contained counter window, **not the full scene**; no slow frame
inside either window is discarded. Asynchronous work can cross the endpoints,
so these are Present-waterline averages, not source-exact accounting.

| Run | Whole-scene frames | Scene FPS | Counter window `(first, last]` | Counter frames / wall seconds | Scene wall covered |
|---|---|---:|---|---|---:|
| 05 perf | 1–1659 | 25.480 | (60, 1620] | 1560 / 59.704 | 91.70% |
| 06 perf | 551–1608 | 16.448 | (600, 1560] | 960 / 58.429 | 90.83% |
| 05 diagnostic | 1–1477 | 22.634 | (60, 1440] | 1380 / 59.338 | 90.93% |
| 06 diagnostic | 670–1813 | 17.838 | (720, 1800] | 1080 / 60.369 | 94.13% |

Every reported reject/pipeline/GPU-error health counter is zero. 05 has full
health-counter coverage; 06 perf/diagnostic leave **48/13 final scene frames
uncovered**. Both 06 runs reach their post-scene GUI timeout (143), accepted
by the runner; this is not full-scene correctness proof. The two perf scene
screenshots were inspected, not a temporal pixel oracle. 05 result artifacts
are retained; 06 does not emit a per-test `.3dr` in this recipe.

The five products are unchanged from H301, whose spots were 30.200/18.357 FPS.
The new lower spots therefore cannot be attributed to a product code change.
Host/pacing variation is not isolated. The diagnostic 06 run is faster than
its perf spot despite extra observers: do not subtract these runs to estimate
observer overhead, optimization benefit, or an optimal FPS baseline.

## Service and wait baseline

All time rows below are **milliseconds per Present in the counter subwindow**.
`offload_replay_cpu_ms` wraps `replayPlannedChunk` with `steady_clock` in
`device_c_chunk_replay.cpp::replayRawChunk`; it is elapsed service time,
including synchronous waits/preemption, **not native thread CPU**. It excludes
the worker's outer cleanup and separately scheduled mutation/StateBlock work.
Append/mark are nested/subsystem scopes and must not be added to replay time.

| Scope / counter | 05 perf | 06 perf |
|---|---:|---:|
| Raw replay service, `offload_replay_cpu_ms` | 22.328 | 23.737 |
| Worker no-consumable-head wait, `offload_worker_idle_wait_ms` | 15.254 | 36.309 |
| Managed mutation apply, `offload_buffer_mutation_apply_cpu_ms` | 0.515 | 0.603 |
| Final draw-run append, `submit_draw_run_append_cpu_ms` | 4.124 | 4.459 |
| Draw resource marking, `submit_draw_run_resource_mark_cpu_ms` | 0.903 | 0.883 |
| Queue writer / commit waits | 0 / 0 | 0 / 0 |
| Offload push backpressure wait | 0 | 0 |
| Encode-thread PSO prefetch, **outside replay** | 2.228 | 2.708 |

Replay service occupies 58.34%/39.00% of these wall windows. These are not CPU
utilizations or removable FPS budgets. No-consumable-head wait is significant,
especially in 06; average FIFO depth sampled before raw pushes is 0.841/1.254.
The depth contains all queued task kinds, while its sampling counter counts
raw enqueue attempts only. There is no exact reported timed-replay-call
denominator; no exact ms/source is claimed. The existing native-thread observer
identifies the app/commit thread, not `ReplayOffloadWorker`; replay-native CPU
remains unmeasured.

The H304/H305 follow-up corrects the initial "empty-input" shorthand:
`ReplayOffloadQueue::pop` also waits on an uncommitted mutation placeholder,
and includes wake/reacquisition latency. See the
[native application CPU and wait follow-up](present-pacing-app-thread-waits.305.md);
its application-thread CPU is not replay-worker CPU.

## Routing and retained work

| Counter per Present, perf subwindow | 05 | 06 |
|---|---:|---:|
| Direct lease draw count | 159.47 | 162.92 |
| Draws attributed to degraded direct spans | 1540.94 | 1774.02 |
| Compatibility batch draws | 1549.95 | 1781.32 |
| Compatibility batches | 21.07 | 25.64 |
| Queue draw-run batch calls | 28.07 | 32.64 |
| Queue input runs | 1556.95 | 1788.32 |
| Queue emitted runs | 548.98 | 623.16 |
| Command buffers | 4.000 | 4.000 |
| Render-pass begins | 15.996 | 19.927 |

The queue receives about 55 input runs per batch call, not one queue acquisition
per draw. Compatibility admission starts/identity cuts are not emitted runs;
lower queue merging still reduces the run count. Likewise,
`replay_span_ordinary_fallback_draws` counts degraded direct-span draws, not
every ordinary draw. `replay_span_state_projections` counts attributed records,
not independently measured projection calls. These populations are not a
partition of one exactly matched source set.

## Separate diagnostics

| Diagnostic scope, ms/Present | 05 | 06 |
|---|---:|---:|
| Entire emission planner | 0.477 | 0.398 |
| Replay service including diagnostic overhead | 25.185 | 20.445 |
| App-thread resource-mark mutex acquisition | 1.088 | 0.674 |
| Replay `submit_draw_run` mutex acquisition | 0.170 | 0.087 |
| Replay direct-slot begin mutex acquisition | 0.148 | 0.056 |

The planner timer includes plans subsequently rejected by
`rotationFreeProductionEligible()`, but excludes the subsequent compatibility
execution. Planner calls are 15.89/19.10 per Present. Zero planner counters in
the perf runs mean **observer disabled**, not zero planning work.

Mutex rows have their own `submitPresent` call tally, independently verified
against reported acquires-per-Present. Their windows are (60,1440] and
(720,1800], approximately scene-aligned but not identical to encode-waterline
attribution. Outer and segment hold rows overlap and are not summed. Zero
writer/backpressure waits do not imply zero mutex acquisition cost.

## Next bounded investigation

1. Add an authoritative replay worker native-TID marker/timed-call count for
   diagnostic use, then separate actual user/system CPU from service and waits.
2. Decompose the compatibility parent into state application, generation/identity
   checks, run snapshot/materialization and final-Slot append. Reuse this exact
   scene recipe and preserve source order, prior-state and resource-lifetime
   contracts. The measured append/mark scopes are stronger candidates than the
   sub-millisecond planner alone; exclusive costs are not yet known.
3. Inspect queue hold ownership affecting synchronous app-thread resource mark.
   Do not infer that replay mutex wait itself is the primary bottleneck, or
   relax the direct rotation/correctness gate to increase eligibility.

PSO prefetch belongs to the later encode investigation. These baseline data
justify further replay attribution, not new worker threads, reordered control
operations, mutation composition or a claimed FPS gain.
