---
domain: mini-replay-bisection
workload: 3DMark05 GT1
title: "Mini-Replay Bisection — the apparatus that isolated per-draw vertex-stage write amplification - Current Overview"
type: domain-overview
status: current
updated: 2026-07-08
source: docs/perfomance/mini-replay-bisection/log.md; docs/perfomance/overview-3dmark05-gt1.md
related: docs/perfomance/mini-replay-bisection/index.md; docs/perfomance/mini-replay-bisection/log.md
---

# Mini-Replay Bisection — the apparatus that isolated per-draw vertex-stage write amplification - Current Overview

> Current, compact view for this performance domain. Historical detail from the former
> top-level `mini-replay-bisection.md` overview is preserved in [log](log.md). Domain landing: [index](index.md).

## Scope

This domain owns the **methodology**: build a standalone, row-local mini-replay
harness that consumes real captured index/stream/cbuf/depth payloads plus dumped
shaders, reproduce the original hot-encoder `VS Buffer Device Memory Bytes Written`
pressure at encoder scale outside Wine/D3D9, then bisect it down to individual
shader-pair / draw windows. It answers: *which concrete inputs reproduce the GT1
hidden vertex-stage write bucket, and at what granularity is it owned?* This is
the apparatus that made [tvb-mechanism-proof](../tvb-mechanism-proof/index.md) possible and that backs the
[index-cache-locality](../index-cache-locality/index.md) win.

## Latest Conclusions

| # | Hypothesis | Verdict | Evidence |
|---|---|---|---|
| H18 | Primitive-conflict metrics can split rank-1 visible failure from rank2-4 exact owner-masked passes | rejected; only final-color metrics separate | [mini-replay-bisection-texture.07](mini-replay-bisection-texture.07.md) |
| H19 | Existing D3D9 occlusion query or winemetal visibility plumbing can supply the missing runtime oracle as-is | rejected as production oracle; diagnostic scout added separately | [mini-replay-bisection-texture.08](mini-replay-bisection-texture.08.md) |
| H20 | A diagnostic Metal visibility scout can supply per-Metal-draw sample-visible counts after GPU completion | implemented diagnostic; not final-color proof; old rank-1 `36..37` is sample-visible | [mini-replay-bisection-texture.09](mini-replay-bisection-texture.09.md) |
| H21 | No-sample visibility rows are the hidden-backend hot locality owner | rejected; zero rows are small and only `-2,016` of `-182,856` LRU32 delta | [mini-replay-bisection-texture.10](mini-replay-bisection-texture.10.md) |
| H22 | Positive Metal visibility samples can be used as the scoped depth-read final-color oracle | rejected; rank2 has `39,835` samples but `0` final-color pixels, and rank1/rank3 are both sample-positive but fail/pass diverge | [mini-replay-bisection-texture.11](mini-replay-bisection-texture.11.md) |

## Current Navigation

- [Domain index](index.md)
- [Historical log](log.md)
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
