---
domain: present-pacing
workload: STALKER H502/r5 fixed original V4 input
title: "Reserve CPU is concentrated in storage growth, chiefly state/layout arrays"
type: evidence
status: local-reserve-cpu-qualified-live-recurrence-open
updated: 2026-10-16
source: traces/stkcop-reserve-owner-h586/{contract.md,observer.hpp,build.py,build.json,run.py,report.json,analyze.py,analysis.json,on0-reserve.json,on1-reserve.json}; traces/stkcop-reserve-owner-h586/{off0,on0,on1,off1}/{census.json,source.bgrx,output.bgrx}
related: present-pacing-replay-census.585.md; present-pacing-sparse-cpu.584.md; present-pacing-self-counter-boundary.513.md
---

# Storage growth, not no-growth admission, owns this capture's reserve cost

H586 localizes H585's approximately 4.8ms reserve scope. Of 84 ordinary batch
reservations, 29 grow storage and own 4.45–5.16ms of observer-inclusive caller
CPU across two executions. The other 55 reservations have only 13.8–15.2us
combined wall time. The two largest individual reservations grow state/layout
arrays, not uniform storage. This changes the next hypothesis from generic
admission/lookup overhead to actual state/payload storage growth.

These are fresh-process replays of a retained capture. They **do not prove
that the allocation cost recurs in warmed live late frames**, nor that all
growth CPU is removable. No product optimization or default change follows.

## Experiment and controls

One private queue object wraps the unchanged
`ChunkSlot::reserveDrawRunBatchStorage` call. Other runtime archive members,
provider, PE and canonical build outputs are untouched; no Wine staging.
The wrapper records 23 vector size/capacity pairs before/after, requested
counts, queue domain, Slot address/sequence, and calling thread identity.
Storage snapshots are outside the timed interval. Output is written at process
teardown from bounded storage, not in the timed reserve. There are no
per-vector clocks or changes to reservation arguments, ownership or fallback.

Fixed order: off0,on0,on1,off1, one binary pair and runner. Product perf and
append diagnostics remain OFF in all four. OFF takes the same wrapper call
without observations; its added call overhead is not claimed zero. ON reads
the calling thread's own libproc CPU counter before/after each reserve using
H513's validated mechanism. The two reads' complete wall durations and 2us
allowance each are retained in the bounds. These are finite endpoint bounds,
not confidence intervals or unobserved native-product CPU estimates.

All four original controls pass: seven source cuts, nine query issues,
five polls/readiness checks, four query results, 670 created/released wrappers,
restoration/unchanged-normal-state, and exact original source/output BGRX.
Both readbacks retain SHA-256
`1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`.
Query validation is H506's same-production transcript diagnostic, not a new
independent GPU occlusion oracle. All diagnostic snapshot counters are zero
because they are OFF, not because rendering performs no work.

Both ON executions have 84 successful reservations on one caller thread and
queue domain, identical requests and before/after storage shapes, totaling
2,828 draws, 394 new runs and 2,343 conservative uniform allowances. Five Slot
addresses occur. Their observed `seqId` values are zero in this Writing phase;
do not interpret these rows as submission-sequence-qualified source identities.
They are process-local, ordered reserve observations, not a lifetime proof.

Independent analysis authenticates build/image/log/trace hashes and readbacks,
checks scopes, dimensions and shape equality, and rejects reversed CPU/time
and impossible capacity fixtures (four positive and five negative checks).

## Measured reserve ownership

| Group | Calls | on0 caller CPU bound | on1 caller CPU bound | on0 / on1 wall sum |
|---|---:|---:|---:|---:|
| At least one vector grows | 29 | 4.453–4.748ms | 4.856–5.155ms | 4.575 / 5.238ms |
| No vector capacity grows | 55 | 0–0.347ms | 0–0.331ms | 0.0152 / 0.0138ms |

The syscall endpoint allowance dominates the tiny no-growth CPU bounds;
the wall sums provide a tighter single-thread execution-time upper limit.
Do not subtract the bounds or treat the small positive raw CPU deltas as pure
reserve execution: syscall tails are included. Growth rows can contain both
allocation/relocation and lookup initialization; they are not allocator-only
timers. Disjoint groups may be combined within this caller interval, not with
parallel Encode CPU to obtain frame latency.

The largest rows in both executions are the same storage events:

| Reserve ordinal (zero-based) | Event | on0 CPU bound | on1 CPU bound |
|---|---|---:|---:|
| 82 | 506 live states; capacity 512→1,024 | 1.605–1.619ms | 1.460–1.471ms |
| 74 | 255 live states; capacity 256→512 | 0.734–0.744ms | 0.684–0.697ms |
| 63 | Payload arena growth | 0.656–0.667ms | 0.758–0.768ms |

Rows 82/74 grow only run records, PSO subviews, hot state, shader layout and
debug snapshots. They add 5,439,488 / 2,719,744 capacity bytes respectively;
neither grows a uniform dimension. Row82 requests only nine draws and seven
new runs, but crosses the existing state-vector threshold.

## Storage shape, not measured memcpy volume

| Dimension | Element size | Growth calls | Added capacity bytes | Live element footprint exposed to growth |
|---|---:|---:|---:|---:|
| Hot state | 8,112 | 13 | 10,391,472 | 9,515,376 |
| Shader layout | 2,248 | 13 | 2,879,688 | 2,636,904 |
| Payload arena | 1 | 16 | 5,987,184 | 5,359,128 |
| All 23 dimensions | varies | 29 distinct reserves | 24,058,240 | 18,646,216 |

Added capacity is the sum of each local increase, **not RSS, process high-water,
or final retained memory**. The footprint column counts pre-growth live
elements times element size. It is not measured memcpy bytes: C++ relocation,
ownership moves and allocator behavior can differ. Conservative uniform
reservation exists, but these results do not support naming it the main
reserve CPU owner.

`include/dxmt9/core_snapshots.hpp` / `FlatDrawStateRecord` contains fixed
render-state, texture-stage and sampler arrays; `DrawShaderLayoutContext`
contains declaration and shader metadata. `clearCommands` in
`src/dxmt9/dxmt9_backend_types.hpp` clears vectors without shrinking them;
shader-layout ownership has a separate detach/restore protocol. Consequently
the initial capture's growth cannot be assumed to repeat once live storage
has warmed. That transfer is the next discriminating gate, before changing
vector representation, provision budgets, or source boundaries.

## Decision

Close the fixed four-run population. Retain the exact local result and do not
extend it to seek a preferred timing sign. Next check whether state/payload
growth recurs in the selected default live late interval with warmed Slots.
If it does, quantify its caller CPU and address the repeated storage work. If
it does not, reject this cold-reserve candidate as an explanation of the
approximately 29–31ms/frame live worker budget and move to batch preparation/
wait/append or other Replay work. The approximately 9.4ms H585 batch residual
remains separate and unattributed. The overarching goal remains active.
