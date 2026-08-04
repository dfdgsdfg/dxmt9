---
type: "Spec Requirements"
title: "Encode Scheduling Requirements"
description: "CPU-ready publication, EncodeSession, partition planning, and Metal execution-lane contracts."
tags: [specs, backend, encode-scheduling, requirements]
---

# Encode Scheduling Requirements

This topic owns backend scheduling from immutable CPU-ready source publication
through logical-pass finalization. It does not change PE/unix wire semantics,
D3D9 command order, sequence-ID lifetime, or Presenter ownership.

The following boundaries are distinct and must never be inferred from one
another:

| Boundary | Meaning |
|---|---|
| Source publication | An immutable CPU-ready source becomes visible to scheduling. |
| Partition edge | A deterministic subdivision of one effective replay stream. |
| Physical encoder segment | One serial encoder, parallel child encoder, or Metal 4 render segment. |
| Logical render-pass boundary | A D3D9/Metal semantic boundary that fixes attachment and pass actions. |
| Submission boundary | A command buffer or jointly committed command-buffer group enters Metal execution. |

The requirements `R-BACK-2.35` through `R-BACK-2.50` and `R-BACK-2.57`
through `R-BACK-2.64` are authoritative here.

## 1. Producer / Encode Overlap

**R-BACK-2.35** The command queue must support producer/encode overlap: work for
frame N+1 may reach CPU-ready or encode-ready state before that frame's
`Present`, allowing non-present offscreen Metal work to be encoded while the
completion thread waits for frame N. At fixed command-buffer, logical-pass,
load/store, and tile-preservation shape, this must be observable as increased
`completion_wait_with_enqueue_ms` or decreased
`completion_wait_without_enqueue_ms`. A queue that advances only a
present-bearing source after the present wait does not satisfy this contract.

**R-BACK-2.36** A CPU-ready publication checkpoint must be deterministic and
must publish only a complete immutable prefix whose retention and sequence
metadata are established. Publication is not a partition edge, physical
encoder segment, logical render-pass boundary, or submission boundary. A
draw-count or byte threshold may select a publication checkpoint, but must not
by itself end an encoder, split a logical pass, or create a command buffer.
Metal split and submit decisions must remain derived from semantic boundaries
and the locality rules in `R-BACK-2.43`, `R-BACK-2.47`, and `R-BACK-2.48`.

**R-BACK-2.37** A run-ahead stage or commit must be non-present: it must not
acquire a drawable, encode `presentDrawable`, or allocate or advance a
frame-latency present token. Only the final present-bearing source carries the
present token and may acquire and present a drawable. Frame pacing remains
synchronized on present completion regardless of offscreen run-ahead depth.

**R-BACK-2.38** Resources referenced by early CPU-ready or non-present work must
be retained and marked against the owning source `seqId` before that source is
visible to scheduling. A publication split must not place a record under a
`seqId` that the importer retention and resource-use snapshot did not cover.

**R-BACK-2.39** Source publication, session admission, partition planning, and
execution-lane selection must be deterministic for identical imported records,
retained metadata, and explicit queue-local state. Wallclock time, worker
availability, and GPU progress feedback must not change their semantic shape.

**R-BACK-2.40** Producer-side CPU work may advance into an immutable CPU-ready
source or source range during the previous frame's present-completion wait,
independently of Metal command-buffer selection. A CPU-ready source owns replay
records, retained handles, sequence metadata, allocator ranges, and access
summaries, but no drawable or present token. Deferring a frame-latency wait to
create this window must not admit a second present tail before the prior gate is
drained and must preserve `R-BACK-2.49` completion order.

**R-BACK-2.41** The scheduler must be able to coalesce consecutive compatible
CPU-ready sources or source ranges into the same `EncodeSession` and Metal
command-buffer chain that a single publication would have produced. Source
storage boundaries must not force command-buffer or logical-pass boundaries.
Coalescing must not reorder work across `Present`, query, readback, initializer
wait, or other semantic barriers.

## 2. EncodeSession

**R-BACK-2.42** An `EncodeSession`, not a source slot, owns logical Metal
encoding lifetime. It owns the active encoder or parent encoder, logical-pass
attachment key, pending clear and pass actions, exact hazards, native dirty and
binding shadows, sidecars and samples, post-commit callbacks, and ordered
completion sources. The physical command buffer remains call-local while
encoding and transfers into submission storage at handoff. A source boundary
does not itself close the session or logical pass.

**R-BACK-2.43** A session may keep one logical render pass open across
consecutive sources only while attachment identity, exact hazards, query and
observation order, encoder kind, initializer waits, and load/store semantics
remain compatible. Render-target/depth/sample changes, non-foldable clears,
attachment reads, blit/resolve/readback/query operations, non-render waits,
`Present`, explicit pass-end diagnostics, and finalization end the logical pass.
`commitChunk()`, CPU-ready publication, `seqId`, partition, and helper-call
boundaries are non-boundaries.

**R-BACK-2.44** A session that encoded visible work must not wait indefinitely
for a future source or present tail. At a bounded release point it must submit
the represented prefix normally, or prove that no source was consumed. It must
not inline-complete unsubmitted visible work, attach a present token to a
non-present prefix, or lose a strict unrepresented suffix.

**R-BACK-2.45** Session storage must remain data-oriented: fixed-size values,
small inline arrays, queue-owned arenas, and bounded spans or views. It must not
deep-copy source slots, concatenate payload arenas, allocate one object per draw,
or retain PE, COM, Objective-C, or unowned process-local pointers.

**R-BACK-2.46** Pass streaming must consume large imported records, uniforms,
bindings, handle lists, and payloads by reference to immutable queue-owned
storage. Copies are permitted only for compact metadata, explicit ownership
transfer, or Metal-required transient materialization; joining sources must not
copy O(total source payload bytes).

**R-BACK-2.47** The execution lane must obey Metal rules. The legacy serial
lane permits only one active encoder per command buffer and cannot resume an
encoder after `endEncoding`. A parallel lane may create ordered child encoders
only inside one sealed logical render pass. A Metal 4 segmented lane may join
physical command-buffer segments only into one sealed logical render pass and
must commit the complete segment group together in order. Neither lane may
cross a semantic logical-pass boundary. Blit, compute, non-render waits, and
present encoding require the render pass to be closed or sealed as required by
the selected lane.

**R-BACK-2.48** Load, store, resolve, deferred-clear, touched-attachment,
visibility, capture, and diagnostic sidecar semantics attach to the logical
render pass, not to source, partition, child-encoder, physical-segment, or
command-buffer helper boundaries. A promoted lane must not increase final
same-key reopens, logical render-pass count, color/depth load/store bytes,
tile-preservation bytes, or command-buffer count against its serial baseline
unless another normative contract requires the increase.

**R-BACK-2.49** One `EncodeSession` may represent consecutive source `seqId`s
backed by one command buffer, a chain, or one jointly committed Metal 4 group.
Tail or joint completion expands into strict per-source completion order. No
represented source may complete before all Metal commands that contain its work
complete; reclaim, query, frame-token, and deferred-destruction consumers still
observe their existing per-source timelines.

**R-BACK-2.50** A scheduling lane may be promoted only after, in order: native
or fake-backend proof of source order, boundaries, fail-open behavior, and
completion expansion; formal or equivalent refinement evidence; runtime visual
and locality evidence; and no-gputrace overlap evidence. A fall in
`completion_wait_without_enqueue_ms` is not progress when displaced into
`queue_writer_wait_ms`, `offload_drain_fence_wait_ms`,
`queue_sequence_wait_ms`, source-admission wait, or another scheduling wait.
Promotion also fails when bounded source/session/partition occupancy remains
pinned at capacity, or command-buffer, logical-pass, load/store, or tile shape
regresses. FPS and Xcode counters are considered only after these gates pass.

## 3. Immutable Partition Contract

**R-BACK-2.57** A planned draw entry must cross into encoding as an immutable,
trivially copyable, standard-layout locator value containing source identity,
command/run/state/parameter indices, one uniform handle, and absolute binding
payload ranges. It must contain no Metal object, pointer, borrowed span, or
retained owner. Invalid metadata rejects the complete explicit plan before any
side effect and falls back to the same selected effective replay stream.

**R-BACK-2.58** Every selected effective replay stream must be consumable through
the serial partition interface using a validated explicit plan or allocation-free
identity ranges. Preflight must prove exact stream coverage and contiguous
`DrawRun` parameter coverage without gaps, overlaps, duplicates, or partial
tails. A partition edge is not a logical-pass, command-buffer, submission, or
session boundary. In the serial lane it is not a physical encoder boundary; in
the parallel lane it may delimit child encoders while preserving one logical
pass. Resolution remains call-local and must not escape to any asynchronous
owner.

**R-BACK-2.59** Published source storage is immutable until reclaim after GPU
completion. Imported headers, records, params, payload arenas, uniforms,
bindings, and retained-handle tables admit no consume-side mutation. Existing
queue lifecycle fields, the encode-worker-only pipeline-prefetch memo, and
storage clearing at reclaim are the only carve-outs. Encode interfaces take
published storage by const reference; new mutation requires an explicit
amendment to this carve-out list.

## 4. Scheduling and Execution Lanes

**R-BACK-2.60** CPU-ready source residency and admission must be bounded
independently of GPU-reclaimed chunk slots. A logical source may reference
immutable queue-owned tape or arena storage without consuming one scarce slot
until GPU completion. Admission must apply deterministic back-pressure, expose
current and peak occupancy plus wait time, retain represented sources until
their completion expansion, and fail open without payload deep copy when the
capacity cannot accept more work.

**R-BACK-2.61** Direct device calls may replace a full deferred-replay drain
with a scoped FIFO-prefix drain only when admission produced complete canonical
resource read/write summaries and a global-state-observation flag. The drain
may stop after the last conflicting raw ordinal, but must never skip or reorder
an earlier record. Global device state, query, reset, shutdown, unknown access,
or invalid or stale summaries require a full drain. Raw-queue entries remain
immutable, and replay and publication watermarks must be separately observable.

**R-BACK-2.62** A production partition planner must deterministically subdivide
the final selected replay stream after Traditional or FrameGraph reorder and
DCE. It must not resurrect, reorder, or duplicate commands. The first production
policy may subdivide only sufficiently large `DrawRun` parameter ranges;
commands, clears, queries, readbacks, and `Present` remain serial coordinator
segments. Unsupported or invalid output falls back to identity ranges before
side effects. Counters must expose identity and explicit plans, range cost and
draw distribution, planner CPU time, validation fallback, and rejection reason.

**R-BACK-2.63** Parallel render-pass execution may use
`MTLParallelRenderCommandEncoder` only after one logical render pass is sealed
and partitioned into ordered, independent ranges. The encode coordinator owns
the command buffer, parent encoder, pass actions, order, join, and completion;
each worker owns only its immutable range, child encoder, and partition-local
native binding shadow. Every child starts from a complete or proven-minimal
first-draw snapshot. Queries, clears, sidecar observations, initializer waits,
present work, and unresolved hazards make a range ineligible. Child creation
order must preserve draw order, all children must end before the parent, and
failure before Metal side effects must deterministically fall back to serial.

**R-BACK-2.64** A Metal 4 segmented lane may encode physical segments in
separate command buffers only for one sealed logical render pass. It must first
gather a finite bounded group, validate suspending/resuming options, prohibit
intermixed compute, blit, query, readback, and present work, and submit the
complete group together in source order. Load occurs only on the first logical
segment, store or resolve only on the last, and completion publishes only after
the joint tail. A capability-selected legacy serial fallback is mandatory; an
already committed command buffer must never be resumed by later work.
