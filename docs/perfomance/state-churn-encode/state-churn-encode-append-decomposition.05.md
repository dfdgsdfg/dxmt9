---
domain: state-churn-encode
workload: 3DMark05 GT2
subcategory: append-decomposition
order: 05
title: The Flush Is Half Bridge-Crossing And Half Commit, 69% Of It Fixed Per Call — And The Fixed Part Is The Pipeline's Clock
date: 2026-07-31
type: experiment-run
status: accepted-attribution
source: experiments/output/app-d3d9-3dmark05-gt2-decim64-postremoval; experiments/output/app-d3d9-3dmark05-gt2-decim16-postremoval; experiments/output/app-d3d9-3dmark05-gt2-chunkrec64-r1; experiments/output/app-d3d9-3dmark05-gt2-chunkrec64-r2; experiments/output/app-d3d9-3dmark05-gt2-chunkrec256-r1; experiments/output/app-d3d9-3dmark05-gt2-chunkrec256-r2
related: docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.04.md; docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.03.md
---

# The Flush Is Half Bridge-Crossing And Half Commit, 69% Of It Fixed Per Call — And The Fixed Part Is The Pipeline's Clock

**Question / hypothesis.**
[append-decomposition.04](state-churn-encode-append-decomposition.04.md) closed
with the only remaining flush-side question: not how often a flush happens, but
what the synchronous half of one *does*. `2.71 ms/present` at `65.5 us` per
flush, never decomposed.

**Method.** No new instrumentation was needed for the first two cuts. The
`.03`/`.04` runs already carry `offload_commit_app_cpu_ms`, which is timed
**inside** `dxmt9c_device_commit_chunk` on the unix side, against the PE-side
`append_flush` phase timer that wraps the whole call. The difference is
everything that is not the unix function. The `64`-vs-`256` cap pair then gives
two points on chunk size, which separates fixed from per-record cost.

**Result — the call splits almost exactly in half.** Same run, cap `64`:

| half | per flush | per present | what is in it |
|---|---:|---:|---|
| PE side | `~32.3 us` | `~1.34 ms` | `CommandChunkV2Builder::seal()`, wire-struct fill, and the wow64 bridge crossing |
| unix side | `33.2 us` | `1.58 ms` | `dxmt9c_device_commit_chunk`'s synchronous body |
| **total** | **`65.5 us`** | **`2.71 ms`** | |

The unix body is, in source order: `prepareV2OffloadChunk` (copy the wire blob
into an owned `RawCommandChunk`, resolve and addref every wrapper, preflight
validate), `importPrevalidatedCommandChunkV2` (parse the record view),
`markResolvedV2Resources` (handle/resource marking), the wire telemetry counter,
and the push to the offload worker.

**Result — and 69% of the unix half is fixed per call, not per record.** Two
points, `47.3` commits/present at `35` draw-records each against `16.1` at `104`:

| cap | commits/present | draw-recs/chunk | us/commit | ms/present |
|---|---:|---:|---:|---:|
| `64` | `47.3` | `35` | `32.0` | `1.517` |
| `256` | `16.1` | `104` | `50.9` | `0.820` |

Linear fit: **`22.3 us` fixed per commit + `0.276 us` per record.** At the
default cap that is `1.054 ms/present` of pure per-commit overhead against
`0.463 ms` of per-record work.

> **Corrected by [.06](state-churn-encode-append-decomposition.06.md), which
> measured the phases directly.** The fixed term is confirmed exactly
> (`22.25 us` summed from per-phase timers against `22.3 us` fitted here — two
> independent methods agreeing to `0.05 us`). The per-record term was **too
> high**: this two-point fit cannot separate per-record work from the
> present-ordinal boundary wait, which the parent timer also spans, so that
> blocking wait was lumped into the per-record remainder. Measured directly it
> is `209 ns`, not `276 ns`, and the fixed share is `~72%`, not `69%`.
> `markResolvedV2Resources` owns the fixed term, and `72%` of *that* is
> acquiring the `CommandQueue` mutex.

**Verdict — and why .04's lever failed, quantitatively.** A `22.3 us` fixed cost
paid `47.3` times per present looks exactly like something batching should
erase, and cap `256` did erase most of it: unix commit CPU fell `1.517 ->
0.820 ms/present`, a real `-0.697 ms`. The frame still got `4%` slower, and the
counters say precisely where it went:

| | cap `64` | cap `256` | delta |
|---|---:|---:|---:|
| `offload_commit_app_cpu_ms` | `1.517` | `0.820` | **`-0.697`** |
| `offload_drain_fence_wait_ms` | `2.193` | `4.901` | **`+2.708`** |
| net | | | **`-2.011 ms/present`** |

against an observed frame-time change of about `+2.2 ms`. The accounting closes
to within `0.2 ms`.

The mechanism is exact rather than hand-wavy. Drain-fence *waits* became less
frequent (`10.62 -> 8.15` per present) while each one became far longer:

| | cap `64` | cap `256` | ratio |
|---|---:|---:|---:|
| per drain-fence wait | `206.4 us` | `601.4 us` | **`2.91x`** |
| (r2 repeat) | `203.9 us` | `587.7 us` | `2.88x` |

`2.91x` against a chunk-size ratio of `2.9x`. **Chunk size is the granularity at
which the producer can observe the worker's progress.** A direct call that must
drain the queue waits, on average, for one chunk's worth of replay — so making
chunks `N` times bigger makes every drain wait `N` times longer, exactly. The
`22.3 us` fixed cost is not overhead sitting beside the pipeline; commit
frequency *is* the pipeline's synchronization clock, and the fixed cost is what
that clock charges per tick.

This is the general statement .04 could only assert: the flush count was never
amortizable, and now the reason is measured rather than argued.

**What is still not decomposed, stated plainly.** Which of the four unix phases
owns the `22.3 us` fixed term. That is the one number in this document with no
attribution behind it, and it is the suspicious one — `22.3 us` is a great deal
for what should be a blob copy plus an enqueue, and it is fixed, so it is not
the copy. The candidates are the owned-blob allocation, the worker queue push
and its condvar notify, `findDirtyQueue`, and
`noteCommitChunkEntryForCompletionGap`. Separating them needs new decimated
scopes inside `dxmt9c_device_commit_chunk`; nothing existing can see it.

> **Answered in [.06](state-churn-encode-append-decomposition.06.md)** — and by
> none of the candidates guessed above. `markResolvedV2Resources` owns
> `19.13` of the `22.25 us`, flat in chunk size, and `72%` of that is waiting
> to acquire the `CommandQueue` mutex: `0.67 ms/present`, `1.25%` of the frame.
> The fixed cost is contention, not work.

The PE half is equally undecomposed, but less interesting: `seal()` versus the
wow64 crossing is a two-way split, and under Rosetta the crossing is expected to
dominate — which would make it a cost of the environment rather than of dxmt9.

**Scope.** The `us/commit` figures use `commit_chunk_draw_submission_batch_records`
per `chunk_admit` as the record count, which counts draw submissions rather than
all records, so the fitted `0.276 us` per record is per *draw* record and the
fixed term absorbs whatever non-draw records cost. Two points define the line, so
the split is a two-point fit, not a characterised curve. One workload.

**Related.**
[append-decomposition.04](state-churn-encode-append-decomposition.04.md) ·
[append-decomposition.03](state-churn-encode-append-decomposition.03.md) ·
[state-churn-encode](index.md)
