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
Explicit asynchronous mode allows a unix diagnostic worker to drain published
batches during collection. It trades retention capacity for concurrent CPU,
memory-bandwidth and I/O interference; no lower-overhead claim follows from
thread separation alone. The chosen mode is part of the immutable request,
receipt and experiment/test provenance, not an automatic overflow fallback.

| Owner | Responsibility |
|---|---|
| Existing collector or sampler | Acquire once at the original gate/cadence; retain population and failure semantics. |
| Producer facade | Check gate, capture observation identity/time, copy bounded POD, publish an owned batch. |
| Unix session controller | Preallocate/register, freeze budgets, own worker readiness, admission and teardown. |
| Unix diagnostic worker | Consume immutable batches, format trace-only reports, adapt/serialize Perfetto packets and persist bounded output. |
| Offline harness | Verify raw/native parity, run Trace Processor, parse structural evidence and publish immutable SQLite/provenance. |

The worker starts and acknowledges readiness before collection. SDK-off and
runtime-off paths create no tracing worker. SDK internals may use their own
threads; the controller remains the single lifecycle authority. An existing
SDK buffer can satisfy a transport stage only when its bounded admission,
ownership and loss behavior meet this contract. Avoid redundant queue/copy
stages; record the actual topology and all associated resource budgets.

## Producer boundary and report snapshots

Frequent events append to preallocated per-producer blocks with a single
producer and a single diagnostic consumer. Thread registration/schema interning
use the cold path or a preallocated bounded slot claim. Unknown schema/thread
or unavailable capacity produces explicit coverage loss, never lazy allocation.
Low-frequency shared diagnostic admission may use bounded atomic attempts;
unbounded CAS retry loops do not satisfy bounded producer work.

Publish on a frozen batch-size threshold. The worker services partial batches
under an explicit periodic/request protocol; it cannot read a block still owned
by a producer. Notifications are coalesced on batch publication, never issued
once per event. Producer threads never spin or wait for worker acknowledgement.
Any publication primitive must have a qualified nonblocking producer path.

Timestamp, source clock/bracket, order, session generation, process/thread
incarnation and work identity are captured at the observation site. Consumer
dequeue time is a separate transport metric, never the event timestamp. Borrowed
pointers, callbacks into producer state and deferred reads of mutable counters
are forbidden. No extra OS sample, percentile evaluation or renderer traversal
is introduced for the asynchronous consumer.

The full perf table reserves a bounded snapshot batch before trace projection.
Each original load/percentile result supplies both its compatibility consumer
and the snapshot, including sample counts and aggregation semantics. The worker
expands that immutable batch into typed Perfetto records. This preserves the
existing sequential observation semantics; it does not claim the whole table
was acquired atomically at one instant. Admission failure drops the trace batch
as a unit while legacy acquisition/output proceeds independently.

Trace-only string formatting, fragmentation and display conversion belong to
the worker. If a legacy consumer already requires formatted bytes, the trace
adapter may copy those bounded bytes once without repeating formatting. Existing
legacy file output and its cost remain attributed to that consumer. Opaque
artifact references become valid only after the owning writer closes/checks the
artifact; this transport does not defer reads of mutable renderer structures.

PE transport remains bounded, versioned, pointer-free and generated. Batch
publication must not add a bridge call per D3D9 operation. Native receipt time
for a PE report remains receipt time; asynchronous delivery cannot upgrade it
to a sampled-PC timestamp. Preserve LastError and guest/native identity rules.

## Publication, loss and teardown

Each block follows exclusive ownership transitions:

```text
Free -> ProducerOwned -> Published -> ConsumerOwned -> Free
```

Publication release/acquire ordering exposes only initialized records. A
consumer may recycle a block only after every downstream user has copied or
released its contents, including SDK serialization. Blocks retain generation
and publication sequence across the handoff; stale handles cannot enter a new
session. Raw source evidence must be persisted before recycling if the native
artifact depends on it. Delayed serialization preserves producer tracks and
original clocks rather than worker emission-time SDK macros.

All storage is budgeted: producer blocks, batch descriptors, report scratch,
schema/track registries, in-flight consumer blocks, SDK buffers, output bytes
and finalization scratch. Freeze size/time/service thresholds before arming.
A blocked file sink or slow SDK consumer exhausts the bounded pool and records
loss; it cannot backpressure production or trigger an unbounded allocation.
Per-source dropped-record/batch counts and an out-of-band failure latch survive
pool exhaustion. Partial SDK/file output is retained as incomplete evidence,
not silently retried as a second successful capture.

Closing stops new admission and publishes final partial blocks only after
their admitted writers finish. The controller then waits for published batches
and applicable GPU observation tickets, finalizes/flushes/stops the SDK, verifies
loss and output status, and seals artifacts plus receipt. All waits occur on
the diagnostic/control path, outside loader callbacks and production locks.
Deadlines mark incomplete output; they do not permit freeing live blocks,
unloading callbacks or reusing a generation. Crash/exit without a successful
receipt remains incomplete. Tracing must never force a production queue/GPU
drain, change query predicates or wake a production wait to expedite shutdown.

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
