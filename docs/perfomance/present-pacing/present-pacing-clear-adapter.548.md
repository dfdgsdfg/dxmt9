---
domain: present-pacing
workload: STALKER CoP pending Clear fusion feasibility
title: "Real-resource Clear admission adapter; vertex sampler and physical extent gaps closed"
type: evidence
status: candidate-adapter-tested-caller-gpu-economy-open
updated: 2026-10-15
source: src/dxmt9/dxmt9_pending_clear_projection.hpp; tests/native/backend/pending_clear_projection_spec.cpp; traces/stkcop-clear-adapter-h548/{verify.py,verification.json,build-final.log,native-test.log,arm64-test.log,x86_64-test.log,dxmt9_pending_clear_fusion.hpp,dxmt9_pending_clear_projection.hpp,pending_clear_fusion_spec.cpp,pending_clear_projection_spec.cpp}
related: present-pacing-clear-model.547.md; present-pacing-clear-feasibility.546.md; ../../../specs/backend/gap.md
---

# Actual resource facts before changing rendering

H548 adds a candidate `projectFacts` adapter from published command identity,
pending `ClearDesc`, current `FlatDrawStateRecord`, queue load policy and
retained Pool records into the H547 shared predicate. **No production encoder
caller or changed rendering policy is enabled.** No new performance run occurs.

## Two concrete coverage gaps

1. H547's sampled-texture array covered 16 slots. Actual D3D9 flat state has
   16 fragment and four vertex sampler slots. The array now holds 20, with a
   compile-time pin to `core::kMaxTextures` and rejection tests for every slot.
   This is a candidate-model coverage repair, not an observed product defect.
2. Surface descriptor dimensions alone are insufficient physical evidence:
   `Pool::createSurfaceForTexture` can retain the parent texture when mip-view
   creation fails. Projection checks real texture width/height against the
   descriptor and rejects disagreement. Logical NULL RT is also rejected by
   this initial envelope. These property reads have not been priced.

Matching Clear/Draw bindings reuse one attachment projection. Facts remain
call-local values, not cached lifetime authorization. The serial following
color load comes from `resolveColorAttachmentLoadAction`, including touched
status, exact-tape preservation and pending backbuffer discard. Scope and
exact read-hazard inputs still require binding at the actual encode call site.

## Verification

`dxmt9-pending-clear-fusion-spec` and the new
`dxmt9-pending-clear-projection-spec` pass under native Meson. The latter
creates real Pool surfaces and mip-view textures on the M1 Metal device with
API validation enabled. It checks:

- source/storage identity, invalid publication references and adjacency;
- independent color/depth/stencil metadata, bound levels and view fields;
- requested versus unrequested first-use color load;
- all 20 direct-resource and sampled-alias slots, including a real mip alias
  in the last vertex sampler;
- real unequal mip extents, descriptor/texture disagreement, sample count,
  resolve target, missing records and logical NULL RT;
- query/active-encoder/exact-hazard scope, sRGB state and pending discard.

Some negative metadata cases deliberately mutate and restore fixture-owned
Pool records. They are projection rejection tests, not production mutation
or concurrent-lifetime tests. Synthetic valid-shaped command references test
field mapping; they do not prove that a live publication owns those resources.

The pure refinement is rebuilt and run independently on arm64 and x86_64
with `-O2 -Wall -Wextra -Werror`: 16,384 guard combinations, 311,296 value
states and 2,490,368 final-store comparisons still pass, plus the new sampler
slot cases. The frozen H537 runtime images remain hash-identical.

## Remaining gate

Actual `startRenderPass` failure settlement must be tested through the real
encode caller: admission alone cannot reset pending Clear. The existing void
start lambda may return without an active encoder, and the new extension must
not inherit unconditional settlement from the old full-key merge branch.
After caller/failure binding, independent color/depth/stencil GPU readbacks
and the original STALKER capture are required before observer-thin CPU tests.

These tests perform **no fused rendering and no pixel readback**. They do not
establish that H546's 65 candidates pass the expanded guards, that the measured
2.071–2.243 ms is removable, or that this is the largest default-live owner.
