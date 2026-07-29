---
domain: const-upload
workload: 3DMark05 GT1
subcategory: volatility
order: 01
title: Cbuf Field Volatility Run
date: undated
type: measurement
status: model
outdated: retired-journal
source: specs/perfomance.plan.md#L4314-L4397
---

# Cbuf Field Volatility Run

> **Outdated — this leaf's only `source:` is the retired `specs/perfomance.plan.md` journal, which was deleted.** The numbers below cannot be re-derived or re-checked. Kept as history; do not cite it as current evidence.

**Question / hypothesis.** Of the vertex-side cbuf rewrite bytes, how many
actually change between consecutive uploads in the same render encoder? Split
each VS/FFP-VS upload into first-use vs rewrite, then rewrite into changed vs
unchanged payload to see whether the full-struct upload is amplification.

**Method.** `DXMT9_PERF_ENCODER_BREAKDOWN=1` extended to compare each VS/FFP-VS
upload against the previous upload in the same encoder. Output:
`experiments/output/app-d3d9-3dmark05-cbuf-field-volatility/{dxmt9.log,result.json,cbuf-field-volatility-summary.md}`.
NOTE: `encode_draw_cpu_ms=24342.145` is NOT a valid CPU baseline — the byte
comparison runs on the hot path. GPU/run-shape counters held class
(`draw_calls=913734`, `argbuf_hybrid_bytes_per_encoder=4584324456`,
`gpu_command_buffer_time_ms=3634.590`).

**Result.** VS uploads: first `2.723%`, rewrite-changed `7.166%`
(`164493907` B), rewrite-unchanged `92.834%` (`2130831229` B). FFP-VS uploads:
first `2.171%`, rewrite-changed `65` bytes (~`0.000%`), rewrite-unchanged
~`100%` (`1423999695` B). All observed VS changed bytes are float4 constants
(`164493907` B); VS int4/bool changed `0`; FFP-VS matrix/blend changed `13`/`52`
bytes (negligible).

**Verdict.** Model. `FfpVsConsts` is effectively stable inside an encoder, so
rewriting it per dirty update is pure write amplification. `VsConsts` is volatile
only in float constants, and only ~`7.17%` of repeated bytes differ. Motivates
two candidates: cache/repoint a stable FFP-VS slice, and split VS float
constants by dirty range.

**Related.** [const-upload](index.md) · prev: [const-upload-class.01](const-upload-class.01.md) · next:
[const-upload-slice.01](const-upload-slice.01.md) (FFP-VS slice reuse) and [const-upload-range.01](const-upload-range.01.md)
(VS float range) · [hidden-backend-storage](../hidden-backend-storage/index.md).
