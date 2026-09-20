---
domain: present-pacing
workload: STALKER CoP Day
title: "H333: pre-fragmentation app-thread occupancy and polling ownership"
type: evidence
status: partial-attribution
updated: 2026-09-20
source: experiments/output/app-d3d9-stkcop-bench-pre-split-wait-owner-r669-day/result.json; experiments/output/app-d3d9-stkcop-bench-pre-split-wait-owner-r669-day/run-policy.json; experiments/output/app-d3d9-stkcop-bench-pre-split-wait-owner-r669-day/dxmt9.log; experiments/output/app-d3d9-stkcop-bench-pre-split-wait-owner-r669-day/thread-cpu.jsonl
related: present-pacing-query-scope-cb-composition.332.md; present-pacing-stalker-scope-fragmentation.331.md
---

# H333: separate app-thread occupancy from query wait ownership

R669 uses the same five images as R666–R668, Day High 1280x720,
Sikarugir, actual occlusion/async fences/covered Pending/publication dedup ON,
END coalescing OFF. Tape and scoped CB joining are OFF: this investigates the
ordinary production path, not the candidate's performance. Receipt/backend
observers sample elapsed phases at stride 64; split census and frame sampling
are ON, log level Info. PE SuspendThread sampling is OFF. The run passes with
zero chunk/command-chunk rejects or GPU errors and a plausible scene capture.
Its FPS is diagnostic, not a new performance reference. Host `dasd` remains
near one CPU core; off-CPU time can include scheduling interference.

## App-thread CPU grows before the large split increase

External native-thread observation targets PID 17583 and the logged commit
thread, transport 1. It retains 372 successful samples and one final
`PROC_PIDTBSDINFO`/ESRCH error after process exit. The observer exits nonzero
and does not convert the missing tail to zero. `summarize()` from
`scripts/tools/observe_darwin_thread_cpu.py` includes only adjacent stable
frame endpoints and rejects unstable/out-of-range endpoints. CPU includes
game, Wine, PE and synchronous Unix; it is not PE-exclusive.

| Log-frame bracket [begin,end) | Covered frame deltas | App CPU ms/frame | Mean cores | Tail CBs / publication Present | Scope publications / publication Present |
|---|---:|---:|---:|---:|---:|
| 200–700 | 333 | 34.59 | 0.902 | 2.018 | 0.014 |
| 1250–1750 | 276 | 30.59 | 0.871 | 4.942 | 2.942 |
| 1750–2100 | 276 | 45.18 | 0.787 | 3.799 | 1.799 |
| 2100–2500 | 339 | 61.34 | 0.581 | 48.301 | 46.300 |

Split columns use differences between the first/last cumulative publication
snapshots observed in each log-frame bracket. Scope publications are the sum
of `scope_empty` and `scope_nonempty` **extent**, not membership-change counts.
CB counts are completion-bearing tails, excluding sub-CBs. Publication Present
ordinals and encoded frame ordinals are distinct; their log-order association
is approximate, not an identity-qualified timestamp join. CPU coverage is a
different partial population and must not be summed with elapsed counters.

The 1750–2100 bracket costs more app CPU despite fewer scope publications
than 1250–1750. The later bracket has both increased CPU and reduced occupancy.
Thus CB fragmentation alone is insufficient to explain the earlier slowdown.
The remaining non-running fraction is **not** proven GPU wait time: it also
contains synchronization, scheduler delay and other application waits.

## What the query observer establishes, and where it stops

Final receipt census: 560,474 attempts, all FLUSH; 25,829 S_OK, 534,644
S_FALSE, zero errors, one fallback. Backend census: 560,473 polls,
533,600 covered-Pending fast-path returns and 26,873 slow-path calls.
There are no deferred markers. Final 8,758 sampled receipt attempts contain:

- captured-raw replay phase: 23.247ms;
- backend phase: 18.963ms;
- snapshot acquisition/copy: 1.931ms;
- capture: 1.690ms; final checks: 0.957ms.

Independently sampled backend queue acquisition totals 2.635ms, maximum
0.791ms. These are sampled elapsed sums, not CPU, whole-run totals or a
removable FPS budget. The receipt/backend sampling tickets differ, so their
sampled sums cannot be subtracted to derive exclusive bridge time. The cold
observer aggregation locks and logging themselves are outside timed phases.

Crucially, the 64th periodic receipt report is already emitted at log-frame
1234 (262,144 attempts). Only the final summary follows. The final-minus-last
delta mixes the entire remaining run; it cannot identify the 1750–2100 wait
owner. For example, reports bracketed by frames 1111 and 1234 contain 65,536
calls / 1,024 samples but only 0.134ms in sampled replay phase and 4.059ms in
backend phase. This does **not** exclude infrequent first-poll stalls: fixed
stride sampling amid repeated Pending polls can miss them.

## Code ownership audit

`pollQueryReceiptImpl()` in `device_c_swapchain_query_stateblock.cpp` captures
the query's raw sequence, then calls `ReplayDrainLedger::waitReceipt()`.
The latter takes the replay-ledger mutex and condition-variable-waits until
that captured target becomes ready, superseded or failed. Its observer phase
mixes mutex acquisition and replay-progress waiting. It does not wait for GPU
completion or all later raw sources.

`CommandQueue::pollQueryFence()` checks qualified coverage and completion
atomics for Pending. Successful refresh, terminal classification and uncovered
submission obligations use `pollQueryFenceImpl()` under the queue mutex.
FLUSH requests submission and returns Pending; it does not call
`waitUntilCompleted()` here. The first slow poll can contend with publishers
and encoders; covered polls bypass that mutex. S_FALSE itself establishes
unreadiness, not a blocked thread or GPU saturation.

StateBlock bridge elapsed and other synchronous bridge totals include their
drain/queue work and computation; this experiment does not isolate their late
cost. Nor do the previous Rosetta module samples identify a game/Wine wait
caller. No game-worker or Wine primitive is assigned the unexplained time.

## Next discriminating measurement

Use a bounded log-frame window around the pre-split region, not another
full-run fixed-stride polling report. Record only genuinely blocked ledger
waits with captured raw identity, entry/exit monotonic times, replay wake
identity and separate mutex-acquisition time. Join replay completion to its
queue publication/space wait owner; include synchronous bridge site labels
for StateBlock, buffer lock and commit. Keep emission outside locks and
preserve observer-OFF behavior. Compare this with external app-thread CPU
over the same window; unattributed intervals remain game/Wine/scheduler
unknown until a valid guest-aware wait stack or equivalent evidence exists.

This narrows the next instrumentation target; it does not complete exclusive
pre-fragmentation wait attribution or justify removing any lifetime/progress
boundary.
