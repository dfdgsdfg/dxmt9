---
domain: index-cache-locality
workload: 3DMark05 GT1
subcategory: coverage
order: 21
title: Extended Scope And Strict Compatible Merge Have No GT1 Coverage
date: 2026-07-21
type: no-gputrace
status: rejected-no-coverage
source: experiments/output/app-d3d9-3dmark05-vs-inv-baseline-gt2-r1-20260721/result.json; experiments/output/app-d3d9-3dmark05-vs-inv-extended-gt2-r1-20260721/result.json; experiments/output/app-d3d9-3dmark05-vs-inv-merge-gt2-r1-20260721/result.json; experiments/output/app-d3d9-3dmark05-vs-inv-both-gt2-r1-20260721/result.json
related: docs/perfomance/index-cache-locality/index.md; docs/perfomance/index-cache-locality/index-cache-locality-offload-promotion-proof.20.md; docs/perfomance/overview-3dmark05-gt1.md
---

# Index-Cache Locality 21 - Extended scope and strict compatible merge

## Question

Can either of these conservative opt-ins reduce GT1 vertex work beyond the
promoted opaque-depth index-cache path?

- `DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE_EXTENDED_SCOPE=1` additionally
  admits greater/greater-equal depth tests and source-replacement `ONE/ZERO`
  additive blending while preserving the alpha-test, stencil, clip-plane,
  depth-write, and fill-mode safety gates.
- `DXMT9_OPTIMIZE_COMPATIBLE_INDEXED_DRAW_MERGE=1` merges only adjacent
  indexed triangle lists with identical uniforms and serialized binding state,
  one instance, no UP data, and a contiguous source-index range.

## Run identity

The four output directory names and result-file name say `gt2`, but they are
**GT1 artifacts**. Every `dxmt9.log` records
`default_selection=GT1-only` and the actual command line
`-gt1 -nosplash -nosysteminfo -noscreens dxmt9_gt2.3dr`; the `.3dr` filename
did not override the explicit `-gt1` selection. The captures also show the GT1
robot/firefight sequence. Treat the suffix as a mislabeled experiment name.

One 120-second no-gputrace run was collected for each flag combination. All
four completed with `status=pass`, no timeout, no reported GPU errors, and
visually coherent GT1 captures. The captures are at different phases and are
therefore a sanity gate, not pixel-equivalence evidence.

| Lane | extended scope | compatible merge |
|---|---:|---:|
| baseline | `0` | `0` |
| extended | `1` | `0` |
| merge | `0` | `1` |
| both | `1` | `1` |

## Result

| Lane | sampled FPS | delta vs baseline | frames | wall time | presents |
|---|---:|---:|---:|---:|---:|
| baseline | `20.746` | — | `2,269` | `109.368s` | `2,270` |
| extended | `20.990` | `+1.18%` | `2,293` | `109.244s` | `2,294` |
| merge | `21.056` | `+1.49%` | `2,298` | `109.140s` | `2,299` |
| both | `20.945` | `+0.96%` | `2,286` | `109.145s` | `2,287` |

The apparent FPS uplift is not attributable to either opt-in because neither
changed the mechanism it was intended to move:

| Coverage metric | baseline | extended | merge | both |
|---|---:|---:|---:|---:|
| unique index-cache candidates | `125` | `125` | `125` | `125` |
| opaque-depth candidates | `125` | `125` | `125` | `125` |
| cache misses / buffers created | `143 / 67` | `143 / 67` | `143 / 67` | `143 / 67` |
| created bytes | `1,386,168` | `1,386,168` | `1,386,168` | `1,386,168` |
| strict-merge draws eliminated | `0` | `0` | `0` | `0` |

The merge accounting is exact in every lane:

```text
draw_calls == submit_draw_run_batch_append_params + draw_geometry_up
```

For the merge lane this is
`1,707,993 == 1,691,907 + 16,086`; for the both lane it is
`1,699,558 == 1,683,556 + 16,002`. Thus every non-UP source draw still emitted
one append parameter and no adjacent pair passed the strict merge predicate.
The per-run lookup/hit totals differ only with the small present/progress
difference; the invariant miss/create/candidate population proves that the
extended gate admitted no new GT1 geometry.

The baseline's `20.746` FPS is also below the current three-run GT1 reference
range (`20.919-21.189`), while all three candidate lanes fall within that
range. Combined with zero mechanism coverage and the non-monotonic both-lane
result, this is run-order/warm-up variation rather than a promotion signal.

## Verdict

**Rejected for current GT1:** keep both flags default OFF. No Xcode/Metal GPU
trace spend is justified because neither opt-in changes submitted work in this
workload.

The extended-scope path may remain as a conservative experiment for GT3; a
correctly selected GT2 run also found zero coverage (H28). Before making the
merge predicate more permissive,
instrument adjacent-pair rejection reasons (uniform mismatch, serialized
binding mismatch, non-contiguous index range, or another gate). Do not build a
joined-index-buffer cache without evidence that one of those rejected classes
has useful volume and safe semantics.
