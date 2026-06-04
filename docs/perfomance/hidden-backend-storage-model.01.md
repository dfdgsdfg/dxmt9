---
domain: hidden-backend-storage
subcategory: model
order: 01
title: Hidden Apple GPU Backend Storage Model
date: 2026-06-01
type: conceptual-model
status: model
source: specs/perfomance.plan.md#L2575-L3010
---

# Hidden Apple GPU Backend Storage Model

**Question / hypothesis.** What is the part of Xcode's `VS Buffer Device Memory
Bytes Written` bucket that dxmt's explicit CPU-side writers, source-visible MSL
`VSOut`, and frontend Metal IR scratch do **not** explain? The working term is
"hidden vertex/tiler/parameter backend storage": an attribution model for Apple
TBDR work that happens after dxmt submits a draw and before the fragment stage
consumes binned primitives. It is not a single public Metal object.

**Method.** Derived hidden-backend classifier emitted in
`frame60-xcode-dxmt-bottleneck-report.md`: subtract Xcode's named
`Tiled Vertex Buffer Bytes` + `Tiled Vertex Buffer Primitive Blocks Bytes` and
dxmt CPU-writer bytes from the top-three VS-buffer-write bucket. Captured from
`app-d3d9-3dmark05-x8-alpha-fill-gputrace-r2`.

**Result.** Top-three VS buffer write `1627.246MiB`; named tiled buffer total
`29.500MiB`; dxmt CPU writer bytes `0.444MiB`; hidden backend write estimate
`1597.301MiB`; hidden/VS-write ratio `0.982x`; backend storage class
`hidden_vertex_tiler_parameter_storage:3`. Per-row hidden ratio: `0.975x`
(`60/2`), `0.991x` (`60/1`), `0.993x` (`60/0`). Top-3 geometry is real large
indexed pressure: ~`715k` triangles, ~`2.1M` submitted dxmt vertices. MSL
`VSOut = 184B`, IR return `184B`, IR scratch `128B` — none move the bucket.

The five-component model (each candidate consumer of the bucket):

1. **Tiled vertex buffer / VS stage-out** — preserves position + varyings +
   fog/color/texcoord + point_size until interpolation. Xcode reports
   `1151-1603B / VS invocation` vs visible `184B` (6x-13x gap).
2. **Primitive / binning / tiler parameter storage** — primitive metadata,
   tile lists, primitive blocks; named counters small (~29.5 MiB, ~55x gap).
3. **Render-state backend shape** — clip/cull/depth/scissor/alpha can change
   how primitives are retained/binned even at fixed `VSOut`.
4. **Attachment store/load / tile preservation** — color/depth store-load
   traffic (secondary, lands in device-write counters).
5. **Compiler/backend spill or hidden scratch** — register pressure / private
   lowering below the visible MSL/AIR forms.

**Verdict.** model (ACCEPTED as the framing). The `1.627 GiB` is **not** owned
by dxmt writers (`0.444 MiB`), visible `VSOut` (`184 B`), or AIR scratch
(`128 B`). The next-probe hint for all three hot rows is
`primitive-backend-pressure-or-state-shape-ab` — move primitive/backend
pressure, not CPU upload bytes. Which sub-component dominates is still OPEN.

**Related.** [[hidden-backend-storage]] · [[hidden-backend-storage-model.02]] ·
[[hidden-backend-storage-attribution.01]] · [[hidden-backend-storage-density.01]] ·
[[tvb-mechanism-proof]] · [[vsout-layout]] · [[baselines]] · [[overview]]
