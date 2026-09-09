---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "Upload queue and buffer arena lock acquisition/hold split"
type: evidence
status: measured-runtime
updated: 2026-09-09
source: experiments/output/upload-arena-locks-H306-20260909/analysis
related: present-pacing-app-thread-waits.305.md; present-pacing-replay-baseline.303.md
---

# Upload queue and buffer arena lock split

This diagnostic overlay on `1f3f0358` separates acquisition latency from
lock-held elapsed work. It changes no queue, mutation, rename or reclaim
policy. `candidate-source.json` freezes the uncommitted source overlay;
the base HEAD alone does not identify the measured implementation.

## Observer contract and coverage

`DXMT9_PERF_UPLOAD_ARENA_LOCK_SPLIT=1` enables the observer implemented in
`src/dxmt9/dxmt9_upload_arena_lock_diag.{hpp,cpp}`. Disabled callers branch
before any diagnostic clock or state construction. The observed scope starts
before the real lock acquisition, times the held interval up to immediately
before unlock, then records after releasing that lock. A failed acquisition
records no acquired operation; callback exceptions still release and record.

The fixed 256-bucket sink is keyed by native TID, site, operation and mode.
It uses lock-free atomics, constant-initialized storage and no sink mutex,
allocation or logging during recording. A bounded atomic-payload sequence
snapshot skips incoherent rows and exposes skipped/overflow/dropped coverage.
Info-level rows and an endpoint marker are emitted every 60 process-global
`CommandQueue::submitPresent` calls, before its queue lock is acquired.
Rows are individually coherent, not one atomic all-thread snapshot. Native
TID reuse over longer process lifetimes is not a thread-generation identity.

Covered sites:

- `DeviceImpl::uploadBufferData` / `uploadBufferDataRange`: queue mutex.
- Buffer `HandleArena::update`, both `updateMany` overloads and
  `reclaimCompleted`: exclusive arena lock.
- Buffer `HandleArena::inspect`: shared arena lock.

`find/insert/clear`, texture/surface arenas, other queue acquisition sites and
other mutexes are **not** covered. Arena operation rows aggregate their callers;
an `update` row alone is not specifically `Pool::markBufferUse`. The existing
Unix commit marker selects the application native TID; other TIDs remain
separate and are not guessed to be replay/encode/finish by cost or ordinal.
No resource identity or causal competing lock-holder trace is collected.

Acquisition latency includes uncontended acquisition cost, observer clock
cost and preemption, not only blocking. Hold time is elapsed, not CPU time.
Upload queue hold includes nested arena wait/work and its diagnostic sink
overhead. **Do not add queue hold to arena wait/hold.** Max hold is process
cumulative at the endpoint, not the maximum of the differenced scene window.

## Recipe and qualification

Sequential OFF/ON pairs for 05 then 06, same candidate products. H305's
Sikarugir cx-24.0.7, PE32/WoW64, named GT2 lanes and feature recipe are retained:
1024x768 / 1280x720; offload on; Tape, segmented transport, inline constant
delta and opaque-depth optimization off. Both modes use Info/perf with frame
sampling and the external native CPU observer; no stack samples, plan split,
legacy queue-mutex split or GPU tracing. These are diagnostic spots, not
uninstrumented FPS or a noise-qualified A/B. README remains unchanged.

The native build, all three canonical runtime builds, enabled/disabled native
probe specs, resource-hazard and encode-session specs, TLA verification and
the runtime install-name audit pass. Fake clocks pin acquisition/hold values;
tests also cover callback/lock failure, actual arena wrappers, concurrent
snapshot coherence and explicit bounded-table overflow. Initial native build
needed an explicit macOS SDKROOT; the subsequent complete build passed.
No build/test workload ran during the benchmarks.

The analyzer uses the first/last loss-free complete snapshot whose preceding
and following frame-log observations lie inside the native scene envelope.
Its denominator is the **submitPresent ordinal difference**, not encoded
Present count or the full-scene frame count. Asynchronous/log endpoint skew
remains explicit; no slow interior samples are removed. The OFF run must have
no observer rows. Native CPU includes game/Wine/PE/synchronous Unix, not PE
alone. Images, source/build/runtime/tool hashes and raw artifacts are retained
in the per-run recipes and measurements.

## Measured acquisition and hold

Application-thread results below are milliseconds per submitPresent, from
05 ordinal `(60, 1380]` (1,320 Presents, native TID 48163276) and
06 ordinal `(600, 1560]` (960 Presents, native TID 48189177).

| Site / operation | 05 calls/P | 05 wait ms/P | 05 hold ms/P | 06 calls/P | 06 wait ms/P | 06 hold ms/P |
|---|---:|---:|---:|---:|---:|---:|
| Upload queue / upload range | 15.966 | 0.585 | 0.116 | 9.184 | 0.159 | 0.008 |
| Upload queue / upload data | 4.731 | 0.537 | 0.302 | 6.327 | 0.746 | 0.373 |
| Buffer arena / exclusive update | 720.271 | 0.682 | 0.860 | 821.195 | 0.703 | 0.886 |
| Buffer arena / shared inspect | 703.453 | 0.138 | 0.100 | 811.950 | 0.116 | 0.109 |

The two disjoint application upload operations sum to **1.123/0.905 ms/P
acquisition and 0.418/0.382 ms/P hold** for 05/06. Full-upload acquisition
averages 113.5/117.9 microseconds per call, versus 0.946/0.856 microseconds
for arena update and 0.196/0.143 microseconds for inspect. Thus upload is a
low-frequency, long-acquisition candidate; arena is a high-frequency,
short-operation candidate. Neither is a measured removable CPU/FPS budget.
No application-thread reclaim occurs in these windows. Other native TID rows
are preserved in the summary without speculative thread-role attribution.

## Observer controls and evidence

| Diagnostic spot | Scene FPS | Native app CPU ms/frame | Native app average cores |
|---|---:|---:|---:|
| 05 OFF | 23.674 | 37.229 | 0.911 |
| 05 ON | 22.049 | 39.227 | 0.891 |
| 06 OFF | 15.202 | 60.869 | 0.945 |
| 06 ON | 15.085 | 61.209 | 0.934 |

Native CPU uses partial envelopes (97.54%, 94.35%, 97.65%, 99.90% coverage),
not exactly the whole-scene FPS endpoints. ON/OFF differences contain both
observer perturbation and single-run variation; do not subtract them as a
calibrated observer cost. Both modes retain the other diagnostic consumers.

All observed health counters are zero. Full-health reporting covers both 05
scenes; 06 OFF/ON retain 29/2 uncovered final frames. Both 06 runners terminate
after the useful scene in the result GUI (143), not mid-scene. Both ON
screenshots were inspected and are nonblack, but this is not full temporal
rendering or pixel-equivalence evidence. All emitted ON snapshots report zero
overflow, dropped events, inconsistent rows and skipped rows; OFF has no rows.

`lock-summary.json` preserves exact endpoints and per-TID deltas.
`final-audit.json` passes artifact/source/build/runtime hash checks, native CPU
and lock-delta reconstruction, and native/TLA test-result checks. All five
product images are identical across these four runs. Compared with historical
H305, both d3d9 PE images and the Unix provider differ; the two PE bridge
images are unchanged. H305 is not an unchanged-product control for H306.

## Interpretation boundary

This closes the H305 acquisition-versus-hold observation gap for the named
sites. It does not establish a safe lock-removal transformation or a removable
FPS budget. The next code-contract audit can examine whether the existing
`completedSeqIdAcquire()` conservative-watermark protocol can cover upload
without a full queue hold, while retaining arena serialization and backing
lifetime. That remains a hypothesis, not an implementation change here.
