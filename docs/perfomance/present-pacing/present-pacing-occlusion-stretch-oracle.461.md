---
domain: present-pacing
workload: Native 32x32 query output scaled to 16x16, 1x/2x/4x samples
title: "Stretch sampler and resolved-source binding regression gates"
type: evidence
status: bounded-arm64-stretch-pass-no-performance-claim
updated: 2026-10-07
source: traces/stkcop-stretch-h461/{h461-before.txt,h461-after.txt,h461-resolve.txt,dxmt9_blit_encoders.cpp,query_occlusion_metal_spec.cpp}
related: present-pacing-occlusion-msaa-oracle.459.md; present-pacing-primary-inplace-bound.460.md
---

# Two concrete StretchRect failures closed in the bounded query fixture

H459 exposed an invalid sampler while testing explicit MSAA StretchRect.
H461 keeps its direct query-pass resolve oracle intact and adds a separate
32x32 to 16x16 stretch/readback for nearest and linear filtering. Scaling
excludes the same-size blit-copy shortcut even for a single-sample source.
Every destination pixel must remain exactly opaque green.

The test precedes the fix and fails Metal API validation at sample count 1:
`maxAnisotropy=0`. Setting `max_anisotroy=1` in
`src/dxmt9/dxmt9_blit_encoders.cpp`, `makeLinearOrNearestSampler`, permits both
single-sample cases but exposes a second failure at sample count 2: the stretch
fragment function expects `texture2d` while the binding is multisample storage.

`encodeStretchRect` now selects the source's associated single-sample resolve
texture when present, matching the existing `encodeReadback` source selection.
It does not add a resolve, move commands, change load/store actions or alter
resource ownership. This implements the existing resolved-source contract;
it does not establish that every possible upstream writer produces a fresh
resolve. No new concurrency/state transition or TLA result is claimed.

## Executed evidence

Apple M1, Xcode 27.0, Metal compiler 32023.921; native arm64 build with the
explicit SDKROOT and command-local LLVM archiver used by H459. With Metal API
validation, these six tests pass:

- `dxmt9-query-occlusion-{off,force}-metal-spec`
- `dxmt9-query-scope-cb-{off,force}-metal-spec`
- `dxmt9-query-ranges-{off,force}-metal-spec`

There are 36 exact-green stretch cases: six policies, three source sample
counts, two filters. Existing direct resolve, depth/stencil, pending-output,
query reset/release and source-composition checks still execute. Tile `off`
and `force` are not real-occlusion OFF/ON; all six enable real occlusion.
Both failing stages remain archived alongside the final passing log.

This is bounded correctness evidence under `R-BACK-9.2` and the existing MSAA
resolve contract, not a sampling-filter quality oracle: uniform green cannot
distinguish nearest from linear weights. Public PE validation, nonuniform and
partially covered pixels, other formats, stale-resolve sequences and x86_64
runtime breadth remain separate. The fixes have not been staged into Wine;
no wild result, FPS improvement, default promotion or CPU-removal amount follows.

## Relation to the CPU objective

This removes a concrete validation obstacle to broader real-occlusion tests;
it is not itself the explanation for STALKER's CPU gap. H455's repeated
visibility-associated Encode differences remain candidate effects, not a
validated removable budget. H460's all-thread audit also leaves a larger
unnamed native consumer unresolved; that ownership join must precede any
claim that Encode is the largest default dxmt9-owned thread.
