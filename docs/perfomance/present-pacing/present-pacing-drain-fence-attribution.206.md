---
domain: present-pacing
workload: 3DMark05 GT2
subcategory: drain-fence-attribution
order: 206
title: One Of 84 Bridge Entry Points Owns 99.8% Of The Drain Fence — And It Is Buffer Lock
date: 2026-07-31
type: experiment-run
status: accepted-attribution
source: experiments/output/app-d3d9-3dmark05-gt2-drainsites-r1; experiments/output/app-d3d9-3dmark05-gt2-drainsites-r2
related: docs/perfomance/frame-lifecycle.md; docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.06.md
---

# One Of 84 Bridge Entry Points Owns 99.8% Of The Drain Fence — And It Is Buffer Lock

**Question / hypothesis.** The commit-replay offload's drain fence
(`R-BACK-2.51(d)`: direct device calls must observe replayed state) costs the
producer thread `~2.2 ms/present` across `~10.6` blocking waits on GT2. It is the
largest single unexplored item in the corrected frame model
([frame-lifecycle](../frame-lifecycle.md), `4.1%` of frame), and no leaf had ever
attributed it: 84 bridge entry points call `drainDeferredReplay`, and the
aggregate counter says only that *something* blocks.

**Method.** New diagnostic `DXMT9_PERF_DRAIN_FENCE_SITES` tags each of the 84
call sites with its C-ABI function name and buckets waits and nanoseconds by
call site. Recorded **only when the drain actually blocks** — `depth() == 0`
already returns early — so the cost is ~10 table probes per present, not one per
bridge call. Buckets on the string-literal *pointer*, so a hit is one compare.
GT2, `perf` profile, `--keep-frontmost`, two runs.

One implementation note worth recording: the report was first emitted from
`~D9CDevice` and produced **nothing**. 3DMark05 never releases the device, so
the destructor does not run — the run's log ends at `[dxmt9-perf]` with zero site
lines despite `11.04` drain waits/present in the counters. Moved to a cumulative
emission every 60 presents, mirroring the PE decimated stats line, which exists
for the same reason.

**Result.** Both runs agree to three decimals.

| entry point | waits/present | ms/present | us/wait | share |
|---|---:|---:|---:|---:|
| **`dxmt9c_buffer_lock`** | `10.89` / `10.87` | **`2.341`** | `215` | **`99.8%`** |
| `dxmt9c_device_get_caps` | `0.01` | `0.004` | `312` | `0.2%` |

**Two of 84 entry points block at all, and one of them is everything.** The
other 82 — every draw, state setter, resource create, query, swapchain call —
never find a non-empty queue, because they are themselves the calls that feed it
or they run at a point where it has already drained.

**Context from the lock counters, same runs:**

| | per present |
|---|---:|
| `d3d9_buffer_lock_calls` | `83.30` |
| ├ `d3d9_buffer_lock_managed_pool` | `62.46` |
| ├ `d3d9_buffer_lock_default_pool` | `20.84` |
| ├ `d3d9_buffer_lock_nooverwrite` | `16.21` |
| └ `d3d9_buffer_lock_discard` | `4.63` |
| `d3d9_buffer_lock_ms` (the lock itself) | `0.882` |

So **`13%` of locks block**, and when one does it waits `215 us` — `2.7x` the
total time all 83 locks spend doing their own work. The fence, not the lock, is
the cost.

**Verdict.** ACCEPTED as attribution. The drain fence is not a diffuse
`4.1%`-of-frame tax spread over the bridge surface; it is one call, and the
waits are `215 us` each because a drain waits for **one chunk's worth of
replay** — the same granularity relation
[.05](../state-churn-encode/state-churn-encode-append-decomposition.05.md)
measured at `2.91x` for a `2.9x` chunk.

**Why the fence is there, and where the asymmetry is.** `dxmt9c_buffer_lock`
forwards to `Buffer::lock`, which for a dynamic buffer decides rename-ring reuse
against `lastUsedSeqId`. Chunks sitting in the offload queue have been
*validated and bulk-marked* but not replayed, so their per-draw marks do not
exist yet; locking before the drain could hand back a backing that queued draws
still reference. That hazard is real — and it is **per buffer**, while the fence
is **global**. A lock on buffer A currently waits out replay of chunks that
reference only B.

**The obvious narrowing, and why it is not implemented here.**
`RawCommandChunk::retainedWrappers` already holds every wrapper each queued
chunk references, so "does the queue reference *this* buffer?" is answerable
without draining. Queue depth is small (bounded at 64, typically 1-2), so the
scan is cheap against a `215 us` wait. **This has not been implemented or
tested**, and the two questions that decide whether it is sound are open:

1. Does `Buffer::lock` touch state the replay mutates for *other* resources —
   shared allocator cursors, the transient arena, the rename ring's global
   bookkeeping — such that a per-buffer test is insufficient?
2. Is `retainedWrappers` a complete reference set for this purpose? It is built
   from the chunk's handle table, so a buffer reached indirectly (bound via a
   stream source in an *earlier* chunk and merely drawn from in this one) may not
   appear.

Ceiling if it works and every lock stops blocking: `2.34 ms/present`, `4.4%` of
the GT2 frame, at 1:1 conversion. That is larger than any other single item
identified so far — larger than the `+2.1%` the legacy-record removal delivered.

**Scope.** Two GT2 runs, and the reproducibility is unusually tight (identical
to three decimals), which is expected for a call-count-driven quantity rather
than a timing one. Nothing here was measured on GT1, GT3, or SFIV; their lock
mixes differ (SFIV is D3D9Ex with a different pool profile). The `215 us` per
wait is a property of this workload's chunk size and replay cost, not a constant.

**Related.**
[frame-lifecycle](../frame-lifecycle.md) ·
[append-decomposition.05](../state-churn-encode/state-churn-encode-append-decomposition.05.md) ·
[present-pacing](index.md)
