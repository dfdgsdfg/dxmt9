---
domain: present-pacing
workload: STALKER CoP Day
title: "H427: Query-preserved pass actions still compute store lookahead"
type: code-audit
status: candidate-unmeasured-no-runtime-change
updated: 2026-10-01
source: src/dxmt9/dxmt9_draw_encoder_chunk.mm; src/dxmt9/dxmt9_render_pass_encoder.mm; traces/query-ranges-h426/analysis.json
related: present-pacing-drawrun-preparation.426.md
---

# H427: Separate fixed-action proof work from diagnostic re-traversal

Audited at `c37be7b1`. This is source evidence, not another runtime measurement.
H426's 22.466 ms pass-entry bucket includes both production work and perf-only
work. It does not establish how much time belongs to the candidates below.

## Concrete candidate

In `src/dxmt9/dxmt9_render_pass_encoder.mm`,
`prepareRenderPassWithStoreProofLookahead` sets
`preserveQueryAttachments = query::metalOcclusionEnabled()` provider-wide.
It nevertheless calls `colorStoreProofForLookahead` for each included color
attachment and `depthStoreProofForLookahead` for each included depth/stencil
aspect. These functions traverse future commands and inspect state/resources.

Under that query-preservation predicate, the production action equations are:

| Decision | Result independent of lookahead proof |
|---|---|
| Color DontCare store eligibility | false (`!preserveAttachments`) |
| Color late/Unknown store eligibility | false (`!preserveQueryAttachments`) |
| Color without resolve | Store |
| Color with resolve | StoreAndMultisampleResolve |
| Depth/stencil DontCare eligibility | false (`!preserveAttachments`) |
| Depth/stencil late/Unknown eligibility | false (`!preserveQueryAttachments`) |
| Included depth/stencil store action | Store |

Load actions, clear values, attachment inclusion, visibility storage and the
late-store attachment ledger still need their existing construction. Nothing
here permits skipping resource lifetime work, hazard checks or query boundaries.
The proof and no-lookahead cause also feed perf counters; preserve diagnostic
meaning rather than labelling an uncomputed proof as a real BlockNoLookahead.

The narrow optimization candidate is therefore **avoid production proof scans
when query preservation fixes the actions and no observer requests the proof**.
This is not a proposal to discard attachment contents or merge passes.

Do not broaden this predicate to `preserveAttachments` alone. Render Tape exact
preservation also sets that value, but late-store eligibility is gated by
`!preserveQueryAttachments`, not by `!preserveAttachments`. That lane needs a
separate proof and cannot inherit this shortcut mechanically.

## Separate observer cost

In `src/dxmt9/dxmt9_draw_encoder_chunk.mm`, `startRenderPass` calls
`beginRenderPassWithStoreProofLookahead`, which prepares the production actions.
After a successful open, its `perf::enabled()` block invokes
`renderPassStoreProofSummaryForLookahead` again for color0/depth proof and touch
distance. This is an additional traversal, not necessarily an identical result:
the production path covers all included color attachments and individual DS
aspects; the summary has different coverage and requests touch distances.

`RenderPassFrameTracker::noteDrawRead` is also perf-gated. It scans the texture
slots against the previous pass key. `consumeActiveSeedMergeContinuation` has
its own attribution gate. Thus the measured pass-entry bucket must not be
reported as Metal encoder creation time or observer-OFF product time.

## Bounded next implementation and validation

1. Add a pure decision for whether store proof is required by production or by
   diagnostics. Exhaust the query-preservation/perf combinations; retain normal
   and tape-only proof behavior. Do not change a hot-path return type merely to
   carry diagnostic data.
2. Compare complete prepared pass outputs for query-preserved inputs with the
   current path: MRT, colorless RT, depth-only, stencil-only, clear, aliasing,
   MSAA resolve, truncated/invalid suffix and future-source boundaries. Check
   load/store actions and late-store ledger, not just the top-level boolean.
3. Retain accurate proof counters when requested. Diagnose the extra summary
   traversal separately; do not silently reuse a depth-only summary as a stencil
   or full-MRT proof.
4. Measure same-input preparation CPU with observer OFF, then validate query
   results/pixels and completion progress. Run the relevant native/model tests
   before a wild run. No FPS claim until an observer-OFF comparison exists.

For further decomposition, record ContinueActive separately from actual pass
open. Within open, distinguish visibility allocation, lookahead/proof work,
`renderCommandEncoder`, encoder configuration and perf-only summary/tracking.
`beginRenderPassWithStoreProofLookahead` already separates preparation, creation
and configuration internally. Failure to prepare or create an encoder must
remain a paired diagnostic outcome rather than disappearing from totals.

No runtime code/default was changed by this audit. Range remains default OFF.
