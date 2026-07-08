---
domain: hidden-backend-storage
workload: 3DMark05 GT1
title: "Hidden Backend Storage — the central GPU explanation - Current Overview"
type: domain-overview
status: current
updated: 2026-07-08
source: docs/perfomance/hidden-backend-storage/log.md; docs/perfomance/overview-3dmark05-gt1.md
related: docs/perfomance/hidden-backend-storage/index.md; docs/perfomance/hidden-backend-storage/log.md
---

# Hidden Backend Storage — the central GPU explanation - Current Overview

> Current, compact view for this performance domain. Historical detail from the former
> top-level `hidden-backend-storage.md` overview is preserved in [log](log.md). Domain landing: [index](index.md).

## Scope

This domain owns the central finding of the whole investigation: the top-three
render encoders write a large `VS Buffer Device Memory Bytes Written` bucket
(~`1.627 GiB` at frame60) that is **not** explained by dxmt's explicit CPU-side
writers (~`0.444 MiB`), the source-visible MSL `VSOut` width (`184 B`), or
frontend Metal IR / AIR scratch (`128 B`). The domain defines a five-component
attribution model for that hidden traffic, validates it against external Apple /
Asahi / Mesa architecture sources, and proves through density and multi-capture
correlation that the bucket scales with **VS invocation count × per-vertex
backend bytes**, not with visible shader shape, CPU upload bytes, or fragment
volume. It does not own any specific fix — it explains *what* costs, and points
every other domain at the lever that actually moves the bucket.

## Latest Conclusions

| # | Hypothesis | Verdict | Evidence |
|---|---|---|---|
| H41 | Recovered capture-layer file route changes measurement availability, not the GPU owner | accepted refresh | [hidden-backend-storage-shape.32](hidden-backend-storage-shape.32.md) (`frame60.gputrace` and Xcode counters exported; first recovered proof GPU `37.475ms`, top-three `98.32%`, top-three VS buffer device write `1779.231 MiB`, partial render count `0`) |
| H42 | Current joined Xcode/dxmt attribution narrows the next GPU gate | accepted next gate | [hidden-backend-storage-shape.33](hidden-backend-storage-shape.33.md) (top-three Xcode rows join to dxmt encoder sidecars; latest integrated capture-layer wrapper refresh reports GPU `37.492ms`, top-three `98.40%`, top-three VS write `1779.246 MiB`; `60/2`, `60/1`, and `60/0` cover different state classes but share the same hidden-density band, dxmt CPU writer bytes negligible) |
| H43 | The `60/0` fragmentless depth-only equality failure was caused by fragmentless routing itself | rejected; keep-VSOut route is equality-safe | [hidden-backend-storage-shape.34](hidden-backend-storage-shape.34.md) (new diagnostic sub-mode keeps the pair-local `VSOut` layout at `0xfff` while omitting the fragment function; route coverage remains `42/42` draws and `97,294/97,294` primitives; pass-end `D24X8` depth and `X8R8G8B8` color both compare with `0` changed bytes) |
| H44 | Removing the `60/0` fragment function while keeping `VSOut=0xfff` reduces hidden VS writes | rejected | [hidden-backend-storage-shape.34](hidden-backend-storage-shape.34.md) (capture-layer route is usable again and exports Xcode counters, but target `60/0` VS buffer write stays flat: `224.918 -> 224.944 MiB`, VS invocations `152,895 -> 152,895`, top-three hidden estimate `1749.858 -> 1749.694 MiB`) |
| H45 | Current shader-dump liveness reopens visible `VSOut` trimming as the next GPU lever | rejected by refresh | [hidden-backend-storage-shape.35](hidden-backend-storage-shape.35.md) (H226 no-gputrace shader dump joins `9/9` top-row VS/PS rows, but top-three still write `1543..1602 B/VS invocation` against `184 B` visible `VSOut`; PS liveness is useful, generic varying trim remains closed) |

## Current Navigation

- [Domain index](index.md)
- [Historical log](log.md)
- [Root 3DMark05 GT1 map](../overview-3dmark05-gt1.md)

## Recent Leaf Documents

- [hidden-backend-storage-shape.35 - Current Shader Dump Join Keeps the Hidden Owner Below Visible VSOut](hidden-backend-storage-shape.35.md)
- [hidden-backend-storage-shape.34 - Fragmentless Depth-Only Keep-VSOut Route Passes Equality but Fails Xcode Counter Gate](hidden-backend-storage-shape.34.md)
- [hidden-backend-storage-shape.33 - Current Xcode/DXMT Attribution Narrows The Next Backend Gate](hidden-backend-storage-shape.33.md)
- [hidden-backend-storage-shape.32 - Recovered Capture Layer Reconfirms Frame60 Hidden VS Write Dominance](hidden-backend-storage-shape.32.md)
- [hidden-backend-storage-shape.31 - Current System Trace Refresh Reconfirms Vertex-Heavy Programmable Routes While Gputrace Remains Layer-Blocked](hidden-backend-storage-shape.31.md)
- [hidden-backend-storage-shape.30 - GPU Efficiency Ceiling Is Separate From Wall-Clock FPS Ownership](hidden-backend-storage-shape.30.md)
- [hidden-backend-storage-shape.29 - Encoder-Summary Route Counters Remove Indexed Per-Draw Requirement From Sidecars](hidden-backend-storage-shape.29.md)
- [hidden-backend-storage-shape.28 - Seq-Range System Trace Sidecar Adds Route Verdicts Without Capture-Layer Startup Mutation](hidden-backend-storage-shape.28.md)
