---
domain: present-pacing
workload: STALKER private Clear-fusion failure settlement
title: "Private failure relay preserves terminal disposition through borrowed callbacks"
type: evidence
status: private-finite-safety-only
updated: 2026-10-15
source: traces/stkcop-clear-failure-relay-h554/{build-r2.json,checks.json,verification.json,verify.py,failure_relay.hpp,relay_spec.cpp,FailureRelay.tla,queue_spec.cpp,prior_spec.cpp,run_test.py,run_prior.py,run_capture.py,capture-serial-receipt.json,capture-fused-receipt.json}; traces/stkcop-clear-failure-relay-h554/r1/{build-receipt.json,results.json}; traces/stkcop-clear-failure-relay-h554/prior-r2/{build-receipt.json,results.json}
related: present-pacing-clear-queue-failure.553.md; present-pacing-clear-economy.552.md; ../../../specs/backend/gap.md
---

# Explicit failure survives the coordinator

H554 repairs H553's finite counterexample in private linked images. Eight
injected failures now stop with query/queue terminal status and no inline
completion or GPU submission. Eight serial OFF controls still submit and
complete. This is not a production promotion or a live CPU/FPS result.

## Mechanism and scope

The candidate throws an identity-qualified `SourceEncodeFailure` only when
its fused Clear cannot open a render encoder. Three borrowed-callback
boundaries preserve that exception through a local `exception_ptr` and
rethrow after leaving the `noexcept` callback. The session coordinator's
function handler calls `poisonTapeFailure()` and
`failStopReplaySourceExecution()` instead of reaching the empty-submission
inline-success branch. Unrelated exceptions keep their previous behavior.

Private copies replace only the encoder, FrameGraph backend and CPU-ready
session coordinator objects in the frozen archive. No canonical runtime source,
Wine staging, bridge ABI or default selector changes. `build-r2.json` pins the
corrected sources and loaded images; the earlier build had a diagnostic format
warning and is not the tested image receipt.

## Actual queue fixtures

All cases use the existing deterministic shutdown-drain seam, real Metal
attachments, Metal API validation, and independent color/depth/stencil readback.
Draw calls are skipped only in these attachment/failure fixtures.

| Population | Matrix | Candidate result |
|---|---|---|
| Fresh session | One/two failing sources × ordinary/exact stores | One failed-open attempt, terminal; no retry, submit, inline completion or reclaim |
| Prior encoded carrier | One successful Clear source followed by one/two failing sources × ordinary/exact stores | Same terminal result; prior unsubmitted carrier is not submitted as success |

The prior source contains two Clears. A forwarding backend witness checks
`tailCommandBufferHasWork`, a real returned command buffer with
`NotEnqueued` status, and reuse of that same buffer by source 2. It retains no
extra carrier ownership. Both attachment readbacks remain at their seeds after
the failure, including the earlier encoded but unsubmitted color Clear.
Completed waterline and completion queue stay zero; all two/three sources
remain resident/inflight. `arenaBuildPoisoned_` remains false: the relevant
terminal witnesses are query-completion failure and `queryPendingTerminal_`,
not a claim that every poison flag changes.

The prior population's authoritative run is `prior-r2`; `prior-r1` lacked
the explicit encoded-work assertion. `r1` owns the fresh-session population.
`verify.py` independently checks all 16 logs, identity/counter tuples, recorded
hashes and loaded paths without rerunning GPU work.

## Model, native relay and original capture

The bounded `FailureRelay` model passes and its erase-failure mutation violates
`FailureNotSuccess` as expected. The native arm64/x86_64 test exercises three
nested relays, success, exact source/storage/sequence/command identity, and
unrelated exception behavior. The model abstracts cleanup and wake delivery;
it is not a C++ lifetime, scheduling or memory-order proof.

Two fresh-process original STALKER capture executions pass source/output
readback, 7-source mapping, 9 Issues / 5 polls / 4 results, and 670 balanced
resource wrappers. OFF witnesses zero fusions; ON witnesses 65. Both source
and output BGRX SHA-256 remain
`1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`.
These executions use real draws, unlike the small fault fixtures. Their query
oracle is the captured production-counter policy, not hardware occlusion.

## Remaining promotion gates

The function handler runs after local session/carrier destruction. The tested
failure has no active encoder and no committed sub-CB. It does not establish
safe retention/drain after earlier sub-CB submission, hardware-query scope
cleanup, active-encoder failure, reset, or live waiter wakeups. These remain
explicit blockers, not facts inferred from successful seed readback.

H552's 1.8055 ms paired-median source-5 saving belongs to H551's images. H554
has not repriced its extra success-path relay bookkeeping. Reprice only after
settlement qualification; do not transfer the old number automatically.
Default-live largest single-thread ownership and transferable CPU removal
budget remain open. No commit or runtime promotion is implied by this leaf.
