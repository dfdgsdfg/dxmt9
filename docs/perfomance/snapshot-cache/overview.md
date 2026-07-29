---
domain: snapshot-cache
workload: 3DMark05 GT1
title: "Snapshot Cache — D3D9 frontend draw-state snapshot/rebuild CPU bottleneck - Current Overview"
type: domain-overview
status: current
updated: 2026-07-08
source: docs/perfomance/snapshot-cache/log.md; docs/perfomance/overview-3dmark05-gt1.md
related: docs/perfomance/snapshot-cache/index.md; docs/perfomance/snapshot-cache/log.md
---

# Snapshot Cache — D3D9 frontend draw-state snapshot/rebuild CPU bottleneck - Current Overview

> Current, compact view for this performance domain. Historical detail from the former
> top-level `snapshot-cache.md` overview is preserved in [log](log.md). Domain landing: [index](index.md).

## Scope

This domain owns the **D3D9 importer-side draw-state snapshot/rebuild** cost.
It started as the single largest CPU consumer in GT1 (~21s per no-gputrace run),
but after the accepted snapshot hash work it is no longer the top current CPU
bucket: snapshot-cache-snapshot.09
(`outdated: evidence-missing`) reports
`d3d9_snapshot_draw_submission_cpu_ms=7196.881` over `1740` presents, while
backend `encode_draw_cpu_ms` is `17711.215`. Those are last measurements from a
run whose artifacts are gone; the ordering claim they support is still the
domain's position, but the two numbers cannot be re-checked.
It covers the `CachedBaseDrawState` instrumentation, the hot-state/uniform
invalidation split, the miss-reason classification (which found stream/IB handle
churn dominates), the binding-agnostic snapshot that tripled hit rate but exposed a
PSO-prefetch/texture mismatch, and the layout-stride fix that made PSO prefetch
functional again. It is a **CPU track**, distinct from the GPU "hidden VS buffer
write" owner ([hidden-backend-storage](../hidden-backend-storage/index.md)).

## Latest Conclusions

| # | Hypothesis | Verdict | Evidence |
|---|---|---|---|
| H34 | A latest black-geometry / transparent-weapon report proves a new hard performance wall | rejected as a wall; accepted as current visual gate. Prefix-native tests pass, H169 rejects full-cbuf as the owner for the sampled black-foreground window, and H172 shows that same broad dark-foreground class also exists in `v0.0.3`. A separate weapon/lighting artifact still needs same-frame or draw-local proof before it redirects the performance plan | [snapshot-cache-visual.02](snapshot-cache-visual.02.md) |
| H35 | The current `f880..960` object-window sample reproduces the close-up weapon/lighting artifact | rejected for this window; current HEAD renders coherent rifle geometry, sparks, bloom, and muzzle flashes with clean no-skip/no-error counters. The close-up artifact remains a separate target requiring its own capture range before demoting perf evidence | [snapshot-cache-visual.03](snapshot-cache-visual.03.md) |
| H36 | A wider current `100..1000:100` internal capture reproduces the red-light / weapon artifact | rejected for this window; current HEAD renders coherent red corridor, wide firefight, `f900` object, and `f1000` close-up frames with `draw_skipped_no_pipeline=0`, `gpu_command_buffer_errors=0`, and `sampled_avg_fps=16.457`. The run remains P4/no-enqueue shaped, so performance work should continue under the `v0.0.3` visual gate | [snapshot-cache-visual.04](snapshot-cache-visual.04.md) |
| H37 | A denser current `1..291:10` red-corridor capture reproduces the reported close-up transparent weapon / black-vertex artifact | target-window miss; the run captures red-corridor and wide-transition frames with coherent dark foreground geometry, `draw_skipped_no_pipeline=0`, `gpu_command_buffer_errors=0`, and `sampled_avg_fps=16.100`, but it does not match the reported close-up camera window. Treat this as continued need for same-window capture, not a wall or closure | [snapshot-cache-visual.05](snapshot-cache-visual.05.md) |
| H38 | Same-generation draw-submission state-copy elision directly causes the latest transparent-weapon / black-vertex report | rejected for the sampled effects-heavy window; `DXMT9_DISABLE_DRAW_SUBMISSION_STATE_ELISION=1` forces `d3d9_snapshot_state_elided=0`, while the default path elides `411,532` states / `4.211GiB`, and both screenshots render coherent bloom, sparks, geometry, and lighting. Keep the knob as an exact-window diagnostic, but do not demote P4 work based on state elision alone | [snapshot-cache-visual.06](snapshot-cache-visual.06.md) |

## Current Navigation

- [Domain index](index.md)
- [Historical log](log.md)
- [Root 3DMark05 GT1 map](../overview-3dmark05-gt1.md)

## Recent Leaf Documents

> 2 of the 8 leaves listed below are marked `outdated:` and open with a banner naming the ground. They are history, not re-checkable evidence.

- [snapshot-cache-snapshot.29 - Batch Miss Semantic Reuse Probe Rejects Small Recent-Key Cache](snapshot-cache-snapshot.29.md)
