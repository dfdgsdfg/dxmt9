---
domain: hidden-backend-storage
workload: 3DMark05 GT2
subcategory: shape
order: 39
title: GT2 R32F Liveness Exposes a Surface-Alias Hazard Gap in Pass Coalescing
date: 2026-07-24
type: measurement-and-correctness-fix
status: accepted
source: experiments/output/app-d3d9-3dmark05-gt2-r32f-subresource-liveness-source-frame279-r1-20260724/dxmt9.log.gz; traces/app-d3d9-3dmark05-gt2-passcoalesce-v2-tape-r1-20260723/analysis/dag; traces/app-d3d9-3dmark05-gt2-passcoalesce-alias-hazard-frame279-r1-20260724/analysis/dag; experiments/output/app-d3d9-3dmark05-gt2-passcoalesce-alias-hazard-frame279-r1-20260724/3dmark05-perf-encoders.csv
related: docs/perfomance/overview-3dmark05-gt2.md; docs/perfomance/hidden-backend-storage/index.md; docs/perfomance/hidden-backend-storage/hidden-backend-storage-shape.38.md; specs/d3d9-renderer/requirements.md; specs/d3d9-renderer/gap.md
---

# GT2 R32F Liveness Exposes a Surface-Alias Hazard Gap in Pass Coalescing

## Question

The two dominant GT2 `2048x2048 R32F` passes repeat almost the same geometry.
Are either pass or any subresources dead, and can the existing framegraph DAG
prove the producer/consumer chain safely?

## Subresource and source-order proof

A no-gputrace traditional run traced only texture
`0x20000010000003e`. Resource creation identifies it as:

| Property | Value |
|---|---:|
| type | `TwoD` (`type=0`) |
| size / format | `2048x2048`, format `16` (`R32F`) |
| mip levels / slices | `1 / 1` |
| usage | `0x2` (render target) |
| surface aliases | every observed alias is `subresource=0, mip=0, slice=0` |

Frame279 source order is unambiguous:

1. R32F surface `0x300008b00000013`: `418` render-target writes.
2. The main-color interval binds texture `0x20000010000003e` at fragment
   stage 0. Binding shadowing reduces this to two physical Metal bind calls;
   the DAG records `134` draw-run read accesses.
3. R32F surface `0x300008400000011`: another `418` render-target writes.

Both surfaces therefore write the same texture, mip, and slice. The first pass
is live: the main-color interval samples it before the second write. The second
pass has no later sample before the next frame starts by clearing/writing the
same subresource again. It is a strong whole-pass DCE candidate, but the current
per-chunk graph cannot prove that future-chunk overwrite and must not drop it.

The trace log was compressed losslessly after analysis (`514MiB -> 15MiB`) to
preserve the full event stream without consuming the remaining trace budget.

## Discovered correctness gap

The pre-fix DAG used two unrelated hazard identities:

- attachment writes: surface handles such as `0x300008a00000017`;
- shader reads: owning texture handle `0x20000010000003e`.

The surface nodes each contained only a Clear/Write, while the texture node
contained only reads. Consequently no RAW, WAR, or WAW edge connected the
R32F producer, main consumer, and final writer.

The pre-fix post-opt frame279 DAG therefore placed the merged main-color pass
first and both R32F passes after it. The corresponding runtime/Xcode topology
had the two R32F encoders adjacent. That changes the consumer from the current
frame's first R32F result to an older value and violates
`R-BACK-32.3`, `R-BACK-32.9`, and parity. The earlier `18 -> 15` render-encoder
performance evidence is historical mechanism data only; it is not valid
promotion or parity evidence.

## Fix

Framegraph hazard construction now accepts a flat resolver supplied by the
unix renderer's retained resource pool:

- attachment/blit surface handles with `SurfaceRecord::aliasTexture` are
  canonicalized to the owning texture handle for access logging and edges;
- shader texture reads already use the canonical texture handle and avoid a
  pool lookup;
- `AttachmentSet` retains exact surface handles, so pass compatibility and
  Metal attachment selection are unchanged;
- whole-texture canonicalization is conservative for dynamic mip/cube sampling.

The native builder regression fixes a
`surface write -> texture read -> surface write` sequence and requires all
three edges:

| Edge | Hazard |
|---|---|
| producer `0 -> 1` | RAW |
| first writer `0 -> 2` | WAW |
| consumer `1 -> 2` | WAR |

The focused framegraph builder/optimizer/linearizer and render backend/observer
test sets pass on arm64, and both arm64 and Rosetta unix-provider builds pass.

## GT2 runtime validation

The fixed progressive/passcoalesce frame279 DAG reports:

| Stage | Render passes | R32F alias access order |
|---|---:|---|
| pre-opt | `18` | pass `1` Clear -> pass `2` Read x`134` -> pass `3` Clear |
| post-opt | `16` | pass `0` Clear -> pass `1` Read x`134` -> pass `2` Clear |

Both snapshots contain the canonical alias resource and RAW/WAW/WAR edges.
The actual encoder order is:

| Encoder | Target | Draws |
|---:|---|---:|
| `0` | `2048x2048 R32F` producer | `367` |
| `1` | `1024x768` main consumer | `585` |
| `2` | `2048x2048 R32F` final writer | `367` |

The fixed lane still coalesces two render passes (`18 -> 16`) while preserving
the producer/consumer/final-writer order. The old lane coalesced three
(`18 -> 15`) by crossing the missing alias hazard.

## Verdict

Higher-level geometry reduction is possible in principle: the final R32F pass
is large enough that removing it would eliminate roughly one dominant pass,
not a sub-percent index-locality tail. It is not yet a legal optimization.
Promotion requires a cross-chunk overwrite proof (or an observation-gated
deferred pass design), query/readback protections, and framebuffer equality
before any performance A/B.

The immediate accepted result is the alias-aware hazard fix. All pre-fix
passcoalesce performance and Xcode comparisons must be re-run; per-pass
R32F invocation/write measurements remain useful workload-shape evidence, but
their reordered whole-frame result is not parity evidence.
