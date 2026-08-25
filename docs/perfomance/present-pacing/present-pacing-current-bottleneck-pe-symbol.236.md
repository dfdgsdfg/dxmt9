---
domain: present-pacing
workload: 3DMark05 GT2
title: "Present-Pacing #236 - Current GT2 Ceiling Is The Producer Thread; PE d3d9.dll Is 10.6%"
type: leaf
status: current
updated: 2026-08-25
source: experiments/output/app-d3d9-3dmark05-current-cap-gt2-r1-20260825; experiments/output/app-d3d9-3dmark05-current-bottleneck-gt2-clean-cpu-r2-20260825; traces/app-d3d9-3dmark05-current-bottleneck-gt2-clean-cpu-r2-20260825/analysis/xctrace-cpu-thread-summary.md; traces/app-d3d9-3dmark05-current-bottleneck-gt2-r1-20260825/analysis/xctrace-metal-gpu-intervals-summary.md; experiments/output/app-d3d9-3dmark05-current-bottleneck-gt2-pe-symbol-r1-20260825; traces/app-d3d9-3dmark05-current-bottleneck-gt2-pe-symbol-r1-20260825/analysis/pe-sampler.md; traces/app-d3d9-3dmark05-current-bottleneck-gt2-pe-symbol-r1-20260825/analysis/pe-sampler-selfpc.csv
related: docs/perfomance/overview-3dmark05-gt2.md; docs/perfomance/baselines/baselines-wild-fps-refresh.04.md; docs/perfomance/state-churn-encode/overview.md
---

# Present-Pacing #236 - Current GT2 Ceiling Is The Producer Thread; PE `d3d9.dll` Is 10.6%

## Question

After the PE recorder and data-boundary refactors, is current GT2 limited by
remaining PE work, replay/encode CPU, or Metal GPU execution?

## Method

The production-shaped run used the normal `perf` profile with frame sampling,
no gputrace, and encoder breakdown off. A second run attached the all-processes
Time Profiler for ten seconds after GT2 entered the scene. The second run kept
the internal encoder breakdown off and reproduced the normal GT2 throughput
range; the trace is used only for thread and stack attribution.

A Metal System Trace sidecar supplied complete per-frame GPU-stage intervals.
Its internal all-frame encoder observer materially reduced throughput, so its
FPS is discarded; only the joined Metal stage duration and route distribution
are used.

PE x86 code cannot be classified reliably from Rosetta-translated xctrace PCs.
A third non-throughput run therefore enabled the repository's Tier 2 sampler:

```sh
DXMT9_PE_MODULE_MAP=1 \
DXMT9_PE_THREAD_SAMPLER=1 \
DXMT9_PE_THREAD_SAMPLER_HZ=250 \
DXMT_LOG_LEVEL=info
```

The sampler suspends the device-creating thread, reads its real Win32 PC, and
classifies that PC against the PE module map. This run is invalid for FPS but
valid for module attribution. The final cumulative group contains `14,453`
samples over `2,700` Presents with zero suspend, context, or resume failures,
no target-thread mismatch, no self-PC overflow, and a passing in-module probe.

## Production frame and pipeline shape

The clean current-cap run records `28.311` sampled FPS, wall p50/p95
`32.326 / 44.567ms`, `3.999` command buffers per Present, `15.778` render
passes per Present, and zero GPU errors.

In the ten-second Time Profiler interval:

| thread | sampled running time | interval utilization | blocking samples |
|---|---:|---:|---:|
| app/PE producer | `10.117s` | approximately one full core | `0` |
| `dxmt9-encode` | `6.022s` | approximately `60%` of one core | `1` |
| replay offload worker | `5.709s` | approximately `57%` of one core | `0` |

The producer is the only saturated serial owner. Replay and encode are large
second-stage costs but execute on separate workers and retain idle headroom.
The replay worker's dominant inclusive frames are
`snapshotDrawSubmissionFromCurrentState()` and
`cachedBaseDrawStateForSubmissionBatch()`; the encode worker is dominated by
`encodeChunk()` and `encodeDraw()`.

The joined Metal sidecar measures a GT2 frame-stage p50/p95 of
`14.460 / 16.143ms`, below both clean wall percentiles. It is the next ceiling
if CPU production is substantially reduced, not the current first ceiling.

## PE module attribution

| PE module | samples | share of producer thread |
|---|---:|---:|
| `3DMark05.exe` | `9,295` | **`64.3%`** |
| `winemetal.dll` | `2,006` | **`13.9%`** |
| `d3d9.dll` | `1,531` | **`10.6%`** |
| Wine, CRT, and other modules | `1,621` | `11.2%` |

`winemetal.dll` is bridge residence rather than the recorder core: the clean
run's bridge counters and producer stacks place buffer lock/unlock, upload,
and `commit_chunk` crossings there. Any remaining gain in this class requires
fewer or smaller resource updates and crossings, not another recorder-local
cache.

The top 32 `d3d9.dll` PC buckets cover `637` of its `1,531` samples. Symbol
lookup groups the visible buckets as follows:

- constant shadow equality/write/setter work: `287` samples, approximately
  `2.0%` of the full producer thread;
- command handle/reference/COM retention work: `183` samples, approximately
  `1.3%` of the producer thread;
- sparse-plan enumeration and record emission: `56` samples, approximately
  `0.4%` of the producer thread;
- the remaining visible and unlisted buckets are dispersed across setters,
  builders, resource methods, and runtime thunks rather than one dominant
  leaf.

## Verdict

The current GT2 first ceiling is the app/PE producer thread, but that does not
mean the PE recorder owns the full core. The game executable alone owns 64.3%
of samples; `d3d9.dll` owns 10.6% and has no remaining single large leaf.
Eliminating the entire PE D3D9 module would give only an approximately 12%
producer-throughput upper bound before the replay, encode, and GPU ceilings
intervene. A realistic recorder-local optimization is much smaller.

Close broad PE leaf tuning as an FPS lane. Keep only structural bridge/resource
update reductions open on the producer side. The next dxmt9-owned priorities
are replay snapshot/materialization elimination and CPU-stage overlap; parallel
Metal encode remains subordinate until a policy lowers the critical-path wall
without duplicating encode CPU.

## Candidate: thinner synchronous admission

Moving more work to the replay worker is viable only as an **admission-boundary
refinement**, not by handing it an unowned PE pointer or unresolved D3D state.
The current worker already owns semantic record replay and draw-submission
construction. Before returning from `commit_chunk`, the producer still must
establish an immutable, bounds-checked packet whose later use cannot race the
application:

- copy or transfer the wire bytes into unix-owned storage;
- validate the envelope and record ranges;
- resolve handle generations and retain every referenced wrapper;
- capture canonical resource identities, mutable-buffer backing generations,
  and raw-residency tokens needed across later discard/rename/unlock calls;
- publish enough resource-access ledger state for direct control calls to fence
  against queued but unreplayed work; and
- preserve present-ordinal pacing and fail-stop visibility.

The default/Legacy path currently also performs combined resource marking and
backing capture synchronously. `R-BACK-2.51` already permits the admitted
CPU-ready Direct lane to defer the exact owning-`seqId` mark until after strict
admission, but it deliberately keeps identities, backing snapshots, wrapper
retention, and raw residency on the producer side. That is the safe cut line.

A bounded next experiment should therefore measure the synchronous prepare,
import, mark/capture, enqueue, and present-wait phases separately, then move
only the portion for which the admission packet already contains an immutable
capability. If mark/capture dominates, the structural solution is a typed
per-chunk access summary plus captured backing-generation tokens that the
worker can consume exactly once. If `winemetal.dll` residence instead comes
from many direct lock/unlock/upload calls, moving commit replay cannot remove
it; those calls require a separate versioned mutation-stream design with
ordered-control, readback, and failure semantics. Do not merge uploads or
crossings speculatively from the current profile alone.
