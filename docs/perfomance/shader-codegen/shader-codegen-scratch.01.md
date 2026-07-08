---
domain: shader-codegen
workload: 3DMark05 GT1
subcategory: scratch
order: 01
title: VS Output Scratch Array Trim Probe
date: undated
type: experiment-run
status: rejected
source: specs/perfomance.plan.md#L3601-L3660
---

# VS Output Scratch Array Trim Probe

**Question / hypothesis.** Does the conservative `float4 outTexcoord[8]` local
scratch array in translated vertex shaders inflate the hidden Xcode VS
buffer-write bucket? Sizing it from emitted/mapped texcoord usage should reduce
VS write traffic if it does.

**Method.** `DXMT9_TRIM_VS_OUTPUT_SCRATCH=1` sizes local `float4 outTexcoord[]`
from emitted/mapped texcoord output usage instead of the 8-slot default
(relative texcoord output access still promotes to all 8 slots). Flag is in the
shader source debug-env key. Wrapper `--trim-vs-output-scratch`, frame 60, paired
`--dump-shaders` + gputrace. Finalizer gates:
`--require-top-vs-buffer-write-decrease`,
`--require-top-unexplained-buffer-write-decrease`,
`--max-top-unexplained-buffer-write-ratio 0.50`.
Candidate run `app-d3d9-3dmark05-trim-vs-output-scratch-frame60-r1`.

**Result.**
- MSL shape did change: top three VS rows report `VS outT[] = 1` and
  `VS outT over B = 0`; the conservative `float4 outTexcoord[8]` scratch was
  removed.
- Xcode counter still flat: `top_vs_buffer_write_mib` `1627.325` → `1627.280`
  MiB (`-0.00%`); `top_unexplained_buffer_write_ratio` stayed `1.000`.
- Total GPU time `33.545ms` → `33.922ms` (`+1.12%`); no meaningful change in
  draw count, stream/IB churn, or CPU writer bytes.
- Regenerated bottleneck report: top-three aggregate VS buffer traffic is
  `2385.2 B/primitive`, `2382.4 B/post-clipped primitive`, and
  `3580.6 B/primitive-tile estimate` — far closer to hidden primitive/binning
  metadata than to the `36-68 B/vertex` source-visible stage-output payload.

**Verdict.** Rejected. Source-visible `outTexcoord[]` scratch is not the owner;
the per-primitive write shape points squarely at hidden backend (TVB/binning)
storage below AIR, which is the primary surviving hypothesis.

**Related.** [shader-codegen](index.md) · [shader-codegen-temps.01](shader-codegen-temps.01.md) (prior step) ·
[shader-codegen-offline.01](shader-codegen-offline.01.md) (offline IR confirms the scratch is only 128 B) ·
[hidden-backend-storage](../hidden-backend-storage/index.md) · [vsout-layout](../vsout-layout/index.md)
