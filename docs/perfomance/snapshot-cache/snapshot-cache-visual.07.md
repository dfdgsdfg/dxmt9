---
domain: snapshot-cache
workload: 3DMark05 GT1
subcategory: visual
order: 7
title: Cross-Lane Reason-Mask Poisoning Redrew Props With The Skinning Layout
date: 2026-07-10
type: no-gputrace
status: accepted-correctness-fix
source: experiments/output/app-d3d9-3dmark05-artifact-scan-r1-20260710; experiments/output/app-d3d9-3dmark05-cbufdump-batchon-r5-20260710; experiments/output/app-d3d9-3dmark05-cbufdump-batchoff-wide-r1-20260710; experiments/output/app-d3d9-3dmark05-stale-hunt-r4-20260710; experiments/output/app-d3d9-3dmark05-artifact-fixed-r2-20260710
related: docs/perfomance/snapshot-cache/index.md; docs/perfomance/snapshot-cache/snapshot-cache-visual.06.md
---

# Snapshot-Cache Visual 07 - GT1 t=40s giant-triangle fix

## Symptom

Long-standing (pre-dating this year's perf work): around demo `t=0:39-0:43`,
giant dark triangles sweep the floor and spike off the running/diving
soldiers. Frame-scan BMP captures put the artifact at frames `~892-967`.

## Isolation chain

1. Knob A/B: only `DXMT9_DISABLE_DRAW_SUBMIT_BATCH=1` removes it (state-copy
   elision, payload dedup, and strict stamp-only batching do not).
2. Per-draw geometry+cbuf dumps (frame 917, shadow encoder): 12 small prop
   meshes drawn with the soldiers' skinning VS (`0x18ffaf75`) + UBYTE4
   blend-indices declaration; their `@12` normal bytes decode as bone
   indices `0..255` and a Python replay of the dumped inputs reproduces
   NDC `21-66` triangle explosions. A wide batch-off dump shows the same
   meshes' true state: rigid VS `0xcf2198` + FLOAT3-normal declaration.
3. Assert-enabled provider run: submission-level batching asserts all hold —
   the corruption had to be upstream of them.
4. A normalization-aware stale-cache probe in the batch-lane hit path fired:
   `cache.generation == live generation` while `cacheVs=skinning,
   liveVs=rigid, decl stale` — the cache itself was poisoned, which every
   submission-level assert is blind to (stale submissions are
   self-consistent).

## Root cause

`Device::cachedBaseDrawStateForSubmissionBatch` decided `reuseShaderLayout`
from the shared `drawStateInvalidationReasonMask_` accumulator, but BOTH
cache lanes clear that mask at their own miss-rebuild tails. Poisoning
sequence: a draw packet switches VS/vdecl (mask gains Shader|FvfVdecl,
stable generation bumps) → the FULL lane rebuilds (imported draw-run replay)
and clears the mask → a binding-only invalidation (SetStreamSource)
re-populates the mask with non-layout bits without bumping the generation →
the BATCH lane misses, reads a "layout-unaffected" mask, reuses the previous
(skinning) shaderLayout, builds `hot` from it, and stamps the fresh
generation — every subsequent same-generation prop draw snapshots, batches,
and renders with the wrong shaders.

## Fix (`a123166d`)

Layout reuse now keys off a dedicated `drawShaderLayoutGeneration_`
(bumped by `invalidateDrawStateCache` whenever the reason can affect the
shader layout; stored per cache lane), immune to cross-lane mask
consumption. Binding-only reasons never intersect the layout mask, so no
extra rebuilds on the hot path.

## Verification

- Stale-cache probe (now two permanent normalization-aware `DXMT_ASSERT`s in
  the batch-lane hit path): silent across a full assert-provider GT1 run.
- Release-build visual recapture at the artifact window: frames `872/912`
  at demo `t=0:40.9/0:42.7` clean (previously wedge/spike frames).
- Regression pin `testBatchLaneShaderLayoutSurvivesInterleavedFullLaneRebuild`
  (core_device_coverage_spec) reproduces the exact poisoning sequence and
  fails against the old mask-based reuse condition.
- Full native suite `608 OK / 1` pre-existing unrelated failure.

## Method notes

The dump-driven Python skinning replay (bone-index range + NDC explosion
check) turned a visual bug into a machine-checkable predicate, and the
"probe the cache-hit invariant, not the consumers" step was what finally
localized a bug that every consumer-level assert passed.
