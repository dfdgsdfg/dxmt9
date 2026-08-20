---
domain: state-churn-encode
workload: 3DMark05 GT2
subcategory: append-decomposition
order: 28
title: Mark Decomposition — 61% Is Queue-Mutex Wait, And It Is Not Frequency Contention
date: 2026-08-20
type: experiment-run
status: accepted-attribution
source: experiments/output/app-d3d9-3dmark05-mark-split-r1; experiments/output/app-d3d9-3dmark05-qmutex-r1; experiments/output/app-d3d9-3dmark05-qmutex-nocache-r1
related: docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.26.md
---

# Mark Decomposition — 61% Is Queue-Mutex Wait, And It Is Not Frequency Contention

Three instrumented GT2 runs against the new sub-splits (`15ae4c09` mark
sub-phases, `48ed259e` per-site queue-mutex profiler).

**1. The mark phase decomposes into waiting** (`mark-split-r1`):
`mark` 63.3 µs/call = **acquire-wait 38.7 (61%)** + pool-mark/capture body
~11.6 + PE-side dedup 10.7 (277 handles/call after the cadence promotion) +
sort 1.6. [.06]'s question — acquire or body — is answered: acquire. The
fixed default-path `mark_lock` wiring (previously only the legacy lane
counted it) is what made this readable.

**2. The per-site profile names the whole field** (`qmutex-r1`,
`DXMT9_PERF_QUEUE_MUTEX_SPLIT`): total acquire-wait across threads
2.14 ms/present. Game-thread victims: `mark_and_capture` 0.601 +
`map_buffer` 0.415 ≈ **1.0 ms/present of producer wall in mutex wait**
(cross-validates `commit_chunk_phase_mark_lock` 0.617 exactly). Loudest
acquirer: `find_reordered_index_buffer` — the index-cache provider's
per-eligible-draw lookup on the encode thread, 405.9 acquires/present,
each a full `bufferArena_.update` with an eviction `remove_if` under the
global queue mutex.

**3. Frequency contention refuted** (`qmutex-nocache-r1`,
`DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE=0`): with all 406 cache-lookup
acquires removed, `mark_and_capture` wait is unchanged (0.601 → 0.610) and
`map_buffer` unchanged (0.415 → 0.418). The producer is not queueing behind
many short holds; it is colliding with **long holders hidden at the
hold-unaccounted sites** (cv-wait/lock-handoff sites where hold time is
deliberately not guessed): the completion/finish loop's under-lock retire
work, `submit_draw_run_batch_impl`'s per-batch hold (22.6/present), and
`map_buffer`'s own hold are the suspects, in that order.

**Where this leaves the track.** The producer's ~1.0 ms/present mutex wait
(≈ +2.8% GT2 if fully removed) has two fix shapes, both now properly scoped:

- **(a) Identify and shorten the long holders** — needs one more instrument
  increment: segment-hold accounting at the handoff sites (finish loop,
  draw-run batch submit, map_buffer). Local fixes, no semantic risk.
- **(b) Take the producer off this mutex entirely** — commit-time marking is
  a per-resource `lastUsedSeqId` stamp (atomic-izable) and binding capture;
  the deferred-marking split already exists for the cpuReadyTape lane
  (`captureChunkBufferBindings` + worker-side `markLegacyResources`). This
  eliminates the wait regardless of holders but touches resource-lifetime
  semantics — `rendering_correctness.rules.md` evidence ladder applies
  (model or exhaustive pin of the mark/reclaim race, native truth table,
  then wild).

Secondary sized items from the same runs: PE-side dedup 0.17 ms/present
(O(n²) over 277 handles — sort+unique/hash, bundle with whichever fix
lands), snapshots-vector churn, and the eviction scan inside every
`find_reordered_index_buffer` lookup (encode-side, slack-covered today).
