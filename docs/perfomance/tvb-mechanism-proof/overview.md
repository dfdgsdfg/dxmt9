---
domain: tvb-mechanism-proof
workload: 3DMark05 GT1
title: "TVB Mechanism Proof — why the index-cache win is real - Current Overview"
type: domain-overview
status: current
updated: 2026-07-08
source: docs/perfomance/tvb-mechanism-proof/log.md; docs/perfomance/overview-3dmark05-gt1.md
related: docs/perfomance/tvb-mechanism-proof/index.md; docs/perfomance/tvb-mechanism-proof/log.md
---

# TVB Mechanism Proof — why the index-cache win is real - Current Overview

> Current, compact view for this performance domain. Historical detail from the former
> top-level `tvb-mechanism-proof.md` overview is preserved in [log](log.md). Domain landing: [index](index.md).

## Scope

This domain owns the **load-bearing mechanism behind the only accepted
production win**. The central GT1 finding is that the top render encoders write
a ~1.6 GiB "VS Buffer Device Memory Bytes Written" bucket that is *not* explained
by dxmt CPU-side writers (~0.4 MiB), visible MSL `VSOut` width (184 B), or AIR
scratch — it is hidden Apple GPU vertex-stage / tiler / **Tiled Vertex Buffer
(TVB) / Parameter Buffer (PB)** storage that scales with
`VS invocations × per-vertex VSOut bytes`. This domain proves the corollary: if
that model is right, reducing post-transform VS invocations (via index-cache
LRU32 reorder) must linearly reduce TVB write traffic and GPU time. It does —
both row-local and full-frame.

## Latest Conclusions

> **Every row below cites a leaf now marked `outdated: retired-journal`.** The
> mechanism these rows accept is still the load-bearing model for the accepted
> index-cache win, but the two proof leaves behind it can no longer be
> re-derived. Treat the model as carried forward, not as freshly verifiable.
> The independent corroboration that survives is
> [hidden-backend-storage](../hidden-backend-storage/overview.md) H53, whose row
> `60/1` lanes measure hidden write volume tracking VS invocations directly.

| # | Hypothesis | Verdict | Evidence |
|---|---|---|---|
| H1 | TVB write bytes scale linearly with `VS invocations × per-vertex VSOut bytes` (Imagination/Asahi PB model) | accepted (model) | [tvb-mechanism-proof-proof.02](tvb-mechanism-proof-proof.02.md) |
| H2 | A row-local index-cache LRU32 reorder lowers VS invocations, named tiled bytes, and GPU time together (geometry/shader locked) | accepted | [tvb-mechanism-proof-proof.01](tvb-mechanism-proof-proof.01.md) |
| H3 | A standalone mini-replay reading `VS Buffer Device Memory Bytes Written = 0 MiB` is an architectural artifact (PB never spills), not a fidelity defect | accepted | [tvb-mechanism-proof-proof.02](tvb-mechanism-proof-proof.02.md) |
| H4 | The mechanism reproduces at full-frame scale through the production opt-in `DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE=1` (target rows only, non-target stable) | accepted | [tvb-mechanism-proof-proof.01](tvb-mechanism-proof-proof.01.md) |
| H5 | Named tiled counters alone are a sufficient pass/fail gate | rejected (they cover ~15% of proxy; demoted to subtype evidence) | [tvb-mechanism-proof-proof.02](tvb-mechanism-proof-proof.02.md) |

## Current Navigation

- [Domain index](index.md)
- [Historical log](log.md)
- [Root 3DMark05 GT1 map](../overview-3dmark05-gt1.md)

## Recent Leaf Documents

> 2 of the 2 leaves listed below are marked `outdated:` and open with a banner naming the ground. They are history, not re-checkable evidence.

- [tvb-mechanism-proof-proof.02 - TVB / Parameter-Buffer Design Reference](tvb-mechanism-proof-proof.02.md)
- [tvb-mechanism-proof-proof.01 - TVB Pressure Mechanism Proof](tvb-mechanism-proof-proof.01.md)
