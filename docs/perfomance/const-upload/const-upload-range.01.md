---
domain: const-upload
workload: 3DMark05 GT1
subcategory: range
order: 01
title: VS Float Range Run
date: undated
type: measurement
status: model
source: specs/perfomance.plan.md#L4474-L4558
---

# VS Float Range Run

**Question / hypothesis.** Now that FFP-VS is cached ([const-upload-slice.01](const-upload-slice.01.md)),
the remaining cbuf bucket is `VsConsts`. Is its size driven by how many float
registers the shader actually *uses*, or by how wide the *dirty* range is? Add
VS upload-plan fields to attribute it.

**Method.** `DXMT9_PERF_ENCODER_BREAKDOWN=1` extended with
`argbuf_cbuf_vs_{uploads,full_struct_uploads,usage_unknown_uploads,usage_indexed_float_uploads,plan_float_regs_sum/max,dirty_float_regs_sum/max,usage_float_regs_sum/max}`.
GT1 run, FFP-VS-cache baseline. Output:
`experiments/output/app-d3d9-3dmark05-vs-range/{dxmt9.log,result.json,vs-range-summary.md}`.
GPU class held (`gpu_command_buffer_time_ms=3633.307`,
`argbuf_hybrid_bytes_per_encoder=3175361720`).

**Result.** VS cbuf bytes `2359914000`, `686711` uploads, avg `3436.546`
B/upload. Full-struct `13.782%`, usage-unknown `1.120%`, indexed-float
`12.662%`. **Avg planned float regs `212.028`, avg dirty `204.999`, avg
shader-used only `30.885`** (max planned/dirty/used `256`/`205`/`206`).
Weighted percentiles: dirty float regs p50/p95/p99 all `205.000`; shader-used
p50 `31.776`, p99 `55.250`.

**Verdict.** Model. The remaining VS cbuf bucket is NOT caused by shaders needing
many constants — the dirty high-water sticks near register `205` while the
average shader uses only ~`31` float regs. A usage-only trim is the wrong fix;
the dirty (stale) range dominates the upload width. Motivates resetting/scoping
dirty ranges; keep the `13.8%` full-struct/indexed path as a separate fallback.

**Related.** [const-upload](../const-upload.md) · prev: [const-upload-slice.01](const-upload-slice.01.md) · next:
[const-upload-dirtyrange.01](const-upload-dirtyrange.01.md) (the dirty-range reset this motivates) ·
[hidden-backend-storage](../hidden-backend-storage.md).
