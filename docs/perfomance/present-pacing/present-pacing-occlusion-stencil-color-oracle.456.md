---
domain: present-pacing
workload: STALKER visibility candidate qualification; native 32x32 single-sample fixture
title: "Query-cut stencil and prior-color preservation oracle"
type: evidence
status: bounded-arm64-oracle-pass-not-promotion
updated: 2026-10-07
source: traces/stkcop-occlusion-qualification-h456/{occlusion-h456-final.txt,occlusion-h456-final.json,query_occlusion_metal_spec.cpp}
related: present-pacing-completed-workload-visibility.455.md
---

# Query cuts preserve stencil and prior color in the bounded fixture

[H455](present-pacing-completed-workload-visibility.455.md) observes a repeated
late Encode CPU reduction with real occlusion. To qualify that reduction as
usable work removal, H456 extends the existing production Metal oracle without
changing runtime code, Wine staging or policy defaults.

The new sequence clears color to magenta and stencil to `0x33`. An unqueried,
color-write-disabled draw replaces stencil with `0x5a`. Three subsequent query
windows use read-only stencil Equal with references `0x5a`, `0x33`, `0x5a`.
Completed results must be exactly `1024`, `0`, `1024`. Changing the reference
rejects a false pass where both writer and readers accidentally use constant
zero. Depth and alpha testing are disabled, isolating stencil rejection.

The real descriptor-accounting seam must record at least four additional
stencil and color Stores, with no additional stencil/color DontCare actions.
Only after all query results complete does readback check every pixel remains
exact magenta. Neither readback nor an unconditional completion wait rescues
the query polling loop. Store accounting complements bytes that might survive
discard accidentally on one GPU.

## Verification

Six arm64 Metal tests pass with API validation, real occlusion and perf counters:

- `dxmt9-query-occlusion-{off,force}-metal-spec`
- `dxmt9-query-scope-cb-{off,force}-metal-spec`
- `dxmt9-query-ranges-{off,force}-metal-spec`

Here `off` and `force` select tile-FFP policy, not occlusion OFF/ON. Every test
enables real occlusion; queried draws retain the provider's portable-FFP policy.
The existing depth, overlapping-query, exact-green, release and reset checks
also run. The benchmark-only branch is unchanged.

The native build required explicit `SDKROOT` for the installed macOS 27 SDK.
Its cached archiver flags also required LLVM `ar`, rather than Apple's `ar`
which rejects `csrD`. A command-local tool alias supplied the expected LLVM
archiver; no project build settings or system tool selection changed. Tests ran
on M1 with Xcode 27.0. These are correctness runs, not CPU/FPS measurements.

## Limits and remaining objective

This closes one single-sample stencil/color counterexample, not all provider
safety. Actual multisample query counts and resolved pixels, public PE breadth,
fault handling and full-scene qualification remain separate gates. No new TLA
claim is made: the lifecycle model does not prove attachment bytes.

H455's observed 4.60–6.11 ms/frame Encode differences remain diagnostic effects,
not a guaranteed removable amount. Primary still includes game/Wine/renderer
CPU; neither this fixture nor H455 establishes the largest overall dxmt9-owned
single-thread cost. No default promotion or README performance update follows.
