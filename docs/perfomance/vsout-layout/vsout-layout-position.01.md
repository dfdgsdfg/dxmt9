---
domain: vsout-layout
workload: 3DMark05 GT1
subcategory: position
order: 01
title: Runtime Position-Only VSOut Probe
date: undated
type: experiment-run
status: rejected
source: specs/perfomance.plan.md#L7756-L7876
---

# Runtime Position-Only VSOut Probe

**Question / hypothesis.** As an extreme lower bound, force `VSOut` to position-only
(`16B`) and a constant fragment result. If visible VSOut width owns the VS-write
bucket, an `184B -> 16B` collapse should drop it ~92%.

**Method.** `DXMT9_PROBE_POSITION_ONLY_VSOUT=1` (intentionally
**correctness-invalid** — emits position only and a constant fragment color; a
bandwidth classifier, not an optimization candidate). Runtime gputrace/Xcode A/B
against `current-normal-gputrace-r1` at frame60. Capture was `partial-log` but all
Xcode/dxmt join, top-PSO, and shader-dump gates passed.

**Result.**

| Metric | Normal | Position-only | Delta |
|---|---:|---:|---:|
| Total GPU | `35.456ms` | `33.924ms` | `-4.32%` |
| Top 3 GPU | `34.837ms` | `33.669ms` | `-3.35%` |
| Top 3 VS buffer write | `1627.240MiB` | `1548.218MiB` | `-4.86%` (`-79MiB`) |
| Top 3 dxmt vertex count | `2,146,185` | `2,146,185` | `0.00%` |
| Position-only MSL VSOut | n/a | `16B` | lower bound |
| VS buffer / expected VSOut | `7.9x` | `88.4x` | hidden traffic remains |

Movement is concentrated in `seq=60 enc=2` (`-79.022MiB`: `-45.361MiB` from fewer
VS invocations, `-33.661MiB` from fewer bytes/invocation); `enc=1`/`enc=0` are
unchanged. Secondary counters also moved in enc=2 (`tiled_vertex_buffer_mib`
`12.625 -> 3.000`, `mmu_limiter_pct` `34.43 -> 17.00%`).

**Verdict.** Rejected as a proportional fix. An `88.4x` reduction in visible VSOut
bytes yielded only `-4.86%` VS-write — wildly non-proportional. The
[vsout-layout-position.02](vsout-layout-position.02.md) fragment-only companion later reproduced the same
`-79MiB` enc=2 movement **without** any VSOut change, proving the delta came from
the constant-fragment/raster/backend interaction, not from visible width. Hidden
backend storage remains the owner.

**Related.** [vsout-layout](index.md) · companion control [vsout-layout-position.02](vsout-layout-position.02.md) · escalated from [vsout-layout-pointsize.01](vsout-layout-pointsize.01.md) · confirms [hidden-backend-storage](../hidden-backend-storage/index.md) · [backend-shape-classifiers](../backend-shape-classifiers/index.md).
