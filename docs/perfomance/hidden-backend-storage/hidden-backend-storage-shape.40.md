---
domain: hidden-backend-storage
workload: 3DMark05 GT1, GT2, GT3, and SFIV
subcategory: shape
order: 40
title: Alias-Aware Pass Coalescing Clears the Default-Promotion Wild Gate
date: 2026-07-25
type: correctness-and-promotion-decision
status: accepted-default-promotion
source: experiments/output/app-d3d9-3dmark05-gt1-all-production-opts-r1-20260724; experiments/output/app-d3d9-3dmark05-gt2-all-production-opts-r1-20260724; experiments/output/app-d3d9-3dmark05-gt3-all-production-opts-r1-20260724; experiments/output/app-d3d9-3dmark05-gt3-all-production-opts-exact-window-r2-20260725; experiments/output/app-d3d9-sfiv-benchmark-all-production-opts-r{1,2}-20260724; experiments/output/app-d3d9-sfiv-benchmark-all-production-opts-r3-20260725; experiments/output/app-d3d9-sfiv-benchmark-current-default-startup-control-r1-20260725; experiments/output/app-d3d9-sfiv-benchmark-default-passcoalesce-r1-20260725; experiments/output/app-d3d9-sfiv-benchmark-default-passcoalesce-perf-r1-20260725; experiments/output/conf-d3d9-triangle-default-passcoalesce-smoke-20260725
related: docs/perfomance/overview-3dmark05-gt2.md; docs/perfomance/hidden-backend-storage/hidden-backend-storage-shape.39.md; specs/d3d9-renderer/gap.md
---

# Alias-Aware Pass Coalescing Clears the Default-Promotion Wild Gate

## Question

After surface/texture alias normalization restored the missing RAW/WAR/WAW
edges, does the production passcoalesce lane remain safe enough to become the
default renderer policy?

## Evidence

The post-fix GT1, GT2, and GT3 runs all completed normally with no observed GPU
or pipeline failure. Their passcoalesced workloads reduced render-pass/present
work relative to the corresponding source-order shape by approximately:

| Workload | Render-pass/present reduction |
|---|---:|
| GT1 | `10.3%` |
| GT2 | `11.2%` |
| GT3 | `8.25%` |

The GT3 exact-window rerun captured the previously sensitive interval at about
`1:05.69`, `1:06.49`, and `1:07.37`. None of those captures reproduced the
filtered-noise quadrant artifact. This is the strongest workload-specific
visual check because it targets the known failure window rather than a generic
completion screenshot.

GT2 retains the corrected `18 -> 16` frame279 topology and the required
`R32F producer -> main consumer -> R32F writer` order. The older `18 -> 15`
result remains invalid historical evidence and is not used for promotion.

An env-clean default SFIV run reached a rendered benchmark scene at the
35-second capture point. Ryu, lighting, logos, and the post-effect path are
visible without a black-start state or an obvious rendering artifact. The run
completed `1,562` successful Presents with no observed Metal, GPU, or pipeline
failure. A separate low-overhead `perf` run encoded `7,320` Presents and
`1,674,130` draws with zero chunk rejects, skipped Presents, GPU command-buffer
errors, pipeline-build failures, or missing-pipeline draws. Its late screenshot
fell back to a desktop capture after title lookup failed, so it contributes
counter/stability evidence only; the earlier window capture is the visual
evidence.

## Scope limits

These were all-production-option runs. They also enabled compatible indexed
draw merge and wider index-cache eligibility, so their cross-build FPS is not a
passcoalesce-only performance A/B. Compatible draw merge selected zero draws in
the measured workloads and remains default-off. The promotion decision uses
the alias/order proof, clean execution, exact GT3 visual window, and reduced
pass volume; it does not claim that every FPS delta belongs to passcoalesce.

The earlier SFIV candidate and traditional-control runs stopped in the same
black pre-initialization state. The later env-clean default run supersedes that
startup-only evidence for the scene-level gate. Its debug-profile overlay is
not a performance baseline because verbose logging materially perturbed the
application; the separate perf run is retained for low-overhead stability and
GPU timing only.

After promotion, an env-clean Sikarugir x86_64 Wine triangle run exercised the
runner's omitted-mode default. The app's self-validation and ABI handshake
passed with no GPU/pipeline error.

## Decision

Promote only `framegraph + progressive + passcoalesce`:

- unset renderer mode selects framegraph;
- unset compatibility profile selects progressive;
- unset feature list enables only passcoalesce;
- explicit `traditional`, `strict`, or an empty/`0` feature list rolls back;
- memoryless, DCE, generic reorder, compatible draw merge, mesh, bindless,
  object scheduling, and GPU-driven execution stay off.

Device-backed pixel parity remains evidence debt in
`specs/d3d9-renderer/gap.md`. The SFIV rendered-scene evidence debt is closed.
Neither result authorizes promotion of any additional framegraph feature.
