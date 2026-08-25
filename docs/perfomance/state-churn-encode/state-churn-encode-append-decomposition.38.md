---
domain: state-churn-encode
workload: 3DMark05 GT2
subcategory: append-decomposition
order: 38
title: Current Buffer-Mutation And Bridge Ledger Requires A Composition Observer
date: 2026-08-25
type: experiment-run
status: accepted-attribution
source: experiments/output/app-d3d9-3dmark05-current-cap-gt2-r1-20260825; docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.25.md; docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.26.md; docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.37.md; src/d3d9/core_buffer.cpp; src/d3d9/device_c_resources.cpp; src/dxmt9/dxmt9_resource_pool.cpp
related: specs/backend/producer-concurrency/spec.md; specs/verification/spec.md; specs/experiments/harness/replay/spec.md
---

# Current Buffer-Mutation And Bridge Ledger Requires A Composition Observer

## Question

After the stable getter and warm-retention harvest, how much current GT2 time
remains in buffer Lock/Unlock, upload, and PE/unix crossings, and can those
operations be merged without changing D3D9 observation order?

## Current ledger

The 2026-08-25 current-cap run records `1,841` encoded Presents and `28.311`
sampled FPS, or approximately `35.322ms` per sampled frame. Its bridge report
contains `1,933,310` calls and `7,270.326ms` of synchronous bridge residence.
That timer includes unix-side execution while the PE PC remains in the bridge
stub; it is not the cost of the ABI transition alone.

| current GT2 lane | calls / Present | ms / Present | full-removal FPS ceiling |
|---|---:|---:|---:|
| all bridge residence | `1,050.1` | `3.949` | `+12.6%` |
| buffer Unlock | `21.32` | `1.622` | `+4.8%` |
| buffer Lock | `21.32` | `0.581` | `+1.7%` |
| buffer Lock + Unlock | `42.64` | `2.203` | `+6.7%` |
| commit chunk | `15.78` | `1.165` | not transferable as one unit; [.37] prices the existing safe cut at `+0.5%` to `+0.9%` |
| buffer/texture/shader AddRef + Release | `~753` | `0.379` | `+1.1%` |
| other bridge residence | — | `0.202` | `+0.6%` |

Every percentage in the final column is a mathematical bound that assumes the
named time disappears from the producer critical path with no replacement
work. It is not an implementation forecast.

The Lock population is mostly dynamic-buffer traffic: `39,251` calls, of which
`29,554` are `NOOVERWRITE` (`75.3%`), `8,473` are `DISCARD` (`21.6%`), and
`1,224` are plain (`3.1%`). The same run records `5.696GB` of locked bytes and
`1,224` MANAGED uploads carrying `8.933GB` of full-shadow data. These totals do
not reveal how many mutations are dead before first use or which adjacent
ranges can be composed.

## Evidence boundary

The easy bridge class has already been harvested. [.25] removed
`2.21ms/Present` of stable getter and retention traffic and converted it into a
matched `+7.2%` GT2 gain. The remaining bridge ledger has a different shape:

- a bare crossing was measured at about `0.3us`, while current buffer Lock and
  Unlock cost about `27us` and `76us` per call; they are overwhelmingly unix
  map, backing, arena, copy, and upload work rather than transition overhead;
- the remaining high-count AddRef/Release population is approximately
  `0.379ms/Present`, so eliminating every such crossing is only a `+1.1%`
  ceiling;
- current surface Lock is `0.017ms/Present` after low-4GB shadow pooling and is
  no longer a lead branch;
- [.37] prices the safe existing-contract commit-worker transfer below one
  percent.

Raw bridge-call batching therefore cannot recover the `3.949ms/Present` total.
The plausible remaining value is inside buffer mutation execution.

## Required mutation algebra

Unlock cannot be delayed or merged solely because two calls name the same
buffer. A candidate mutation must carry at least:

```text
(resource identity, backing generation, disposition,
 byte-range patch, source ordinal, failure/completion disposition)
```

Composition is permitted only before the first intervening observer or GPU
use:

- two same-generation plain patches compose with last-writer-wins bytes while
  retaining the untouched base;
- disjoint `NOOVERWRITE` ranges may concatenate on the same valid backing;
- `DISCARD` starts a fresh generation and kills an earlier mutation only when
  no draw or observer consumed that generation;
- Unlock success may be returned only after immutable payload capacity,
  resource retention, and any failure-visible reservation are established;
- every draw must retain the exact latest preceding generation, never a later
  CPU shadow after another Unlock.

Draw/ProcessVertices use, read Lock, query/readback, Update/copy control,
cross-thread ordered observation, Destroy/Reset/device-lost, and capture/lease
generation fixation are conservative composition barriers. Chunk boundaries
are not sufficient proof by themselves.

## Required observer before implementation

Add a cold, observation-only per-buffer mutation ledger before designing a
mutation stream. For every successful writable Unlock it must report:

- disposition, range, bytes, and generation;
- ordinal distance to first GPU use or CPU observer;
- zero-use generations and `DISCARD -> DISCARD` chains with no intervening use;
- conservative mergeable `NOOVERWRITE` range count, union bytes, and overlap;
- rejection reason for every non-composable adjacent mutation;
- time split for wow64 writeback, queue-lock acquisition, backing rotation,
  arena update, shadow copy, and live-contents copy; and
- candidate calls, bytes, and measured CPU time saved, not call count alone.

The observer must share the production source-qualified identity and generation
model and remain outside throughput evidence. Render Tape `ResourceMutation`
events provide the replay oracle; the formal/native binding must pin
latest-preceding-generation, CPU-read visibility, `DISCARD` freshness,
`NOOVERWRITE` in-flight safety, exactly-once use, and failure ordering.

## Verdict

Current evidence supports a realistic combined GT2 expectation of roughly
`+2%` to `+4%` from remaining crossing cleanup plus buffer-path reduction, not
the `+12.6%` all-bridge ceiling. A `+5%` or larger branch is credible only if
the observer finds at least about `1.5ms/Present` of dead or conservatively
composable mutation time.

Use `0.5ms/Present` of measured mergeable Unlock time as the design gate. Above
that threshold, a typed versioned mutation stream may justify its semantic and
verification cost. Below `0.2ms/Present`, close the lane and prioritize replay
snapshot/materialization instead. Do not implement speculative upload merging
from call counts or byte totals alone.
