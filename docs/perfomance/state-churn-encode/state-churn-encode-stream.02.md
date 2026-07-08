---
domain: state-churn-encode
workload: 3DMark05 GT1
subcategory: stream
order: 02
title: Encoder Delta Breakdown
date: undated
type: measurement
status: tooling
source: specs/perfomance.plan.md#L5049-L5110
---

# Encoder Delta Breakdown

**Question / hypothesis.** Confirm with explicit delta sub-counters that stream
Metal binds are caused by handle changes (not offsets), and that IB delta is
essentially all handle churn.

**Method.** GT1 with
`DXMT_EXPERIMENT_PROFILE=perf DXMT_3DMARK05_DIRECT=1 DXMT9_PERF_ENCODER_BREAKDOWN=1`.
Output: `experiments/output/app-d3d9-3dmark05-encoder-delta-breakdown`.
Validation included `audit_perf_counter_table.py`, `audit_perf_counter_callsites.py`,
and `run_experiment.py ... --timeout 80`.

**Result.** `present_encoded=1260`, `draw_calls=917011`. Run counters:
`commit_chunk_draw_delta_stream=795814`, `_stream_handle=1006329`,
`_stream_offset=101924`, `_stream_stride=72366`; `commit_chunk_draw_delta_ib=752596`
with `_ib_handle=752596` (identical — IB delta is *entirely* handle churn).
`bind_vertex_buffer=1071590`, `bind_index_buffer=911184`,
`transient_upload_bytes=2120990572`, `gpu_command_buffer_time_ms=3646.246`.
Encoder-log aggregation: `stream_metal_binds=1097242` with
`stream_metal_bind_handle_changes=1019904` vs `_offset_changes=94372`;
`ib_metal_binds=933966` with `765945` handle changes; argbuf cbuf `1066185552`.

**Verdict.** Tooling. ~1.02M stream binds from handle changes vs only ~94k from
offset changes; `commit_chunk_draw_delta_ib_handle == commit_chunk_draw_delta_ib`.
The next target is resource-handle alternation, not offset-only caching.

**Related.** [state-churn-encode](../state-churn-encode.md) · prev: [state-churn-encode-stream.01](state-churn-encode-stream.01.md) ·
next: [state-churn-encode-stream.03](state-churn-encode-stream.03.md) ·
[state-churn-encode-statedelta.01](state-churn-encode-statedelta.01.md) (state-delta bucket split).
