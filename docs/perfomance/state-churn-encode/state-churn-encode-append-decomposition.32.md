---
domain: state-churn-encode
workload: 3DMark05 GT2
subcategory: append-decomposition
order: 32
title: T2b/T2c + Scan Batch Land — Counters All Win, FPS Null, And The Lock-Path Slack Rule
date: 2026-08-21
type: experiment-run
status: accepted-mechanism
source: experiments/output/app-d3d9-3dmark05-qmutex-t2bc-r1; experiments/output/app-d3d9-3dmark05-t2bc-{a1,a2,a3,b1,b2,b3,b4}; experiments/output/app-d3d9-3dmark05-sampler-reset-attr
related: docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.31.md; docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.30.md
---

# T2b/T2c + Scan Batch Land — Counters All Win, FPS Null, And The Lock-Path Slack Rule

Four increments landed together, each model- or evidence-first (`c9511f31`
T2b/c, `dd9273d4` scan batch, `525e89f2`+`4ae8717a` interleaving harness,
plus the shape-(d) ownership fix it forced):

**Mechanism, all confirmed by counters** (`qmutex-t2bc-r1` vs T2a''s r1):
`map_buffer`'s fast path stops touching the queue mutex outright (20.9 → 0
acquires/present on both segments); the commit path's capture leaves the
lock (mark phase 0.62 → 0.43 ms/present, `mark_lock` 0.195 → 0.148); queue
total acquire-wait 1.32 → 1.00 ms/present and total hold 5.68 → 4.63
(cumulative since the T2 series began: wait 2.03 → 1.00, duty cycle
~17% → ~13%). **The restamp window's first wild measurement: 1,066,758
checks, 0 fires** — the hazard the model guards never occurs on GT2; the
insurance costs one gated add. Zero GPU errors; conformance 234/235
(known set) at every step; full host suite 726/726 including the new
interleaving spec.

**The harness earned its keep on day one.** Its source-text audit fired on
the T2b counter insertion (drift caught, mirror re-pinned), and its
cross-thread schedules exposed a real semantic gap: construction-bound
ownership tokens misfire on D3D9's legal create-on-A/render-on-B pattern —
fixed with a first-use CAS-binding shape (d) on the Pool's producer token.

**FPS: null, and this null teaches the rule.** B-first bracket
(B 28.85±0.23, A 28.72±0.07, −0.4% ≈ noise; sha-verified, zero errors).
Combined prediction was +2.5-2.9%; measured ≈ 0. Placed next to the series'
other datapoints a pattern closes:

| increment | path | converted? |
|---|---|---|
| T2a' mark-lock wait (commit path) | commit, ×16/present | **+1.22%** (.30) |
| shadow alloc 0.64 ms (Lock path) | buffer/surface Lock | null (.31) |
| T2b/c map waits + capture (Lock/commit-capture) | mostly Lock path | null (this) |
| scan batch 0.45 ms (append/commit micro-costs) | spread, sub-noise each | not separable |

**Lock-path costs do not convert on GT2** — the producer is ahead of the
pipeline when the app locks buffers, so waits and work removed there come
out of run-ahead slack, not the frame. Commit-path waits convert. This
refines [.31]'s burst rule into a path rule: before predicting fps from a
producer-side saving, ask *which entry family* carries it; the answer for
GT2 is that only commit-path (and presumably present-path) savings are
wall-coupled. The 5b burst instrumentation is therefore moot for the lock
lane — two independent nulls already answer it empirically.

**5a resolved free of charge** (fresh sampler run, binary-matched join —
after the session learned the hard way that self-PC buckets only join
against the exact staged dll, now snapshotted at run time):
`CommandChunkBuilder::reset()` has vanished from the top self-PC buckets —
the cadence promotion (44 → 8 seals/present) dissolved it; no work
warranted. The fresh top list instead names **`appendHandle` at 9.5% of
d3d9 self-PC (~0.35 ms/present)** — the one scan the batch deliberately
skipped ("record-local window is tens"), refuted by call volume (thousands
of appends/present × per-reference calls). It is the next mechanical
candidate, with the caveat that as an append-path cost its fps conversion
is plausible but not certain under the path rule. touchConstShadow (13.8%)
remains the known hard item; info-level logging artifacts (~18%) discount
the rest.

**Where the producer ledger stands.** Queue-mutex side: the producer now
acquires only the frozen-ticket re-read (16×, 0.146 wait) and slow-path
map/visibility waits; the dominant remaining holder is the worker's slot
append (3.4-3.8 hold — T2d, model required) and the encode-side
`find_reordered_index_buffer` wait. Game-thread compute: touchConstShadow
0.42, appendHandle ~0.35, buildSparseState ~0.23, lock-path unix work
(2.6, slack-absorbed — deprioritized by the path rule).
