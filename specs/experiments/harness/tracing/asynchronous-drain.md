---
type: "Spec"
title: "Bounded Trace Producers and Asynchronous Drain"
description: "Owned batch transport, cold output work and measurable observer overhead."
tags: [specs, tracing, perfetto, concurrency, overhead]
---

# Bounded Trace Producers and Asynchronous Drain

Defines R-HARN-TRACE-16.1–16.6 as an extension to the
[engine capture lifecycle](spec.md#63-session-ordering) and
[shared diagnostic consumers](remaining-consumers.md). This is a target
contract; implementation and qualification are tracked in [gap](gap.md).

## Modes and ownership

Deferred mode retains bounded records and serializes after the measurement
window closes. It remains the default and the capture-only observer control.

The integrated async topology persists owned binary batches during collection.
Canonical GPU lane assignment, cross-batch relations and native Perfetto export
require the whole captured session and run on the diagnostic controller after
close. The spool worker performs persistence and recycling; it does not stream
an independently projected native timeline. Both threads are diagnostic owners,
and neither may invoke production work to expedite completion.

The frozen `transport` object selects `mode` (`deferred` or `async`),
`batch_events` (default 64, maximum 4096), `service_interval_ms` (default 10,
maximum 1000) and `transport_events` (default 16384, maximum 65536, at least
four batches). The default async layout has 64 producer pairs and two 4096-event
whole-report slots. A slow consumer causes explicit loss instead of blocking a
producer. Receipts disclose the actual producer/report capacities and spool
digest. Deferred mode creates no spool worker or spool artifact.

The configured byte envelope charges retained events, async transport and a
conservative 8192-event reservation for process-wide full-report scratch. Raw
spool size is bounded by the retained-event ceiling. Cold canonical copies and
SDK buffers retain their separately declared export bounds. Changing mode or
capacity can change retention and is not by itself an overhead improvement.
Async mode trades retained-event capacity for concurrent spool CPU, memory
bandwidth and I/O. It carries no automatic overflow fallback and no lower-cost
claim follows from thread separation. The mode is frozen in request, receipt
and experiment/test provenance.

| Owner | Responsibility |
|---|---|
| Existing collector or sampler | Acquire once at the original gate/cadence; retain population and failure semantics. |
| Producer facade | Check gate, capture observation identity/time, copy bounded POD, publish an owned batch. |
| Unix session controller | Preallocate/register, freeze budgets, own worker readiness, admission and teardown. |
| Async spool worker | Consume immutable batches and persist bounded binary event records during collection; retain ownership on partial/failed writes. |
| Diagnostic controller | After close, reconstruct the event sequence, assign canonical GPU lanes/relations, export native Perfetto and publish receipt/proof. |
| Offline harness | Verify raw/native parity, run Trace Processor, parse structural evidence and publish immutable SQLite/provenance. |

Only async mode starts a worker, which acknowledges spool readiness before
collection. Deferred mode retains bounded events and creates no worker or spool
artifact. SDK-off and runtime-off paths create no tracing worker. Native
Perfetto export runs after close on the controller in both modes; the async
worker does not assign GPU lanes, resolve cross-batch relations or serialize
Perfetto. The controller remains the single lifecycle authority. Record the
actual topology and all associated resource budgets.

## Producer boundary and report snapshots

Frequent events append to preallocated per-producer blocks with a single
producer and a single diagnostic consumer. Thread registration/schema interning
use the cold path or a preallocated bounded slot claim. Unknown schema/thread
or unavailable capacity produces explicit coverage loss, never lazy allocation.
Writer lifecycle admission uses one atomic `fetch_add` probe and a paired
decrement on rejection; it does not retry a CAS loop. Deferred shared-diagnostic
partition reservation uses a separate bounded four-attempt CAS protocol.

Async producers publish on the frozen batch-size threshold. The worker services
partial batches under an explicit periodic/request protocol; it cannot read a
block still owned by a producer. Notifications are coalesced on batch
publication, never issued once per event. Producer threads never spin or wait
for worker acknowledgement. Deferred mode retains its bounded event buffers
until close. Each publication path must remain nonblocking for producers.

Timestamp, source clock/bracket, order, session generation, process/thread
incarnation and work identity are captured at the observation site. Consumer
dequeue time is a separate transport metric, never the event timestamp. Borrowed
pointers, callbacks into producer state and deferred reads of mutable counters
are forbidden. No extra OS sample, percentile evaluation or renderer traversal
is introduced for the asynchronous consumer.

The full perf table reserves a bounded snapshot batch before trace projection.
Each original load/percentile result supplies both its compatibility consumer
and the snapshot, including sample counts and aggregation semantics. The
producer projects those captured values into a fixed typed batch and admits it
as one contiguous observation batch; cold export resolves display text and
native records afterward. This preserves existing sequential observation
semantics; it does not claim the whole table was acquired atomically at one
instant. Admission failure drops the trace batch as a unit while legacy
acquisition/output proceeds independently.

Trace-only string formatting, fragmentation and display conversion stay on the
cold export path after collection. In async mode the worker only persists owned
batch bytes. Existing legacy file output and its cost remain attributed to that
consumer. Opaque artifact references become valid only after the owning writer
closes/checks the artifact; this transport does not defer reads of mutable
renderer structures.

PE transport remains bounded, versioned, pointer-free and generated. Batch
publication must not add a bridge call per D3D9 operation. Native receipt time
for a PE report remains receipt time; asynchronous delivery cannot upgrade it
to a sampled-PC timestamp. Preserve LastError and guest/native identity rules.

## Publication, loss and teardown

Each async transport block follows exclusive ownership transitions:

```text
Free -> ProducerOwned -> Published -> ConsumerOwned -> Free
```

Publication release/acquire ordering exposes only initialized records. A
consumer may recycle a block only after its complete record bytes have been
persisted to the bounded spool. Blocks retain generation and publication
sequence across the handoff; stale handles cannot enter a new session. The
controller later reads the sealed spool to reconstruct the canonical event
sequence and serialize the native artifact. Delayed serialization preserves
producer tracks and original clocks rather than worker emission-time SDK macros.

All storage is budgeted: producer blocks, batch descriptors, report scratch,
schema/track registries, in-flight consumer blocks, SDK buffers, output bytes
and finalization scratch. Freeze size/time/service thresholds before arming.
A blocked async spool sink exhausts the bounded pool and records loss; it cannot
backpressure production or trigger an unbounded allocation. SDK/export failure
after close marks the final artifact incomplete.
Per-source dropped-record/batch counts and an out-of-band failure latch survive
pool exhaustion. Partial SDK/file output is retained as incomplete evidence,
not silently retried as a second successful capture.

Closing stops new admission and publishes final partial blocks only after
their admitted writers finish. Async mode then waits for published batches and
worker spool closure. The controller waits for applicable GPU observation
tickets, reconstructs events, finalizes/flushes/stops the SDK, verifies loss and
output status, and seals artifacts plus receipt. All waits occur on the
diagnostic/control path, outside loader callbacks and production locks.
Deadlines mark incomplete output; they do not permit freeing live blocks,
unloading callbacks or reusing a generation. Crash/exit without a successful
receipt remains incomplete. Tracing must never force a production queue/GPU
drain, change query predicates or wake a production wait to expedite shutdown.

Completion has a finite decision boundary. The controller first durably publishes
a candidate receipt, then checks the absolute deadline and atomically decides
complete/incomplete. A separate `receipt.json.completion.json` proof binds that
decision, generation, request digest, candidate digest and decision/deadline
timestamps. New candidate receipts set `proof_required`; a candidate alone never
qualifies. Missing, late-decision, incomplete or mismatched proofs are rejected.
Publishing the already-decided proof can itself block in filesystem I/O, so it
is outside the decision budget; there is no hard filesystem latency guarantee.
The owner stays retained and rearm remains closed until proof publication ends.
Stop callers use bounded control acquisition and observe the decision; offline
consumers additionally require the durable proof before qualifying artifacts.

## Verification and overhead qualification

| Requirements | Required evidence |
|---|---|
| 16.1–16.2 | Disabled-path audit; allocation/lock/formatting instrumentation; bounded registration/admission; no per-event wakeup; SDK-off/on topology and budget checks. |
| 16.3 | Reused mutable source fixture, original-time delayed drain, exact UINT64/percentile populations, bounded full-table batch and legacy-only/trace-only/dual-consumer parity. |
| 16.4–16.5 | Deterministic slow/failed sink, pool exhaustion, partial batch, late writer, publication/recycle race, stop/restart, stale generation and deadline fixtures; lifecycle model/code binding including intentional counterexamples. |
| 16.6 | Pinned raw/native/SQLite round trip for both modes, output-loss rejection, matched repeated workload controls and immutable experiment/test provenance. |

Extend observer lifecycle verification under R-VERIF-1.1–1.4 for publication,
recycling and shutdown; deterministic safety fixtures do not prove live timing.
Production rendering/retirement semantics are unchanged by contract. Any change
to those semantics requires the owning rendering refinement evidence first.

Compare identical acquisition/category/window selections in observer-off,
capture-only/deferred-output and asynchronous-output controls. Keep legacy
consumer selection fixed and declare whether SDK services were started. Record
event volume, logical D3D9/bridge/chunk counts, producer call cost and CPU,
worker CPU, notification count, peak memory and buffer occupancy, drain latency,
SDK/source/output loss, frame throughput and frame-time p50/p95/p99. Diagnostic
self-measurement must itself be bounded and its instrumentation declared.

Freeze numeric producer/worker/memory/loss and frame-time regression budgets
before collection, use repeated workload-qualified runs, and report dispersion
and correctness outcomes. Instrumented microbenchmarks establish emitter cost;
live Wine/Metal runs establish workload interference. A faster producer with
worse frame tails or relevant loss does not pass the workload budget. Do not
subtract estimated overhead from measured durations to manufacture a baseline.

`scripts/tools/qualify_trace_observer.py` checks supplied repeated-control run
records against a separate numeric budget file. The budget's timezone-qualified
`frozen_at` must precede every run; it has no built-in pass thresholds. Provide
at least three matched repetitions for each of `observer-off`, `deferred` and
`async`, with identical workload/build/runtime/renderer/profile/selection
provenance, passing correctness, and source references for every metric. Each
run records complete frame-time samples and process/producer/worker CPU, memory,
buffer occupancy, notifications, bridge counts and source/SDK/output loss.
Unavailable metrics remain absent and fail the gate. The tool consumes evidence;
it does not run Wine workloads or promote native fixtures to workload evidence.
