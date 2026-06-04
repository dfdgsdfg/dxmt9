---
domain: state-churn-encode
workload: 3DMark05 GT1
subcategory: encoder
order: 01
title: Encoder Breakdown Run
date: undated
type: measurement
status: tooling
source: specs/perfomance.plan.md#L4072-L4131
---

# Encoder Breakdown Run

**Question / hypothesis.** What attribution buckets explain per-render-encoder
CPU/write cost? Add `DXMT9_PERF_ENCODER_BREAKDOWN=1` to emit one
`[dxmt9-perf-encoder]` row per render-encoder close and read the real byte/churn
totals instead of guessing from global counters.

**Method.** GT1 run with `DXMT9_PERF_ENCODER_BREAKDOWN=1`. Output:
`experiments/output/app-d3d9-3dmark05-encoder-breakdown/{dxmt9.log,encoder-breakdown-summary.md}`.

**Result.** `14986` encoder rows. Stream: `1233222` samples, `1086136` Metal
binds, `1009541` handle changes (the dominant churn signal) vs `93383` offset /
`69811` stride changes. IB: `930990` samples / `925201` binds with `758581`
handle changes. Byte buckets: argbuf table `22566304`; argbuf cbuf
`4643320552` (~`4.64GB`, the largest write bucket); `setVertexBytes`
`14895840`; transient vertex `1049812488` (~`1.05GB`, concentrated in specific
passes); transient index `108024` (negligible).

**Verdict.** Tooling. First direct attribution: cbuf writes (~4.64GB) then
transient vertex (~1.05GB) are the largest measured CPU-side writers, and
stream/IB churn is *handle* churn — not offset/stride. Sets the two follow-on
targets: cbuf mirror split and draw-run redesign around stable stream/IB handles.

**Related.** [[state-churn-encode]] · next: [[state-churn-encode-encoder.02]] ·
[[state-churn-encode-stream.01]] (stream split re-run) ·
[[const-upload]] (the 4.64GB cbuf bucket) ·
[[hidden-backend-storage]] (these CPU writers are not the GPU bucket).
