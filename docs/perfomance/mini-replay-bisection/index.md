---
domain: mini-replay-bisection
workload: 3DMark05 GT1
title: "Mini-Replay Bisection — the apparatus that isolated per-draw vertex-stage write amplification"
type: domain-index
status: current
updated: 2026-07-08
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

- [mini-replay-bisection-texture.11 - Visibility Positive Semantic Join](mini-replay-bisection-texture.11.md)
- [mini-replay-bisection-texture.10 - Visibility Scout Cache Join](mini-replay-bisection-texture.10.md)
- [mini-replay-bisection-texture.09 - Metal Visibility Scout Wiring](mini-replay-bisection-texture.09.md)
- [mini-replay-bisection-texture.08 - Occlusion Oracle Feasibility Gate](mini-replay-bisection-texture.08.md)
- [mini-replay-bisection-texture.07 - Primitive Conflict Selector Scout Requires Final-Color Oracle](mini-replay-bisection-texture.07.md)
- [mini-replay-bisection-texture.06 - Rank-4 Real-Texture Gate Is Color-Exact Owner-Masked](mini-replay-bisection-texture.06.md)
- [mini-replay-bisection-texture.05 - Rank-3 Real-Texture Gate Is Also Color-Exact Owner-Masked](mini-replay-bisection-texture.05.md)
- [mini-replay-bisection-texture.04 - Rank-2 Real-Texture Gate Is Color-Exact but Owner-Masked](mini-replay-bisection-texture.04.md)
- [mini-replay-bisection-texture.03 - Ranked Real-Texture Semantic Gate Queue](mini-replay-bisection-texture.03.md)
- [mini-replay-bisection-replay.03 - Wider Encoder2 Payload Capture](mini-replay-bisection-replay.03.md)
- [mini-replay-bisection-texture.02 - Real-Texture Replay Rejects Exact 60/2 Cache-Opt Proof](mini-replay-bisection-texture.02.md)
- [mini-replay-bisection-semantic.02 - Scoped 60/2 Depth-Read No-Blend Cache-Opt Replay](mini-replay-bisection-semantic.02.md)
