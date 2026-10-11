---
domain: present-pacing
workload: STALKER CoP Normal replay preparation
title: "Normal pre-arm setup closure on the original H481 input"
type: evidence
status: offline-preflight-only
updated: 2026-10-11
source: traces/stkcop-normal-restore-h482/{setup_probe.cpp,setup-probe.tsv,setup-probe.stderr,setup-inspection.json,setup-inspection.stderr,setup-probe-sha256.txt}
related: present-pacing-normal-restore.482.md; present-pacing-normal-history-input.481.md; ../../../specs/experiments/gap.md
---

# Required owners are now enumerated, not inferred from seed presence

After committing H482 as `c20704e9`, a resource-free native probe reuses
`buildGeneralReplayPlan`, `buildCaptureSourceReplayPlanV4`,
`renderTapeInitialSetupV4SnapshotPrefix` and `buildRenderTapeInitialSetupV4`
on the existing H481 bundle. All four admissions succeed. It authenticates
actual blob bytes and builds metadata only; it never creates a device, calls
replay or weakens the stable-Exact launcher gate.

The official bundle inspector also succeeds again. The manifest SHA-256
remains `ab9b808f08c071cef1f4dd64d498ec87333109f9cec9a236e84536c0854f3e85`.
Construction/arm/freeze remain Normal at revision zero, with
`content_authority=unknown` and `replay_qualified=false`.

## Actual closure

The setup-only prefix contains 11 events. The existing V4 plan contains
646 hidden owners, 5,044 seed operations (not unique blobs), 427,768,410 seed
bytes, 16,777,216 allocation-only bytes, and 11 late surface-alias descriptors.
These are setup inventory sizes, not runtime cost measurements.

| Required surface group | Count | Existing pre-arm coverage |
|---|---:|---|
| Current backbuffer wrappers | 2 | Prefix; both must resolve to the same actual backbuffer handle |
| Standalone D24S8 | 1 | Prefix; complete canonical depth/stencil seed |
| Seeded texture alias | 1 | Prefix parent and wrapper |
| Late aliases of seeded texture owners | 10 | Parents prestaged; wrappers still created at logical definition |
| Late NULL RT alias | 1 | Allocation-only parent prestaged; wrapper still lazy |
| Late DF24 produced alias | 1 | Neither parent nor wrapper prestaged |

All 16 required surface generations have exact original-history rows; five
other census rows have no surface definition in this interval. There is no
reason to invent definitions for those unused rows. Existing H482 settlement
preserves unrelated local history.

The missing produced parent is texture `4294967318/g1`, defined at zero-based
event 5398; its surface `4294967319/g1` follows at event 5399. Its descriptor
is `ProducedByCapturedPass`, so the current initial-setup builder intentionally
skips it. Moving its physical allocation before arm must not manufacture a
seed, move the defining clear, or grant content authority before that clear.

NULL texture `4294967320/g1` is already allocation-only staged. Its alias
`4294967321/g1` has original **touched=true despite having no seed**. This is
a concrete counterexample to deriving history from seed availability. The
other late color aliases also require their original history before use.

The two current-wrapper initial seeds have identical digest, extent and
subresource. This removes a conflicting-seed question for this input only;
it is not actual-local-handle or GPU alias evidence.

## Implementation consequence

`Texture::surfaceLevel` in `src/d3d9/core_texture.cpp` lazily creates a surface
handle and stores only a weak surface reference. Resolving a surface once and
discarding its wrapper is insufficient: later logical activation could create
a different handle. A Normal setup owner must retain the actual wrapper and
transfer that same acquisition into the active table at ObjectDefine.

The next bounded integration therefore needs:

1. Authenticated Normal-only setup coverage for required parents and aliases,
   including allocation-only preparation of the admitted produced DF24 parent.
2. Hidden ownership of all 12 late wrappers before the actual local setup
   checkpoint; logical activation remains at the original event.
3. Original touch/discard settlement after seed effects and before local arm.
   Later logical seed consumption must not repeat seed-derived restoration.
4. A Normal-compatible backbuffer seed-completion path. The existing
   `applyGeneralMutation` calls Exact-only `completeRenderTapeBackbufferSeed`;
   simply omitting the provider's Exact toggle cannot work.

The original action policy must stay Normal throughout. Neither captured
completion labels nor hidden creation authorize completion or content reads.
The original-vs-replay GPU oracle and read-first content contract remain
separate gates. No provider integration, live run, GPU comparison, CPU timing,
largest-owner conclusion or removable budget is claimed here.

## Verification note

The three H482 focused native tests were rerun successfully before commit.
The H482 explicit performance-document source audit passes. The repository-wide
audit still reports missing old build-log artifacts in three unrelated leaves
(mini-replay capacity recovery, GPU position invariance, and query default
promotion); it is not reported as passing and those historical files are not
rewritten by this investigation.
