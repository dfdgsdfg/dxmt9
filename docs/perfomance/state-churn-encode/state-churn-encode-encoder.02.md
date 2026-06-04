---
domain: state-churn-encode
workload: 3DMark05 GT1
subcategory: encoder
order: 02
title: Encoder Binding/Bytes Breakdown Run
date: undated
type: measurement
status: tooling
source: specs/perfomance.plan.md#L5730-L5805
---

# Encoder Binding/Bytes Breakdown Run

**Question / hypothesis.** Extend the per-encoder breakdown to split stream/IB
binding churn, argbuf table/cbuf, `setVertexBytes`, and *geometry* transient
vertex/index bytes per render encoder, so each hot encoder's CPU/write payload
is separable.

**Method.** GT1 with
`DXMT_EXPERIMENT_PROFILE=perf DXMT_3DMARK05_DIRECT=1 DXMT_DISABLE_AUTO_EXPAND_INDEXED=1 DXMT9_PERF_ENCODER_BREAKDOWN=1`.
Output: `experiments/output/app-d3d9-3dmark05-encoder-binding-bytes/` plus
`encoder-binding-bytes-{summary.md,top.csv,stream-top.csv}`. Manually stopped
after a 1200-present sample (`result.json` status=fail / rc 143).

**Result.** `14078` encoder lines, `16971` stream-detail lines, `draw_calls=882147`.
Stream Metal binds `1019484` with `959520` handle changes (dominant) vs `87370`
offset / `67009` stride. IB binds `882147` (≈per-draw) with `719771` handle
changes. Bytes: argbuf table `21458624`; argbuf cbuf `1007812488` (~1.0GB);
VS+FFP-VS cbuf `490925320` (~48.7% of cbuf); `setVertexBytes` `14114352`;
**transient vertex `0`** (no UP/expanded geometry transient with auto-expand off);
transient index `102816`.

**Verdict.** Tooling. Reinforces the handle-churn finding: with auto-expand off,
geometry transient vanishes, but the same hot encoders still carry large cbuf
payloads and per-draw stream/IB handle churn. Confirms transient_vertex is not
the primary target; the per-draw stream+IB binding payload is.

**Related.** [[state-churn-encode]] · prev: [[state-churn-encode-encoder.01]] ·
[[state-churn-encode-expand.01]] (auto-expand off removes geometry transient) ·
[[state-churn-encode-statedelta.03]] (stream+IB pair as the payload target) ·
[[const-upload]] (cbuf bucket).
