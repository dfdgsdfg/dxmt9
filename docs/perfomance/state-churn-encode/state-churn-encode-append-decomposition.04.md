---
domain: state-churn-encode
workload: 3DMark05 GT2
subcategory: append-decomposition
order: 04
title: Cutting Chunk Flushes 2.9x Costs 4% FPS — The Flush Bill Moves, It Does Not Leave
date: 2026-07-31
type: experiment-run
status: rejected-lever
source: experiments/output/app-d3d9-3dmark05-gt2-chunkrec64-r1; experiments/output/app-d3d9-3dmark05-gt2-chunkrec64-r2; experiments/output/app-d3d9-3dmark05-gt2-chunkrec256-r1; experiments/output/app-d3d9-3dmark05-gt2-chunkrec256-r2
related: docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.03.md
---

# Cutting Chunk Flushes 2.9x Costs 4% FPS — The Flush Bill Moves, It Does Not Leave

**Question / hypothesis.**
[append-decomposition.03](state-churn-encode-append-decomposition.03.md) found
the chunk flush is now append's largest single component: `2.71 ms/present`,
`5.1%` of the GT2 frame, `41.4` bridge commits per present at `65.5 us` each —
and that `41.4` is just `2,707` appends divided by the default
`DXMT9_PE_CHUNK_MAX_RECORDS = 64`. The knob already exists, so the test is free.
Does raising the cap convert into frame rate?

**Method.** Interleaved A/B/A/B, `64` against `256`, two runs per side, equal
`240 s` cooldown before each, GT2, `perf` profile, frame sampling, no gputrace,
`--keep-frontmost`, `--no-encoder-breakdown`, at `8364aff2`. Scene fps from
per-frame `wall_ms`, steady body (frames `>= 30`, `<= 200 ms`).

**Result — the mechanism works.**

| cap | `chunk_admit`/present | frames | **median scene fps** | p95 |
|---|---:|---:|---:|---:|
| `64` r1 | `47.3` | `1,167` | `19.095` | `69.7 ms` |
| `64` r2 | `47.4` | `1,169` | `19.270` | `69.4 ms` |
| `256` r1 | `16.1` | `1,149` | `18.316` | `70.8 ms` |
| `256` r2 | `16.1` | `1,143` | `18.496` | `70.1 ms` |

Flushes per present fall `47.4 -> 16.1`, a **`2.9x`** reduction. (Not the `4x`
the record cap alone implies, because `DXMT9_PE_CHUNK_MAX_BYTES` — `262,144` by
default — becomes the binding constraint once records are no longer.)

**Result — and it costs frame rate.** Median scene fps `19.18 -> 18.41`,
**`-4.0%`**, with the two spreads disjoint in the losing direction
(`19.095-19.270` against `18.316-18.496`). Frame counts agree. Zero GPU errors
on all four runs.

**Verdict.** REJECTED as a lever, and the reason is the one .03 flagged in
advance rather than a surprise: a flush is a bridge commit whose **synchronous**
half — wire validation, import, handle marking — scales with the chunk it
commits. Committing `2.9x` less often means committing `2.9x` more per commit,
so the per-present bill does not fall; and the coarser producer-to-worker handoff
costs more than the reduced call count saves. The `41.4` flushes were never
`41.4` units of pure overhead that batching could amortise away.

This retires the obvious reading of .03 — "append's largest component is the
flush, so flush less" — while leaving .03's attribution itself intact. The
`2.71 ms/present` is real; it is simply not addressable by moving the cap.

**What this rules out, and what it does not.** It rules out the record cap, and
by the same argument a `DXMT9_PE_CHUNK_MAX_BYTES` sweep: raising the byte cap
would only extend the same trade further in the same direction, so it is not
worth the runs. It does **not** rule out reducing what a flush *does* — the
synchronous validation/import/handle-marking half is the part that scales, and
it has never been decomposed. That, not the flush count, is where a flush-side
win would have to come from.

**Scope.** Two runs per side on one workload. The disjoint spreads make the
direction solid at this size, but `-4.0%` is a point estimate, not a
characterised curve; no intermediate caps were tested because the sign of the
effect is what decides the lever, and it is negative.

**Related.**
[append-decomposition.03](state-churn-encode-append-decomposition.03.md) ·
[append-decomposition.02](state-churn-encode-append-decomposition.02.md) ·
[state-churn-encode](index.md)
