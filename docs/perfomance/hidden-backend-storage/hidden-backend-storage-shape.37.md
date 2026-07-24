---
domain: hidden-backend-storage
workload: 3DMark05 GT2
subcategory: shape
order: 37
title: GT2 Black Draws Are a Depth Prepass, Not the Main Hidden-Write Owner
date: 2026-07-24
type: measurement
status: accepted-classification-rejected-primary-owner
source: traces/app-d3d9-3dmark05-gt2-passcoalesce-order-store-frame279-xcode-r1-20260724/frame279.gputrace; traces/app-d3d9-3dmark05-gt2-passcoalesce-order-store-frame279-xcode-r1-20260724/analysis/frame279-counters-xcode.csv; traces/app-d3d9-3dmark05-gt2-passcoalesce-order-store-frame279-xcode-r1-20260724/analysis/frame279-indexed-state-class-xcode-proxy.md; experiments/output/app-d3d9-3dmark05-gt2-passcoalesce-order-store-frame279-xcode-r1-20260724/3dmark05-perf-encoders.csv; experiments/output/app-d3d9-3dmark05-gt2-passcoalesce-order-store-frame279-xcode-r1-20260724/3dmark05-perf-indexed-probe-draws.csv; docs/perfomance/hidden-backend-storage/hidden-backend-storage-shape.34.md
related: docs/perfomance/overview-3dmark05-gt2.md; docs/perfomance/hidden-backend-storage/index.md; docs/perfomance/hidden-backend-storage/hidden-backend-storage-shape.36.md
---

# GT2 Black Draws Are a Depth Prepass, Not the Main Hidden-Write Owner

## Question

Xcode's frame279 GPU debugger shows many draw attachments whose color preview
is black. Are these failed or redundant color draws responsible for the large
hidden VS invocation/write bucket?

## Method

The existing order-aware pass-coalescing frame279 capture was reused; no new
capture was made. The first black draw was inspected at
`Draw[seq=279,prim=2376]`, Metal call `41`, and joined to the same-run full
encoder summary and indexed per-draw telemetry. The route summary provides
complete draw-class totals. The indexed stream provides exact shader/state and
draw-index blocks for the depth-only class. Xcode's encoder counters provide
the whole-frame and whole-encoder denominators.

The green outline in Xcode's attachment viewer is the selected geometry
highlight. At the indexed draw call, Color 0 remains black while the depth
attachment contains the highlighted geometry. The bound depth/stencil state
is `LessEqual, Write Yes`.

## Result

The black family is one homogeneous depth-prepass route:

| Property | frame279 value |
|---|---:|
| encoder | `279/0` |
| draw-index blocks | `0..8`, `75..180`, `279..283` |
| draws | `120` |
| primitives | `94,980` |
| submitted vertices | `284,940` |
| color write | `0x0` |
| depth | enabled, write enabled, `LessEqual` |
| alpha blend / alpha test | off / off |
| stencil / clip / scissor | off / off / off |
| cull / fill | back / fill |
| VS / PS / PSO shapes | `3 / 1 / 3` |
| VSOut key | `0xfff` |

The full route counter confirms that these are the only depth-only draws in
the frame. Encoder 0 has another `1,209` programmable textured draws, while
encoders 1 and 2 contain no color-write-off route.

| Denominator | Draw share | Primitive share | Submitted-vertex share |
|---|---:|---:|---:|
| encoder 0 (`1,329` draws, `1,300,260` primitives, `3,889,904` vertices) | `9.03%` | `7.30%` | `7.33%` |
| full frame (`2,176` draws, `2,129,001` primitives, `6,376,127` vertices) | `5.51%` | `4.46%` | `4.47%` |

The corresponding full-frame Xcode replay costs `149.701ms`, issues
`3,012,831` VS invocations, and writes `8,758.891MiB` through
`VS Buffer Device Memory Bytes Written`. Encoder 0 owns `90.595ms`,
`1,867,847` VS invocations, and `5,209.335MiB` of that write. Xcode did not
export a class-scoped counter row for the 120 depth draws, so those encoder
totals must not be assigned directly to the black class.

Using encoder 0's average invocation/vertex and bytes/invocation only as a
proportional proxy gives about `136,822` VS invocations, `381.6MiB` VS write,
and `6.64ms` GPU time for the black route: roughly `4.4-4.5%` of the full
frame. This is not a draw-class measurement, but it gives the correct order of
magnitude and is consistent with the class's `4.47%` submitted-vertex share.

## Proxy Caveat

`frame279-indexed-state-class-xcode-proxy.md` assigns `81.729ms` and
`4,699.526MiB` to this class by distributing the whole encoder counter row
according to `effective-miss32`. That is a ranking proxy, not attribution.
Only `126/1,329` encoder-0 draws are represented in that indexed class table,
and `120` of those `126` happen to be the depth-only route. The proxy therefore
allocates almost the entire encoder denominator across an incomplete sample.
It must not be used to claim that the black draws own encoder 0.

## Existing Controlled A/B

The earlier GT1 `60/0` class is nearly the same-sized depth-only route:
`97,294` primitives and `291,882` vertices. Its controlled
fragmentless/keep-VSOut experiment is directly relevant:

| Metric | Baseline | Fragmentless, keep `VSOut=0xfff` |
|---|---:|---:|
| target GPU time | `5.474ms` | `5.496ms` |
| target VS invocations | `152,895` | `152,895` |
| target VS buffer write | `224.918MiB` | `224.944MiB` |
| depth/color equality | exact | exact |

Removing the fragment function did not move the VS invocation or hidden-write
bucket. The position-only `VSOut=0x0` variant was not a legal alternative:
it changed `39.803060%` of the pass-end `D24X8` bytes.

## Verdict

Inspecting the black draws was useful: it identifies a real depth prepass and
rules out shader failure or random invisible color work. It does not identify
the primary GT2 ceiling. The class is semantically meaningful, covers only
`4.46%` of frame primitives, and the already-completed fragmentless A/B shows
that deleting its fragment function does not reduce hidden VS writes.

Do not skip or collapse these draws without a depth-equality proof. Do not
spend another GT2 capture on a fragmentless-only variant. If exact current-GT2
cost is still required, obtain draw/class-scoped Xcode counters from the
existing capture; do not reuse the incomplete indexed-class proxy. The
performance branch with material headroom remains invocation or backend-write
reduction across the dominant programmable textured/color routes in encoders
0, 1, and 2.
