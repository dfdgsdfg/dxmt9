---
domain: present-pacing
workload: STALKER CoP Day high max-threads 2
title: "H371: Tuned tail still starves replay and encode"
type: evidence
status: partial-wait-attribution
updated: 2026-09-26
source: traces/app-d3d9-stkcop-tuned-tail-h371/{receipt.jsonl,supply.csv,supply-analysis.json,wait.csv,sample.txt,run.py}; experiments/output/app-d3d9-stkcop-bench-tuned-tail-h371/{result.json,dxmt9.log}
related: present-pacing-xray-worker-abba.370.md; present-pacing-xray-worker-yield.369.md
---

# H371: Keep the workload tuning; follow the guest wait owner

The user elected to retain `-max-threads 2` as STALKER-specific tuning.
`experiments/README.md` now records its explicit invocation and baseline
separation. This does not change the catalogue default, dxmt9 thread policy,
other games, or README benchmark values.

A separate perf-enabled diagnostic run uses that setting with bounded supply
and wait traces at Presents 2700–2720, plus a three-second native sample
triggered at observed frame 2701. It passes with zero reported GPU errors.
No build, tests or other experiment ran concurrently. This is not another
observer-OFF throughput measurement.

## Qualified bounded supply evidence

Supply closes with zero overflow/incomplete events. Trimmed boundaries are
2702–2718: 16 frame intervals over 1595.509ms (diagnostic window 10.028 FPS).

| Metric | Observation |
|---|---:|
| App thread CPU / Present | 43.984ms |
| App core utilization | 0.452 |
| Replay thread CPU / Present | 23.439ms |
| Replay core utilization | 0.241 |
| Encode thread CPU / Present | 21.623ms |
| Encode core utilization | 0.217 |
| Replay input wait / Present | 73.128ms |
| Replay queue empty fraction | 77.84% |
| Encode unpublished-input gap / Present | 77.637ms |
| Encode ready dispatch/tail gap / Present | 0.285ms |

Thread envelopes have slightly different endpoints; scopes overlap and must
not be added. App CPU includes game, Wine, PE and synchronous Unix. These
figures are not directly comparable to H367 as a tuning delta: frame ordinal
does not align scenes across runs with different throughput.

Wait trace reports `closed=1`, `overflow=0`, `incomplete=0`, `unknown=0`,
`cv_entries=0` and no rows. Its hooks cover query receipt, resource replay,
buffer lock/unlock and query queue acquisition, not all waits in the process.
An empty trace cannot rule out Wine/game waits or uninstrumented Present waits.

## Native stack corroboration

The same-process native sample records 188 observations per selected thread:

| Thread | Observed stack |
|---|---|
| X-RAY Primary, TID 13336246 | 175 observations in `NtWaitForAlertByThreadId +387`, previously mapped to kevent |
| X-RAY Secondary, TID 13336854 | 187 observations in the same native wait API |
| Replay, TID 13336853 | 181 observations in `ReplayOffloadQueue::pop` condition-variable wait |
| Encode, TID 13336849 | 182 observations in `QueueLifecycleController::reserveReadySlotBatchPrefix` condition-variable wait |
| Remaining helper, TID 13336563 | 108 observations in `NtYieldExecution` |

Replay and encode are waiting for input, rather than these sampled stacks
showing long execution inside encodeDraw. The primary and secondary reach the
same wait API, but **not necessarily the same wait object or dependency**.
Wine's repeated dispatcher unwind still hides the guest caller. No binding to
audio, a game critical section, dxmt9, or the helper-completion barrier is proved.

The sample lasts three seconds and starts after the supervisor observes frame
2701; supply is a different bounded interval. Sampling itself perturbs the
process. Stack proportions are neither CPU shares nor exact wall-time wait
fractions and should not be reconciled numerically with the supply CPU table.
The supervisor uses POSIX monotonic time; its log observations still do not
provide exact frame/Present identity joins.

## Decision

Do not choose encode parallelism or remove queue waits as the next tail fix
from this window. Input starvation and guest-thread waiting remain the more
direct leads after worker tuning. Next capture a bounded **guest caller/stack
and wait-object identity** for the primary and secondary, then identify the
waker. Distinguish a game synchronization dependency from Wine scheduling and
dxmt9 callbacks before changing semantics. Such stopped-thread diagnostics
must remain outside performance qualification and preserve balanced resume
on every failure path. Full audio correlation remains open.
