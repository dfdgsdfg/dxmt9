---
domain: vsout-layout
workload: 3DMark05 GT1
subcategory: half
order: 01
title: Half VSOut Probe
date: 2026-06-05
type: experiment-run
status: rejected
source: specs/perfomance.plan.md#L22497-L22660
---

# Half VSOut Probe

**Question / hypothesis.** A non-reorder compiler/backend-shape axis: does
requesting *half-precision* user varyings in the shared `VSOut` reduce
Apple/AGX hidden-expanded TVB / parameter-backend storage (which scales with
per-vertex VSOut bytes)? Unlike position-only it preserves primitive order, draw
count, geometry, and render state, so any counter movement is attributable to
stage-out precision, not cache-locality reorder.

**Method.** `DXMT9_PROBE_HALF_VSOUT=1` (wrapper `--probe-half-vsout`): `half4` for
color/secondaryColor/texcoord0..7, `half` for fogFactor; `position`, `pointSize`,
`clipDistance0` stay float. FFP/translated FS cast half stage-in back to float at
the consumer boundary; default-off source shape kept stable. Frame50 Xcode proof
run with `--baseline-joined <current-normal frame50> --require-tvb-mechanism-proof
--require-top-row-key-match --require-shader-dump-matches
--max-top-triangle-delta-ratio 0.05 --timeout 420`. No-gputrace smoke passed
first (88/88 shader dumps used half VSOut, no yellow-frame regression).

**Result.** Finalizer intentionally **failed** `--require-tvb-mechanism-proof`:

| Metric | Current-normal | Half VSOut | Delta |
|---|---:|---:|---:|
| Total GPU | `35.024ms` | `36.161ms` | `+3.24%` |
| Top 3 GPU | `34.390ms` | `35.558ms` | `+3.40%` |
| Top 3 VS buffer write | `1627.372MiB` | `1587.583MiB` | `-2.44%` |
| Top hidden backend write | `1597.615MiB` | `1566.263MiB` | `-1.96%` |
| Top VS B/invocation | `1447.859B` | `1419.823B` | `-1.94%` |
| Top VS invocations | `1,178,584` | `1,172,471` | `-0.52%` |
| Top draw calls / vertices / triangles | unchanged | unchanged | — |

Movement is mostly bytes/invocation in row `50/2` (VS write `-4.05%`, named tiled
`-32.65%`) but row GPU times regress (`50/2 +3.35%`, `50/1 +8.31%`).

**Verdict.** Rejected as the current GT1 frame50 owner. Half VSOut compiles and
runs, and shifts a small slice of the backend bucket (`-2.44%` VS write,
`-1.96%` hidden), but fails the mechanism gate because GPU time regresses
(`+3.40%`). It is not the dominant owner. (Follow-up audit noted `clip_distance`
is already absent from the hot rows, so it is not a remaining width axis.)

**Related.** [vsout-layout](index.md) · last in the visible-width sequence after [vsout-layout-position.02](vsout-layout-position.02.md) · refutes precision-width as owner, confirms [hidden-backend-storage](../hidden-backend-storage/index.md) · gated by [tvb-mechanism-proof](../tvb-mechanism-proof/index.md) · [shader-codegen](../shader-codegen/index.md).
