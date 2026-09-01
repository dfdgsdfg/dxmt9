---
type: "Spec Requirements"
title: "Architecture Requirements"
description: "Archicture requirements and compatibility contracts."
tags: [specs, archicture, requirements]
---

# Architecture Requirements

This document defines project-wide architecture contracts for dxmt9. Subsystem
specs may add narrower requirements, but they must not contradict these rules.

The directory name is kept as `specs/archicture/` to match the existing project
request. The topic describes overall architecture.

---

## 1. DXMT-Shaped Ownership

**R-ARCH-1.1** dxmt9 must preserve the DXMT-shaped ownership split:

- PE D3D9 layer owns COM ABI, Windows-visible validation, state shadowing,
  getters, state blocks, and HRESULT policy.
- PE recorder owns command chunk construction and retained handle derivation.
- `winemetal` bridge owns ABI marshalling only.
- unix importer owns packet validation, canonicalization, handle lookup, and
  retention before queue ownership begins.
- `CommandQueue` owns ordered replay, encode/finish threads, sequence IDs,
  chunk lifetime, and frame tokens.
- `Presenter` owns drawable acquisition and presentation encoding.

**R-ARCH-1.2** Hot-path D3D9 traffic must preserve DXMT's deferred execution
model. `Set*`, ordinary `Draw*`, ordinary `Clear`, and present sequencing must
record or import batchable work rather than requiring one Wine PE/unix bridge
call per D3D9 operation.

**R-ARCH-1.3** Intentional divergence from upstream DXMT must be justified by one
of: Wine PE/unix boundary constraints, ABI stability, D3D9-specific semantics,
Metal/macOS host constraints, or data-oriented ownership. Divergence must be
documented in the owning `spec.md`.

**R-ARCH-1.4** D3D7 and D3D8 compatibility layers must lower into the D3D9
frontend model before work enters the shared backend architecture. They must not
create a second backend execution architecture.

---

## 2. Data-Oriented Design

**R-ARCH-2.1** Hot-path data must be expressed as flat records, spans, views,
handle indices, and byte arenas wherever practical. The architecture must prefer
SoA or AoSoA storage for command replay, draw state, draw parameters, payload
bytes, uniform payloads, retained handles, and wire records.

**R-ARCH-2.2** The Wine PE/unix wire format must be POD, versioned, fixed-layout,
bounds-checked, and pointer-free. Wire records must not contain COM pointers,
Objective-C object pointers, unix-side object pointers, vtables, lambdas,
`std::function`, allocator-owned containers, or process-local pointers.

**R-ARCH-2.3** Queue-local execution storage must separate hot state decisions
from cold/debug payloads. Examples include `FlatDrawStateRecord` for hot
resource/PSO decisions, `DrawParam` arrays for per-draw fields, payload arenas
for variable UP data, and sidecar/debug records for inspection.

**R-ARCH-2.4** Span or view parameters crossing a module boundary are borrowed
for the duration of the call only unless the type name and documentation say
otherwise. The receiver must copy, intern, or translate borrowed data into
owned storage before returning.

**R-ARCH-2.5** Copies are allowed when they establish ownership, cross process or
ABI boundaries, build contiguous wire blobs, or stage user-provided D3D9 memory.
Copies must not hide unnecessary per-draw heap churn on the common hot path.

**R-ARCH-2.6** Architecture-sensitive performance regressions must be measured by
logical operation count, bridge operation count, chunk commit count, allocation
or capacity-growth count, payload bytes, uniform interning hit/miss, and frame
or throughput metrics where relevant.

**R-ARCH-2.7** The architecture design must keep an end-to-end render flow from
D3D9 API call through GPU completion and must classify expected bottlenecks as
CPU submission, bridge/import, queue storage, encode/cache, GPU execution,
presentation, or synchronous readback. Each bottleneck class must name observable
counters or evidence before it is optimized.

---

## 3. Boundary Contracts

**R-ARCH-3.1** The PE frontend must expose D3D9-compatible public behavior while
keeping backend-facing data independent from PE COM object lifetime. Backend
commands must refer to opaque handles or canonical value records, not COM
pointers.

**R-ARCH-3.2** The bridge boundary must communicate chunks and resource commands
through stable C ABI records. ABI records must use explicit sizes, versions,
offsets, and ranges that can be validated before decoding.

**R-ARCH-3.3** The unix importer must reject malformed records, stale handles,
invalid ranges, unsupported versions, and unexpected reserved fields before
queue execution observes them.

**R-ARCH-3.4** Queue and encoder code must consume imported records,
`FlatDrawStateView`, flat state keys, compact resource descriptors, payload
spans, and queue-local handles. It must not reach back into PE `DeviceState` or
PE COM objects to interpret already-submitted work.

**R-ARCH-3.5** GPU-visible execution must preserve D3D9 ordering requirements
while still allowing DXMT-style batching: render-pass merging, deferred clears,
exact-hazard-driven encoder splits, pipeline/depth/argument cache reuse, and
asynchronous command-buffer completion.

---

## 4. Reference And Provenance Policy

**R-ARCH-4.1** dxmt9 must remain compatible with MIT licensing for project-owned
code. MIT-licensed DXMT code or material may be used only with required MIT
copyright and permission notices preserved.

**R-ARCH-4.2** Wine D3D9 tests may be used as behavioral oracles for Windows D3D9
and Wine runtime compatibility. dxmt9 must not copy Wine implementation code,
wined3d structure, or LGPL-covered source into MIT project code.

**R-ARCH-4.3** DXVK, D9VK, and similar projects may be inspected as structural or
algorithmic references only. Their source code must not be copied into dxmt9
implementation, tests, generated code, or specs unless a separate license review
explicitly approves a compatible import path.

**R-ARCH-4.4** Reference-derived findings must be recorded as behavior,
architecture, tests, or benchmarks. Specs must distinguish "behavioral oracle",
"structure reference", and "implementation source".

**R-ARCH-4.5** Conformance tests, shader corpora, expected images, or generated
fixtures imported from external projects must carry provenance and license
metadata before they are committed.

---

## 5. Verification And Merge Readiness

**R-ARCH-5.1** Architecture conformance must be evidenced by native unit tests,
Wine PE conformance tests, shader runner readback tests, TLA+ models,
benchmarks, and gap tracking. Runtime readback alone is not enough for packet or
state-transform correctness.

**R-ARCH-5.2** The architecture must keep enough deterministic observer points to
prove bridge batching, chunk ordering, handle retention, sequence ID monotonicity,
encoder lifecycle, present frame latency, and resource lifetime without relying
on sleeps or wall-clock timing.

**R-ARCH-5.3** DXMT merge compatibility must be reviewed as ownership and
execution-shape compatibility, not source identity. Equivalent dxmt9 modules may
use different record layouts where the Wine boundary requires POD or C ABI
storage.

**R-ARCH-5.4** Any architecture requirement that is not implemented or not
evidenced must be tracked in `specs/archicture/gap.md` with the owning
`R-ARCH-*` IDs.

---

## 6. Concurrency Model

**R-ARCH-6.1** The architecture must identify the concurrent agents that may
observe or mutate rendering state: application/Wine API thread, PE recorder,
bridge/import call frame, queue writer, encode thread, finish/completion thread,
Metal/GPU execution, presenter/layer access, and optional sidecar workers such as
pipeline compilation. An implementation that enables parallel partition
encoding must additionally identify one encode coordinator and every optional
partition worker as distinct agents.

**R-ARCH-6.2** Ordinary hot-path submission must be fire-and-forget after queue or
import ownership is established. `Set*`, ordinary `Draw*`, ordinary `Clear`, and
ordinary queued copy/present work must not wait for GPU completion unless a
specified back-pressure or D3D9-visible synchronization rule applies.

**R-ARCH-6.3** Explicit synchronization boundaries must be named in the owning
design. Examples include readback APIs, `GetData(..., D3DGETDATA_FLUSH)`,
`WaitForVBlank`, present frame-latency gates, ring-slot back-pressure, reset or
lost-device drains, and shutdown.

**R-ARCH-6.4** CPU/GPU lifetime safety must be expressed through queue-owned
sequence IDs, frame tokens, retained handles, and deferred destruction. CPU-side
resource release or reuse must not expose a freed Metal object to in-flight GPU
work.

**R-ARCH-6.5** Queue parallelism must be bounded. The application thread may
record or submit future chunks while the encode thread and GPU process older
chunks, but ring capacity, chunk size limits, frame-latency tokens, CPU-ready
source/tape capacity, unsubmitted-session capacity, and partition-job capacity
must provide deterministic back-pressure. Adding a staging queue must not make
total retained work unbounded.

**R-ARCH-6.6** Cross-thread shared state must have one owner or an explicit
mutex/condition/atomic protocol. Borrowed spans and views must not be stored by a
thread that can outlive the submitting call unless the receiver first copies,
interns, or translates the data into owned queue storage.

**R-ARCH-6.7** Concurrency guarantees must be evidenced with TLA+ models,
debug-mode assertions, deterministic observer tests, or benchmark counters.
Sleeps, wall-clock timing assumptions, and GPU timing alone are not sufficient
proof of ordering, progress, or lifetime safety.

**R-ARCH-6.8** Pacing axes must be independent. The architecture must expose at
least three separately advancing progress signals:

- `completedSeqId` — advanced by every command-buffer completion; consumed by
  query resolution, readback waits, and resource reclaim;
- `presentCompletedSeqId` (frame token) — advanced only by present-bearing
  command-buffer completion; consumed by frame-latency gates;
- ring-slot occupancy — bounded by chunk admission; consumed by queue writer
  back-pressure.

A wait on any one of these signals must not block progress on the other two
beyond the formal ordering invariant `presentCompletedSeqId ≤ completedSeqId`.
In particular, a stalled `GetData(... D3DGETDATA_FLUSH)`, `GetRenderTargetData`,
or any other seqId-driven wait must not delay present admission, present
completion, or frame-token advance; a frame-latency gate must not delay query
resolution or resource reclaim.

**R-ARCH-6.9** Pacing independence must be observable. The architecture must
expose counters for the spread between `completedSeqId` and
`presentCompletedSeqId`, the maximum ring-slot occupancy under load, and the
wait time attributed to each signal separately. Cross-axis blocking is a
regression and must be detectable from these counters without timing-based
heuristics.

**R-ARCH-6.10** The encode coordinator must exclusively own `EncodeSession`,
logical render-pass state, Metal command-buffer or joint-group ordering, parent
parallel encoders, completion expansion, and finalization. An optional partition
worker may own only one immutable range, its child or segment encoder, and
partition-local native binding state. Workers must not mutate published source
storage, session-global hazards or pass actions, completion identities, or
sequence/frame-token state. Coordinator join must complete before pass or
submission finalization.

**R-ARCH-6.11** Scheduling pressure must be observable without weakening the
three progress signals in `R-ARCH-6.8`. Counters must separately expose current
and peak CPU-ready source occupancy, unsubmitted-session occupancy,
partition-job occupancy, source-admission wait, partition join wait, and the raw
replay and publication watermarks. These observations are pressure diagnostics,
not substitutes for `completedSeqId`, `presentCompletedSeqId`, or ring-slot
occupancy, and a wait displaced into them is a regression under `R-ARCH-6.9`.

---

## 7. Minimal-Copy Policy

This section consolidates the copy and ownership rules that are otherwise spread
across R-ARCH-2.4/2.5, R-ARCH-6.4/6.6, `spec.md` §2.2, and the backend
specializations (R-BACK-2.17, R-BACK-2.23, R-BACK-2.26, R-BACK-5.7,
R-BACK-12.13, R-CORE-11.11). It states the warm-path materialization floor: each
unique encoder-visible record or payload must reach owned storage at most once,
and only when the encoder will actually read it. Subsystem specs specialize this
policy; they must not restate or weaken it.

**R-ARCH-7.1** A byte copy on the warm draw path is permitted only to establish
ownership across a thread, process, or ABI boundary; stage user-provided D3D9
memory (R-CORE-11.11); build a contiguous POD wire blob (R-ARCH-2.2); or intern a
payload into queue-owned storage on a cache miss. Every other per-draw byte copy
after warm-up is a regression (R-ARCH-2.5).

**R-ARCH-7.2** The producer must not materialize a per-draw state or payload
record that draw-run batching or uniform dedup will discard or collapse into a
shared record. When a generation/lane or equivalent stamp proves a run shares one
canonical state, only the surviving shared record may be materialized; per-draw
differences must ride compact per-draw fields — `DrawParam` and
binding-override/snapshot payloads (R-ARCH-2.3) — not a duplicated full state
copy.

**R-ARCH-7.3** Large draw payloads — shader constants, FFP matrices,
DrawPrimitiveUP geometry, and shader/layout sidecars — must be referenced by a
stable queue-local handle, span, or hash. A payload whose hash or handle is
unchanged from a resident copy must be referenced, not re-copied; hot PSO and
resource decisions must read hashes and compact flat records, not full constant
arrays (R-BACK-2.17).

**R-ARCH-7.4** Where the owned destination slot and its lifetime are determined
before a record is built, the producer must construct the record directly into
that owned storage. Pre-reserved arena and SoA slots (R-BACK-2.23, R-BACK-2.26)
exist for this; building a record in an intermediate carrier and then copying it
into the owned slot on the warm path is a regression.

**R-ARCH-7.5** On a unified-memory device, data the GPU reads must be constructed
in place into a shared-storage Metal allocation; that in-place build is the
upload and must not be preceded by a separate CPU-struct-then-staging copy
(R-BACK-5.7). A shared backing is a Metal allocation, not an arbitrary CPU
pointer: the GPU reads it only through that allocation, and the allocation's
lifetime must follow queue-owned sequence-ID retention (R-ARCH-6.4), not CPU-side
scope.

**R-ARCH-7.6** Conformance to this section must be evidenced by per-class copy and
byte counters — per-draw state-copy bytes, discarded-materialization count,
uniform intern hit/miss, transient and argument-buffer upload bytes, and
warm-path heap-allocation count — not by frame timing alone (R-ARCH-2.6).
Per-draw materialization that exceeds one surviving record per draw-run group, or
a second copy of a payload whose hash is unchanged, is a regression signal.

**R-ARCH-7.7** Every CPU-side copy or materialization on the PE-record,
PE/unix import, replay, queue, encode, or GPU-handoff path must have one stable
copy-class identity from `spec.md` §2.3. An enabled ledger must report, per
class, calls, bytes, inclusive CPU time, and peak simultaneously retained
bytes, and must classify the class as `necessary` or `removable` with a named
ownership or ABI reason. The stable descriptor/report fields are
`identity`, `classification`, and kebab-case `reason`. Cumulative bytes must
not be presented as retained memory, and aggregate bridge or replay time must
not be attributed to byte copying. A new class or a reclassification is a
specification change. With the
ledger disabled, the path must read no clock, allocate no observer storage,
and perform no counter update beyond one cached-null branch. The owner argument
must be explicit at every production call site: PE wire construction uses the
PE registry and Unix replay/provider/queue/runtime work uses the Unix registry.
When a Metal `newBuffer` or `replaceRegion` call performs an implicit transfer,
the known byte/call event may be recorded with zero inclusive CPU time; the API
duration must not be charged to `copy_ns` because it includes allocation or
driver work.

**R-ARCH-7.8** Encoder-visible work follows the abstract ownership refinement
`ProducerOwned -> RawOwned -> ReplayBorrowed -> FinalOwned -> Encoding ->
GPUInFlight -> Completed -> Reclaimed`. Every accepted GPU-bearing identity must advance in
that order, at most once per stage, or take a specified pre-effect rollback or
zero-GPU-work terminal disposition. `ReplayBorrowed` and `Encoding` are
synchronous capability states, not owners: neither may be retained by an
asynchronous task, callback, queue node, or command-buffer completion. A
representation change may stutter within one abstract stage, but must not
create two independently reclaimable owners for the same identity.

**R-ARCH-7.9** A direct-construction change may remove only a class marked
`removable`. It must preserve the pointer-free PE ABI, exact wire bytes where
the wire is externally compared, effective command order and fields, resource
identity and retention, failure disposition, replay boundaries, and completion
waterlines. The subsystem owner must supply transactional rollback and a
legacy-versus-direct equivalence harness before the direct path can replace the
legacy path.

**R-ARCH-7.10** Promotion of a copy-removal path requires, in order: bounded
formal refinement for ownership, rollback, and progress; native byte/command
equivalence and deterministic queue-observer evidence; GPU visual/readback and
validation evidence for encoder-visible changes; bounded Wine evidence for a
PE or bridge change; then matched wild counters proving the named removable
class fell without displacement into retention, waits, or another copy class.
Timing alone, a lower call count, or a speculative merge is not promotion
evidence.

**R-ARCH-7.11** Every accepted producer batch must have one stable
`EndToEndSourceIdentity` from PE commit through terminal reclaim. The identity
must qualify the producer event ordinal, authenticated raw/source identity,
queue sequence, storage generation, and completion identity. Physical
contiguous, segmented, `ChunkSlot`, Arena, or early-retirement representations
may refine that identity but must not create a second logical command stream or
independently reclaimable completion owner.

**R-ARCH-7.12** An `ImmutableSemanticSource` is the authoritative ordered
semantic byte stream for one `EndToEndSourceIdentity`. Publication seals its
record order, payload bytes, qualified resource identities, and control
dispositions. A representation change may copy or directly adopt those bytes
only through a named R-ARCH-7.7 ledger class; it must preserve one reclaim
authority and must not mutate the published semantics.

**R-ARCH-7.13** Consumers must access a published source through a
`SynchronousSourceFacade` issued by a generation-qualified `SourceLease`. The
facade is a non-owning value view: it may expose checked typed records, bounded
spans, and `(region, offset, length)` locators only for the issuing synchronous
scope. It must not expose an ABI-crossing pointer, Arena page pointer, mutable
source storage, or a span/capability that can be retained by a queue node,
session, callback, completion object, or asynchronous task.

**R-ARCH-7.14** Contiguous and segmented physical sources must resolve to the
same facade semantics. PE/unix transport remains pointer-free and
bounds-checkable. Direct adoption requires an explicitly negotiated
shared-ownership ABI that atomically transfers the whole source lease; in its
absence the unix importer must establish `RawOwned` storage through the named
`copy.bridge.raw-owned` class. Partial adoption, role-local publication, and
embedded process-local pointers are invalid.

**R-ARCH-7.15** Unix resource resolution, hazard/pass summaries, PSO keys,
first-draw snapshots, and other derived encode data belong to a compact
`ResolvedSourceSidecar`, not to the immutable wire. A sidecar must be qualified
by the source identity and storage generation, must not own a second copy of
the semantic payload, and must not outlive the source lease unless it has been
projected to a locator-free completion value. Metal and Objective-C objects
remain unix-owned and must never enter the PE wire or source facade.

**R-ARCH-7.16** Replay direct construction must be a pure bounded plan followed
by one transactional emission into final queue-owned storage. Planning may own
counts, offsets, masks, hashes, and locators, but no encoder-visible record or
payload byte. Unsupported record families, insufficient capacity, ordered
controls, or unresolved lifetime evidence must fail closed before effects;
post-adoption or post-encoder failure must follow the specified fail-stop path.

**R-ARCH-7.17** Serial and partitioned encoding may transfer only immutable,
source-qualified ranges and locator-free snapshots across threads. Each worker
must reacquire a synchronous facade under the same source lease and generation.
The coordinator alone owns session-global encoder, render-pass action, hazard,
Present, query, completion, and reclaim state unless a narrower subsystem
requirement proves an explicit transfer.

**R-ARCH-7.18** A source may reach `Reclaimed` exactly once only after all
synchronous borrows have returned, every encoder or ordered-control effect has
settled, the completion authority has advanced, and all source-qualified
resource pins and sidecars are releasable. Reclaim that frees admission credit
must publish the generation-qualified wake consumed by blocked producers. A
zero-GPU terminal path must settle the same identity without fabricating a GPU
milestone.

**R-ARCH-7.19** The end-to-end contract must be verified as one composition,
not inferred from green component models. One deterministic lifecycle trace and
one bounded formal refinement must cover PE acceptance/import, source sealing,
facade borrow/return, direct or compatibility replay, serial or selected-child
encode, completion, and reclaim. Subsystem models may abstract adjacent stages
only through the transitions and fields defined by `EndToEndSourceIdentity`.
