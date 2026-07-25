---
domain: hidden-backend-storage
workload: 3DMark05 GT1 and GT2
title: "Hidden Backend Storage — the central GPU explanation - Current Overview"
type: domain-overview
status: current
updated: 2026-07-25
source: docs/perfomance/hidden-backend-storage/log.md; docs/perfomance/overview-3dmark05-gt1.md; docs/perfomance/hidden-backend-storage/hidden-backend-storage-shape.36.md; docs/perfomance/hidden-backend-storage/hidden-backend-storage-shape.37.md; docs/perfomance/hidden-backend-storage/hidden-backend-storage-shape.38.md; docs/perfomance/hidden-backend-storage/hidden-backend-storage-shape.39.md; docs/perfomance/hidden-backend-storage/hidden-backend-storage-shape.40.md; docs/perfomance/hidden-backend-storage/hidden-backend-storage-shape.41.md
related: docs/perfomance/hidden-backend-storage/index.md; docs/perfomance/hidden-backend-storage/log.md; docs/perfomance/overview-3dmark05-gt2.md
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
| H51 | GT2's final dominant R32F pass is dead across frames and can be removed by current DCE | accepted liveness; rejected current-window reachability | [hidden-backend-storage-shape.41](hidden-backend-storage-shape.41.md) (`503/503` target frames contain two write runs and `0` read-first frames; current-default frames `278..280` repeat `R32F Clear -> main Read x133 -> final R32F Clear`, then the next frame Clears first. The final pass's shared depth is also immediately cleared. Query/readback evidence is zero, but encode ready depth is `1` for all `531` samples, so current per-chunk DCE and ready-only batching cannot obtain the cross-chunk proof.) |
| H50 | The alias-aware passcoalesce lane is safe enough for default promotion | accepted default promotion with explicit evidence debt | [hidden-backend-storage-shape.40](hidden-backend-storage-shape.40.md) (GT1/GT2/GT3 complete without observed GPU/pipeline failure; corrected GT2 producer/consumer order remains; exact captures across the known GT3 1:06–1:08 glitch window are normal; render-pass/present work falls `8.25–11.2%`. An env-clean default SFIV run also reaches a valid rendered scene and a separate perf run records `7,320` Presents with zero GPU/pipeline failure. Only passcoalesce is promoted; device-backed pixel parity remains open.) |
| H49 | One of the two dominant GT2 R32F passes may be dead, and the current DAG can prove their subresource/consumer order | accepted liveness candidate; accepted correctness gap and fix | [hidden-backend-storage-shape.39](hidden-backend-storage-shape.39.md) (both surfaces alias the same `TwoD`, one-level `R32F` mip0/slice0; source order is `418 writes -> sample -> 418 writes`; the first pass is live and the final pass is a cross-chunk DCE candidate. Pre-fix passcoalesce split surface writes from texture reads and moved the consumer before the producer. Alias-aware RAW/WAW/WAR restoration produces a safe `18 -> 16` render-pass result instead of the invalid `18 -> 15` topology) |
| H48 | Extending LRU32 reorder eligibility to the `229` alpha-tested draws in each dominant GT2 R32F pass can materially reduce VS invocations | rejected; accepted index-locality bound | [hidden-backend-storage-shape.38](hidden-backend-storage-shape.38.md) (valid alpha-test candidates move LRU32 only `238,571 -> 238,484`, `-0.0365%`, with `0` gate passes; `69` other draws already have `miss32 == unique`; captured VS invocations `486,280` match the effective LRU64 estimate `486,697` within `0.086%`, leaving at most `0.56%` whole-frame invocation headroom across both passes) |
| H47 | The many black draw previews in the GT2 frame279 Xcode debugger are failed/redundant color work and the main hidden-write owner | rejected as primary owner; accepted as depth-prepass classification | [hidden-backend-storage-shape.37](hidden-backend-storage-shape.37.md) (the only color-write-off route is encoder 0's early `120`-draw depth prepass: `94,980` primitives and `284,940` submitted vertices, or `4.46%/4.47%` of the frame; Xcode shows black Color 0 with depth output; the similarly sized GT1 fragmentless/keep-VSOut route left VS invocations and write flat) |
| H46 | GT2's `~8 FPS` rate is owned by Wine/Rosetta or translation CPU work, and the large VS write is parameter-buffer overflow spill | rejected CPU/overflow explanations; accepted emitted-GPU-work attribution | [hidden-backend-storage-shape.36](hidden-backend-storage-shape.36.md) (full-frame Xcode native replay remains `126.77-131.678ms`, or `7.59-7.89 FPS`, at Xcode's reported `Medium` performance state; `2,529,660` VS invocations write `6,952.646MiB`, `15.66x` the visible VSOut expectation; all `19` encoders report `0` partial renders) |
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

- [hidden-backend-storage-shape.41 - GT2 Final R32F Pass Is Observationally Dead but Needs Cross-Chunk Scheduling](hidden-backend-storage-shape.41.md)
- [hidden-backend-storage-shape.40 - Alias-Aware Pass Coalescing Clears the Default-Promotion Wild Gate](hidden-backend-storage-shape.40.md)
- [hidden-backend-storage-shape.39 - GT2 R32F Liveness Exposes a Surface-Alias Hazard Gap in Pass Coalescing](hidden-backend-storage-shape.39.md)
- [hidden-backend-storage-shape.38 - GT2 R32F Alpha-Test Draws Are Already at the Index-Locality Floor](hidden-backend-storage-shape.38.md)
- [hidden-backend-storage-shape.37 - GT2 Black Draws Are a Depth Prepass, Not the Main Hidden-Write Owner](hidden-backend-storage-shape.37.md)
- [hidden-backend-storage-shape.36 - GT2 Full-Frame Native Replay Preserves the GPU Ceiling Without Partial Renders](hidden-backend-storage-shape.36.md)
- [hidden-backend-storage-shape.35 - Current Shader Dump Join Keeps the Hidden Owner Below Visible VSOut](hidden-backend-storage-shape.35.md)
- [hidden-backend-storage-shape.34 - Fragmentless Depth-Only Keep-VSOut Route Passes Equality but Fails Xcode Counter Gate](hidden-backend-storage-shape.34.md)
- [hidden-backend-storage-shape.33 - Current Xcode/DXMT Attribution Narrows The Next Backend Gate](hidden-backend-storage-shape.33.md)
- [hidden-backend-storage-shape.32 - Recovered Capture Layer Reconfirms Frame60 Hidden VS Write Dominance](hidden-backend-storage-shape.32.md)
- [hidden-backend-storage-shape.31 - Current System Trace Refresh Reconfirms Vertex-Heavy Programmable Routes While Gputrace Remains Layer-Blocked](hidden-backend-storage-shape.31.md)
- [hidden-backend-storage-shape.30 - GPU Efficiency Ceiling Is Separate From Wall-Clock FPS Ownership](hidden-backend-storage-shape.30.md)
- [hidden-backend-storage-shape.29 - Encoder-Summary Route Counters Remove Indexed Per-Draw Requirement From Sidecars](hidden-backend-storage-shape.29.md)
- [hidden-backend-storage-shape.28 - Seq-Range System Trace Sidecar Adds Route Verdicts Without Capture-Layer Startup Mutation](hidden-backend-storage-shape.28.md)
