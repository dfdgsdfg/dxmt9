---
domain: state-churn-encode
subcategory: statedelta
order: 01
title: Draw-Run State-Delta Bucket Run
date: undated
type: measurement
status: tooling
source: specs/perfomance.plan.md#L5521-L5601
---

# Draw-Run State-Delta Bucket Run

**Question / hypothesis.** Of the state-delta draw-run breaks, which state group
actually stops the scan — stream-only, IB-only, texture, shader, FVF/vdecl, or
mixed? Split `commit_chunk_draw_run_break_state_delta` into sub-buckets that sum
exactly to the parent.

**Method.**
`DXMT_EXPERIMENT_PROFILE=perf DXMT_3DMARK05_DIRECT=1 DXMT_DISABLE_AUTO_EXPAND_INDEXED=1 python3 scripts/run_apps/run_experiment.py run app-d3d9-3dmark05 --output-suffix state-delta-buckets-fixed --timeout 180`.
Output: `experiments/output/app-d3d9-3dmark05-state-delta-buckets-fixed/`.

**Result.** `commit_chunk_draw_run_break_state_delta=204401` (=100%), split:
`mixed=175086` (`85.66%`), `stream_only=28845` (`14.11%`),
`texture_only=470` (`0.23%`), and `ib_only / shader_only / fvf_vdecl_only /
other_only = 0`. Sub-buckets sum exactly to `204401`. Context:
`draw_run_scans=816974`, `submits=562`, `break_type_const_upload=594288`,
`delta_stream=707453` (`_handle=895054`), `delta_ib=669126` (`_handle=669126`),
`encode_draw_cpu_ms=14913.430`, `render_pass_begin=13031`,
`completion_wait_ms=22694.376`.

**Verdict.** Tooling. Pure IB / shader / FVF-vdecl changes are not standalone
run breakers; most failures are mixed deltas. A stream-only tweak helps only
~14%; the real fix must carry per-draw mixed stream/IB deltas (or pre-scan
record coalescing of const records).

**Related.** [[state-churn-encode]] · next: [[state-churn-encode-statedelta.02]] ·
[[state-churn-encode-drawrun.01]] (the parent break families) ·
[[const-upload]] (const-upload still dominates type breaks).
