---
domain: present-pacing
workload: 3DMark05 GT2 (matched queue-mutex profiles)
date: 2026-08-31
title: Queue T2d reserve-copy-commit economic gate remains deferred
type: evidence
status: deferred
source: docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.36.md; docs/perfomance/present-pacing/present-pacing-copy-materialization-ledger.237.md
related: specs/backend/producer-concurrency/gap.md; specs/backend/producer-concurrency/spec.md; specs/verification/tla/QueueT2dReserveCopyCommit.tla
---

# Queue T2d reserve-copy-commit economic gate remains deferred

## Decision

T2d remains **deferred**. No production concurrency change was made:
`CommandQueue::submitDrawRunBatchImpl` still holds the queue mutex through its
append, and the frozen-ticket re-read/restamp path remains intact. The gate is
closed because the matched profile does not show a waiting victim in the
append segment; the append's large hold is local work, not producer pacing.

## Reproducible matched evidence

The accepted matched GT2 queue-mutex ON/OFF pair at commit `3eaac5a8` is
recorded in append decomposition `.36`; it is the profile used here, rather
than an unqualified comparison with today's checkout. Both runs passed with
zero GPU command-buffer errors and zero command-chunk rejects. The cumulative
ON table reports:

| Queue-mutex segment | Acquires/Present | Acquire wait ms/Present | Hold ms/Present |
|---|---:|---:|---:|
| `find_reordered_index_buffer` | 405.745 | 0.7851 | 0.0823 |
| `mark_chunk_resources_and_capture_buffer_bindings` | 15.802 | 0.1500 | 0.0027 |
| `submit_draw_run_batch_impl` | 22.567 | 0.0643 | n/a |
| `note_present_dequeued` | 0.999 | 0.0354 | 0.0002 |
| `submit_draw_run_batch_impl/append` | 562.148 | **0** | **3.3471** |
| **all acquire-wait rows** | — | **1.0536** | — |

The append segment therefore has no measured waiting victim. The parent
submit acquire's `0.0643 ms/Present` is not an append wait and must not be
reclassified as one. Under R-BACK-43.8, let `W` be append-segment acquire wait:
`W < 0.2 ms/Present` closes the gate, `0.2 <= W < 0.5` requires repeated
matched profiles, and `W >= 0.5` opens investigation subject to lifecycle and
wild evidence; the `3.3471 ms/Present` hold is local work and never substitutes
for `W`. Independently, the Task 4 materialization ledger measures queue,
mutation, and arena work at `<=0.041 ms/Present` in GT2 (and `<=0.001` in
SFIV), agreeing with deferral.

Task 7 lifecycle work supplies deterministic native/model owner evidence for
mixed Draw+Present settlement, reset/teardown, generation-qualified identity,
completion/reclaim, and wake attribution. Together with Task 4's materialization
ledger, that evidence agrees with deferral, but no same-HEAD lifecycle wild
export was captured; the remaining wild observer artifact is explicitly open.
The `1,069,104` restamp checks and zero fires from `.36` remain
observability only: zero fires do not weaken ownership or authorize removing
the frozen-ticket protocol.

## Formal preflight and counterexamples

`specs/verification/tla/QueueT2dReserveCopyCommit.tla` is a bounded
decision-gate model, not a production implementation. It models one charged
reservation over a writing slot that already contains `PrefixBytes`, checkpoints
that prefix, constructs `privateReservationBytes` separately, freezes slot
generation/ticket, and permits `Commit` to publish only checkpointed prefix plus
a complete private reservation. Rollback restores the exact checkpointed prefix,
capacity, and waiter wake, while mark/capture/reclaim-preserving publication
discipline remains unchanged. Rollback and recycle guards are one-shot/bounded
booleans or owner transitions, so TLC cannot loop through unbounded diagnostic
counters.

The final TLC run verifies the production configuration with no error. Its
deliberately broken companions fail for the intended invariants, rather than
`TypeOK`:

| Configuration | Expected result |
|---|---|
| `QueueT2dReserveCopyCommit.half-appended-slot.counterexample.cfg` | `Invariant NoHalfAppendedSlot is violated` |
| `QueueT2dReserveCopyCommit.stale-reservation.counterexample.cfg` | `Invariant NoStaleReservationWrite is violated` |
| `QueueT2dReserveCopyCommit.lost-prefix-rollback.counterexample.cfg` | `Invariant RollbackRestoresPrefix is violated` |

The model's `Reserve` action leaves the writing slot unchanged while copying its
valid prefix into `reservationPrefix`; `Copy` appends only to the private
reservation byte sequence. `Publish` assigns `halfAppendedVisible`, and
`Rollback` assigns the restored slot bytes and `wakeSignaled`, with each omitted
from its corresponding `UNCHANGED` tuple. `scripts/check/verify_tla.sh` runs all
three expected failures as named checks, including the lost-prefix rollback
control.

## Reopen conditions and deferred protocol shape

Reopen T2d only when a matched queue-mutex profile shows `W >=0.5 ms/Present`
(or repeated profiles in the `0.2 <= W <0.5` band), lifecycle observer evidence
identifies the owner/victim relationship, and matched wild safety/error evidence
remains clean. If opened, the protocol must reserve bounded capacity under the queue
mutex; freeze `(slot, generation, sequence/ticket)`; copy/construct only into
private reservation storage outside the mutex; and commit under the mutex after
validating identity and complete count. Publication must not expose a
half-appended slot, stale commits must fail closed, rollback must detach and
restore the exact checkpointed prefix plus capacity/wake waiters, and
mark/capture/reclaim plus publication and wake ordering must remain unchanged.

Until those conditions are met, the append remains serialized and T2d stays
deferred.
