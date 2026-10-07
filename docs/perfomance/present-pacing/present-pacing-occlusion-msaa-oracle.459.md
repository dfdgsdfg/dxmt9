---
domain: present-pacing
workload: Native 32x32 Metal occlusion fixture, 1x/2x/4x samples
title: "Actual multisample query counts and query-pass resolved color"
type: evidence
status: bounded-arm64-msaa-pass-separate-stretch-failure
updated: 2026-10-07
source: traces/stkcop-occlusion-msaa-h459/{msaa-final.txt,msaa-final.json,query_occlusion_metal_spec.cpp,explicit-stretch-failed.txt,explicit-stretch-failed.json,explicit-stretch-fixture.cpp}
related: present-pacing-occlusion-stencil-color-oracle.456.md; present-pacing-completed-workload-visibility.455.md
---

# Query-pass 2x/4x MSAA oracle passes; standalone StretchRect remains unqualified

H455's repeated late Encode CPU reduction motivates qualifying the real
occlusion provider. H459 extends H456's production-core fixture to actual 2x
and 4x multisample attachments without changing runtime code or defaults.

The requested sample count is checked in the resource records, actual Metal
color/depth texture types must be multisample, and the color resolve texture
must be a distinct single-sample role. Expected counts come from viewport area
times the requested sample count, not from a reference query result.

| Surface samples | Full coverage | Stencil accept / reject / accept |
|---|---:|---|
| 1x | 1,024 | 1,024 / 0 / 1,024 |
| 2x | 2,048 | 2,048 / 0 / 2,048 |
| 4x | 4,096 | 4,096 / 0 / 4,096 |

Every configuration also executes the existing checks for unqueried prior-depth
preservation, zero depth visibility, consecutive draws, UP/indexed-UP, nested
query membership, source/pass cuts, blocked completion and unchanged pending
output, alpha rejection, release and reset. Query completion is reached by
polling before color readback. Exact green and prior magenta pixels are checked
through the core readback path, which selects the query pass's `resolveTexture`
for multisample sources. No extra render/resolve pass is inserted to repair or
mask the query pass's output. Store accounting still forbids color/depth/stencil
DontCare actions in the preservation windows.

## Executed validation

On Apple M1, Xcode/xctrace 27.0 and the Metal 32023.921 toolchain, all six arm64
tests pass with Metal API validation and real occlusion enabled:

- `dxmt9-query-occlusion-{off,force}-metal-spec`
- `dxmt9-query-scope-cb-{off,force}-metal-spec`
- `dxmt9-query-ranges-{off,force}-metal-spec`

Each process now runs 1x, 2x and 4x. `off`/`force` selects tile policy, not real
occlusion OFF/ON. The range-benchmark and snapshot-only invocations retain their
single-sample scope. Native compilation uses the previously required explicit
SDKROOT and command-local LLVM archiver alias. No Wine or catalogue run occurs.

There is no runtime state-machine change, so no new TLA transition or proof is
claimed. This is the concrete attachment/count layer absent from the existing
bounded lifecycle model, not a replacement for that model.

## Rejected readback preparation: independent StretchRect failure

The initial fixture inserted an explicit multisample-source StretchRect into a
single-sample destination before readback. At 2x, Metal API validation aborts:
`MTLSamplerDescriptor's maxAnisotropy value (0) is invalid`.
`src/dxmt9/dxmt9_blit_encoders.cpp`, `makeLinearOrNearestSampler`, zero-initializes
`WMTSamplerInfo` without setting its `max_anisotroy` field. The archived failure
and initial fixture are retained. This is not evidence of an occlusion count
failure; it is also not a passing public MSAA resolve path.

The final fixture reads the query pass's own resolved storage directly, which
isolates the intended preservation invariant. It does not fix or qualify the
separate StretchRect path. Full promotion must retain that known failure as an
open boundary concern rather than interpreting six passing tests as complete
MSAA support.

## Remaining objective

This adds bounded sample/resolve correctness, not an FPS or CPU measurement.
MSAA sample masks and partially covered edges, public PE breadth, fault paths,
whole-scene behavior and observer-OFF performance remain open. H455's
4.60–6.11 ms/frame diagnostic Encode differences do not become a guaranteed
removable budget. Default-path Primary CPU still mixes game/Wine/renderer work,
so the largest overall dxmt9 single-thread owner remains unranked.
