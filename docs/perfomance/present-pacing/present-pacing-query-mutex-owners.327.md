---
domain: present-pacing
workload: STALKER CoP Day
title: "H327: queue mutex owner audit and read-mostly polling gate"
type: evidence
status: partial
updated: 2026-09-19
source: experiments/output/app-d3d9-stkcop-bench-queue-holders-r652-day/result.json; experiments/output/app-d3d9-stkcop-bench-queue-holders-r652-day/xrEngine_dxmt9.log; src/dxmt9/dxmt9_command_queue.cpp; src/dxmt9/dxmt9_queue.cpp; src/dxmt9/dxmt9_device.cpp; src/dxmt9/dxmt9_resource_initializer.mm
related: present-pacing-query-backend-poll-cost.326.md
---

# H327: separate long holders from repeated polling readers

## Method

Commit `767e2bf1`, unchanged runtime binaries from H326. Canonical runner,
Sikarugir cx24.0.7, Day High r2.5 1280x720, perf profile, health counters ON,
validation OFF. `DXMT9_PERF_QUEUE_MUTEX_SPLIT=1`, backend observer OFF,
`DXMT_LOG_LEVEL=info`, timeout 300s, capture delay 120s, output suffix
`queue-holders-r652-day`. No concurrent session build/test/profiler.
Other user/OS activity is not excluded. This is contention triage, not an FPS
comparison or new visual qualification.

The preceding R651 run exited PASS but used the unrecognized
`DXMT9_LOG_LEVEL=info`; the real logger stayed at perf's WARN default and
discarded the Info site rows. Do not use it as holder evidence. R652 corrects
the logger variable, with a separate output directory.

## R652 observed holders

Runner PASS, returncode 0, no timeout or failure markers; window capture
succeeded. No scene inspection or observer-OFF performance comparison is claimed.
The last periodic site report is at 2,460 Presents; the whole-run counter
contains 2,479 submitted Presents.
It contains 19 named rows and no overflow row. Cumulative hold intervals:

| Site | Hold ms | Hold ms / reported Present | Maximum hold ms |
|---|---:|---:|---:|
| `commit_current_chunk/post_wait` | 2865.482 | 1.165 | 8.176 |
| `run_finish_loop/retire` | 913.625 | 0.371 | 0.815 |
| `run_encode_loop/submit` | 849.361 | 0.345 | 1.010 |
| `run_encode_loop/dequeue` | 237.151 | 0.096 | 0.405 |

Publication is the largest **instrumented** holder. The separate whole-run
`prepare_slot_publish_cpu_ms=2556.476` and nested
`prepare_slot_resource_mark_cpu_ms=2552.926` indicate resource marking dominates
that preparation scope. Their end boundaries differ from the periodic table;
do not subtract them from the table to assert an exact remainder or add them
as independent costs.

The synchronous `mark_chunk_resources_and_capture_buffer_bindings` site has
239,266 acquisitions, 4708.181ms wait but only 28.981ms measured hold
(43.5us maximum). It is a substantial waiter, not the largest measured holder.
These are elapsed intervals and not additive CPU-frame budgets. H326's query
waiter and these rows are from separate runs: no causal per-poll join is proved.

## Code ownership

| Segment | Actor and work under the queue mutex |
|---|---|
| `commit_current_chunk/post_wait` | Publishing caller (normally replay): revalidate writer, assign sequence, run `prepareSlotForPublish` including `markSlotResourcesUnlocked`, seal/publish Tape metadata and scheduling bookkeeping. Not the preceding CV wait. |
| `run_encode_loop/submit` | Encoder: retain completion sources, enqueue submission and associated lifecycle bookkeeping. Encoding and post-commit callbacks are outside this segment. |
| `run_finish_loop/retire` | Finish worker: source reclamation transitions and allocator/pool/transient reclamation callback. Detached owner destruction is bracketed outside the queue lock. |
| `run_encode_loop/dequeue` | Encoder: select/validate ready prefix, resolve source identity, commit reservation and prepare worker snapshot. Not GPU encoding time. |
| `mark_chunk_resources_and_capture_buffer_bindings` | Synchronous import path: mark resource use and capture backing bindings; acquisition wait and actual hold must be compared separately. |

These rows do not cover all queue owners. In particular DeviceImpl resource
creation/destruction and managed rotation, initializer entries, query methods,
and portions of direct Slot transactions lack equivalent outer hold coverage.
The observer sink itself can take its table mutex while the queue mutex is
held; hold samples exclude that aggregation delay. Long holds include host
descheduling and are not pure CPU time. No waiter-to-holder identity/time join
exists. The largest measured owner is not proof that it caused a particular
H326 polling wait or a late benchmark stall.

## Read-mostly decision

Keep runtime unchanged in this review. The first candidate is a bound-receipt,
already-covered **Pending-only** fast path, with locked success/initializer
refresh and locked new FLUSH requests. This targets repeated observations
without requiring a coherent lock-free successful result snapshot immediately.
An ordinary release/acquire completion watermark already exists, but stop,
initializer status tracking and request coverage do not share its authority.

The [query design review](../../../specs/d3d9/queries/read-mostly-polling.md)
maps the candidate to existing TLA invariants and lists required stale-read,
reset/terminal, identity, lost-request/wake and initializer counterexamples.
It is an implementation gate, not a claim those extensions are already proved.
Do not delete resource marking or reclamation merely because it appears in a
long-held segment. First split publication marking versus remaining bookkeeping
and close missing owner coverage with bounded, outside-lock aggregation.
