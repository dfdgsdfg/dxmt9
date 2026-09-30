---
domain: present-pacing
workload: native release and encode lifecycle fixtures
title: "H419: Authenticate closed-prefix issuance at the actual FIFO acknowledgement"
type: evidence
status: candidate-provenance-closed-runtime-integration-open
updated: 2026-09-30
source: traces/query-ranges-h419/{native.log,x64.log,tla.log}; tests/native/backend/{session_release_event_spec.cpp,encode_session_lifecycle_spec.cpp}
related: present-pacing-close-outcome-oracle.418.md; ../../../specs/backend/query-scope-ranges.md
---

# H419: An acknowledged ordinal is not a snapshot receipt

Base `5c1e2635` commits H418. Its candidate `certify` accepted the caller's
previous snapshot after checking only the transport's acknowledged ordinal.
That was explicitly not a trusted production capability: changing snapshot
fields could retain the same ordinal without authenticating its contents.

`acknowledgeAndCertify` now checks the exact current FIFO head and consumes it
through the real transport acknowledgement before creating a witness. Invalid
context/outcome, incomplete coverage, stale/forged snapshot, extra queued work
or terminal state cannot issue a receipt. Failure leaves the queue unchanged.
The method must execute under the queue lock when integrated; it does not lock
internally. Existing production acknowledgement code is unchanged.

## Evidence

- The existing 2,048 context/policy combinations remain covered.
- Another 512 issuance combinations check authority/coverage rejection and
  unchanged FIFO state, or exactly one successful head consumption.
- All seven snapshot fields are independently forged and rejected before issue.
- Invalid helper outcome, wrong sequence/raw coverage and repeated issuance are
  rejected, with the original head and acknowledgement ordinal preserved.
- `closePassIsIdempotentAndSessionResumesOnSameCommandBuffer` uses the real
  helper's `Closed` result, issues the candidate receipt via the actual FIFO,
  checks adjacent admission and confirms a second close is a no-op.
- Before the next source executes, busy state rejects reuse. After it reopens
  the same command buffer, the advanced work generation still rejects the old
  witness even if the supplied publication sequence has not changed.

Native focused tests pass 6/6 (three repetitions), and x86_64 tests pass 2/2.
The lifecycle fixture suppresses Metal calls through its existing fake recorder;
it is model/code evidence, not a new GPU oracle. H418 owns real query/pixel
evidence. The existing TLA suite passes all 227 configurations (65 safety,
five progress, 157 expected counterexamples); this is prerequisite regression
evidence only, not a new proof of production receipt reuse.

## Remaining integration boundary

The actual helper outcome and epoch/quiescence context still need trusted
coordinator ownership. Queue selected-prefix coverage advances before encoder
execution in `runCpuReadySessionEncodeLoop`; it cannot replace an execution
generation. Invalidation must precede unlocking for new work, submission or
finalization, including successor-source selection and failure/stop edges.
`makeEncodeContext` also has dirty-state effects; skipping its call requires an
explicit invariant, not simply a matching sequence.

Next integration must bind those mutation edges and prove FIFO/raw progress,
terminal handling and submission/completion obligations before omitting a wake.
This change does not add runtime storage, skip a release, change default policy
or establish an FPS improvement.
