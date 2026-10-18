---
domain: present-pacing
workload: STALKER Clear-fusion private submitted-prefix failure repair
title: "Drain the submitted prefix before unwinding failed session ownership"
type: evidence
status: private-bounded-drain-qualified
updated: 2026-10-15
source: traces/stkcop-clear-failure-drain-h556/{contract.md,build.json,model-check.json,verification.json,verify.py,Drain.tla,submitted_spec.cpp,queue_spec.cpp,capture-serial-receipt.json,capture-fused-receipt.json}; traces/stkcop-clear-failure-drain-h556/r1/{build-receipt.json,results.json}; traces/stkcop-clear-failure-drain-h556/submitted-r1/{build-receipt.json,results.json}
related: present-pacing-clear-submitted-failure.555.md; present-pacing-clear-failure-relay.554.md; ../../../specs/backend/gap.md
---

# Keep failure owners alive through an ordered GPU drain

H556 changes only private coordinator failure handling relative to H554.
After the borrowed callback returns an explicit typed failure, but before
the enclosing session unwinds, the coordinator marks terminal and stops
admission. With the scheduling mutex released it commits/waits an empty fence
on the same Metal queue. It then rethrows the original failure. The failed
pending tail is never submitted, callbacks are not invoked as successful,
and source completion/reclaim does not advance.

The fence is failure-only. Successful encoding gets no extra fence, clock or
resource retain. This prototype terminates the process if it cannot establish
the drain; it is not graceful device-loss recovery. A hung GPU can still block
the wait. The model does not claim liveness without GPU progress.

## Verified scope

- Bounded `Drain` safety passes; early-release and failed-tail-submit mutations
  violate their intended invariants.
- Eight coordinator controls/failures repeat H554's fresh-session matrix.
- Eight further cases cover an already-submitted prefix. While the independent
  GPU gate is closed, the terminal flag is visible and the session-owned
  sentinel remains alive. Only after gate release and GPU completion does
  failure unwinding release it, without invoking success callbacks. OFF controls
  invoke the callback once after completion.
- Original STALKER OFF/ON replay preserves source/output pixels, seven-source
  mapping, 9 Issues / 5 polls / 4 results and 670 balanced resource wrappers.
  ON witnesses 65 Clear fusions. These runs have real draws; small failure
  fixtures skip draws to isolate attachment/ownership behavior.

Independent log/hash verification binds the two matrices and original oracles
to `build.json`. The prior committed color Clear remains visible; failed depth
Clear remains absent. No rollback of already-submitted GPU effects is claimed.

## Still excluded

The sentinel proves the session retention boundary, not every resource-specific
query/allocator cancellation rule. Hardware-query pending-count settlement,
live reset/waiter races, GPU hang recovery and default-live transfer remain
unqualified. Production-counter capture replay does not prove hardware occlusion.
Canonical sources and Wine installation remain unchanged.

Static call-site review also identifies two direct
`encodeCpuReadySessionSource` calls in the deferred-terminal-suffix prefix and
natural-drain branches. They bypass `encodeWithBorrows`, where this prototype
places the drain. Those failure paths are not covered by the fixture and must
share the failure boundary before live promotion. The ordinary-source results
must not be generalized to all coordinator paths.

Net CPU economy must compare the full repaired candidate with the old serial
image. H552's saving cannot simply be carried forward. The largest default-live
single-thread owner and transferable removal budget are still open.
