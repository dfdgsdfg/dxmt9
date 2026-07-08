---
domain: index-cache-locality
workload: 3DMark05 GT1
subcategory: screenblend
order: 01
title: Screen-Blend Index-Order Optimization Validation
date: 2026-06-02
type: validation
status: inconclusive
source: specs/perfomance.plan.md#L11517-L11650
---

# Screen-Blend Index-Order Optimization Validation

**Question / hypothesis.** Can the earlier diagnostic `large4096 && alpha-blend &&
scissor` reverse-order signal be promoted to an env-gated optimization that rewrites
indexed triangle order for true screen-blend draws, and does it reproduce the
diagnostic VS-write win?

**Method.** `DXMT9_OPTIMIZE_SCREEN_BLEND_INDEX_ORDER=1`
(`_ROW=60/4`, `_CLASSES=large4096,alpha-blend,scissor`); wrapper
`--optimize-screen-blend-index-order --optimize-screen-blend-index-order-row 60/4
--optimize-screen-blend-index-order-classes large4096,alpha-blend,scissor`, frame 60,
baseline `measure-index-cache-gputrace-r1`, full shape/coverage gates. Predicate:
alpha blend on, `SRC=InvDestColor`, `DEST=One`, `OP=Add`, separate-alpha off,
depth test on, depth write off, alpha-test/stencil/clip-plane off; transient
reordered IB, no vertex expansion.

**Result.** Exact scope: `4` optimized draws, `127,656B` reordered index data,
`0` probe-order bytes. Total GPU `34.391→33.238ms` (`-3.35%`); hot GPU
`33.741→32.637ms` (`-3.27%`). But hot VS buffer write `1472.747→1472.827MiB`
(`+0.01%`) — the first-order bucket did not move. Per row `60/4` `9.031→8.528ms`
(`-5.56%`) GPU with VS write `+0.05%`. Earlier diagnostic `-7.46%` VS-write win
was NOT reproduced.

**Verdict.** Inconclusive / secondary. Correctly-scoped, measurable `~3.3%`
GPU-time win, but the old VS-write reduction was a diagnostic-context artifact, not
"reverse these four draws." Hidden Apple vertex/tiler/backend storage (`~1.47GiB`
hot VS write) remains the dominant owner.

**Related.** [index-cache-locality](../index-cache-locality.md) · next: [index-cache-locality-screenblend.02](index-cache-locality-screenblend.02.md)
(later cache-based screen-blend approach) · [primitive-reorder-diagnostics](../primitive-reorder-diagnostics.md)
(the reverse-order origin) · [hidden-backend-storage](../hidden-backend-storage.md) · [baselines](../baselines.md) (measure-index-cache).
