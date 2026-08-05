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

**R-BACK-2.37** A run-ahead stage or commit must be non-present: publishing an
Arena source that contains `Present` must not acquire a drawable, encode
`presentDrawable`, or allocate or advance a frame-latency present token. The
Presenter retains exclusive ownership of drawable acquisition and present
encoding. Only the final present-bearing source may receive the present token,
and only when the encode coordinator reaches that source's ordered Present
tail. Frame pacing remains synchronized on present completion regardless of
offscreen run-ahead depth. Abort or device-loss cleanup before Present encoding
must release any reserved pacing state without synthesizing a successful
present or source completion.

**R-BACK-2.38** Resources referenced by early CPU-ready or non-present work must
be retained and marked against the owning source `seqId` before that source is
visible to scheduling. A publication split must not place a record under a
`seqId` that the importer retention and resource-use snapshot did not cover.

**R-BACK-2.39** Source publication, session admission, partition planning, and
execution-lane selection must be deterministic for identical imported records,
retained metadata, and explicit queue-local state. Wallclock time, worker
availability, publication arrival timing, completion-wait state, and GPU
progress feedback must not change their semantic shape. Ordered release-event
fences and fixed configured capacities are explicit queue-local state.

**R-BACK-2.40** Producer-side CPU work may advance into an immutable CPU-ready
source or source range during the previous frame's present-completion wait,
independently of Metal command-buffer selection. A CPU-ready source owns replay
records, retained handles, sequence metadata, allocator ranges, and access
summaries, but no drawable or present token. Deferring a frame-latency wait to
create this window must not admit a second present tail before the prior gate is
drained and must preserve `R-BACK-2.49` completion order. Admission assigns one
unique, strictly increasing global `sourceOrdinal` and one unique `seqId` in
the same order. One logical source may own an ordered chain of packed Arena
payload blocks, but every block in that chain shares the same `rawOrdinal`,
`sourceOrdinal`, `seqId`, retention lifetime, publication transaction, and
completion source. Once all bounded storage for the immediate raw-FIFO
successor is reserved, its generation-stamped `PublicationTicket` fixes those
identities until that complete source seals or aborts. No segment or block may
publish independently, and no later source may become ready while an earlier
admitted source remains unsealed.

**R-BACK-2.41** The scheduler must be able to coalesce consecutive compatible
CPU-ready sources or source ranges into the same `EncodeSession` and Metal
command-buffer chain that a single publication would have produced. Source
storage boundaries must not force command-buffer or logical-pass boundaries.
Coalescing must not reorder work across `Present`, query, readback, initializer
wait, or other semantic barriers. An open session's next-source frontier is the
oldest not-yet-represented FIFO source ordinal fixed when the session parks or
finishes its current ready snapshot. Later publication, worker timing, or a
younger ready source must not replace or bypass that frontier. The coordinator
may append the frontier only after its complete payload-block chain is Ready,
or must process its ordered control/compatibility disposition before examining
any younger source.

## 2. EncodeSession

**R-BACK-2.42** An `EncodeSession`, not a source slot, owns logical Metal
encoding lifetime. It owns the active encoder or parent encoder, logical-pass
attachment key, pending clear and pass actions, exact hazards, native dirty and
binding shadows, sidecars and samples, post-commit callbacks, and ordered
completion sources. While streaming is open, the physical command buffer is
owned by the coordinator's session envelope or pending submission record;
individual encode calls borrow it, and finalization transfers it into submitted
queue storage. A source boundary does not itself close the session or logical
pass. The coordinator must select one finite already-ready FIFO snapshot and
attach its maximal compatible prefix. An open session may remain parked when no
source is ready and producer quiescence creates no D3D-visible progress
obligation. Source arrival, ready-snapshot exhaustion, worker scheduling,
completion-wait state, and GPU
timing must not themselves decide whether to finalize; only the ordered
queue/API release events in `R-BACK-2.44` may do so.

**R-BACK-2.43** A session may keep one logical render pass open across
consecutive sources only while attachment identity, exact hazards, query and
observation order, encoder kind, initializer waits, and load/store semantics
remain compatible. Render-target/depth/sample changes, non-foldable clears,
attachment reads, blit/resolve/readback/query operations, non-render waits,
`Present`, explicit pass-end diagnostics, and finalization end the logical pass.
`commitChunk()`, CPU-ready publication, `seqId`, partition, and helper-call
boundaries are non-boundaries.

**R-BACK-2.44** A session that encoded visible work may remain open across
producer quiescence, but must submit its represented prefix when an ordered
event creates a D3D-visible or queue-progress obligation: a Present tail,
explicit Flush, direct observation or readback, producer wait for a covered
`seqId`, source-admission or raw-writer capacity dependency, a fixed
source/byte/draw/command-buffer cap, a semantic independent-submission boundary,
orderly shutdown, or device loss. Each event carries an ordered fence and must
not overtake older raw or CPU-ready work. Wallclock timeout, spin count,
completion-wait/GPU state, ready-snapshot exhaustion, and worker-arrival timing
are forbidden release inputs. At release the session must submit normally or
prove that no source was consumed; it must not inline-complete unsubmitted
visible work or attach a present token to a non-present prefix. Snapshot entries
beyond the maximal compatible prefix remain in `Ready` FIFO order and never
enter a snapshot-owned lifecycle state. A represented prefix rolled back before
any Metal side effect from that newly represented batch must return ahead of
every younger source; an older already-emitted session prefix is never rewound.

**R-BACK-2.45** Session storage must remain data-oriented: fixed-size values,
small inline arrays, queue-owned arenas, and bounded spans or views. It must not
deep-copy source slots, concatenate payload arenas, allocate one object per draw,
or retain PE, COM, Objective-C, or unowned process-local pointers. Session
source lists contain generation-checked source and storage locators plus compact
completion metadata, never direct page pointers or allocator-owned containers.
A source-table locator index is named `retainedSourceIndex`; the name
`sourceOrdinal` is reserved for the global monotonic publication identity.
Replay, FrameGraph, and diagnostic locators qualify each command as
`(retainedSourceIndex, commandIndex)` or an equivalent source ID plus command
index. These command locators are attribution and resolution identities, not
completion identities.

**R-BACK-2.46** Pass streaming must consume large imported records, uniforms,
bindings, handle lists, and payloads by reference to immutable queue-owned
storage. Copies are permitted only for compact metadata, explicit ownership
transfer, or Metal-required transient materialization; joining sources must not
copy O(total source payload bytes). The replay worker must construct admitted
records and payloads directly into a final ordered chain of one or more packed
Arena payload blocks after wire ownership is established. Each ordinary block
is a non-wrapping page run and carries only consecutive regions of the same
logical source. Sealing publishes the complete chain atomically, and session
attachment adds only source/block locators and completion metadata. A canonical
record is indivisible across block boundaries. A record larger than the
ordinary per-segment limit is a jumbo record: it occupies one dedicated jumbo
block, may exceed `maxPagesPerPayloadSegment`, remains bounded by
`maxPagesPerSource`, and is never split, copied into a second representation,
or assigned a second command index. After a source enters `Represented`, its
completion pin permits the synchronous encode call to resolve call-local spans
outside the scheduling lock; those spans must not escape the call or outlive
the represented source. Reservation sizing must use only bounds-validated wire
headers, lengths, and structural counts, without executing the semantic
transform twice. Every arena-backed SoA region reserves its final exact or
conservative capacity once before append; source construction must not
reallocate, relocate, or copy an earlier extent while growing.

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
observe their existing per-source timelines. Payload blocks, payload segments,
FrameGraph nodes, partitions, and command indices must never create independent
completion signals. Source-qualified `(source, commandIndex)` attribution may
identify replay and diagnostics, but completion and reclaim remain exactly once
per logical source `seqId`. Source metadata, tape pages,
retained handles, and allocator tickets may be reclaimed only after that source
is completed, no synchronous snapshot borrows it, and every older source has
reached the same reclaim point. Reclaim first makes the source inaccessible in
`Reclaiming`, destroys re-entrant owners and releases retained resources outside
the scheduling lock while its pages remain pinned, then atomically returns the
pages and advances source/page generations.

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
Until all gates pass for the unified Arena lane, `DXMT9_CPU_READY_TAPE` remains
default off and the legacy payload-owning path remains a supported rollback
path. Making Arena allocation unconditional, deleting legacy rollback, or
changing default allocation/admission policy is itself a promotion and requires
the same ordered evidence.

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
independently of GPU-reclaimed chunk slots. The physical owner must be a
queue-created fixed-capacity `CpuReadyTape`: a preallocated circular page arena
plus a fixed source-descriptor ring. A logical `CpuReadySourceId` and every
member of its `CpuReadyStorageRef` block chain must carry slot/page indices and
allocation generations; generation mismatch is stale and must fail before
dereference. One source reserves one all-or-nothing ordered chain of
non-wrapping page runs, is invisible while `Writing`, and becomes read-only only
after an atomic `Sealed -> Ready` publication of the complete chain that has
completed validation, retention, resource marking, summaries, `sourceOrdinal`,
and `seqId` assignment. Only its compact `PublicationTicket` control metadata
is scheduler-visible while `Writing`; no payload block is resolvable until the
whole source is `Ready`.

Capacity must be bounded simultaneously by source descriptors, total resident
pages/bytes, `maxPagesPerPayloadSegment`, `maxPayloadSegmentsPerSource`,
`maxPagesPerSource`, retained-handle entries, and session source references.
`maxPagesPerPayloadSegment` bounds each ordinary packed segment;
`maxPayloadSegmentsPerSource` bounds ordinary plus jumbo segments; and
`maxPagesPerSource` bounds their total pages, including a jumbo segment that
exceeds the ordinary segment limit. These are independent validated limits,
not aliases for one capacity. Admission reserves all descriptors, blocks, and
pages for the head source transactionally before construction. It uses fixed
high/low watermarks and FIFO head-of-line ordering. Only the replay worker may
wait for tape admission; the encode
coordinator may park an open session for future publication but must never wait
while holding the scheduling lock or for free capacity after a release event
from `R-BACK-2.44`; the finish thread never waits for publication or capacity.
Admission pressure publishes the corresponding ordered release event so a
represented prefix is submitted before that prefix can participate in a
capacity/completion cycle. Reclaim by the finish thread is the normal admission
wake-up owner and must be able to acquire the tape metadata lock and release
pages even while the replay worker is blocked. A source that exceeds the total
page or segment-count limit, or a jumbo record that exceeds the total source
limit, must use the ordered legacy one-source rollback path or fail the
already-invalid oversized input; temporary pressure must not create a second payload copy, reorder
sources, or hide a represented prefix. Current/peak occupancy, high-water hits,
admission wait, segment/jumbo counts, bypass reason, and reclaim wakeups must be
observable.

**R-BACK-2.61** Direct device calls may replace a full deferred-replay drain
with a scoped FIFO-prefix drain only when admission produced complete canonical
resource read/write summaries and a global-state-observation flag. The drain
may stop after the last conflicting raw ordinal, but must never skip or reorder
an earlier record. Global device state, query, reset, shutdown, unknown access,
or invalid or stale summaries require a full drain. Raw-queue entries remain
immutable, and replay and publication watermarks must be separately observable.

**R-BACK-2.62** A production partition planner must deterministically subdivide
the final selected replay stream after Traditional or FrameGraph reorder and
DCE. FrameGraph construction and replay resolution consume a read-only
`SourcePayloadView` over one logical source's complete payload-block chain;
storage blocks do not define DAG, replay, partition, or completion boundaries.
The view exposes stable source-qualified command locators and call-local record
resolution, but neither the graph nor an asynchronous partition may retain raw
page pointers. The planner must not resurrect, reorder, or duplicate commands.
The first production policy may subdivide only sufficiently large `DrawRun`
parameter ranges; commands, clears, and `Present` remain serial coordinator
segments.

Until canonical Arena sizing and ownership exist for Query, Readback, and
`UpdateTexture`, any source or direct operation requiring one of them uses an
explicit non-payload ordered control disposition. Such a disposition occupies
its original FIFO/order fence, is not represented as a partial Arena source,
closes or submits an older incompatible session prefix before observation or
copy work, and executes through the existing compatibility/direct path before
any younger source. It may not be reordered, DCE-proved through, or mistaken
for an empty payload publication. Unsupported or invalid partition output falls
back to identity ranges before side effects. Counters must expose identity and
explicit plans, range cost and draw distribution, planner CPU time, validation
fallback, control-disposition reason, and rejection reason.

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
