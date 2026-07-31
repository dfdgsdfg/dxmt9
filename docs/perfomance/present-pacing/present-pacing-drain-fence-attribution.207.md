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

**Verdict — and the ceiling narrows again, downward.** The MANAGED half is
recoverable and the code already says why. `Pool::mapWaitSeqId` returns `0` for
managed buffers, with **R-BACK-5.11** stated inline:

> the core Buffer's CPU shadow is authoritative for every MANAGED lock.
> Read-only locks only inspect it; writable locks publish it into an idle/fresh
> backing at unlock, so neither needs a GPU wait here.

So managed locks already wait on nothing at the pool level — **the drain fence
is the only thing making them block.** That is `61.7-64.7%` of the fence, or
**`1.4-1.7 ms/present`, `2.6-3.2%` of the GT2 frame**, against .206's
`1.7-2.15 ms`.

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
