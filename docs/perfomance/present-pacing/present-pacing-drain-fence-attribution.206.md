---
domain: present-pacing
workload: 3DMark05 GT2
subcategory: drain-fence-attribution
order: 206
title: One Of 84 Bridge Entry Points Owns 99.8% Of The Drain Fence — And It Is Buffer Lock
date: 2026-07-31
type: experiment-run
status: accepted-attribution; inferences corrected 2026-07-31 after review
source: experiments/output/app-d3d9-3dmark05-gt2-drainsites-r1; experiments/output/app-d3d9-3dmark05-gt2-drainsites-r2
related: docs/perfomance/frame-lifecycle.md; docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.06.md; specs/backend/requirements.md
---

# One Of 84 Bridge Entry Points Owns 99.8% Of The Drain Fence — And It Is Buffer Lock

> **The measurement stands; several inferences drawn from it did not.** An
> adversarial review reproduced every number and then took apart most of what
> this document concluded around them. Corrected in place, marked where it was
> wrong: the "concentration" is a workload artifact rather than a property of
> the fence, the ceiling was overstated because the fence self-clears, the
> `215 us` is the tail of an in-flight chunk rather than a whole one, and the
> narrowing this proposed is provably unsound. It also surfaced two genuine
> ordering holes, fixed in `3204452b`.

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

**Two of 84 entry points block at all, and one of them is everything.**

> **Corrected.** The original text explained this as "the other 82 never find a
> non-empty queue, because they are themselves the calls that feed it or they
> run at a point where it has already drained." **That is wrong — they are
> simply never called.** From the same runs' bridge counters:
>
> ```
> bridge_state=4        bridge_resource_create=236     (whole run)
> bridge_draw=55828  ==  bridge_commit_chunk=55828     (every draw rides a chunk)
> ```
>
> Roughly 70 of the 84 fenced entry points are called a handful of times or
> never, so they cannot block regardless of queue state. The honest statement is
> that **GT2's hot direct-call surface is `buffer_lock` and nothing else** — the
> concentration is an artifact of the chunked PE design and this workload, not a
> property of the fence. That flips the implication: narrowing the fence around
> locks fixes GT2's mix, and a texture-updating or readback-using app
> redistributes the cost onto entry points this experiment says nothing about.

**Context from the lock counters, same runs:**

| | per present |
|---|---:|
| `d3d9_buffer_lock_calls` | `83.30` |
| ├ `d3d9_buffer_lock_managed_pool` | `62.46` |
| ├ `d3d9_buffer_lock_default_pool` | `20.84` |
| ├ `d3d9_buffer_lock_nooverwrite` | `16.21` |
| └ `d3d9_buffer_lock_discard` | `4.63` |
| `d3d9_buffer_lock_ms` (the lock itself) | `0.882` |

So **`13.1%` of locks block**, and when one does it waits `215 us`. The fence,
not the lock, is the cost. (The original compared that against
`d3d9_buffer_lock_ms` as "`2.7x` the time all 83 locks spend doing their own
work" — that counter includes GPU-completion waits inside `mapBuffer`, so it is
not purely lock work and the multiple is not a clean one.)

**Verdict.** ACCEPTED as attribution. The drain fence is not a diffuse
`4.1%`-of-frame tax spread over the bridge surface; on this workload it is one
call.

> **Corrected — the `215 us` is not "one chunk's worth of replay".** The
> original text claimed that, and linked it to
> [.05](../state-churn-encode/state-churn-encode-append-decomposition.05.md)'s
> `2.91x`-for-a-`2.9x`-chunk granularity relation. Recomputed: worker replay
> averages `21,185.7 ms / 55,828 chunks = 0.379 ms/chunk`, and the wait is
> `0.216 ms` — **`0.57` of one chunk**. The correct model is *the tail of the
> chunk already in flight*, which is what you expect when a lock arrives
> mid-replay (queue depth at commit is typically `0`, not the "1-2" the original
> text asserted). Good news inside the correction: it is replay-bound, not
> scheduling-bound. And it opens an option the original missed — **splitting
> chunks would shrink the per-wait cost roughly linearly**, independent of any
> aliasing test.

**Why the fence is there, and where the asymmetry is.** `dxmt9c_buffer_lock`
forwards to `Buffer::lock`, which for a dynamic buffer decides rename-ring reuse
against `lastUsedSeqId`. Chunks sitting in the offload queue have been
*validated and bulk-marked* but not replayed, so their per-draw marks do not
exist yet; locking before the drain could hand back a backing that queued draws
still reference. That hazard is real — and it is **per buffer**, while the fence
is **global**. A lock on buffer A currently waits out replay of chunks that
reference only B.

**The narrowing this originally proposed, and why it is the wrong mechanism.**
The first version suggested testing `RawCommandChunk::retainedWrappers` — "does
the queue reference *this* buffer?" — and left two questions open. Both are
answerable from the code today, and the answers retire the proposal:

1. **`retainedWrappers` is incomplete.** A chunk's handle table lists only
   handles referenced by sections *present in its records*, and
   `appendBindingSection` emits a streams section only when bindings changed. A
   buffer bound in chunk `N` and merely drawn from in `N+1` does not appear in
   `N+1`'s table. A per-buffer test built on it would answer "not referenced"
   for a buffer that queued draws are actively reading.
2. **The `Buffer::lock` question was misdirected.** Its shared-state effects run
   under the queue mutex and are serialized. The actual gap is that
   `committedSequenceWaitTarget` clamps the wait to `lastCommittedSeqId_`, which
   makes queued-but-unreplayed chunks invisible to the existing per-buffer wait.
   The per-buffer signal already exists in the pool via the bulk mark; scanning
   `retainedWrappers` at lock time re-derives worse information at higher cost.

**And a third hazard the original named nowhere: `unlock`, not `lock`, is where
a write publishes.** Managed writable locks rotate and upload at unlock, and
`dxmt9c_buffer_unlock` was unfenced. That was safe only because the matching
lock had drained — but the PE recorder can seal a chunk *between* Lock and
Unlock. Fixed in `3204452b` along with the unfenced texture/surface lock-unlock
pairs and the READBACK inline-replay lane, which never drained at all.

**Corrected ceiling: `~1.7-2.15 ms/present`, not `2.34`.** The original assumed
every blocking lock could be exempted. It cannot, for a structural reason:
**the fence self-clears.** The first fenced lock in a replay window drains the
queue for every lock after it — which is why only `13.1%` of locks block at all.
Exempting a class does not delete the window, it moves the (shorter) wait to the
next still-fenced lock. By lock class: `73.7%` READONLY (nearly all MANAGED,
which already waits on nothing), `19.5%` NOOVERWRITE (exempt only under a
reading of the app contract that covers queued-unreplayed draws), and
`5.6%` DISCARD plus writable-managed — these hit the hot per-frame dynamic
buffers the just-committed chunk almost certainly references, so an aliasing
test says "aliased" and they keep blocking.

The instrument cannot settle this because it does not record the flags or pool
of the *blocked* subset.

> **Refined in [.207](present-pacing-drain-fence-attribution.207.md).** The
> estimate above used the all-locks mix, which is the wrong denominator: the
> blocked subset is skewed `7x` toward DISCARD (`40%` of blocked against `5.6%`
> of all locks) and READONLY is only `46-48%` of it, not `73.7%`. The MANAGED
> half is the exemption candidate at **at most `~1.34-1.45 ms/present`** under
> baseline conditions — an upper bound, not a measurement, and one that does
> **not** yet subtract what surviving DISCARD locks inherit from the freed
> windows under the self-clearing behaviour described just above. .207 first
> stated this as a measured `1.4-1.7` and is corrected in place.

**Scope, and four presentation corrections.**

- The headline table is the **present-1140 emission**, not run-final. Run-final
  is `10.93` waits/present, `2.360 ms/present`, `215.9 us/wait`. The per-present
  fence cost rises monotonically through the run (`2.07 -> 2.36`), so it is
  phase-dependent — the two runs' agreement "to three decimals" is agreement at
  the same truncation point, not run-to-run stability of a steady-state value.
- That tightness was originally explained as "expected for a call-count-driven
  quantity". Wrong: blocking is a timing race — a lock landing inside a replay
  window. The tightness comes from workload determinism.
- The `get_caps` row is a **load-phase artifact**: all 16 waits occur before
  present 60. Quoting it as `0.01` waits/present implies a steady-state rate.
- These runs need Info-level logging for the emission while the perf baseline
  runs at Warn. The wait itself is measured directly, but cross-run comparison
  against Warn-level baselines carries that perturbation.

Two GT2 runs. Nothing here was measured on GT1, GT3, or SFIV; their lock mixes
differ (SFIV is D3D9Ex with a different pool profile). The `215 us` per wait is
a property of this workload's chunk size and replay cost, not a constant.

**Related.**
[frame-lifecycle](../frame-lifecycle.md) ·
[append-decomposition.05](../state-churn-encode/state-churn-encode-append-decomposition.05.md) ·
[present-pacing](index.md)
