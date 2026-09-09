---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "Queue and buffer arena contention: owners, amplification and narrowing candidates"
type: evidence
status: code-audited
updated: 2026-09-09
source: experiments/output/upload-arena-locks-H306-20260909/analysis/lock-summary.json; experiments/output/replay-baseline-H303-20260909/analysis/replay-baseline-summary.json; experiments/output/app-d3d9-3dmark05-app-thread-waits-H305-gt2-diagnostic-r1-20260909/app-stack-30s.sample.txt; experiments/output/app-d3d9-3dmark06-app-thread-waits-H305-gt2-diagnostic-r1-20260909/app-stack-30s.sample.txt
related: present-pacing-upload-arena-locks.306.md; present-pacing-app-thread-waits.305.md; present-pacing-replay-baseline.303.md
---

# Queue and buffer arena contention audit

This is a read-only follow-up on `1f3f0358` plus the H306 diagnostic overlay,
not a new benchmark or synchronization implementation. It re-examines the
existing evidence independently of FPS: acquisition count, blocked supply,
critical-section length and progress matter even without a frame-rate change.

## Confirmed expensive shapes

| Evidence | 05 GT2 | 06 GT2 | Attribution limit |
|---|---:|---:|---|
| H306 app upload queue acquisition, ms/P | 1.123 | 0.905 | Acquisition latency, not pure blocked time |
| H306 non-app arena exclusive updates/P | 20,684.882 | 22,552.159 | Operation/TID aggregate, not a caller count |
| Same non-app arena acquisition, ms/P | 2.036 | 1.852 | Includes uncontended acquisitions |
| Same non-app arena hold, ms/P | 2.010 | 2.204 | Does not include unlock or sink recording |
| H303 queue `commit_current_chunk/post_wait` hold, ms/P | 1.043 | 0.965 | Historical diagnostic; whole segment, not resource-mark exclusive |
| H303 queue `run_encode_loop/submit` hold, ms/P | 0.716 | 0.364 | Historical diagnostic; not actual encode duration |

H306 uses the exact windows documented in its leaf. Its high-update native
TIDs are 48163752/48192038. Neither has an authoritative role marker in that
run, so they are not relabelled replay from thread creation order. H305's
separate all-thread samples do prove that replay reaches both
`submitDrawRunBatch` and `prepareSlotForPublish` / `markBufferSnapshotUse`.
These older TIDs and samples cannot identify H306's individual wait holders.
The H303 windows are `(60,1440]` / `(720,1800]`; its product images differ from
H306. Do not combine the three experiments into a single additive CPU budget.

### 1. Upload takes a broader lock than its pool operation requires

In `src/dxmt9/dxmt9_device.cpp`, `DeviceImpl::uploadBufferDataRange` acquires
`queue_.mutex_`, then calls the pool range upload while still holding it.
`Pool::uploadBufferDataRange` in `src/dxmt9/dxmt9_resource_pool.cpp` explicitly
discards both `device` and `completedSeqId`: it validates and updates only the
buffer-arena-protected record. The pool API documents no queue-lock requirement.
Thus the range path has a concrete lock-scope mismatch, not just an expensive
memcpy hypothesis. Its H306 app acquisition is 0.585/0.159 ms/P while queue
hold is only 0.116/0.008 ms/P.

Full upload and `rotateManagedBufferForMutation` differ: they use the completion
watermark to choose backing reuse/allocation. `completedSeqIdAcquire()` already
provides a conservative read for the map fast path. Extending that protocol is
a candidate, not a consequence proven by the current MapFastRead model. A
stale-low read can affect backing allocation; arena serialization, retained
resource lifetime, replay residency and producer API order must remain intact.

### 2. Buffer stamping expands into repeated arena acquisitions

In `src/dxmt9/dxmt9_command_queue.cpp`, `submitDrawRunBatch` batches/deduplicates
logical state handles, but `markDrawBindingSnapshotResources` still walks
every payload and invokes `Pool::markBufferSnapshotUse` separately per binding.
Override marks likewise use individual `markBufferUse` calls. Each reaches
an exclusive arena update. The existing batch marker covers logical/active
backings; it is not a drop-in replacement for captured-backing marking.

At publication, `prepareSlotForPublish` unconditionally calls
`markSlotResourcesUnlocked`. `visitSourceCommandResources` in
`src/dxmt9/dxmt9_source_payload.hpp` visits run state, each draw's overrides,
and each draw's captured snapshots, without a unique-resource closure. This
revisits the same classes already marked at ingress, now with the final slot
sequence. H305 samples show this path inside replay's commit publication.

This is the concrete amplification mechanism to investigate against H306's
20k-plus update population. **The measurements do not yet prove what fraction
is duplicate, nor assign all updates to snapshot stamping.** A safe reduction
must distinguish `(handle generation, logical/active vs captured backing,
final seqId)` and retain every backing's waterline. Ingress marking at an
older ticket cannot replace final publication stamping. Do not replace the
visitor with logical-handle-only dedup or suppress publication marks wholesale.

### 3. One queue hold couples replay materialization to unrelated uploads

`submitDrawRunBatch` holds the queue mutex across binding preparation, payload
size calculation, segment planning, storage reserve, resource stamping and
final Slot append, except for explicit lifecycle release/reacquire boundaries.
The existing `submit_draw_run` outer probe deliberately skips hold timing;
its internal lifecycle segments do not cover all preparation/append work.
Therefore H303's table is not a complete ranking of queue holders.

`commit_current_chunk/post_wait` includes final-slot resource marking; despite
its name it measures held work after the CV predicate, not GPU waiting.
`run_encode_loop/submit` covers post-encode source validation and submission
bookkeeping. Encoding itself runs with the queue mutex released. None of these
rows establishes that a particular upload waited on that exact operation.

The buffer arena has one shared mutex for the entire arena, not one per buffer.
Consequently independent buffers compete too. Long copy/rename work holds this
same arena lock, while frequent short stamps repeatedly acquire it. H305 also
samples shared-mutex unlock/internal-mutex/notification work. H306 ends hold
timing before unlock, so it is not a full acquire/release cost measurement.

## Next bounded proof and measurement

1. Close the range-upload queue-lock necessity audit first; then separately
   prove the full-upload conservative watermark read. Preserve bounds,
   NOOVERWRITE/DISCARD admission, capture visibility, failure and reclaim.
2. Count stamping by caller phase and exact qualified identity before choosing
   a reduction. The existing resource-mark overlap ledger can establish
   coverage/duplicates in a separate diagnostic run, but has its own mutex and
   must not be used to price unperturbed contention.
3. Add bounded lock-instance/native-TID/owner-site intervals, including
   release/reacquire and failed acquisition, for waiter/holder overlap.
   Account for missing holders, shared readers, dropped records and unlock
   cost. A sampled owner at wait start is not the owner for the whole wait.
   Keep timing sinks outside observed locks and disabled paths cold.
4. Use exact-sequence/backing-qualified closure and bounded batch stamps to
   reduce acquisition count. Do not exchange many tiny locks for one unbounded
   frame-long hold. Prepare immutable data off-lock only with a valid retained
   source/slot transaction and revalidation before publication.

Acceptance is fewer unnecessary acquisitions and lower independently measured
wait/hold tails and synchronization CPU, with no progress/lifetime regression.
TLA/native refinement must retain monotone waterlines, pin-before-mark,
stamps-before-capture, exact backing coverage, stale-generation rejection and
completion-before-reclaim. A spinlock substitution or FPS-only result does not
discharge these obligations. No product lock has been removed in this audit.
