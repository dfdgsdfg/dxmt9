---
domain: state-churn-encode
workload: 3DMark05 GT2
subcategory: append-decomposition
order: 29
title: Segment Holds Unmask The Owner — The Worker's Slot Append Copy Holds 3.4 ms/present
date: 2026-08-20
type: experiment-run
status: accepted-attribution
source: experiments/output/app-d3d9-3dmark05-qmutex-seg-r1; experiments/output/app-d3d9-3dmark05-qmutex-seg-r2
related: docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.28.md; docs/superpowers/specs/2026-08-20-producer-queue-concurrency-design.md
---

# Segment Holds Unmask The Owner — The Worker's Slot Append Copy Holds 3.4 ms/present

Segment-hold accounting inside the lock-handoff sites (`883ed57c`,
`ae5cdbda`) answers [.28]'s open question. GT2, ~1,741 presents per run.

**Recorded hold ledger (ms/present):**

| segment | samp/p | hold | avg µs | max µs |
|---|---|---|---|---|
| `submit_draw_run_batch_impl/append` (worker: slot copy) | 560 | **3.42** | 6.1 | 9,869 |
| `submit_draw_run_batch_impl/mark` (worker: pool stamps) | 560 | 0.93 | 1.7 | 828 |
| `commit_current_chunk/post_wait` (slot publish) | 1 | 0.83 | 834 | 1,868 |
| `run_encode_loop/submit` | 1 | 0.43 | 429 | 999 |
| `map_buffer/finalize` | 21 | 0.28 | 13.1 | 722 |
| `mark_and_capture` (producer body) | 16 | 0.16 | 10.0 | 240 |
| everything else | — | ~0.15 | — | — |

Total recorded hold ≈ 6.1 ms/present — a ~17% duty cycle on the queue mutex,
dominated by the replay worker's per-batch **append** (the
`appendDrawRunBatch` slot copy) at 560 batches/present, with marking second.
This is fully consistent with the producer's measured ~38 µs average acquire
wait: it queues behind 6-µs appends routinely and occasionally behind the
multi-ms spikes (append max 9.9 ms, publish 1.9 ms).

**Why append cannot simply leave the lock.** The writing slot is NOT
worker-exclusive: the producer's map-wait path
(`mapBuffer` → `queueLifecycle_.commitCurrentChunk`) can force-publish the
current writing slot to unblock a readback (Wine #66 visibility). Unlocked
appending could let a forced publish ship a half-appended slot. The fix is a
protocol, not a code motion:

- **Reserve-copy-commit** (the design's next increment): under the lock,
  bump-allocate the batch's region in the slot and take a ticket; release the
  lock; copy draws/payloads into the reserved region; re-acquire briefly to
  commit the region (advance the slot's valid watermark). Publishers ship
  only the committed prefix — a forced publish during a copy ships the slot
  without the uncommitted tail, which the worker then appends to the next
  slot. Needs a small formal model of slot append/publish interleaving
  (extends the QueueLifecycle family) before implementation, per the
  producer-concurrency track's discipline.
- **Worker marking → arena-stamp exception** (0.93 ms, plus the producer's
  own 0.16 body): the pool header's documented queue-mutex-free stamp path.
  `ProducerMarkReclaim.tla` needs a symmetric `WorkerStampMark` action — the
  worker holds its chunk's retained refs until `releaseRetainedWrappers`
  after replay, so the same pin-ordering argument applies verbatim.

**Expected shape after both:** queue-mutex duty cycle drops from ~17% to
~2-3% (publish + finalize + residuals), and the producer's ~1.0 ms/present
acquire wait should collapse proportionally — directly measurable with the
same profiler before any fps claim.
