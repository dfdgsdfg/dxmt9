---
domain: index-cache-locality
workload: 3DMark05 GT1
subcategory: triage
order: 01
title: 50/2 Remaining Bottleneck Triage
date: 2026-06-04
type: measurement
status: inconclusive
outdated: retired-journal
source: specs/perfomance.plan.md#L20100-L20257
---

# 50/2 Remaining Bottleneck Triage

> **Outdated — this leaf's only `source:` is the retired `specs/perfomance.plan.md` journal, which was deleted.** The numbers below cannot be re-derived or re-checked. Kept as history; do not cite it as current evidence.

**Question / hypothesis.** After the opaque-depth win, row `50/2` is the unresolved
full-frame owner. What splits it, and is texture/fragment material dependence the
first-order cost? (If not, the owner stays hidden vertex-stage storage.)

**Method.** The r4 no-mutate indexed-probe CSV breaks `50/2` into material/state
families. A scoped texture-source A/B probe was run:
`DXMT9_PROBE_FORCE_TEXTURE_WHITE=1` (`_ROW=50/2`,
`_CLASSES=depth-read,screen-blend,textured`), wrapper
`run_3dmark05_perf_probe.sh --suffix row50-2-screenblend-texturewhite-gputrace-r1
--frame 50 --probe-force-texture-white-row 50/2 --probe-force-texture-white-classes
depth-read,screen-blend,textured --target-row-key 50/2`, baseline `cache-opt-candidate-frame50-r4`.

**Result.** `50/2` = `103` screen-blend + `42` blend-off + `42` standard-alpha draws;
shared stable state (depth less-equal/write-off, back-cull, stride `24`, no
alpha-test/stencil/clip, color-write `0xf`, all textured). Texture-white applied to
`probe_force_texture_white_draws=103`: total GPU `35.900→36.435ms` (`+1.49%`); target
`50/2` GPU `+0.74%`; `50/2` VS write `981.190→963.763MiB` (`-1.78%`); VS invocations
`-1.32%`; named tiled buffers `-18.93%`. Residual hidden backend `1,584.643MiB`.
Non-target `50/1` regressed while `50/0` improved.

**Verdict.** Inconclusive — texture sampling / fragment material is NOT the first-order
owner for `50/2`; demote texture-source to a secondary contributor. Rejected axes:
`sort-min-index` (worse LRU/invocations), min-gain-0 (no win), broad depth/cull/scissor
probes, broad force-texture-white. Owner remains OPEN: hidden vertex/tiler/backend
storage. Next high-signal work = same-row primitive/backend-shape A/B, or a small
real-input semantic-tolerance replay gate for `50/2`.

**Related.** [index-cache-locality](index.md) · [index-cache-locality-screenblend.03](index-cache-locality-screenblend.03.md)
(50/2 mechanism) · [index-cache-locality-screenblend.04](index-cache-locality-screenblend.04.md) (explicit exact/`lsb1`
policy) · [backend-shape-classifiers](../backend-shape-classifiers/index.md) (texture/state axes) ·
[primitive-reorder-diagnostics](../primitive-reorder-diagnostics/index.md) (sort-min-index reject) · [hidden-backend-storage](../hidden-backend-storage/index.md)
(the OPEN owner) · [mini-replay-bisection](../mini-replay-bisection/index.md) (real-input replay path).
