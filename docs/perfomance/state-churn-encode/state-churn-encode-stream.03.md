---
domain: state-churn-encode
workload: 3DMark05 GT1
subcategory: stream
order: 03
title: Encoder Unique Handle Breakdown
date: undated
type: measurement
status: tooling
source: specs/perfomance.plan.md#L5111-L5178
---

# Encoder Unique Handle Breakdown

**Question / hypothesis.** Is per-draw stream/IB handle churn caused by per-draw
object *creation* (e.g. lock/rename), or by repeated *alternation* among a
bounded set of reused handles?

**Method.** GT1 with `DXMT9_PERF_ENCODER_BREAKDOWN=1` extended with per-encoder
stream/IB unique-handle count, bytes, dynamic/write-only flags, pool buckets,
and an overflow sentinel. Output:
`experiments/output/app-d3d9-3dmark05-encoder-unique-handle-breakdown`.

**Result.** `draw_calls=916250`, `draw_expanded_indexed=5837`,
`gpu_command_buffer_time_ms=3645.5`. Stream: `stream_unique_handles=466992`
(`stream_unique_bytes=47818268240` ≈47.8GB), overflows `0`, all `466992`
write-only, `458616` managed-pool vs only `8376` default-pool / `8376` dynamic.
IB: `ib_unique_handles=356186` (`ib_unique_bytes=4519097808` ≈4.5GB),
`354295` write-only, `356186` managed-pool. Heavy encoder examples reuse small
bounded sets, e.g. `seq=1015 enc=11`: 549 draws but only `184` stream / `93` IB
unique handles; `seq=1191 enc=11`: 147 draws, `48` stream / `24` IB unique.
`transient_vertex_bytes=1056136680`.

**Verdict.** Tooling. Handle churn is *alternation*, not per-draw creation —
heavy encoders cycle among ~184 stream / ~93 IB handles, overwhelmingly
managed-pool write-only buffers. This weakens the lock/rename hypothesis and
points the fix at run-base/per-draw binding representation. Also flags
auto-expand (`5837` expansions ≈ 1.056GB transient) as a separate amplifier.

**Related.** [state-churn-encode](index.md) · prev: [state-churn-encode-stream.02](state-churn-encode-stream.02.md) ·
[state-churn-encode-expand.01](state-churn-encode-expand.01.md) (disable auto-expand follow-up) ·
[snapshot-cache](../snapshot-cache/index.md) (binding-agnostic snapshot reuse of bounded handle sets).
