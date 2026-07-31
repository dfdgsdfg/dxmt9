---
domain: state-churn-encode
workload: 3DMark05 GT2
subcategory: append-decomposition
order: 06
title: The Fixed Per-Commit Cost Is Queue-Mutex Contention — 72% Of It, And 1.25% Of The Frame
date: 2026-07-31
type: experiment-run
status: accepted-attribution
source: experiments/output/app-d3d9-3dmark05-gt2-phase-off; experiments/output/app-d3d9-3dmark05-gt2-phase-on; experiments/output/app-d3d9-3dmark05-gt2-phase-on-r2; experiments/output/app-d3d9-3dmark05-gt2-phase-rec256-r1; experiments/output/app-d3d9-3dmark05-gt2-phase-rec256-r2; experiments/output/app-d3d9-3dmark05-gt2-locksplit-r1; experiments/output/app-d3d9-3dmark05-gt2-locksplit-r2
related: docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.05.md
---

# The Fixed Per-Commit Cost Is Queue-Mutex Contention — 72% Of It, And 1.25% Of The Frame

**Question / hypothesis.**
[append-decomposition.05](state-churn-encode-append-decomposition.05.md) fitted
`22.3 us` of fixed per-commit cost inside the synchronous half of
`dxmt9c_device_commit_chunk` and could not say which of its four phases owned
it, calling that "the one number in this document with no attribution behind it,
and it is the suspicious one." This closes that.

**Method.** New heavy opt-in `DXMT9_PERF_COMMIT_CHUNK_PHASE_SPLIT` adds five
timers inside the commit body plus one inside `CommandQueue::markChunkResources`.
GT2, `perf` profile, `--keep-frontmost`, no gputrace. Three configurations:
split OFF (to size the instrument), split ON at the default record cap, and
split ON at cap `256` — the second chunk-size point is what separates fixed from
per-record for each phase individually, which one point cannot do.

**Result — the phase split, at the default cap.** Two runs.

| phase | us/commit | ms/present |
|---|---:|---:|
| `markResolvedV2Resources` | `18.4` / `18.8` | `0.872` / `0.892` |
| `prepareV2OffloadChunk` | `8.53` / `8.80` | `0.405` / `0.417` |
| `waitPresentOrdinalBoundary` (blocking) | `5.66` / `4.35` | `0.269` / `0.206` |
| queue push + notify | `2.37` / `2.23` | `0.113` / `0.106` |
| `importPrevalidatedCommandChunkV2` | `0.09` / `0.09` | `0.004` / `0.004` |
| unattributed | `0.96` | |

The parts sum to `35.99` against a parent of `36.01 us/commit`. The instrument
costs `2.5-3.2 us/commit` for its five clock pairs (parent `32.80` with the
split off), which does not touch the ranking.

**Result — fixed versus per-record, per phase.** With the cap-`256` point
(`35 -> 104` draw records per commit):

| phase | fixed us/commit | per record |
|---|---:|---:|
| `markResolvedV2Resources` | **`19.13`** | `~0` (fitted `-15 ns`, noise) |
| `prepareV2OffloadChunk` | `1.59` | `200 ns` |
| queue push + notify | `1.47` | `23 ns` |
| `importPrevalidatedCommandChunkV2` | `0.06` | `1 ns` |
| **sum** | **`22.25`** | `209 ns` |

`22.25 us` against the `22.3 us` .05 fitted from the parent counter alone. Two
independent methods, agreeing to `0.05 us`. **`markResolvedV2Resources` is the
fixed term**, flat in chunk size; `prepareV2OffloadChunk` — the blob copy and
wrapper addref — is the per-record term, as its job implies.

**Result — and 72% of that is waiting for a lock.** `markChunkResources` opens
with `std::unique_lock lock(mutex_)` on the `CommandQueue`'s main mutex, then
runs a short loop over unique handles. Timing the acquire separately:

| | r1 | r2 |
|---|---:|---:|
| `markResolvedV2Resources` total | `19.29 us` | `19.82 us` |
| of which queue-mutex acquire | **`13.86 us` (71.9%)** | **`14.36 us` (72.5%)** |
| marking work behind the lock | `5.43 us` | `5.45 us` |
| acquire, per present | **`0.657 ms`** | **`0.681 ms`** |

**Verdict.** The largest single identified item in a PE chunk flush is not work.
It is the producer thread contending with the encode thread for the
`CommandQueue` mutex: `~14 us` per commit, `47.4` commits per present,
**`0.67 ms/present` — `1.25%` of the GT2 frame.** The actual resource marking
behind that lock is `5.4 us`, and it is flat in chunk size because the loop is
over *unique* handles, which a bigger chunk does not multiply.

The whole flush now reads end to end:

```
flush 65.5 us
├─ PE side          ~32 us   seal() + wire struct + wow64 bridge crossing
└─ unix side        ~33 us
   ├─ mark           19.3    ├─ queue-mutex acquire   13.9  (72%)  <- contention
   │                         └─ marking work           5.4
   ├─ prepare         8.7    1.6 fixed + 200 ns/record (blob copy, wrapper addref)
   ├─ present wait   ~5.0    blocking, amortised over all commits
   ├─ enqueue         2.3
   └─ import          0.1    free
```

**What this is worth, stated plainly.** Eliminating the contention entirely
would return `0.67 ms` of a `53.4 ms` frame. Applying the H193/H214/.02 Rosetta
discount — producer CPU removed converts to wall at roughly a third — that is
about `+0.4%` FPS. **This is a precisely located and small target**, and naming
it precisely is the result; it is not a promise of frame rate. It also explains
the shape of .04 from a different direction: cap `256` did cut acquisitions
`2.9x`, and still lost, because the drain-fence granularity it traded away cost
several times more than the contention it removed.

**What is not established.** Which side of the mutex the producer is waiting
*for* — the encode thread's own hold time, or self-contention among producer
paths — is not measured; only that the wait exists and how big it is. The PE
half (`seal()` versus the wow64 crossing) also remains a two-way split; under
Rosetta the crossing is expected to dominate, which would make it a cost of the
environment rather than of dxmt9.

## Is the mutex structural? Mostly yes — read this before attacking it

"Contention" reads like an oversight, so it is worth separating what is forced
from what is a choice. Three different things are involved:

**1. The per-resource operation does not need a lock at all.**
`pool_.mark{Texture,Surface,Buffer}Use` is
`rec.lastUsedSeqId = std::max(rec.lastUsedSeqId, seqId)` — a monotonic max on a
`u64`, the textbook lock-free case. The mutex is not held for this.

**2. ~~What it does protect is a real invariant~~ — WRONG, corrected 2026-07-31.**
This section claimed the lock keeps the `nextSeqId_` snapshot fresh so that no
resource is pinned below a sequence that will use it. **That invariant is not
this lock's.** The chunk's actual usage sequences are allocated *later*, by the
replay worker, after intervening publishes have advanced `nextSeqId_` — so the
bulk mark is systematically a **lower bound relative to actual use, lock or no
lock**. Correctness is carried by two other marking layers, both correctly
ordered:

- **Replay-time per-draw marks** (`submitDrawRun`, `submitClear`, …) mark with
  `nextSeqId_` in the *same* `mutex_` critical section as the append, so the
  marked seq equals the slot's eventual publish seq. Exact.
- **Publish-time full-slot walk** (`prepareSlotForPublish` →
  `markSlotResourcesUnlocked`) re-marks every command's resources with the final
  `slot.seqId`, including the Present source that no submit-time path marks.

The repo's own record already said this and the original text contradicted it:
`specs/backend/gap.md` describes the sync bulk mark as covering "liveness only".
And that liveness window is independently covered by wrapper retention —
`prepareV2OffloadChunk` addrefs every chunk-referenced wrapper until post-replay
release, so `destroyPending` cannot be set for one in that window at all. The
bulk mark appears to be belt-and-suspenders, and it is the belt costing
`0.67 ms/present`.

What *is* true: `lastUsedSeqId` gates release (`dxmt9_resource_pool.cpp` asserts
`record.lastUsedSeqId <= completedSeqId` before freeing), so an inflated stamp
is a deferral, never an assert or a use-after-free. Two costs go unstated
though: a stamp can name a sequence that is never allocated (an all-state chunk
appends nothing), making the "delay" unbounded until some later publish; and
`markBufferUse` also bumps the active rename-ring entry, so a bulk mark extends
the current DISCARD backing's lifetime and converts into allocation pressure.

**3. The placement is a requirement — but the cheapest fix does not touch it.**
`R-BACK-2.51(a)` mandates that "wire header/range validation, import, and
handle-marking" stay "synchronous on the app thread before any record is handed
off." *Deleting* the bulk mark is therefore a spec change needing its own hazard
argument and TLA work.

But this section originally stopped there, treating the requirement as the end
of analysis. It is not. **Making `nextSeqId_` a `std::atomic<u64>` and dropping
`mutex_` from `markChunkResources` keeps the marking synchronous, on the app
thread, before handoff — fully compliant — and removes the entire `13.9 us`
acquire.** The stamped value's semantics are unchanged: it is a lower bound with
the lock held, and remains one without it (per point 2, safety is carried
elsewhere).

Two corrections to the original lock-free argument, which was right about the
conclusion and wrong about the reasoning:

- **"A conservative (larger) snapshot" does not exist.** `nextSeqId_` is a plain
  `std::uint64_t` written under `mutex_`; a racy read is UB until it becomes
  atomic, and it can only be stale-**low** — the under-pin direction. Lock-free
  is safe because the mark is redundant, *not* because over-pinning is safe.
- **"Lock-free max" is a misnomer.** `Pool::mark*Use` goes through
  `HandleArena::update`, which takes the arena's `shared_mutex` exclusively per
  handle. No pool-side atomics are needed or possible; the only change is
  `nextSeqId_`'s type. The residual `5.4 us` of marking work still contends on
  that arena lock — it moves off the queue-wide lock, it does not vanish.

One asymmetry worth carrying forward: the producer *waits* `13.9 us` and *holds*
only `5.4 us`, so it is mostly waiting on other holders rather than on its own
work. But note what that measurement is and is not — **acquire time is a
thread's own waiting, not the waiting it causes**, so comparing it to
[encode-phase.196](state-churn-encode-encode-phase.196.md)'s `0.018 ms/present`
for the *worker's* acquire (as the original text did, calling it "`37x`
cheaper") does not exonerate the worker. The hold-time suspects, visible in the
code and unmeasured, are the worker's `submitDrawRunBatchImpl` (compat scan +
binding prep + snapshot + mark + append, all under `mutex_`) and the **finish
thread's pool GC**, which holds `mutex_` across three full arena scans per
completed seqId. Instrument holds, not acquires, if this is picked up.

~~The practical conclusion is a bound … totals `1-2%` of the GT2 frame.~~
**Withdrawn 2026-07-31.** That figure was arithmetically false — flush alone is
`5.1%` of frame, section encode `4.5%`, envelope `3.5%` — and it was reached by
applying the unestablished "3x Rosetta discount" and silently excluding the
`4.1%` drain-fence wait. See
[frame-lifecycle](../frame-lifecycle.md#5-what-the-numbers-say-to-do) for the
corrected accounting. This document sizes one item at `1.25%` of frame; it does
not size the remaining opportunity, and it should not have been read as an
argument for stopping.

**A correction to .05.** Its fixed term (`22.3 us`) is confirmed exactly, but its
per-record term of `276 ns` was too high: the two-point fit could not separate
per-record work from the present-ordinal wait, which the parent timer also spans,
so the wait was lumped into the per-record remainder. Measured directly, the
per-record total is `209 ns`. The `69%`-fixed share becomes about `72%` once the
wait is removed from the denominator.

**Scope.** One workload. Two runs per configuration; the phase ranking is far
larger than the run-to-run spread, but the fixed/per-record split is still a
two-point fit. The instrument perturbs the parent by `2.5-3.2 us/commit`, which
is `~9%` and is why the OFF control was run.

**Related.**
[append-decomposition.05](state-churn-encode-append-decomposition.05.md) ·
[append-decomposition.04](state-churn-encode-append-decomposition.04.md) ·
[state-churn-encode](index.md)
