---
domain: state-churn-encode
subcategory: drawrun
order: 01
title: Draw-Run Failure Shape
date: undated
type: measurement
status: model
source: specs/perfomance.plan.md#L3791-L3820
---

# Draw-Run Failure Shape

**Question / hypothesis.** Why does the importer draw-run scanner almost never
batch draws? Quantify what breaks the run scans so the per-draw encode hot path
(stream bind, IB bind, FVF decode, PSO lookup, upload) can be reduced.

**Method.** Draw-run scanner instrumentation, reading
`commit_chunk_draw_run_*` counters and the `ImportedDrawRunScanStop` reasons.

**Result.** Break classes: `const_upload=659938`, `state_delta=232121`; per-draw
deltas `stream=793059`, `IB=750041`. Draw-run output:
`submits=580`, `records=1580` against ~`913k` draws — i.e. ~99.94% of draws fail
to batch into a run. Result: stream bind, IB bind, FVF decode, PSO lookup, and
upload costs all repeat at draw frequency.

**Verdict.** Model. Confirms the per-draw encode path stays hot because almost
nothing batches. Two distinct break families emerge — constant-upload boundaries
(largest) and state-delta breaks (second) — motivating both the const-upload
boundary semantics and the binding-override draw-run redesign.

**Related.** [[state-churn-encode]] · next: [[state-churn-encode-drawrun.02]] ·
[[state-churn-encode-statedelta.01]] (state-delta bucket split) ·
[[state-churn-encode-churn.01]] (stream/IB churn redesign) ·
[[const-upload]] (const-upload is the larger break class).
