---
domain: present-pacing
workload: production-loop native query release fixture
title: "H420: Queue-owned closed-prefix qualification, without wake elision"
type: evidence
status: production-qualification-integrated-elision-open
updated: 2026-09-30
source: traces/query-ranges-h420/{native-final.log,x64-final.log,tla.log}; tests/native/backend/cpu_ready_session_join_spec.cpp
related: present-pacing-close-witness-provenance.419.md; ../../../specs/backend/query-scope-ranges.md
---

# H420: Qualification authority moves into the actual coordinator

Base `39acfae9` commits H419. `ClosedPrefixCoordinator` is now noncopyable and
owned by `CommandQueue` under its mutex. It retains the queue identity, epoch,
encoder-work generation and optional receipt. It has no operation to restart
a stopped epoch; a new queue gets a new identity.

## Runtime integration

The range candidate invalidates authority before another release action,
source planning/selection, session finalization and submission. Terminal WSI
release and scheduling stop destroy the epoch. Generation exhaustion disables
qualification rather than wrapping. Idle waits preserve a closed receipt.

After the actual ClosePass helper returns and the lock is reacquired, exact
FIFO acknowledgement may issue a receipt. Missing helper/session, pending
events or publication beyond covered execution prevent qualification but do
not prevent an otherwise valid ordinary acknowledgement. There is no producer
read of mutable EncodeSession state.

No caller consumes the receipt to skip work. All ordinary posts, wakeups,
acknowledgements, raw-fence advancement and submission/completion behavior stay
in place. This adds bounded queue-local candidate state, not an FPS improvement.
The existing range policy remains default OFF; no new runtime knob is added.

## Deterministic evidence

The production-loop fixture publishes two compatibility sources, posts a
ClosePass and pauses Encode after its actual acknowledgement. It requires a
receipt for that exact publication, then publishes a younger source while
Encode is still paused. The older receipt is immediately ineligible. After
Encode resumes, source work has invalidated it. Carrier preservation, source
order and eventual completion assertions remain from the existing fixture.

The initial test incorrectly assumed Arena routing while ranges were enabled;
the candidate deliberately uses compatibility storage. Explicit diagnostic
publication boundaries now establish the required three source identities.
This was a fixture setup error, not a relaxed receipt gate.

Native repeated focused and full session-loop tests pass 12/12; x86_64 focused
tests and install-name audit pass 4/4. Pure tests also cover invalid/no-helper
qualification, newer publication, stop invalidation and rejection of revival.
Another 512 guard combinations compare coordinator qualification with ordinary
acknowledgement, requiring identical return status, FIFO head/size, acknowledged
ordinal and terminal latch state. This checks that failed qualification does not
alter the reference transition.
The existing TLA suite passes all 227 configurations and remains prerequisite evidence, not a new refinement
proof of receipt consumption. No new GPU exact or wild FPS claim is made.

## Next boundary

Connect a qualified current FIFO request to the receipt only after modeling
producer/Encode interleavings, reset/terminal and submission/raw progress.
`hasReceiptFor` is inspection only, not an admission capability. In particular,
selected-prefix coverage still must not be confused with execution, and skipping
`makeEncodeContext` requires preserving its dirty-state effects. H418's no-op
helper evidence does not by itself prove those omitted surrounding operations.
