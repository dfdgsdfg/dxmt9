---
domain: present-pacing
workload: 3DMark05 GT2
subcategory: drain-fence-attribution
order: 207
title: The Blocked Locks Are Mostly MANAGED — And R-BACK-5.11 Already Says Those Need No GPU Wait
date: 2026-07-31
type: experiment-run
status: accepted-attribution
source: experiments/output/app-d3d9-3dmark05-gt2-lockclass-r1; experiments/output/app-d3d9-3dmark05-gt2-lockclass-r2
related: docs/perfomance/present-pacing/present-pacing-drain-fence-attribution.206.md; docs/perfomance/frame-lifecycle.md
---

# The Blocked Locks Are Mostly MANAGED — And R-BACK-5.11 Already Says Those Need No GPU Wait

**Question / hypothesis.**
[.206](present-pacing-drain-fence-attribution.206.md) put the corrected
recoverable ceiling at `~1.7-2.15 ms/present`, but that estimate came from the
**all-locks** population — 73.7% READONLY, 5.6% DISCARD — and assumed the
blocked subset resembles it. It does not have to. Which classes actually block?

**Method.** `DXMT9_PERF_DRAIN_FENCE_SITES` extended to classify a lock **at the
moment its drain blocks**, by flags and pool, using the same encoding the
`d3d9_buffer_lock_*` counters use so the blocked subset compares directly
against the whole population. Recorded only on a blocking drain. GT2, `perf`,
two runs.

Read the emitted class line against the **site line from the same emission**,
not against run-final counters: both are cumulative and emitted together every
60 presents, and mixing them undercounts. The class counts sum to the blocked
count exactly in both runs, which is the check that they were read consistently.

**Result.** DISCARD / NOOVERWRITE / plain partition the blocked locks; READONLY
is an orthogonal flag that overlaps them.

| | run 1 (@1080 presents) | run 2 (@1020) |
|---|---|---|
| blocked locks | `11,827` | `10,640` |
| fence cost | `2.589 ms/present` | `2.216 ms/present` |

| class | % of blocked | % of fence time |
|---|---:|---:|
| plain (no DISCARD, no NOOVERWRITE) | `54.2` / `56.7` | `61.7` / `64.7` |
| DISCARD | `40.8` / `39.9` | `30.3` / `31.7` |
| NOOVERWRITE | `5.0` / `3.4` | `8.0` / `3.7` |
| *(READONLY, orthogonal)* | `46.2` / `48.0` | `58.5` / `61.3` |

| pool | % of blocked |
|---|---:|
| MANAGED | `54.2` / `56.7` |
| DEFAULT | `45.8` / `43.3` |

**Two structural facts fall out.** `plain` and `pool_managed` are *identical* in
both runs (`6407`/`6407`, `6037`/`6037`): every blocked MANAGED lock is plain,
and every DISCARD is DEFAULT. The blocked population is two clean groups —
MANAGED plain locks (most of them read-only) and DEFAULT dynamic-buffer
rotations.

**And the blocked subset does not resemble the whole.** READONLY is `73.7%` of
all locks but only `46-48%` of blocked ones; DISCARD is `5.6%` of all locks but
`40%` of blocked ones. Blocking is skewed `7x` toward DISCARD, which is exactly
what you would expect — a DISCARD arrives when the app is mid-frame writing new
geometry, which is when the queue has work. **Estimating the recoverable share
from the all-locks mix, as .206 did, was the wrong denominator.**

**Verdict — the MANAGED half is the exemption candidate, but the number below is
a loose upper bound, not a measurement.**

> ## Corrected 2026-07-31, same day: two errors in this section
>
> **1. The safety argument did not discriminate.** This section originally
> reasoned: `Pool::mapWaitSeqId` returns `0` for managed buffers under
> R-BACK-5.11, so "managed locks already wait on nothing at the pool level —
> the drain fence is the only thing making them block" ⇒ recoverable.
> **`mapWaitSeqId` also returns `0` for NOOVERWRITE (`:1281`) and for
> DISCARD dynamic rename (`:1296`)**, and the counters confirm it empirically:
> `map_buffer_no_wait_seq == map_buffer_calls == 84.09/present`. *No GT2 lock of
> any class ever waits at the pool level* — including the DISCARD half this leaf
> calls unrecoverable. The premise is true of everything and therefore explains
> nothing.
>
> The real distinction, which this leaf only gestured at: a managed lock returns
> a pointer into the core CPU shadow (`Buffer::lock`) and nothing in chunk
> replay writes that shadow, so its *contents* genuinely cannot observe replay.
> DISCARD differs because it rotates the rename ring **at lock time** against
> `lastUsedSeqId`, which queued-unreplayed draws have not bumped. That hazard
> analysis is the argument; `mapWaitSeqId == 0` is not.
>
> **2. The ceiling ignores this document's own predecessor.**
> [.206](present-pacing-drain-fence-attribution.206.md) established that **the
> fence self-clears** — the first fenced lock in a replay window drains it for
> every lock after, which is why only `13.1%` block. Computing "recoverable =
> plain share of fence time" assumes exempting MANAGED deletes those windows. It
> does not: DISCARD locks, interleaved in the same frames, inherit part of them.
> Blocked time actually removed is **less than** `plainNs`, by an unmeasured
> amount. Two leaves three paragraphs apart applied contradictory models to the
> same mechanism and this one used the wrong one.
>
> Two further caveats on the magnitude: these runs carry the classifier and Info
> logging, and their fence is `2.36-2.61 ms/present` against `2.17-2.34` in the
> Warn-level baselines — the same shares give `~1.34-1.45` under baseline
> conditions. And `plain == pool_managed` is a **marginal-count identity**:
> `noteBlockedLockClass` counts flags and pool independently, with no
> (flags x pool) cell and no per-pool nanoseconds, so "MANAGED = 61.7-64.7% of
> fence time" is `plainNs` re-labeled. The identity holds at every emission in
> both runs and has a structural backstop (D3D9 forbids DYNAMIC on MANAGED), so
> it is very probably right — but it is inferred, not measured.

**Honest form of the claim:** an exemption of MANAGED locks removes **at most**
`~1.34-1.45 ms/present` under baseline conditions (`~2.5-2.7%` of frame), before
subtracting whatever the surviving DISCARD locks inherit from the freed windows.
Nothing has been implemented or tested.

The DEFAULT DISCARD half is not recoverable by exemption: those rotate a backing
that queued draws reference, which is the hazard the fence exists for.

**One caveat that constrains the fix shape.** R-BACK-5.11 also says the writable
managed lock publishes *at unlock*, into a backing chosen by an idle test
against `lastUsedSeqId` — a stamp queued-unreplayed draws have not bumped. So a
narrowing may exempt the managed **lock** but must keep the **unlock** fence
(added in `3204452b`). Exempting both would reopen the hazard from the other
side.

**Scope.** Two GT2 runs; the class *shape* reproduces tightly (DISCARD `40.8`
vs `39.9`, plain `54.2` vs `56.7`) while the absolute ms/present differs
(`2.589` vs `2.216`), which is the phase dependence .206 records. GT2 only —
a workload with a different pool mix will have a different blocked subset, and
this leaf's whole point is that the blocked subset is not predictable from the
overall one. No exemption has been implemented or tested.

**Related.**
[.206](present-pacing-drain-fence-attribution.206.md) ·
[frame-lifecycle](../frame-lifecycle.md) ·
[present-pacing](index.md)
