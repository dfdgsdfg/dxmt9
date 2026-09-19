---
domain: present-pacing
workload: STALKER CoP Day
title: "H331: occlusion membership publications and pre-fragmentation CPU"
type: evidence
status: partial-attribution
updated: 2026-09-20
source: experiments/output/app-d3d9-stkcop-bench-query-split-attribution-r662-day/result.json; experiments/output/app-d3d9-stkcop-bench-query-split-attribution-r662-day/xrEngine_dxmt9.log; experiments/output/app-d3d9-stkcop-bench-query-split-attribution-r662-day/dxmt9.log; experiments/output/app-d3d9-stkcop-bench-query-split-attribution-r662-day/thread-cpu.jsonl; experiments/output/app-d3d9-stkcop-bench-query-split-attribution-r662-day/app-thread-pre-fragmentation.sample.txt; experiments/output/app-d3d9-stkcop-bench-app-thread-pre-split-r663-day/result.json; experiments/output/app-d3d9-stkcop-bench-app-thread-pre-split-r663-day/dxmt9.log; experiments/output/app-d3d9-stkcop-bench-app-thread-pre-split-r663-day/thread-cpu.jsonl
related: present-pacing-query-pending-publication.330.md
---

# H331: the late cut is primarily an occlusion membership boundary

Base `645624ed` commits H329/H330. The diagnostic-only overlay extends the
existing split observer with cumulative snapshots at at most 4,096 Present
publications and successful nonempty/empty scope replacements. Scope extent
counts replacements that published a populated writer. These are **not**
BEGIN/END API labels: nested queries can keep membership nonempty at END.
No rendering, submission, lifetime or runtime default policy is changed.

## R662: publication reasons

Day High, 1280x720, Sikarugir cx24.0.7, actual occlusion/async fences/Pending/
publication dedup ON, END coalescing OFF. Split observer and per-Present
counters ON. Encoder-close diagnostics are restricted to source sequences
7,000–18,000; other query observers OFF. Exact recipe is in `run-policy.json`.
The canonical native and x86_64 Unix builds succeed, including install-name
fixup. No build/test runs during the benchmark. Runner PASS; eight health
counters zero. This is a heavily instrumented diagnostic, not FPS evidence.

2,387 complete publication-clock snapshots survive. Subtract cumulative counts
at the two endpoints and divide by their Present difference:

| Publication-clock interval | 100→700 | 1600→1900 | 1900→1950 | 2100→2380 |
|---|---:|---:|---:|---:|
| SemanticBoundary publications/P | 0.403 | 1.830 | 0.080 | 54.336 |
| Scope→nonempty populated-writer publications/P | 0.075 | 0.603 | 0.040 | 3.382 |
| Scope→empty populated-writer publications/P | 0.328 | 1.227 | 0.040 | 50.954 |
| QueryEnd publications/P | 1.000 | 1.000 | 1.000 | 1.000 |
| FLUSH publications/P | 0 | 0 | 0 | 0 |
| Empty ENDs/P | 0.328 | 1.227 | 0.040 | 50.954 |
| All publications/P | 2.403 | 3.830 | 2.080 | 56.336 |
| Completion-bearing tail CBs/P | 2.403 | 3.823 | 2.120 | 56.336 |

The two scope-publication extents account for the SemanticBoundary population
in these windows. In the late interval, most scope-empty replacements publish
the queried draws; the following END finds an empty writer and reuses the
preceding prefix. It does not publish another populated source. The remaining
one QueryEnd publication per Present is not the ~54/P explosion.

Source evidence:

- `src/d3d9/core_resources.cpp`, `Device::issueQuery`: construct immutable
  membership and call `setOcclusionScope` before updating public issue state.
- `src/dxmt9/dxmt9_command_queue.cpp`, `CommandQueue::setOcclusionScope`:
  publish a populated old writer with `SemanticBoundary`, then replace scope.
- The same file, `CommandQueue::publishQueryEnd`: empty ENDs share the existing
  published prefix; their later FLUSH request may still request submission.
- `src/dxmt9/render/session_source_policy.cpp`,
  `sessionSourceCanBeHead` / `sessionSourceCanAppendToPending`: scoped sources
  cannot join cross-source encode sessions.
- `src/dxmt9/dxmt9_draw_encoder_chunk.mm`, `encodeChunkImpl`: membership changes
  close an active encoder with `OrderedControl`; independent source finalization
  can already have closed that encoder, so not every membership cut logs that
  reason.

The filtered encoder census contains 22,492 render encoders: 9,736 `final`,
7,821 `rt_change`, 4,302 `clear`, 633 `present`. `final` encoders contain 387,091
draws (~39.8 each), consistent with many short source tails. These are not
exclusive query-caused pass counts, and source sequences are not Present IDs.
Tail-CB counts exclude initializer and mid-source sub-CBs; do not equate them
to `command_buffers`. FLUSH publication zero does **not** mean zero FLUSH calls
or zero submission pressure on an already-published prefix.

## R662: pre-fragmentation CPU and rejected native attribution

The external native-thread CPU helper selects PID 18967, TID 571436 from the
production commit-entry marker and preserves process-start identity. Partial
stable-endpoint intervals before stack sampling show:

| Encode Present selection | Covered frame deltas | App-thread CPU ms/frame | Mean occupied cores |
|---|---:|---:|---:|
| 100–699 | 537 | 32.998 | 0.834 |
| 1600–1899 | 180 | 50.379 | 0.741 |
| 1900–1949 | 36 | 53.645 | 0.633 |

This is actual thread CPU, including game/Wine/PE/synchronous Unix, under
diagnostic logging; it is neither PE-only nor an observer-OFF cost estimate.
Partial sampling shows rising CPU per frame **and** falling CPU occupancy:
the interval growth is not explained by CPU work alone. The trace later stops
fail-closed at its 8 MiB incremental-log bound (142 successful samples, one
slow query, then fatal); no missing tail is treated as zero CPU.

An external 10-second `/usr/bin/sample` starts at observed encode frame 1952.
The next checked log position is 2064, by which time the split count has risen;
this bracket is not a pure pre-fragmentation sample. Exact-TID parsing gives
525 stack samples, 100% opaque/Rosetta leaf labels and 88% repeated-dispatcher
unwind patterns. Only 29 show provider context (18 query-get-data, six commit,
four buffer lock/unlock, one shader release). This is **rejected as exclusive
game/Wine/PE/Unix attribution**. No wait-frame result or native leaf percentage
can establish where the CPU time went. Preserve the failed evidence, not a
fabricated four-way budget.

## Next boundary

The measured optimization target is **query-scope-aware encode-session
composition**, not merely deferring `publishQueryEnd()`. Scope boundaries
still need exact query membership, independent visibility contributions,
failure propagation and completion-before-S_OK. A design may preserve render
pass boundaries while sharing a CB across multiple sources; it must not
advertise deleting required visibility boundaries or resource lifetime pins.
No such runtime policy is changed by this investigation.

The distinct pre-fragmentation CPU owner still needs qualified guest-PC
evidence. Native Rosetta unwinding is not an adequate replacement.

## R663: guest-PC follow-up, strongly perturbed

`experiments/output/app-d3d9-stkcop-bench-app-thread-pre-split-r663-day/`
preserves the two logs, `result.json`, `daybenchmark.result`, `run-policy.json`,
capture and `thread-cpu.jsonl`. All five images match R662; Unix hash is
`7e3f90849da04af5`, the four PE images are unchanged from H330. Split and
encoder-breakdown observers OFF, frame sampling ON; the existing PE sampler
runs at 100 Hz with module map and full self-PC coverage. Actual occlusion,
async, Pending and publication dedup remain ON; END coalescing OFF.

Runner PASS/0, no timeout, all eight health counters zero. It naturally exits
after 543.919 seconds; benchmark reports 1,516 frames / 441.993 seconds.
This severe diagnostic slowdown disqualifies FPS and any claim that observed
stalls have their normal workload duration. An attempted termination after the
decision to stop the diagnostic finds the PID already gone; no signal is
delivered. Do not classify the run as killed or timed out. The runner's
configured wait timeout starts after capture delay, not at initial launch.

All 26 retained cumulative sampler groups have valid full self-PC coverage,
zero suspend/context/resume failures and no target-thread mismatch. The last
periodic group is Present 1,560 / 40,412 samples, not a process-final census.
Subtracting interior reports gives stopped guest-PC populations:

| PE Present reports | Samples | dxmt9 d3d9.dll | Bridge DLL | xrCPU_Pipe.dll | ntdll.dll | Unreported module residual |
|---|---:|---:|---:|---:|---:|---:|
| 120→420 | 1,374 | 11.43% | 20.89% | 2.62% | 11.79% | 5 |
| 720→900 | 871 | 10.45% | 18.83% | 10.22% | 15.96% | 3 |
| 960→1080 | 896 | 10.83% | 13.50% | 25.78% | 12.17% | 4 |
| 1080→1200 | 5,168 | 1.45% | 2.69% | 5.67% | 81.11% | 4 |

Percentages use all interval samples, not just displayed modules. Module
reporting keeps only top 20, so the small nonnegative residual stays unknown.
Other game modules (`xrRender_R2`, `xrCDB`, `xrParticles`, etc.) are omitted
from this compact table, not attributed to PE or Wine. Bridge PCs may be saved
guest context during Unix execution/wait. These are **not exclusive CPU-time
fractions** and must not be multiplied by native CPU ms/frame.

Nearby encode windows confirm that this rise occurs before large CB
fragmentation: IDs 960–1079 average 5.62 CB/P and 111.8 ms interval;
1080–1199 average 8.35 CB/P and 536.3 ms interval. PE and encode ordinals
are adjacent pipeline clocks, not a proven exact per-frame join. The latter
duration in particular is strongly perturbed by the suspension sampler.

The independent CPU helper records 240 successful snapshots without a fatal
read-bound error. In partial stable intervals it measures 45.894 ms/P and
0.784 cores for encode IDs 120–419, 86.342 ms/P and 0.768 cores for 960–1079,
then 110.768 ms/P and 0.232 cores for 1080–1199. This corroborates mixed CPU
work and non-running time **in this diagnostic**, not natural-state costs.

The guest evidence names game CPU-pipeline work and Wine-side suspended/waiting
context as separate follow-up targets; it does not prove an ntdll bug, a
particular wait primitive, software skinning, or the caller of that wait.
`xrCPU_Pipe.dll` exports worker APIs including `ttapi_RunAllWorkers`, but the
sampler does not resolve PCs within that module, so this is an investigation
lead, not function-level attribution. A low-perturbation caller-qualified wait
receipt is needed to finish that attribution. Broad PE micro-optimization is
not justified by these populations.

## Toolchain and verification boundary

The Metal debugging preflight finds Xcode 27 and xctrace available, but the
separately installed Metal Toolchain is missing (`xcrun metal --version`
fails). No system install or GPU trace is attempted. Existing runtime images
and C++ builds remain usable; these experiments use CPU-side queue logs,
native CPU receipts and guest-PC sampling. Runtime Metal correctness is not
newly proven by the diagnostic counters.

R662's 2,387 snapshot groups have all 11 event kinds, contiguous ordinals,
one queue identity and monotonic count/extent fields. Native and canonical
x86_64 C++ builds pass. The focused query tests exercise the observer enabled;
no source lifetime or query state-machine transition was changed.
Final native focus passes 2/2 and x86_64 focus plus install-name audit passes
3/3 (`build/meson-logs/h331-query-focus.txt` and
`build-x86_64-builtin/meson-logs/h331-query-focus.txt`). The complete existing
TLA suite passes in 204.49 seconds (`build/meson-logs/h331-tla.txt`), after
both diagnostic processes have exited. Documentation source audit and
`git diff --check` pass. The post-645624ed observer/documentation changes are
packaged together; rendering and submission policy remain unchanged.
