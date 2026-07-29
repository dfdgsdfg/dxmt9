---
domain: backend-shape-classifiers
workload: 3DMark05 GT1
title: "Backend Shape Classifiers — correctness-invalid state toggles that test ownership of the hidden VS-write bucket - Current Overview"
type: domain-overview
status: current
updated: 2026-07-08
source: docs/perfomance/backend-shape-classifiers/log.md; docs/perfomance/overview-3dmark05-gt1.md
related: docs/perfomance/backend-shape-classifiers/index.md; docs/perfomance/backend-shape-classifiers/log.md
---

# Backend Shape Classifiers — correctness-invalid state toggles that test ownership of the hidden VS-write bucket - Current Overview

> Current, compact view for this performance domain. Historical detail from the former
> top-level `backend-shape-classifiers.md` overview is preserved in [log](log.md). Domain landing: [index](index.md).

## Scope

This domain owns the family of **correctness-invalid diagnostic probes** that
toggle a single render / raster *state* (alpha blend, depth write, depth compare,
cull, scissor, fog, texture sampling, fragment visibility, indexed expansion,
alpha-test discard) to ask one question: does that state own the dominant Xcode
"VS Buffer Device Memory Bytes Written" bucket (~1.6 GiB across the top-3 GT1
frame60 encoders)? These probes deliberately render the wrong image — they are
classifiers gated on Xcode VS-write / VS-invocation deltas, **never optimizations**.
Almost every state was rejected: it moves GPU timing and sometimes the small
*named* tiled counters, but not the hidden bucket. Two state axes are notable
exceptions: a scoped **alpha-blend**-off on the large4096+alpha class moved the
bucket substantially, and forced **indexed expansion** nearly doubled it
(confirming indexed-submission pressure is real and must be kept).

## Latest Conclusions

| # | Hypothesis | Verdict | Evidence |
|---|---|---|---|
| H12 | Fragment texture sampling owns the bucket | secondary (GPU −3.72%, top-3 VS write −3.24%, enc2-specific) | backend-shape-classifiers-texture.01 *(removed: retired-journal; in git history)* |
| H13 | Hidden writes are coupled to fragment visibility | rejected (VS write +0.042 MiB, GPU +5.13%) | backend-shape-classifiers-visible.01 *(removed: retired-journal; in git history)* |
| H14 | Indexed-submission pressure drives the bucket | confirmed (expand: GPU +87.74%, VS write +98.10%) — keep indexed path | backend-shape-classifiers-expand.01 *(removed: retired-journal; in git history)* |
| H15 | Alpha-test discard owns the bucket / force-frag delta | rejected (GPU +1.72%, VS write +0.00%) | backend-shape-classifiers-alphatest.01 *(removed: retired-journal; in git history)* |
| H16 | Rifle muzzle fire correctness changes perf interpretation | visual-positive/perf-coupled. The public `01:05` oracle shows several rifle shots as compact barrel-attached round white/yellow bloom discs. Current split-payload artifacts reproduce that shape; same-run geometry promotes `0x80`, and after-draw color history confirms the two-triangle `0x80` sprite as the local writer (`seq=1094`, post-split `enc=3/draw=0/cmd=320`, `bright=706`, `white=196`, `warm=909` in the candidate ROI). `0x7f/0x75` remain broad/non-local for that target. This resolves the visual writer for the wide infantry scene, but not the main FPS owner: skipped/error/hazard/map-wait counters stay zero, while RT/depth/clear/present pass churn and Xcode GPU-counter proof remain open | [backend-shape-classifiers-alpha.04](backend-shape-classifiers-alpha.04.md), baselines-frame60.03 *(removed: evidence-missing; in git history)* |

## Current Navigation

- [Domain index](index.md)
- [Historical log](log.md)
- [Root 3DMark05 GT1 map](../overview-3dmark05-gt1.md)

## Recent Leaf Documents

> 7 of the 8 leaves listed below are marked `outdated:` and open with a banner naming the ground. They are history, not re-checkable evidence.

- [backend-shape-classifiers-alpha.04 - Rifle Muzzle Bloom Correctness Gate for Alpha/Effect Rows](backend-shape-classifiers-alpha.04.md)
