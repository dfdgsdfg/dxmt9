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
hazard-driven encoder splits, pipeline/depth/argument cache reuse, and
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
