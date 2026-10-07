---
domain: present-pacing
workload: Public PE32/PE64 occlusion and XYZRHW edge oracle
title: "Screen-space pixel-center mapping closes the retained visibility edge failure"
type: evidence
status: bounded-raster-fix-verified-whole-scene-and-cpu-budget-open
updated: 2026-10-07
source: traces/stkcop-pixel-center-h466/{run.py,pixel-center-fix.diff,winemetal_dxmt9.so}; traces/stkcop-pixel-center-h466/{native-test.log,rosetta-test.log}; traces/stkcop-pixel-center-h466/{x86,x64}-{on,off,delay}-edge; traces/stkcop-pixel-center-h466/{x86,x64}-on-overscan
related: present-pacing-occlusion-edge-isolation.464.md; present-pacing-primary-transfer-checkpoint.465.md; present-pacing-completed-workload-visibility.455.md
---

# A bounded correctness fix, not a new CPU saving

H464's 225-sample/31-missing-pixel rectangle was a screen-space center mismatch.
`src/dxmt9/dxmt9_ffp_shaders.cpp`, `makeFfpVertexSource`, now adds `(0.5,0.5)`
to POSITIONT/XYZRHW XY before viewport normalization and Y inversion. D3D9's
integral pixel centers thereby map to Metal's half-integral centers. The input
rectangle -0.5..15.5 becomes Metal's 0..16 rectangle.

Do not restore the transformed branch's same-sign NDC `halfPixelFixup`: this
screen-space mapping requires NDC offsets `(+1/width,-1/height)`. The existing
test forbidding that transformed fixup remains. New source-contract assertions
require the screen offset before normalization and Y inversion. Runtime
viewport and FFP uniform target-size overrides remain unchanged.

## Verification

Apple M1, Xcode 27.0, Metal 32023.921, Sikarugir CX24.0.7. Public oracles run
with Metal API validation, perf counters OFF, and atomic shared-Wine staging
restoration. Built provider SHA-256:
`cb96a69d5335c177b4ca9026d7599713d8bbdfab8b22ff9121402c8a91c34e86`.

| Input / provider | PE32 | PE64 | Pixel verdict |
|---|---|---|---|
| Edge / real visibility | 256 / 0 / 256 | 256 / 0 / 256 | All direct and scaled pixels exact |
| Edge / 200 ms delayed completion | 256 / 0 / 256 | 256 / 0 / 256 | Exact; pending sentinels preserved |
| Edge / conservative reference | 2 / 2 / 2 | 2 / 2 / 2 | Exact; only three expected count failures |
| Overscan / real visibility | 256 / 0 / 256 | 256 / 0 / 256 | All direct and scaled pixels exact |

Delay runs observe PE32 pending counts 286/147/235 and PE64 294/178/210.
No Present or readback supplies progress until each query is complete. Reissued
queries and repeated ready reads retain the existing checks. H464's unchanged
WineD3D edge oracle already supplies the 256/0/256 and exact-pixel reference.
The changed shader is compiled/executed by the public PE draw, not only matched
as a generated source string.

`dxmt9-core-ffp-state-key-spec` passes in host and x86_64 builds. Initial host
attempts hit local SDK discovery and Apple-ar/LLVM-ar command mismatches; setting
the current SDK and supplying LLVM ar for the cached `csrD` recipe resolves
them without changing repository build settings. The updated x86_64 source
test initially retained one old string assertion, then passed after that
assertion was updated to the new `screenPosition` name.

No queue, resource-lifetime or completion transition changes. This is a pure
shader-coordinate transform, so source-contract and concrete GPU pixel tests
are the applicable refinement layer; no new TLA result is claimed.

## Remaining decision

This closes the retained edge counterexample for the visibility candidate.
It does not qualify every pretransformed primitive, multisample edge, texture
filter weight, public query combination, or whole STALKER scene. It is not an
Encode optimization and has no measured CPU/FPS benefit of its own.

Real visibility remains default OFF. H455's 4.60–6.11 ms/frame Encode effect
still needs matched current-build workload/correctness qualification. H465's
rejected Primary transfer is independent: fixing this raster error does not
establish which default thread has the greatest dxmt9 CPU cost.
