---
domain: state-churn-encode
workload: 3DMark05 GT1
subcategory: statedelta
order: 03
title: Exact Stream+IB State-Delta Bucket
date: undated
type: measurement
status: tooling
source: specs/perfomance.plan.md#L5806-L5878
---

# Exact Stream+IB State-Delta Bucket

**Question / hypothesis.** The `mixed_pair_stream_ib` counter overstates the
target because group3/4 stops can also include the stream+IB pair. Add an
*exact* counter — `commit_chunk_draw_run_break_state_delta_stream_ib_only`,
counted only for exactly-two-group stops whose groups are stream and IB — to
size the precise draw-run payload target.

**Method.**
`DXMT_EXPERIMENT_PROFILE=perf DXMT_3DMARK05_DIRECT=1 DXMT_DISABLE_AUTO_EXPAND_INDEXED=1 python3 scripts/run_apps/run_experiment.py run app-d3d9-3dmark05 --output-suffix stream-ib-exact --timeout 150`.
Output: `experiments/output/app-d3d9-3dmark05-stream-ib-exact/` (1200-present
sample; manually stopped, status=fail / rc 143).

**Result.** `state_delta=218721` (=100%): `stream_only=30882` (`14.12%`),
`texture_only=566` (`0.26%`), `mixed=187273` (`85.62%`), `mixed_group2=180874`,
`stream_ib_only=179721` (`82.17%` of all state-delta), `mixed_pair_stream_ib=182632`.
Exact stream+IB-only = `95.97%` of mixed, `99.36%` of group2. Stream-only +
exact stream+IB = `96.29%` of state-delta. `break_type_const_upload=630433`
(`2.88x` state-delta). Implementation status: `DrawParam` now carries a serialized
`DrawBindingOverride` range; `scanImportedDrawRun()` accepts stream-only and
stream+IB-only changes as run-compatible.

**Verdict.** Tooling. Names the first payload target precisely: per-draw stream
and IB bindings cover the large majority (`96.29%`) of state-delta breaks. But
const-upload boundaries remain `2.88x` larger — a separate problem — so stream+IB
payload is a CPU/run-coalescing fix, not the whole perf fix.

**Related.** [state-churn-encode](../state-churn-encode.md) · prev: [state-churn-encode-statedelta.02](state-churn-encode-statedelta.02.md) ·
[state-churn-encode-binding.01](state-churn-encode-binding.01.md) (the accepted override CPU win) ·
[const-upload](../const-upload.md) (the larger, separate const-upload break class).
