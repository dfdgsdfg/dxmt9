---
domain: present-pacing
workload: STALKER CoP Day
title: "H364: Observer-OFF baseline and remaining serial encode budgets"
type: evidence
status: partial-attribution
updated: 2026-09-25
source: experiments/output/app-d3d9-stkcop-bench-encode-residual-h364-control/{result.json,daybenchmark.result,actual.png}; experiments/output/app-d3d9-stkcop-bench-encode-residual-h364-diagnostic/{result.json,daybenchmark.result}; experiments/output/app-d3d9-stkcop-bench-encode-residual-h364-bounded/{result.json,daybenchmark.result}; traces/app-d3d9-stkcop-encode-residual-h364/{analyze.py,analysis.json,planning.json,supply.csv,supply-bounded.csv}
related: present-pacing-binding-plan-default.363.md; present-pacing-serial-encode-owners.361.md
---

# H364: Measure the promoted implementation before another optimization

HEAD `72fce02e`, unchanged runtime. Sikarugir cx24.0.7, Day/high,
1280x720, Apple M1, Xcode 27.0. Catalogue runner only; no builds/tests
overlap app execution. Staging reports no work before launch. The Unix image
SHA-256 prefix is `a02c70a0e12b4fed`; PE32 `4d30258d6e7d0c3e`, PE64
`cae268981dc6cc85`. Binding-plan reuse remains unconditional.

## Observer-OFF baseline

Perf profile with explicit `DXMT_PERF_COUNTERS=0`, no supply/phase/capture
observers. The receipt confirms counters OFF; therefore there is no GPU-error
counter to interpret as zero. Benchmark-owned frame data reports:

- 3,062 frames / 123.358 seconds = **24.822 FPS**.
- 40–50s: 44.90 FPS; 80–90s: 12.80; 90–100s: 8.10.
- 100–110s: 10.50 FPS; 110–120s: 11.80.

Runner PASS. The saved window screenshot shows the scene rather than loading
or a desktop fallback. This is not continuous visual verification or an exact
GPU oracle. The late slowdown remains with diagnostic counters disabled.
One run is a current baseline, not proof of a gain over H361/H362 or README.
Window times are reconstructed benchmark-relative frame completion, not
encoded-Present identities.

## Separate encode attribution

The diagnostic run enables perf counters, draw phase split, stream-bind split,
per-frame sampling and source planning split. It passes, reports zero GPU
command-buffer errors, 2,642 encoded Presents, 1,187,672 command-plan builds
and 6,272,892 reuses. Its 20.259 FPS is **not** an observer-overhead estimate:
input populations and source boundaries differ from the OFF run.

These counters are elapsed scope time, **not native CPU service**. Denominator
is encoded Presents, including startup, not benchmark frame count. Parent and
child rows overlap and must not be added together.

| Elapsed scope | ms / encoded Present |
|---|---:|
| encodeChunk | 24.150 |
| encodeDraw | 17.362 |
| Binding packet (inside encodeDraw) | 1.012 |
| Binding packet plan (inside packet) | 0.654 |
| Stream bind aggregate | 4.300 |
| Index-site stream bind | 1.840 |
| Index source resolve (inside index setup/site) | 0.663 |
| Programmable vertex stream-bind site | 1.178 |
| Texture stream-bind site | 0.902 |
| Uniform build | 0.915 |
| Pipeline lookup | 1.088 |
| Draw issue | 1.290 |

Sequential phase scopes identify remainder 5.516ms, setup 3.529ms,
argbuf/uniform/packet 2.770ms, vertex bind 1.999ms, stream preparation 1.732ms,
base state 1.306ms and FFP vertex 0.190ms. The remainder is the common indexed
setup/issue tail, not unexplained pure overhead; the native timer's destructor
closes it at early returns. Sequential phases total 17.041ms versus the wider
17.362ms encodeDraw timer; the 0.321ms envelope difference is not assigned to
a product operation.

Command-local reuse is exercised. Historical H361 packet cost was 3.975ms/P,
but changed workload/observation prevents claiming the difference as a measured
optimization saving. Remaining per-draw index/stream binding and setup are
better next attribution targets than rebuilding the sampler plan again.
`dxmt9_draw_encoder_draw.mm::encodeDraw` still resolves the index resource
while consuming the actual backing snapshot; removing this lookup requires
preserving generation/lifetime and snapshot fallback authority. No raw-pointer
cache or lifetime-policy change is made here.

## Source trace qualification

The initial broad 1900–2600 planning trace exceeds the fixed sink by 1,310,533
events. Its header reports `closed=1`, `incomplete=0`, but `overflow!=0`:
**the entire transcript is rejected for source cost attribution**. It is
retained as failure evidence. Whole-run perf counters above are independent
of this incomplete transcript. A bounded retry is required, not partial-row
salvage or a larger hot-path sink.

The 2350–2370 retry enables only ordinary perf counters, supply trace and
planning split (not draw/stream/frame observers). All five staged images match
the other runs. Runner PASS, 20.184 diagnostic FPS, zero GPU errors. The trace
has 35,489 rows, zero overflow/incomplete writers, 96 complete planning scopes
and no clipped/aborted planning scope. Ninety-five scopes also have complete
outer/backend endpoints and command-count identity joins.

| Source sequence | Commands | Graph CPU | Optimizer CPU | Planning CPU | Outer Encode CPU |
|---|---:|---:|---:|---:|---:|
| 5707 | 524 | 0.185ms | **35.494ms** | 35.692ms | 61.629ms |
| 5687 | 430 | 0.155ms | **10.749ms** | 10.914ms | 32.849ms |
| 5667 | 299 | 0.116ms | 1.692ms | 1.817ms | 18.155ms |
| 5643 | 438 | 0.364ms | 0.050ms | 0.443ms | 24.219ms |

Identity is this artifact/process plus backend/queue owner and sequence, not
sequence alone. Source 5709 has a separately complete planning scope of
36.313ms (optimizer 35.694ms), but its outer endpoint crosses the trace end;
it is excluded from the fully joined 95-source totals. Those totals are
57.308ms planning CPU, including 48.927ms optimizer and 7.385ms graph building.
The script retains all joined source identities and phase clocks.

**A tens-of-milliseconds planning spike is reproduced, and localized to
`runOptimizer`, not graph construction or sampler-plan preparation.** This is
not identity-equivalent replay of H361's 814-command source. Similar command
counts also have very different costs: command count alone is not a sufficient
complexity predictor; pass/edge shape and rejected/merged candidate counts
need to accompany a reproducible planning input.

One important attribution limit remains. The default optimizer runs lifetime,
pass coalescing, then load/store planning; `runPassCoalesce` recomputes lifetime
afterward. With perf enabled, `FrameGraphBackend::onSourceReadyImpl` also sets
`collect_passcoalesce_return_diagnostics=true`. Its `ReturnDiagnosticTracker`
retains candidate identity vectors and linearly checks previously seen windows.
The core algorithm separately repeats pair scans/reachability/graph rebuilds.
The measured optimizer phase includes **both** algorithm and diagnostics.
It is therefore not evidence that observer-OFF production spends 35ms on the
same work, nor yet proof that pass coalescing itself dominates lifetime/loadstore.

Next priority: a source-qualified pure planning-input fixture that preserves
passes, commands, edges, resource accesses and alias resolution. Compare the
same graph with return diagnostics OFF/ON, then split lifetime/coalescing/
loadstore. Require identical ordered replay plan and action outputs before
changing traversal/storage. This isolates the spike without first needing
full STALKER volume/query GPU replay. In parallel, the residual encodeDraw
budget points to index/stream binding; it does not justify parallel Encode or
predict an FPS gain.

## Fixed-input capture remains a separate gate

Current `D3D9VolumeTextureImpl::LockBox` in
`src/d3d9/d3d9_pe_device_child_surface.cpp` still invalidates capture tracking
after a successful volume lock. H343 observed this on the actual STALKER
startup path. No new failed Tape launch is used to rediscover the same blocker.
Further, Render Tape capture selects a different streaming Arena/source-bound
profile and changes attachment seed/store policy. A capture-mode source cannot
silently stand in for the previous ordinary 814-command source.

This campaign can collect ordinary source timing without changing production
boundaries, but it does **not** provide a replayable large STALKER source or
same-input spike reproduction. Closing volume/query capture/provider semantics
is a separate implementation task. A smaller pure FrameGraph planning-input
fixture may be sufficient for CPU spike reproduction, but it would not be a
full render capture or GPU correctness oracle.

## Reproduction and verification

Use `run_experiment.py run app-d3d9-stkcop-bench` with
`DXMT_EXPERIMENT_PROFILE=perf`, `DXMT_STKCOP_LANE=day`,
`DXMT_STKCOP_PRESET=high`, unique output suffixes, `--capture-delay-sec 100`
and timeout 360s (control) / 480s (diagnostics). Control sets
`DXMT_PERF_COUNTERS=0`. The first diagnostic enables
`DXMT9_PERF_ENCODE_DRAW_PHASE_SPLIT`, `DXMT9_PERF_STREAM_BIND_PHASE_SPLIT`,
`DXMT9_PERF_FRAME_SAMPLING`, `DXMT9_CPU_SUPPLY_PLANNING_SPLIT` and a supply
path/window 1900–2600. The retry drops the three per-draw/frame selectors and
uses a fresh supply path/window 2350–2370.

`scripts/run_python.sh traces/app-d3d9-stkcop-encode-residual-h364/analyze.py`
checks image parity, baseline counter policy and qualified native clocks, then
writes a new `analysis.json` without overwriting existing evidence. The standard
planning analyzer independently produces `planning.json`. No runtime code,
policy, test model or capture admission changes; no commit is made.
