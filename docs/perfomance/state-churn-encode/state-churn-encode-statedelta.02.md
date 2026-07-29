---
domain: state-churn-encode
workload: 3DMark05 GT1
subcategory: statedelta
order: 02
title: Mixed State-Delta Pair Run
date: undated
type: measurement
status: tooling
outdated: retired-journal
source: specs/perfomance.plan.md#L5602-L5728
---

# Mixed State-Delta Pair Run

> **Outdated — this leaf's only `source:` is the retired `specs/perfomance.plan.md` journal, which was deleted.** The numbers below cannot be re-derived or re-checked. Kept as history; do not cite it as current evidence.

**Question / hypothesis.** Inside the dominant *mixed* state-delta bucket, how
many groups change together, and which category pair dominates? Split mixed by
group count and pair participation.

**Method.**
`DXMT_EXPERIMENT_PROFILE=perf DXMT_3DMARK05_DIRECT=1 DXMT_DISABLE_AUTO_EXPAND_INDEXED=1 python3 scripts/run_apps/run_experiment.py run app-d3d9-3dmark05 --output-suffix mixed-delta-pairs --timeout 180`.
Output: `experiments/output/app-d3d9-3dmark05-mixed-delta-pairs/`.

**Result.** `state_delta=182638`; `mixed=156537` split into
`group2=151231` (`96.61%` of mixed), `group3=3081`, `group4plus=2225` (sum =
mixed). `mixed_with_stream=156414`, `mixed_with_ib=152600`,
`mixed_with_texture=5429`, `mixed_with_shader=2119`, `mixed_with_fvf_vdecl=1132`.
The dominant pair `mixed_pair_stream_ib=152600` (`82.17%` of all state-delta).
Context: `submits=474`, `break_type_const_upload=529689`,
`delta_stream=630771`, `delta_ib=596664`, `encode_draw_cpu_ms=14717.562`.
The plan also notes the backend reason it is not a scanner-only change:
`DrawRunCommandRecord` owns one `stateIndex`/`uniformHandle` and `DrawParam`
carries no per-draw stream/IB handles.

**Verdict.** Tooling. Nearly all mixed breaks are 2-group stream+IB. The first
batching fix worth designing is a draw-run payload that carries per-draw stream
and IB bindings; an offset-only rule is not aligned with the shape.

**Related.** [state-churn-encode](index.md) · prev: [state-churn-encode-statedelta.01](state-churn-encode-statedelta.01.md) ·
next: [state-churn-encode-statedelta.03](state-churn-encode-statedelta.03.md) ·
[state-churn-encode-binding.01](state-churn-encode-binding.01.md) (the override fix this motivated).
