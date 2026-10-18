---
domain: present-pacing
workload: STALKER Clear-fusion private coordinator failure coverage
title: "Share the failure drain across ordinary and deferred source entry points"
type: evidence
status: private-bounded-entry-coverage-qualified
updated: 2026-10-15
source: traces/stkcop-clear-failure-boundary-h558/{contract.md,build.json,verification.json,verify.py,suffix_spec.cpp,run_suffix.py,capture-serial-receipt.json,capture-fused-receipt.json}; traces/stkcop-clear-failure-boundary-h558/r1/{build-receipt.json,results.json}; traces/stkcop-clear-failure-boundary-h558/submitted-r1/{build-receipt.json,results.json}; traces/stkcop-clear-failure-boundary-h558/suffix-r2/{build-receipt.json,results.json}
related: present-pacing-clear-failure-drain.556.md; present-pacing-clear-net-economy.557.md; ../../../specs/backend/gap.md
---

# Close the deferred-fragment failure-boundary bypass

H558 privately extracts H556's failure-only terminal/drain handler and shares
it between borrowed ordinary encoding and both direct deferred-fragment calls.
The direct wrapper releases the scheduling mutex around encoding, reacquires
it before handling typed failure, and drains the same Metal queue with the
mutex released before session owners unwind. No failed tail is submitted and
no failed source receives successful completion or reclaim.

## Verified scope

| Population | Cases | Evidence |
|---|---:|---|
| Fresh ordinary failure and OFF controls | 8 | Original coordinator matrix |
| Submitted-prefix failure and OFF controls | 8 | Real SharedEvent gate and session ownership sentinel |
| Deferred prefix/suffix, ordinary/exact stores, default/forced split, OFF/ON | 16 | Actual registered fragments, source identity and terminal disposition |
| Original captured STALKER serial/fused | 2 | Real draws, exact source/output pixels and preserved source/query/wrapper oracles |

The deferred fixture builds `Draw(A) -> depth Clear -> Draw(B)` with a Writing
successor. The actual coordinator selects prefix `[0,1)` and natural suffix
`[1,3)`. Prefix failure is an injected typed exception before backend work;
suffix failure is the actual fused Clear null-open path. These are distinct
failure mechanisms, not two hardware allocation failures.

On failure, a session-owned sentinel survives while a same-queue SharedEvent
gate holds GPU progress. After the gate opens, failure unwinding releases it
without invoking its success callback. OFF controls invoke that callback once.
Failed depth remains at the seeded `.75`; serial depth becomes `.25`.

Both original captures preserve seven sources, 9 Issues / 5 polls / 4 results,
670 balanced wrappers and exact BGRX SHA-256
`1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`.
The fused arm witnesses 65 fusions. Query authority remains production-counter,
not hardware occlusion; capture content authority remains unknown.

`verify.py` checks image/source/log hashes, exact matrix membership, fragment
counts, terminal identities and original-capture readbacks. A frozen-source
text audit finds exactly two raw encode calls, both inside failure wrappers,
and two direct-wrapper call sites. This is not an AST proof or a new TLA+
route-refinement proof. H556's drain model remains the bounded abstract model.

## Preserved fixture correction and provenance

`suffix-r1` incorrectly required a prior committed sub-CB merely because a
Clear ended a render pass. That does not imply a PerRenderPass commit. Its
failed assertion, source and script remain preserved. `suffix-r2` covers the
real default no-prior-commit shape separately from a forced-prefix population
using PerNRecords=1 and disabling mid-chunk commits in the suffix.

The fixture explanation was appended to `contract.md` after the runtime build.
The original `build.json` is unchanged. Verification reconstructs and hashes
the pre-amendment contract prefix against that receipt, and checks the amended
contract against the suffix fixture receipt. Runtime source/image hashes did
not change with this test correction.

## Decision and next gate

The two direct-entry coverage gaps identified in H556 are closed for these
finite fixtures. Canonical runtime sources and Wine installation remain
unchanged. GPU-hang recovery, hardware-query cancellation and live reset/wake
races are not qualified. Failure to establish the drain remains process
fail-stop in this private prototype.

H557's 1.9955 ms paired-median local saving belongs to its earlier image; it is
not a timing result for H558 or removable live ms/frame. The next useful gate
is matched live ownership/economy qualification, not another unchanged local
timing population. The largest default-live single-thread owner remains open.

## Live-image preparation (H559; no live run)

`traces/stkcop-clear-live-h559/build-receipt.json` links H551 serial and the full
H558 candidate into the same H538 A prefix-OFF runtime with identical existing
worker self-CPU observers. `preflight-verification.json` independently checks
both linked images against the original capture, including exact pixels,
source/query/wrapper outcomes and 0/65 fusion witnesses. No Wine files were
staged. This qualifies the relinked images for further preparation, not timing.

The live contract selects Encode self CPU per observed frame over fixed spatial
knots, with A/A and AB/BA controls. The runner/analysis receipt is not yet frozen;
safe bounded wineserver exit and hash-verified restoration must be wired before
launch. Boundary-only Primary CPU remains mixed game/Wine/dxmt9 and cannot
establish a global renderer maximum. The new image pair must not inherit H538
or H557 performance numbers.

Follow-up: [H559 live result](present-pacing-clear-live.559.md) subsequently
freezes the runner and completes all six runs. Its matched-observer Encode
screen passes; global default ownership and clean FPS transfer remain open.
