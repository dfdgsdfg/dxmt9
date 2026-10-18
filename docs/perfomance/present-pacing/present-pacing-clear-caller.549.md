---
domain: present-pacing
workload: STALKER CoP pending Clear fusion feasibility
title: "Private actual caller and attachment oracle pass; 63 depth Clears rejected by backed NULL RT"
type: evidence
status: private-caller-passed-nullrt-envelope-limits-coverage
updated: 2026-10-15
source: traces/stkcop-clear-caller-h549/{contract.md,build.py,build.json,build.log,caller_spec.cpp,build_test.py,test-build.log,run_test.py,serial-test.json,fused-test.json,serial-test.log,fused-test.log,run_capture.py,capture-serial-receipt.json,capture-fused-receipt.json,capture-serial.log,capture-fused.log,dxmt9_draw_encoder_chunk.mm}; traces/stkcop-clear-census-h550/{contract.md,build.py,build.json,build.log,run_capture.py,capture-serial-receipt.json,capture-fused-receipt.json,capture-fused.log,analyze.py,analysis.json,dxmt9_draw_encoder_chunk.mm}
related: present-pacing-clear-adapter.548.md; present-pacing-clear-feasibility.546.md; ../../../specs/backend/gap.md
---

# Actual caller succeeds, but the intended depth population is excluded

H549 privately replaces only `dxmt9_draw_encoder_chunk.mm` in the frozen H537
runtime archive. The original equality branch remains unchanged. An additional
branch calls the H548 projection/shared classifier, opens the actual Draw
encoder with pending Clear and settles only after successful opening.
Null opening throws before resetting pending Clear. The shared runtime source,
provider staging and default policy remain unchanged.

## Correctness evidence

| Gate | Result | Scope |
|---|---|---|
| Real `encodeChunk` attachment fixture, serial/candidate | Seven aspect masks pass each | Independent expected color/depth/stencil values after seeded attachments and actual pass opening/finalization |
| Actual extended branch witnesses | Four masks: color, depth, stencil, depth+stencil | Remaining masks use the unchanged equality branch |
| Null render-encoder injection | Four extended masks pass | Throws, returns no successful submission, retains original source/generation/command identity, leaves seeded pixels unchanged |
| Original H502/r5 STALKER capture | Serial and candidate pass | Seven source identities, balanced 670 wrappers, nine Issues/five polls/four results and original source/output image |

The attachment fixture uses `DXMT_SKIP_ALL_DRAWS=1` to isolate pass load/store
effects. It drives the actual caller and independent GPU readbacks, but does
not claim shader or geometry coverage. It preserves final attachments through
exact-tape mode. The subsequent original capture exercises actual draws with
that debug switch absent. Each run gets a fresh process/device. Loaded private
image paths and hashes are recorded. The fixture's first two build attempts
used an incorrect readback helper name; the preserved build log records those
compile errors before the successful build.

Both original source/output images retain SHA-256
`1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`.
The query transcript is the production-counter diagnostic, not a hardware
visibility oracle. Capture authority remains `content_authority=unknown` and
`timing_qualified=0`; this result does not upgrade it.

## H550: why only two actual fusions?

H549's original-capture witness reports only source sequence 8 command pairs
36→37 and 583→584, both color-only. H550 adds an unpriced rejection census to
the same private policy. It independently repeats serial/candidate original
oracles, both passing, and records all 65 decisions:

- Two color-only candidates admit.
- All 63 depth-only candidates reject as `Unresolved`.
- Every rejection has color slot 0 `Format::NullRt` (55), **a non-null physical
  texture**, and actual positive width/height matching its surface descriptor.
- The depth attachment remains resolved. The projection's blanket NullRt
  exclusion causes the rejection, not adjacency, query scope or extent.

This distinction is real in the current implementation. `Pool::createSurface`
special-cases NullRt to omit storage; `Pool::createTexture` and
`Pool::createSurfaceForTexture` can supply a backed alias. The format policy
maps NullRt to BGRA8, and `prepareRenderPassWithStoreProofLookahead` includes a
color attachment based on texture presence. The census proves this backed
shape in the frozen input, not its frequency in the default live tail.

Do not bypass the gate by pretending a backed NullRt is an absent attachment.
The next refinement must distinguish logical format from actual physical
attachment participation and preserve the serial Draw's load/store behavior.
Test backed aliases separately from truly storage-free NullRt surfaces.

## Objective and remaining work

No CPU comparison was run: witness logging and duplicate projection in H550
are not timing evidence. Two color fusions do not establish removal of the
2.071–2.243 ms H546 measured across all 65 standalone Clears. The largest
default-live single-thread owner and safe removal budget remain unproven.

Next: backed/storage-free NullRt refinement and actual-caller GPU tests, then
repeat the original input and price only the changed eligible population.
Full live-queue terminal settlement on failed opening, ordinary non-exact
load/store policy and broader format coverage also remain promotion gates.
No commit or default-on change occurred. The independent agent review route
was unavailable (`agent type is currently not available`); no independent
review is claimed.
