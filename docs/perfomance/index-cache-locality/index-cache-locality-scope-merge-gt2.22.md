---
domain: index-cache-locality
workload: 3DMark05 GT2
subcategory: coverage
order: 22
title: GT2 Confirms Extended Scope And Strict Merge Are No-Ops
date: 2026-07-21
type: no-gputrace
status: rejected-no-coverage
source: experiments/output/app-d3d9-3dmark05-vs-inv-gt2-baseline-r1-20260721/result.json; experiments/output/app-d3d9-3dmark05-vs-inv-gt2-extended-r1-20260721/result.json; experiments/output/app-d3d9-3dmark05-vs-inv-gt2-merge-r1-20260721/result.json; experiments/output/app-d3d9-3dmark05-vs-inv-gt2-both-r1-20260721/result.json
related: docs/perfomance/index-cache-locality/index.md; docs/perfomance/index-cache-locality/index-cache-locality-scope-merge.21.md; docs/perfomance/overview-3dmark05-gt2.md
---

# Index-Cache Locality 22 - GT2 extended scope and strict merge

## Question

GT1 did not exercise either the conservative extended index-cache scope or
strict adjacent compatible indexed-draw merge. Does the heavier GT2 forest
workload provide useful coverage or a throughput signal?

## Run

Four 120-second no-gputrace runs used the same current build, Sikarugir Wine,
the `perf` profile, frame sampling, and frontmost supervision. Every launcher
log records the actual command line
`-gt2 -nosplash -nosysteminfo -noscreens dxmt9_gt2.3dr`; this corrects the
earlier GT1 selection mistake. The four lanes pin the two experimental flags
to `0/0`, `1/0`, `0/1`, and `1/1`.

All runs completed normally with `status=pass`, return code zero, no timeout,
no failures, and zero GPU command-buffer, skipped-pipeline, pipeline-build, or
V2-reject errors. Their captures are closely phase aligned at time
`58.47-58.73s`, frames `500-502`, and show the same visually coherent glowing
tree/forest scene. This is a strong capture sanity check, though not an exact
same-frame pixel proof.

## Result

| Lane | sampled FPS | delta | frames | wall time | wall p50 / p95 | GPU-CB p50 / p95 |
|---|---:|---:|---:|---:|---:|---:|
| baseline | `8.201` | — | `552` | `67.312s` | `104.706 / 160.076ms` | `22.893 / 28.393ms` |
| extended | `8.185` | `-0.20%` | `550` | `67.194s` | `105.100 / 161.631ms` | `23.111 / 28.492ms` |
| merge | `8.211` | `+0.12%` | `552` | `67.229s` | `104.109 / 161.782ms` | `23.109 / 28.351ms` |
| both | `8.198` | `-0.04%` | `551` | `67.213s` | `105.000 / 160.050ms` | `22.747 / 28.357ms` |

Neither path changes its owning mechanism:

| Coverage metric | baseline | extended | merge | both |
|---|---:|---:|---:|---:|
| unique index-cache candidates | `61` | `61` | `61` | `61` |
| opaque / screen-blend candidates | `61 / 0` | `61 / 0` | `61 / 0` | `61 / 0` |
| cache misses / buffers created | `61 / 37` | `61 / 37` | `61 / 37` | `61 / 37` |
| created bytes | `518,946` | `518,946` | `518,946` | `518,946` |
| strict-merge draws eliminated | `0` | `0` | `0` | `0` |

The strict merge accounting remains exact:

```text
draw_calls == submit_draw_run_batch_append_params + draw_geometry_up
```

For the merge lane this is
`928,368 == 924,504 + 3,864`; for the combined lane it is
`927,032 == 923,175 + 3,857`. Every non-UP source draw therefore still emits
one Metal draw. Extended-scope lookup volume changes only with the one- or
two-present progress difference: the invariant candidate/miss/create
population proves that it admits no new GT2 geometry.

## Verdict

**Rejected for current GT2.** The `-0.20%` to `+0.12%` FPS spread is noise,
and no Xcode/Metal GPU trace spend is justified because submitted geometry is
unchanged. Keep both experiment flags default OFF.

GT1 and GT2 now agree. GT3 is the only remaining existing 3DMark05 workload
that could establish coverage. For merge work, rejection-reason telemetry
should precede any semantic expansion or joined-index-buffer cache.
