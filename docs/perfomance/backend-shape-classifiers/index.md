---
domain: backend-shape-classifiers
workload: 3DMark05 GT1
title: "Backend Shape Classifiers — correctness-invalid state toggles that test ownership of the hidden VS-write bucket"
type: domain-index
status: current
updated: 2026-07-08
source: docs/perfomance/overview-3dmark05-gt1.md
related: docs/perfomance/backend-shape-classifiers/overview.md; docs/perfomance/backend-shape-classifiers/log.md
---

# Backend Shape Classifiers — correctness-invalid state toggles that test ownership of the hidden VS-write bucket

Latest tracked row: `H16` - Rifle muzzle fire correctness changes perf interpretation (visual-positive/perf-coupled. The public `01:05` oracle shows several rifle shots as compact barrel-attached round white/yellow bloom discs. Current split-payload artifacts reproduce that shape; same-run geometry promotes `0x80`, and after-draw color history confirms the two-triangle `0x80` sprite as the local writer (`seq=1094`, post-split `enc=3/draw=0/cmd=320`, `bright=706`, `white=196`, `warm=909` in the candidate ROI). `0x7f/0x75` remain broad/non-local for that target. This resolves the visual writer for the wide infantry scene, but not the main FPS owner: skipped/error/hazard/map-wait counters stay zero, while RT/depth/clear/present pass churn and Xcode GPU-counter proof remain open).

## Start Here

- [Current overview](overview.md) - latest conclusion and active gates only.
- [Historical log](log.md) - long-form chronology moved from the old domain root.
- [Root 3DMark05 GT1 map](../overview-3dmark05-gt1.md)

## Recent Leaf Documents

- [backend-shape-classifiers-alpha.04 - Rifle Muzzle Bloom Correctness Gate for Alpha/Effect Rows](backend-shape-classifiers-alpha.04.md)
