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
documented in the owning `design.md`.

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
evidenced must be tracked in `specs/gap.md` with the owning `R-ARCH-*` IDs.

---

## 6. Concurrency Model

**R-ARCH-6.1** The architecture must identify the concurrent agents that may
observe or mutate rendering state: application/Wine API thread, PE recorder,
bridge/import call frame, queue writer, encode thread, finish/completion thread,
Metal/GPU execution, presenter/layer access, and optional sidecar workers such as
pipeline compilation.

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
chunks, but ring capacity, chunk size limits, and frame-latency tokens must
provide deterministic back-pressure.

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

---

## 7. Minimal-Copy Policy

This section consolidates the copy and ownership rules that are otherwise spread
across R-ARCH-2.4/2.5, R-ARCH-6.4/6.6, `design.md` §2.2, and the backend
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
