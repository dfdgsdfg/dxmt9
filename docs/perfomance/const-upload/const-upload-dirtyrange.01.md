---
domain: const-upload
workload: 3DMark05 GT1
subcategory: dirtyrange
order: 01
title: Dirty Range Reset Run
date: undated
type: experiment-run
status: accepted
source: specs/perfomance.plan.md#L4559-L4634
---

# Dirty Range Reset Run

**Question / hypothesis.** The VS cbuf upload width is dominated by a stale dirty
high-water (~`205` regs) far above actual shader use (~`31`)
([[const-upload-range.01]]). Fix `DirtyState` consumption so clearing VS/PS
constant dirty bits also clears the matching range high-water counters, keeping
`maxChangedVsF` scoped to pending dirty work instead of letting one old
high-register write inflate every later VS upload.

**Method.** `DirtyState` semantics change: clear bit + range counter. GT1 run,
VS-range baseline. Output:
`experiments/output/app-d3d9-3dmark05-dirty-range-reset/{dxmt9.log,result.json,dirty-range-reset-summary.md}`.

**Result.** vs VS-range baseline: `argbuf_hybrid_bytes_per_encoder`
`3175361720→1064316728` (`-66.48%`), `transient_upload_bytes`
`4218883220→2111563388` (`-49.95%`), `transient_upload_cpu_ms` `-11.35%`,
`encode_draw_cpu_ms` `-14.54%`. `gpu_command_buffer_time_ms`
`3633.307→3679.573` (same class). Cbuf class: total `-66.90%`; VS
`2359914000→487548784` (`-79.34%`); PS `-49.50%`; FFP-VS/FFP-PS same class.
VS upload avg bytes `3436.546→705.195`; planned float regs avg
`212.028→41.320`; **dirty float regs avg `204.999→0.382`**; shader-used regs
avg unchanged (`30.876`).

**Verdict.** Accepted (CPU/upload win). Stale dirty-range counters were a real
cbuf write amplifier; this removes most VS cbuf traffic and halves total
transient upload bytes. Because `gpu_command_buffer_time_ms` stays in class,
VS/FFP-VS cbuf writes are NOT the primary GT1 GPU limiter. Next targets: stream/
IB churn and render-pass/store, with the `13.8%` indexed/full-struct VS fallback.

**Related.** [[const-upload]] · prev: [[const-upload-range.01]] · next:
[[const-upload-dirtyrange.02]] (the Xcode capture confirming GPU unmoved) ·
[[state-churn-encode]] · [[render-pass-store]] · [[hidden-backend-storage]].
