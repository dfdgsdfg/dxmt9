---
domain: present-pacing
workload: STALKER CoP H481 original Normal source, x86 release external stack discovery
title: "Observed replay contains substantial capture ancestry; full Slot validation is a separate product candidate"
type: evidence
status: stack-discovery-only-no-cpu-share-or-removal-amount
updated: 2026-10-11
source: traces/stkcop-provider-sample-h490/{run-startup.py,run.py,analyze.py,receipt.json,sample-r2-receipt.json,sample.txt,sample-r2.sample.txt,sample-tool.log,sample-r2-tool.log,population.json,sample-r1.txt,sample-r2.txt}; traces/stkcop-provider-sample-h490/{sample-r1,sample-r2}/{supply.csv,caller.json,analysis.json}; traces/stkcop-provider-phases-h489/build-receipt.json
related: present-pacing-provider-phases.489.md; present-pacing-prepared-replay-cpu.488.md; present-pacing-root-game.477.md
---

# Do not treat the whole planned-replay envelope as product CPU

H489 places most observed provider CPU inside planned replay, but cannot
separate V4 qualification from ordinary work. A bounded external wall-stack
sample now confirms concrete qualification ancestry inside that call. It also
identifies full existing-Slot validation as a distinct product-path candidate.
Neither finding is a numerical removable-CPU budget or a largest live-game
owner conclusion.

No runtime code or binary changes occur in this investigation. The exact H489
executable and both runtime image hashes are verified against its receipt
before launch. The original H481 Normal input and existing readback/identity
oracle are unchanged. The Metal skill's doctor/readback workflow is used
(Xcode 27, Apple M1 Metal 4); there is no gputrace, Wine staging or UI input.

## Setup miss and bounded repair

The first `sample PID 15 1` attaches immediately at process launch. Its report
contains startup and input-hash work but no `runProviderReplay` stack. It is
retained as a missed target window, not evidence of zero replay cost. The
application still completes and passes its original oracle.

The second launch changes only attachment timing: the external launcher polls
the existing `prepared=1` output, then attaches the same sampler without
pausing or adding a handshake to the target. This report contains the intended
translated x86 caller. No further repetitions or selection of favorable CPU
numbers are performed. The first report's initial ARM64 startup heading must
not override the executable's verified x86_64 architecture.

## Disjoint stack population, not CPU attribution

`analyze.py` parses call-tree indentation, subtracts direct child weights to
obtain disjoint node populations, and retains every leaf under both
`runProviderReplay` and `replayPlannedChunk`. The three planned-replay stack
roots have weights 48, 3 and 1. Parent and child weights are not added twice.
All 52 weights are accounted for:

| Known ancestry | Observations |
|---|---:|
| Capture identity/emission/provider qualification | 26 |
| Supply clock/queue segment observer | 2 |
| Other planned replay, including unknown leaves and waits | 24 |

Fifteen of the capture observations pass through
`QueueLifecycleController::observeTapeRuntimeNewCommands`; three more pass
through `TapeEmissionObserver::appendLocal`. The others include provider
before/after-record qualification and runtime begin-record/dispatch access.
Unknown Rosetta/JIT leaf PCs are retained under their known caller ancestry,
not invented symbol names. The remaining population includes actual reserve,
uniform comparison, sparse application, direct append and Present wait work.

These are periodic wall-state observations from one short perturbed execution,
not randomized CPU samples. In particular a Present condition-variable wait
appears in the population. **26/52 is not a claim that half of replay CPU is
removable or capture-only**, and no ratio is multiplied by H489's milliseconds.
The sampled run's planned-call native CPU is 56.037 ms, but cross-run differences
are not an estimated sampler tax and are not subtracted.

## Concrete code paths exposed

The capture paths have real algorithmic work beyond a nullable callback:

- `src/d3d9/device_c_render_tape_runtime_identity_v4.cpp`, `bindEmission`,
  locates each record by event/record identity with a linear scan of accumulated
  records, once for validation and again for binding. Pending-draw indices
  already participate in validation, but replacing lookup requires preserving
  rollback, coordinator and exact locator checks. It is a harness/capture
  improvement, not automatically a product optimization.
- `src/d3d9/device_c_render_tape_runtime_identity_core.cpp`, `observe`, takes
  the identity root mutex and dispatches append/publication/completion evidence.
- `src/dxmt9/dxmt9_render_tape_runtime_observer.hpp`, `Activity`, separately
  admits and releases each short observation under the endpoint mutex and
  maintains quiescence. Removing that protocol would change cancellation and
  retirement guarantees, not merely logging.

The product candidate is separate:

- `src/dxmt9/dxmt9_backend_types.hpp`, `ChunkSlot::commandPayloadsInRange`,
  walks every existing command and every DrawRun parameter, checking state,
  uniform and payload ranges.
- `src/dxmt9/dxmt9_direct_continuation.hpp`, `directContinuationAdmission` and
  `directSlotPhysicalCapacityCovers`, invoke that complete validation.
- `src/dxmt9/dxmt9_source_payload.cpp`,
  `TransactionalChunkSlotAssembler::reserve` and `prepare`, also validate the
  whole destination. The sample explicitly includes `commandPayloadsInRange`
  under continuation admission; it does not measure every other listed call.

Repeated validation of a growing immutable prefix is therefore a concrete
redundancy candidate. This does not establish how many checks execute per
source or their exclusive CPU cost. Do not remove them as redundant assertions:
they are release-mode admission/commit checks. A safe change needs a
generation-qualified validated-prefix capability, preservation across permitted
append operations, invalidation on mutation/rollback/reuse, and validation of
the appended suffix. Existing direct-slot admission truth tables and malformed
payload tests must continue to reject corrupt data. No such optimization is
implemented or promoted by this experiment.

## Verification and next decision

Both completed processes preserve Normal policy, 16 restored surfaces,
12 aliases, 779/779 wrappers and source mapping 0/1/2 to local sequences 3/4/5.
All four original source/output readbacks have SHA-256
`f99e419861d3c4127e96d8ab56dca62d56421919a5962084ae4ecd95ee42ce5a`.
The strict caller/provider reducer and original-source reducer pass for both;
the first sample remains unusable for stack attribution. All-resource content
authority and default-game transfer remain unproved.

The next product experiment should price the full-Slot validation candidate
with preserved checks or a proven prefix witness, while explicitly keeping V4
qualification outside any claimed product savings. Further broad replay
percentages from this harness would not close the original objective. Live
Primary attribution and a correctness-qualified work-removal comparison remain
necessary; H477's failed all-call-clock method remains closed.
