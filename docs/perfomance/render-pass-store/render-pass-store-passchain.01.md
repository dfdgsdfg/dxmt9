---
domain: render-pass-store
workload: 3DMark05 GT1
subcategory: passchain
order: 01
title: Pass-Chain Split Measurement Run
date: undated
type: measurement
status: inconclusive
source: specs/perfomance.plan.md#L4837-L4905
---

# Pass-Chain Split Measurement Run

**Question / hypothesis.** Is the same-key re-entry budget dominated by one
attachment (so a single-attachment DontCare proof could suffice), and what
transition pattern do adjacent passes actually follow?

**Method.** Split the same-key re-entry preservation budget into color and depth
bytes and classified adjacent render-pass transitions with new counters:
`render_pass_same_key_reentry_color_preservation_bytes`,
`render_pass_same_key_reentry_depth_preservation_bytes`,
`render_pass_transition_rt_change_same_depth`,
`render_pass_transition_same_rt_depth_change`,
`render_pass_transition_rt_depth_change`. Validation run:
`experiments/output/app-d3d9-3dmark05-pass-chain-split/`.

**Result.** Comparable run shape to color next-clear
([render-pass-store-dontcare.02](render-pass-store-dontcare.02.md)): `present_encoded=1260`,
`draw_calls=916159`, `render_pass_begin=14691`,
`render_pass_same_key_reentry=2787`,
`render_pass_same_key_reentry_preservation_bytes=62222499840` (~62.22 GB, 37.1% of
tile preservation), `gpu_command_buffer_time_ms=3626.690`. New split:

| Counter | Value | Reading |
|---|---:|---|
| `..._reentry_color_preservation_bytes` | `31111249920` | color = half (~31.11 GB) |
| `..._reentry_depth_preservation_bytes` | `31111249920` | depth = the other half (~31.11 GB) |
| `render_pass_transition_rt_change_same_depth` | `2559` | ~2.03 per present |
| `render_pass_transition_same_rt_depth_change` | `0` | no same-color/different-depth switching |
| `render_pass_transition_rt_depth_change` | `10873` | **~8.63 per present; most switches change BOTH RT and depth** |

**Verdict.** Inconclusive for a narrow fix; OPEN for the real one. The re-entry
budget is split ~50/50 color/depth, so neither a color-only next-clear proof nor a
depth-only proof is sufficient. Most adjacent pass switches change both RT and
depth (`10873`) while same-RT/depth-change is `0`. The remaining render-pass
target is a dependency-aware pass ordering/coalescing design — determine whether
intervening passes are independent enough to batch same-key work, or prove a
broader live-out discard with concrete read/use evidence — not a single-attachment
store policy.

**Related.** [render-pass-store](index.md) · prior: [render-pass-store-dontcare.02](render-pass-store-dontcare.02.md)
(zero-hit color next-clear) · [render-pass-store-reentry.01](render-pass-store-reentry.01.md) (the ~62 GB budget
this splits) · [hidden-backend-storage](../hidden-backend-storage/index.md) (the P0 owner this P1 track sits
behind).
