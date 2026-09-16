---
domain: mini-replay-bisection
workload: 3DMark05 GT1
title: "Mini-Replay Bisection — the apparatus that isolated per-draw vertex-stage write amplification"
type: domain-index
status: current
updated: 2026-09-16
source: docs/perfomance/overview-3dmark05-gt1.md
related: docs/perfomance/mini-replay-bisection/overview.md; docs/perfomance/mini-replay-bisection/log.md
---

# Mini-Replay Bisection — the apparatus that isolated per-draw vertex-stage write amplification

Latest tracked row: `H22` - Positive Metal visibility samples can be used as the scoped depth-read final-color oracle (rejected; rank2 has `39,835` samples but `0` final-color pixels, and rank1/rank3 are both sample-positive but fail/pass diverge).

## Start Here

- [Current overview](overview.md) - latest conclusion and active gates only.
- [Historical log](log.md) - long-form chronology moved from the old domain root.
- [Root 3DMark05 GT1 map](../overview-3dmark05-gt1.md)

## Recent Leaf Documents

- [Snapshot-local dedup: fitting flat states dominate the recovered CPU cost](snapshot-local-dedup.13.md)
- [Direct fixed-uniform projection reuse: equivalent values, small local cost reduction](fixed-uniform-projection.12.md)
- [GT2 replay owners: snapshot and Slot materialization ahead of planning](replay-cost-owners.11.md)
- [GT2 CPU phases: replay-local Direct benefit, unstable 1MiB increment](cpu-phase-attribution.10.md)
- [GT2 corrected policy matrix: small Direct benefit, no 5MiB promotion](gt2-qualified-policy-matrix.09.md)
- [CPU attachment authority: corrected Clear work and qualified A/A](attachment-authority.08.md)
- [CPU slow modes: software attachment Clear and diagnostic-only phase accounting](cpu-shadow-slowmode.07.md)
- [GT2 route/capacity matrix: exact pixels, unresolved CPU timing variation](gt2-policy-matrix.06.md)
- [Qualified capacity: denial recovery and compatibility growth verified](capacity-ownership-recovery.05.md)
- [Vertex-position invariance: retained GT2 pixels and public CPU/GPU policy transaction qualified](gpu-position-invariance.04.md)
- [Strict GPU policy qualification and source-boundary fidelity/residual](gpu-policy-qualification.03.md)
- [Qualified low-observer CPU policy comparison](cpu-policy-measurement.02.md)
- [Fixed-input replay policy experiment — bounded CPU/Metal evidence and GT2 capture residual](fixed-input-replay.01.md)
- [mini-replay-bisection-vertexremap.01 - Vertex Remap Is A Null On Row 60/1, And Its Positive Control Failed](mini-replay-bisection-vertexremap.01.md)
- [mini-replay-bisection-texture.09 - Metal Visibility Scout Wiring](mini-replay-bisection-texture.09.md)
- [mini-replay-bisection-texture.08 - Occlusion Oracle Feasibility Gate](mini-replay-bisection-texture.08.md)
