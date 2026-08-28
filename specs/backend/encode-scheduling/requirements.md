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

The requirements `R-BACK-2.35` through `R-BACK-2.50`, `R-BACK-2.57`
through `R-BACK-2.67`, and `R-BACK-2.76` through `R-BACK-2.84` are
authoritative here.

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
the same order. The v2 `EventSerial` lane retains one logical source identity
per PE chunk/event and is reachable in both compatibility mode and as the
complete-event fallback from `SegmentSerial`. The opt-in v2 `SegmentSerial`
lane may instead map one PE chunk/event to an ordered, bounded group of
identity-bearing source segments; each segment has its own `sourceOrdinal`,
`seqId`, retention lifetime, and completion source, while the event retains one
group transaction and capture token. Physical payload blocks remain storage
only and do not acquire identities by themselves. The v1 identity
sidecar/schema is retired: a reader must reject it before projection, provider
invocation, or source publication, and must not reinterpret it as either v2
identity lane. Once all bounded storage for the immediate raw-FIFO successor
is reserved, its generation-stamped `PublicationTicket` fixes the complete
event-group identity list until every segment seals or the whole group aborts.
No segment may publish independently, and no later event or segment may become
ready while an earlier admitted group remains unsealed.

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
any younger source. A sessionless fresh frontier may tentatively retain one
Ready source for one exact ordered-tail Writing successor under the bounded
contract in the specification. An active session ordinarily consumes its
current Ready head immediately. The only active-session exception is the
source-local terminal-suffix transaction in `R-BACK-2.43`: after encoding the
source's natural prefix, the coordinator may defer only a value-described
terminal `Clear(B), DrawRun(B)` suffix while waiting for the exact ordered-tail
Writing successor that begins with a returning `DrawRun(A)`.

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

Fixed planner-window exhaustion, selection of the next bounded window, and a
producer-quiescent park/wake while the same open session retains the same
uncommitted command buffer are also non-boundaries. None may end the active
render encoder, replace or commit its command buffer, resolve a provisional
Store action, publish logical-pass actions or sidecars, or create a source
completion entry. This cross-window continuity contract is limited to the
legacy serial lane inside that one open session and command buffer. After
`endEncoding` or command-buffer commit, the legacy lane cannot resume the pass;
continuity across physical command buffers is exclusively the bounded Metal 4
segmented contract in `R-BACK-2.64`.

When the session owns an active render pass, the coordinator may plan one
bounded FIFO source window without waiting for another source. The session
exposes one immutable bounded snapshot of that pass's
attachment identity, sample count, and complete deduplicated attachment-write
dependencies. A commandless virtual predecessor seeds the combined window DAG.
Every planned command is identified by `(retainedSourceIndex, commandIndex)` or
an equivalent generation-checked source locator. The initial cross-source lane
may move only `DrawRun` commands and must retain the exact complete natural live
command set; DCE remains source-local until it has a separate proof contract.
A moved same-pass command is accepted only when the combined DAG plus seed
proves the intervening work relocatable. Invalid, incomplete, overflowed,
duplicate, missing, stale, non-draw-moving, unmerged, or dependency-wedged
plans preserve natural FIFO source/command order before any Metal side effect.
The seed, source-window edges, and source/run boundaries are not Metal commands
and must not create a render-pass, command-buffer, source-completion, or reclaim
boundary. Replay registers completion sources exactly once in natural FIFO
order independently of command replay order, and still applies the exact
attachment, hazard, initializer, and tile-route checks required above before
continuing the active encoder.

The serial coordinator has one narrower source-qualified terminal-suffix
exception for the exact measured shape `DrawRun(A), Clear(B), DrawRun(B) |
DrawRun(A)`. The current source must already be `Represented` and admitted to a
live session whose complete active-render dependency snapshot proves `A`; it
must contain exactly those three commands, and the exact consecutive successor
must contain exactly the returning `DrawRun(A)`. The coordinator may encode the
first current-source `DrawRun(A)` and defer the remaining two-command suffix
only when the capacity snapshot proves one generation-stamped ordered-tail
Writing successor whose complete physical claim fits `successorHeadroom`. The
allocation-free narrow validator must re-resolve both payloads and prove exact
source identity and adjacency, complete command coverage, a full Clear,
attachment/sample compatibility, disjoint aliased attachment resources, and no
`B`-reads-`A` or successor-`A`-reads-`B` hazard before accepting replay
`A -> A | Clear(B) -> B`. The universal multi-source permutation validator
continues to reject cross-source movement over non-draw commands and must not be
relaxed.

The held suffix is coordinator-owned value state. At the prefix transaction
edge, the coordinator must retain only value snapshots: exact current and
expected-successor source/storage generations, raw/source ordinals and `seqId`s,
Ready-slot and completion identity, admission and command ranges, fragment
accounting, ordered release, lease generation/reserved/used/successor-remaining
capacity, the prefix-time replay frontier, active-render dependency snapshot and
instance token, and the full capture boundary. The capture boundary includes
both the capture controller and a pending carrier whose capture is attached or
already started. No `SourcePayloadView`, span, page pointer, session pointer,
Metal object, or other borrowed storage may survive the park. The whole current
source remains `Represented`; its completion is registered once in natural FIFO
order and it cannot retire until the suffix's final synchronous borrow and
replay finish.

If the exact successor becomes Ready, it is reserved as
`TentativeRepresented`, charged once against the live lease, re-resolved, and
all value snapshots above are revalidated after planning and again immediately
before commit and the first reordered successor replay. Rejection before that
successor effect restores only the unaffected successor, uncharges its
tentative capacity, and drains the older suffix in natural order. It never
restores or replays the already-effectful current prefix. The first reordered
successor replay call is the join effect boundary; failure after it is
fail-stop. Ordered release, producer wait, initializer work,
query/readback/update/Present, stop or device loss, writer identity loss,
headroom or lease failure, capture-boundary change, and admission or writer
pressure force a natural suffix drain rather than manufacture a submission
boundary. Semantic drains have priority; after they are excluded, an exact
Ready successor must win over simultaneous admission or writer pressure.

The coordinator must reserve the exact selected Ready prefix as
`TentativeRepresented` while holding the scheduling mutex and snapshot every
identity needed to validate the transaction, including source locators and
generations, the ordered-release fence, the capacity-lease generation, capture
and initializer boundary state, and the replay frontier. It must then release
the scheduling mutex before combined FrameGraph planning, resource-alias
resolution, or any observer/export work. After planning, it must reacquire the
mutex and revalidate the complete snapshot and exact FIFO prefix before
committing the sources to `Represented` or `Encoding`. A stale snapshot must
discard the plan and restore that exact prefix to its original Ready positions
without a Metal effect, completion registration, release advance, or observer
call.

Once a reordered fragment replay crosses the first-effect boundary, the
transaction observer may run exactly once and only after every fragment replay
and carrier fold succeeds. It must observe sources in natural FIFO order with
the scheduling mutex released. Fragment-local backend planning remains
suppressed so that neither a stale tentative retry nor an individual fragment
duplicates transaction observation. Natural plans, rejected permutations, and
unproved moved heads retain the source-local natural fallback.

For source-local planning, session storage must expose one typed replay-frontier
state. A session with no active render or blit encoder, no active-render flag,
and neither a deferred clear nor its command-identity sidecar is a clean closed-
encoder frontier; this does not mean that its command buffer contains no older
work. At that frontier a valid, complete, duplicate-free permutation with the
same live command set may move its first source command without an active-render
seed. An active render frontier may move the head only through the complete
snapshot and exact seed-head proof above. Pending-clear, active-blit,
active-render-unproved, malformed storage, and sessionless injected-command-
buffer frontiers must preserve the natural head.

Bounded multi-source preflight requires exact successor `sourceOrdinal` and
`seqId` identities. `rawOrdinal` is an optional bookkeeping coordinate: zero
means absent, and comparable nonzero values must increase but need not be
adjacent because StateOnly or Legacy raw work may interpose without publishing
a CPU-ready source. A missing or forward-gapped raw identity is not itself a
semantic boundary and does not relax any capture, initializer, ordered-release,
semantic, admission, replay, or dependency fence.

The coordinator must preflight the complete qualified permutation, its
maximal same-source replay runs, FIFO completion registration, admission state,
and command-buffer carrier capacity before entering the first reordered replay
call. Before that call, rejection may discard the plan and execute natural FIFO
order with no visible effect. Entering the first replay call is the post-effect
boundary because the encoder may commit an internal command-buffer segment
before returning. Any later replay, carrier-fold, attribution, finalization, or
publication failure is fail-stop; it must not restore Ready state, inline-
complete, or submit a partial reordered prefix.

**R-BACK-2.44** A session that encoded visible work may remain open across
producer quiescence, but must submit its represented prefix when an ordered
event creates a D3D-visible or queue-progress obligation: a Present tail,
explicit Flush, direct observation or readback, producer wait for a covered
`seqId`, a deterministic fixed source/page/byte/draw/command-buffer session cap,
a semantic independent-submission boundary, orderly shutdown, or device loss.
Each event carries an ordered fence and must not overtake older raw or CPU-ready
work. Wallclock timeout, spin count, completion-wait/GPU state, live admission or
raw-writer pressure, ready-snapshot exhaustion, and worker-arrival timing are
forbidden release inputs. Capacity pressure may delay session opening or source
publication, and may wake the coordinator to process an already-established
semantic or fixed-cap event, but must not invent a submission boundary. At
release the session must submit normally or prove that no source was consumed;
it must not inline-complete unsubmitted visible work or attach a present token
to a non-present prefix. Snapshot entries beyond the maximal compatible prefix
remain in `Ready` FIFO order and never enter a snapshot-owned lifecycle state. A
represented prefix rolled back before any Metal side effect from that newly
represented batch must return ahead of every younger source; an older
already-emitted session prefix is never rewound.

Physical payload retirement after synchronous encode is not a release event and
must not submit or close the session. The fixed session boundary is charged by
encoded-unsubmitted work (source, draw, and command-buffer credits), while
source/page/byte/block/Ready/retention/allocator credits describe payload
residency and may return before submission. GPU outstanding work and device-loss
settlement are separate completion credits and may not influence grouping.

**R-BACK-2.45** Session storage must remain data-oriented: fixed-size values,
small inline arrays, queue-owned arenas, and bounded spans or views. It must not
deep-copy source slots, concatenate payload arenas, allocate one object per draw,
or retain PE, COM, Objective-C, or unowned process-local pointers. Session
source lists contain either generation-checked source/storage locators or
queue-owned generation-stamped completion receipts plus compact metadata, never
direct page pointers or allocator-owned containers. Receipt storage is bounded,
flat, source-kind-neutral, and cannot resolve Tape pages.
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
completion signals. Explicit identity-bearing source segments are logical
sources and therefore create one completion source each; they replay in
event-local segment order and their completions expand in the same flattened
FIFO order. Event-level settlement occurs only after the final segment settles.
Source-qualified `(event, segment, commandIndex)` attribution may identify
replay and diagnostics, but completion and reclaim remain exactly once per
logical source `seqId`. Eligible source payload storage may retire after its
final synchronous encode borrow ends, before submission or GPU completion, only
after a queue receipt has become the completion authority. This releases
payload pages, publication controls, and physical lease credit but not Metal
work, callbacks, diagnostic/resource owners, query/frame tokens, resource
last-use watermarks, or GPU completion credit. Present, pending clear,
query/readback/update or ordered control, and any remaining payload borrow are
ineligible. Both early retirement and legacy post-completion reclaim first make
the source inaccessible in `Reclaiming`, detach ownership while locked, destroy
re-entrant owners outside the scheduling lock while pages remain pinned, then
relock to return pages, advance source/page generations, release control credit,
and notify producers. After receipt activation or any Metal effect, failure is
fail-stop; rollback is permitted only before both. Completion consumes mixed
receipt and legacy identities once in strict `seqId` order and does not change
resource waterlines.
For a represented source split by the terminal-suffix transaction, the current
source's receipt cannot activate or detach until its suffix is consumed and no
payload borrow remains. The successor may replay first, but completion
registration and receipt retirement remain dense natural FIFO: current source
exactly once, then successor exactly once.

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
Promotion evidence must separately conserve payload residency/publication
credit, encoded-unsubmitted work, receipt depth, and GPU-outstanding completion
credit; moving occupancy from one axis to another is not a locality win.
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

**R-BACK-2.59** Published source storage is immutable while it is resolvable.
Imported headers, records, params, payload arenas, uniforms, bindings, and
retained-handle tables admit no consume-side mutation. Existing queue lifecycle
fields, the encode-worker-only pipeline-prefetch memo, and storage clearing
after an exclusive `Reclaiming` transition are the only carve-outs. Ordinarily
that transition follows GPU completion; an eligible source may instead enter it
after synchronous encode installs a generation-stamped queue receipt and proves
that no payload locator or borrow escapes. Encode interfaces take published
storage by const reference. Receipt-backed completion cannot dereference the
retired source, and stale/duplicate/ABA receipt use must fail before callbacks,
waterlines, or release. New mutation or eligibility requires an explicit
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
is scheduler-visible while `Writing`; no payload block or identity segment is
resolvable until the whole source or event group is `Ready`. In the segmented
streaming lane, all segment descriptors, segment payload blocks, identities,
and event-group metadata become visible in one atomic `Sealed -> Ready`
publication; a partially ready event group is invalid.

Capacity must be bounded simultaneously by source descriptors, total resident
pages/bytes, `maxPagesPerPayloadSegment`, `maxPayloadSegmentsPerSource`,
`maxPagesPerSource`, retained-handle entries, session source references, and
the independent `maxIdentitySegmentsPerEvent` and
`maxIdentitySegmentsPerSession` limits. `maxPagesPerSource` bounds one
compatibility source; a segmented event group additionally charges the sum of
all segment reservations against the event and session limits. These
identity-segment limits are not aliases for `maxPayloadSegmentsPerSource`.
`maxPagesPerPayloadSegment` bounds each ordinary packed segment;
`maxPayloadSegmentsPerSource` bounds ordinary plus jumbo segments; and
`maxPagesPerSource` bounds their total pages, including a jumbo segment that
exceeds the ordinary segment limit. These are independent validated limits,
not aliases for one capacity. Admission reserves all descriptors, blocks, and
pages for the head source transactionally before construction. It uses fixed
high/low watermarks and FIFO head-of-line ordering. Only the replay worker may
wait for tape admission. A multi-source SegmentSerial request must wait on the
complete source-layout batch and the same predicate used by begin: every
required control slot from the current write index is free as one contiguous
ring range, no Arena build is active, and Tape pressure has cleared. A free
first control must not release the waiter while any later required control is
occupied. The waiter must park on the queue condition variable without polling
or re-entering the begin/wait loop until that complete predicate or terminal
stop holds. The single-source API retains the same one-layout behavior. The
encode coordinator may park an open session for future publication but must
never wait while holding the scheduling lock or for free capacity after a release event
from `R-BACK-2.44`; the finish thread never waits for publication or capacity.
Admission and session representation must obey the capacity-credit and headroom
contract in `R-BACK-2.65`; live pressure must not submit a represented prefix or
otherwise choose its boundary. Reclaim by the finish thread is the normal
admission wake-up owner and must be able to acquire the tape metadata lock and
release pages even while the replay worker is blocked. A source that exceeds
the total page or segment-count limit, or a jumbo record that exceeds the total
source limit, must use the ordered legacy one-source rollback path or fail the
already-invalid oversized input; temporary pressure must not create a second
payload copy, reorder sources, hide a represented prefix, or fragment a session.
Current/peak occupancy, high-water hits, admission wait, physical
segment/jumbo counts, identity-segment counts, group-abort/compatibility
bypass reason, and reclaim wakeups must be observable.

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
The sealed-pass child builder, independently of the serial planner's 32-draw
target, must choose at most `floor(totalDraws / 64)` children within the
existing two-to-16 bound, cover draws exactly in order, and give every child at
least 64 draws; fewer than two qualifying children selects serial before
effects. Multi-command passes must preserve whole-command indivisibility and
use deterministic earliest qualifying prefixes while a complete 64-draw
suffix remains, absorbing a thinner final suffix into its predecessor. The
builder must validate exact command/draw coverage and distinguish no genuine
two-child work, planner invariant/search failure, child capacity, and pass
capacity.
Pass extraction is per interval, not per source. Every source command is
classified into one total role: draw, Clear boundary, Present boundary,
non-child coordinator boundary, or unsupported. `SurfaceCopy`, `StretchRect`,
`Readback`, `ColorFill`, and `DepthResolve` own their own short-lived encoder,
so the coordinator has already ended the render encoder when it replays them;
they therefore terminate a pass interval at exactly the position `Clear` and
`Present` occupy and stay at their serial position as sealing locators that
are never child ranges. One such command must not reject unrelated intervals of
the same source. A coordinator command that a same-attachment draw resumes
immediately after would split one logical pass: both the interrupted interval
and the resuming interval fail closed with the coordinator-command reason, and
the builder must never speculate that a pass resumes across it. Only a command
kind the builder cannot classify fails the whole source closed. Counters must
separate the non-child coordinator commands met from the subset that failed a
pass closed.
Workers may re-resolve source-qualified locators only while the coordinator
holds the synchronous source residency pin; no payload pointer may escape the
joined execution. Before parent creation, one immutable pass-wide binding proof
must select either Stage 1 direct binding or Stage 2b direct constant buffers
at VS/PS slots 0 and FFP slots 3 for every draw. Missing PSO metadata, mixed
ABIs, slot-30 argument tables, resource arrays, or a draw override that can
rebuild the prefetched PSO make the complete pass ineligible before effects.
Each child owns a zeroed binding shadow with every uniform class initially
dirty. It must not share the queue-owned argument encoder, mutable table shadow,
or argument-buffer constant cache between children. After complete locator,
ABI, PSO, and uniform re-resolution, and before render-pass preparation or
parent creation, the pure economics classifier must either accept the pass or
select the exact serial pre-effect replay. Production economics counters must
conserve `considered = accepted + serial_fallback`, and typed mutually
exclusive rejection reasons must sum to `serial_fallback`. The classifier must
reject a child-size spread greater than the same 64-draw quantum and require
both a PSO change and a uniform identity change at every produced child
boundary; internal pass churn cannot pay for a child first-bind reset. The
parallel provider must remain non-default when matched wild evidence reduces
local encode wall time but regresses end-to-end throughput.

**R-BACK-2.64** A Metal 4 segmented lane may encode physical segments in
separate command buffers only for one sealed logical render pass. It must first
gather a finite bounded group, validate suspending/resuming options, prohibit
intermixed compute, blit, query, readback, and present work, and submit the
complete group together in source order. Load occurs only on the first logical
segment, store or resolve only on the last, and completion publishes only after
the joint tail. A capability-selected legacy serial fallback is mandatory; an
already committed command buffer must never be resumed by later work.

**R-BACK-2.65** CPU-ready admission must reserve deterministic headroom before
opening an `EncodeSession`. Configuration defines a bounded capacity vector for
one ordinary Direct source's worst-case reservation footprint, including payload
pages, circular-wrap padding, block descriptors, source/Ready entries,
retention, and allocator tickets. `successorHeadroom` must cover that complete
vector. A generation-stamped session capacity lease reserves the fixed maximum
unsubmitted session footprint plus `successorHeadroom`; a configuration whose
sum exceeds any admission high watermark is invalid. The coordinator acquires
the lease before representing the first session source. Residency already owned
by Ready sources incorporated into the lease is charged once, not reserved a
second time. Older submitted residency may delay lease acquisition or source
publication until reclaim, but GPU completion timing must not change which
sources share a session.

At first acquisition, the coordinator may credit exactly one already-resident
`Writing` source against, and never beyond, `successorHeadroom` only when the
Tape observation proves it is the unique ordered tail, owns a valid publication
reservation and current source/storage generations, and its complete physical
claim fits the successor vector. The credit is only a no-double-count proof:
the acquired lease must still reserve the full fixed session footprint and full
`successorHeadroom`. `TentativeRepresented`, `Represented`/encoding,
`Submitted`, `Completed`, and `Reclaiming` residency remains older unavailable
capacity. A structurally valid Writing claim beyond headroom is also ordinary
unavailable capacity. Multiple/non-tail writers, invalid publication identity,
or checked capacity arithmetic make the snapshot invalid and must fail-stop
rather than wait for a generation that cannot repair structural corruption.

At a fresh frontier before first acquisition, the serial coordinator may
speculatively reserve exactly one compatible, present-free Ready head as
`TentativeRepresented` only when the capacity snapshot proves exactly one
generation-stamped ordered-tail Writing successor. This is a pre-admission
lookahead hold, not session
representation: it acquires or mutates no capacity lease, admission charge,
completion entry, or Metal effect, and it must restore the exact snapshot to
the FIFO front before either the successor joins normal bounded selection or
any fallback executes. Ordered release, producer sequence wait, initializer
work, shutdown, writer identity loss, Arena admission pressure, or
compatibility-writer pressure forces restore followed by exact single-source
progress. A simultaneously Ready exact successor takes precedence over a live
pressure observation because the required forward source already exists. The
coordinator acquires or charges the normal lease only after restore, before the
selected source becomes final `Represented`/encoding state. Restore failure is
structural corruption and fail-stops the Tape.

An active session uses the distinct terminal-suffix transaction in
`R-BACK-2.43`, not the pre-admission Ready-head hold above. Its already-
represented current source and live lease may wait only after the natural
prefix has encoded and only when the snapshot proves one exact ordered-tail
Writing successor whose complete claim fits the still-available
`successorHeadroom`. The wait owns the current source's residency and encoded
work exactly once, holds no scheduling lock, and neither acquires duplicate
physical credit nor releases the current source. Release/control, wait,
initializer, capture-boundary, stop/loss, writer-identity, headroom, and lease-
generation invalidation drain first. Once those semantic drains are absent, the
exact successor becoming Ready wins simultaneous admission or writer pressure;
pressure without that Ready identity drains naturally and resumes ordinary
progress.

Each admitted source is charged against a fixed encoded-work cap and a physical
residency vector. Encoded work retains `maxSessionSources`, `maxSessionDraws`,
and `maxSessionCommandBuffers` until submission; the bounded implementation cap
is 128 sources. Physical `residentSources`, pages, bytes, blocks, Ready entries,
retention entries, and allocator tickets may be uncharged only after successful
post-encode payload retirement.
`SourceSemanticSummary::byteCount` is a representation-specific publication
extent used by validation and telemetry: Legacy records its logical replay
extent, while Arena records its exact constructed Tape extent. It must not be
reused directly as a universal session byte charge. The byte credit uses a
distinct typed scheduler-owned Tape charge: an Arena source charges its exact
constructed Tape byte extent, while a Legacy compatibility source charges its
reserved Tape pages. Legacy `ChunkSlot` vector heap bytes are outside this byte
axis and remain bounded only indirectly by compatibility source and slot limits;
promotion must not describe this credit as a complete physical-memory bound.
The source-work and command-buffer-work dimensions remain active after physical
retirement. If the next Ready source would cross a work credit, or a still-
resident source would cross a physical credit, the coordinator
posts one ordered `SessionCap` event at the predecessor fence, leaves the
candidate `Ready`, and submits exactly the deterministic predecessor prefix. A
candidate larger than the ordinary Direct reservation footprint is classified
before construction as an isolated bounded Arena session, ordered legacy
rollback, or invalid input; it must not consume successor headroom or split an
already-open session in response to current occupancy. Raw-writer or Arena
admission pressure is not a release reason and must not post a
`SessionReleaseEvent`. If an Arena admission waiter appears while the first
lease is denied by older unavailable residency, the coordinator may execute
one FIFO Ready source through the existing standalone serial path at most once
per exact denied `(seqId, sourceOrdinal)` identity. The identity must remain
unchanged through the denial, and the source must be a non-Present Direct Arena
source that already owns its complete physical residency, whose semantic
payload shape fits `ordinaryDirect`, and whose complete physical reservation
fits the admission high-water vector. `ordinaryDirect.pages` compares payload
pages without circular-wrap padding; the independent high-water, lease,
retirement, and completion accounts retain payload plus wrap-padding pages and
the complete byte/descriptor/ticket charge. This bounded escape acquires no
lease, reserves no capacity, opens no session, and permanently consumes the
token for that exact identity. A different FIFO head may consume its own token
without a capacity-generation transition, but the same identity must not
execute twice; the fixed Ready/source bounds therefore bound total escapes in
one pressure episode. A capacity-generation transition keeps retry priority
and does not erase a consumed identity token. The escape may reduce that exact
source to a singleton submission; GPU timing must not append it to a session,
widen any later group, fabricate completion, or re-arm the same identity. The
same exact-identity standalone mechanism may also run after admission clears
when a producer `waitForSequence` fence is active, but only for an eligible FIFO
head whose `seqId` is at or below the exact ordered target. Admission pressure
and producer-fence escapes have distinct actions and counters. The identity is
consumed only after reservation commit; every pre-commit restore retains its
eligibility, while stale, ineligible, or beyond-target identities fail closed.
The producer-fence action creates no admission release, session widening,
lease/capacity transition, or new capture target. A
SegmentSerial source retains its atomically
published event-group metadata, FIFO execution, per-segment completion, and
tail settlement; the escape cannot expose a Writing group member or alter
publication/abort atomicity.
Promotion requires zero pressure-created session releases and observable lease
current/peak/denial, reserved/used/slack credits, successor-headroom minimum,
fixed-cap release reason, and isolated or rollback reason. Every accepted
source/page `SessionCap` event must also expose the predecessor source/page
usage, candidate payload pages, candidate wrap-padding pages, candidate total
required pages, required total sources/pages, and whether sources, pages, or
both exceeded their limits. This observation must not affect classification,
the selected predecessor fence, or session grouping.

**R-BACK-2.66** Render scheduling must expose stable provider configuration as
four typed, independently resolved axes:

- source delivery is `Compatibility` or `Streaming`;
- source identity granularity is `EventSerial` or `SegmentSerial`;
- partition execution is `IdentitySerial`, `ExplicitSerial`, or
  `ExplicitParallel`; and
- command-buffer segmentation is `Disabled` or `Metal4`.

Every named mode is a `StableProvider` under `R-BACK-42.1`; implementation and
default state remain tracked separately in the render-provider registry.

The runtime must resolve the complete configuration once at device or command-
queue creation and keep it immutable for that queue. `Compatibility` uses the
payload-owning source path, while `Streaming` selects bounded CPU-ready Tape
publication and `EncodeSession` source streaming; ordered Legacy, Inline, and
control dispositions remain valid fallbacks inside the streaming mode.
`EventSerial` is mandatory for Compatibility and is the default Streaming
identity lane. `SegmentSerial` is opt-in, requires the bounded event-group
publication contract in R-BACK-2.40 and R-BACK-2.60, and must fall back the
complete event to EventSerial compatibility when any segment cannot be proved.
`IdentitySerial` uses the allocation-free identity cursor,
`ExplicitSerial` runs the deterministic production partition planner on the
single encode coordinator, and `ExplicitParallel` runs the same validated plan
through eligible parallel children with mandatory per-pass serial fallback.
`Metal4` is orthogonal to partition parallelism and must retain the capability-
selected fallback in `R-BACK-2.64`.

The source and partition axes must not imply one another. In particular,
explicit serial planning and eligible parallel execution must remain usable
with either source-delivery mode; parallel execution must not require promotion
of the Tape. Selecting a provider mode must not implicitly enable FrameGraph
semantic optimizers such as pass coalescing or DCE, alter Presenter policy, or
weaken any order, lifetime, load/store, completion, or locality contract.

The canonical process selectors are `DXMT9_RENDER_SOURCE_MODE` with values
`compatibility|streaming`, an identity-granularity selector with values
`event|segment`, `DXMT9_RENDER_PARTITION_MODE` with values
`identity|serial|parallel`, and `DXMT9_RENDER_SEGMENT_MODE` with values
`off|metal4`. The identity-granularity default is `event`; the current full
default is
`compatibility + event + identity + off`. Until migration is complete,
`DXMT9_CPU_READY_TAPE=0|1` is a compatibility alias for only the source-delivery
axis when the canonical source selector is unset; it is not a separate provider
mode. An unknown value must fail closed to that axis's default and emit one
bounded warning. Requested and resolved axes, capability fallback, per-pass
lane fallback, and mode-specific work counts must be observable.

These selectors describe supported rendering-provider modes, not temporary
experiment probes. Promotion under `R-BACK-2.50` may change a default but must
not make the previous mode unreachable. Removing a supported mode requires an
explicit requirement amendment, migration evidence, and replacement regression
coverage; ordinary hot-path cleanup must preserve every supported selector.

**R-BACK-2.67** Encode scheduling must carry a composed end-to-end temporal
progress proof in addition to the per-component models. The scheduling
pipeline is a streaming/reactive system whose observed failure class is
liveness, not invariant violation — a waiter parked forever while its wake
condition already holds (the 2026-08 tape lease/wake wedge) — and
per-component models that are each individually live do not establish that
their composition is live.

- (a) A composition model must span abstracted admission, capacity
  lease/wake, source publication, session continuation, completion release,
  and present pacing in one specification, and must prove under weak
  fairness of the runtime's own steps: every accepted source leads to its
  completion release; every Present-bearing source leads to either successful
  drawable publication or an explicit skip disposition and then settlement;
  and lost-wakeup freedom — no reachable state parks a waiter whose wake
  predicate holds with no enabled step that notifies it. GPU settlement is an
  explicit environment/fairness assumption, not queue-owned weak fairness.
  Component interiors may be abstracted to their published interface
  transitions, and each abstraction must name the detailed model it
  summarizes so refinement drift is reviewable. The model's accepted-source
  and Present antecedents must be reachable under an explicit source-arrival
  assumption; terminal teardown must use stage-specific drain, disposition,
  release, and settlement actions rather than one transition that fabricates
  every downstream milestone.
- (b) Every liveness-critical wait predicate and wake site in the
  implementation must be bound to its model transition by an isomorphism
  pin — a native truth-table spec in the `PresentOrdinalWaitIsomorphism`
  pattern — so the proof stays attached to the code it claims to cover. A
  new wait/notify pair on the scheduling path must not land without either
  a pin or a recorded gap row.
  Tests of notifications must invoke the production owner and park on the
  production condition variable; a local fixture that mutates a synthetic
  predicate and calls its own `notify_all` is not evidence for a wake site.
- (c) The runtime must expose bounded liveness watchdog observability:
  counter or log evidence sufficient to distinguish "obligations pending
  with no progress" from legitimate idleness in a wild run without
  gputrace, so a liveness regression is detectable on first occurrence
  rather than by black-screen reproduction.
- (d) The composed pressure cycle must cover a multi-source Arena batch whose
  complete control predicate needs at least two distinct Ready-head singleton
  drains while the capacity generation remains unchanged. The model and a
  deterministic native composition pin must prove exact-head token
  conservation, FIFO ownership, at-most-once escape per identity, admission
  retry only after the full batch predicate becomes Ready, and no
  pressure-created release or capacity transition. The native pin must use the
  production admission predicates, condition variables, and serial execution
  path; polling, sleeps, and synthetic occupancy arrays are not evidence.

The composition model runs under `dxmt9-verify-tla` with the existing
models. This requirement adds verification obligations only; it must not
change scheduling behavior.

## 5. Parallel Policy Safety Foundation

These requirements define the policy algebra separately from its economics.
They apply to `ExplicitParallel` exploration and do not authorize a runtime
implementation or a default change by themselves.

**R-BACK-2.68** Parallel policy evaluation must produce two separate,
immutable value results: a semantic safety certificate and an economics
record. The safety certificate describes only facts required to refine the
sealed serial replay stream; the economics record describes bounded cost and
benefit estimates. An economics score must never make an unsafe or unproved
candidate eligible, and a safe certificate must not claim a performance win.

**R-BACK-2.69** The policy proof core must define that only a complete,
generation-checked, proof-carrying parallel plan may reach Metal child or
segment creation. The certificate must bind the sealed source/pass identity,
ordered range vector, complete coverage proof, coordinator-command exclusion,
pass-wide attachment and exact-hazard facts, pass-action epoch, route/ABI
compatibility, first-draw snapshots, and fixed capacity bounds. Every locator,
generation, count, and arithmetic input must be checked before the first Metal
side effect. A stale, incomplete, unknown, inconsistent, or overflowed
certificate is invalid and cannot be partially consumed. The production
coordinator must own exactly one adapter into this proof core, placed after
complete locator/ABI/PSO/uniform re-resolution and before render-pass
preparation or any parent/child Metal effect. The adapter must build the
authoritative snapshot input from the owning sealed-pass batch and resolve
exact per-child coverage from the live source while the residency pin is held.
Only a certificate-valid candidate may enter selection, only the selected plan
may execute, and every other outcome selects the exact serial replay. The
adapter must not weaken any existing pre-effect rejection: the economics
classifier must still be evaluated for every considered candidate so its
attribution keeps its meaning, it may only add a rejection, and execution
requires the certificate, the classifier, and the selector to agree. Typed adapter counters
must conserve `considered = certificate_valid + certificate_invalid` and
`considered = selected + serial_fallback`, with `selected <= certificate_valid`.
The provider stays default-off and the adapter makes no promotion claim.

The pass-action epoch is a per-pass fact, not a source-wide one. The producer
must issue the coordinator proof for each sealed pass it emits and stamp that
proof with that pass's own action epoch. The certificate must not accept a
stored epoch as evidence of itself: it must independently re-derive the
expected action epoch for the sealed interval by folding the shared command
classifier over the generation-pinned effective replay stream in
replay-ordinal order, and must reject the certificate when the re-derived
value differs from the stamped one or when the interval does not begin at a
pass-opening draw. The re-derivation must be a pure fold over immutable stream
facts with no clock, floating-point value, allocation, or dependence on
iteration order beyond the replay-ordinal sequence, and repeating it over the
same stream must yield the same value. Source and storage generation checks
must still fail closed before the re-derivation runs. The producer and the
certificate must share one implementation of the epoch state machine and one
implementation of the command classifier.

The coordinator's published seed epoch must be the epoch domain's first value
on every source, whether or not that source carries an open encode session. The
epoch domain is scoped by (source, sequence id) and is never compared across
sources, and whether a source starts a pass is carried separately by the
source-start boundary fact. Zero is the domain's invalid sentinel and must never
be published as a seed: the epoch witness must refuse it, so a zero-seeded
source could only stamp candidates from a fold the certificate is unable to
re-derive. The producer must therefore fail a zero-seeded source closed with the
typed pass-action-epoch reason before observing any candidate, rather than
publishing candidates that are certificate-invalid by construction.

A sealed pass must end in exactly one of three ways, and the certificate must
accept exactly those three. It may end at the source end with no sealing
locator; at a coordinator-owned sealing command that the coordinator still
replays serially at `replayOrdinalEnd`; or at an attachment change, in which
case no coordinator command occupies `replayOrdinalEnd` and the sealing locator
is the first draw of the next pass — the same spelling the coordinator's
encoder-split attribution maps to a render-target change. The third form
carries one additional obligation: the certificate must independently re-derive
that the sealing ordinal itself opens a pass and that its action epoch differs
from the sealed interval's, which is what distinguishes the next pass's first
draw from a draw that belongs inside the interval. A draw sealing locator
without that proof is invalid.

**R-BACK-2.70** A certified plan must cover the effective replay stream with
ordered, contiguous, non-overlapping source-qualified ranges. The flattened
child draw sequence must equal the serial draw sequence exactly once, with
complete `DrawRun` parameter coverage and no gap, overlap, duplicate, partial
tail, or cross-source jump. Clear, Present, query, readback, update, wait,
sidecar, and every other coordinator command remain at their serial position;
they are never represented as child ranges and never disappear from the
coverage proof.

The coverage proof must not be bounded by a fixed per-child command-row
capacity. A child owning more source commands than any storage array must be a
normal input, not a rejection. The resolver must stream its resolved commands
through an accumulator whose state is constant in the number of commands, and
every accumulator must be exact: a hash or other lossy summary must not decide
coverage, because a collision would admit a false accept. The accumulator must
enforce order, contiguity, non-overlap, and non-emptiness against the previous
command's boundary as each command is appended, must retain the exact running
command and draw totals for comparison against the child plan's claimed
totals, must retain the first resolved command for the plan's begin/count
predicates, and must report the first failure with its reason and command
locator. A resolver must not be able to construct a coverage accumulator in
any state other than the one its own appends produced.

**R-BACK-2.71** Eligibility is pass-wide, not fragment-local. The proof must
cover the complete sealed logical pass: one attachment/sample identity and
all exact canonical read/write hazards; one unambiguous load/store/action
epoch; one supported render route and child-binding ABI; and a complete
first-draw snapshot for every child. A locally valid range cannot override a
pass-wide unknown, conflict, action mismatch, ABI mismatch, or missing
first-draw fact. Coordinator-owned pass actions, sidecars, completion, and
parent/segment lifecycle remain outside child ownership.

**R-BACK-2.72** Economics and selection must be deterministic checked fixed-
point value transforms. Scores may use only bounded integer inputs captured in
the policy snapshot; they must not depend on wallclock time, worker arrival,
GPU progress, allocation addresses, or floating-point rounding. Checked
addition, multiplication, and conversion are mandatory. Arithmetic overflow,
invalid normalization, empty candidate sets, or an inconsistent score record
invalidates the selection and selects the serial plan. Among otherwise equal
safe candidates, selection must prefer fewer children, then the canonical
lexicographic range vector. The result must be identical for identical input
values regardless of evaluation order or worker scheduling.

**R-BACK-2.73** Safety and selection must be monotone under added negative
evidence. If a policy snapshot is made no less conservative by adding a
coordinator command, hazard, attachment ambiguity, action-epoch mismatch,
ABI uncertainty, missing first-draw fact, stale generation, capacity limit, or
overflow condition, its eligible-plan set may only stay the same or shrink;
it must not gain a parallel plan. Removing such a fact may restore a plan only
after the complete certificate is recomputed. Economics observations may
change ranking among already-safe plans but must not weaken this safety
monotonicity.

**R-BACK-2.74** Any invalid or overflowed plan, proof, score, tie-break key,
range vector, or capacity calculation must fail closed to the exact serial
effective replay stream before child/segment creation, pass-action mutation,
completion registration, or command-buffer submission. The serial fallback is
not a best-effort approximation and must preserve ordered contiguous draw
coverage, coordinator commands, logical-pass actions, completion, and
reclaim semantics. A failure after the first Metal effect is a fail-stop
invariant breach; it is never a recoverable policy rejection.

**R-BACK-2.75** The policy remains default-off and no promotion claim may be
made from algebraic scoring, TLC, exhaustive/SMT checking, Render Tape
comparison, or a Metal validation run alone. Policy promotion requires the
ordered evidence bundle in `R-BACK-2.50` and `R-VERIF-6.4`, including repeated
GPU-visible correctness, matched locality/economics, supervised wild
correctness, and performance evidence. Existing serial and source-local
parallel modes remain reachable while this evidence is incomplete.

**R-BACK-2.76** A v2 identity component must distinguish the retired
`dxmt9.render_tape.identity.v1` schema from the current v2 grammar. A v1
header, version, or one-source mapping must be rejected before staging,
projection, provider invocation, or source publication; the reader must not
silently upgrade or reinterpret its bytes. v2 `EventSerial` is the reachable
one-source-per-event compatibility identity: it uses one complete event-local
record range, one `sourceOrdinal`, one `seqId`, and one completion source.
v2 `SegmentSerial` is a separate opt-in projection and may only be selected
when its complete event-group proof in `R-BACK-2.77`–`R-BACK-2.80` succeeds.

**R-BACK-2.77** A v2 `SegmentSerial` event is one ordered publication group
containing one or more identity-bearing source segments. Its generation-
stamped event-group lease must reserve, before construction, the complete
vector for every segment: source descriptors, identity slots, payload blocks
and page runs, retention entries, resource-mark claims, publication controls,
and session headroom. The lease records the event ordinal, segment count,
segment record ranges, and the next `sourceOrdinal`/`seqId` pair as one
immutable ticket. Assignment is exact and flattened: event order is primary,
`segmentIndex` is secondary, and each segment receives the next strictly
increasing `sourceOrdinal` and `seqId`; no identity is assigned from a physical
page/block boundary. Capacity or checked-arithmetic failure leaves every
cursor, watermark, and identity counter unchanged and selects the complete
EventSerial fallback before construction.

**R-BACK-2.78** SegmentSerial publication and replay must be all-or-nothing.
The group transitions `Reserved -> Writing -> Sealed -> Ready` as one
transaction; every segment descriptor, payload range, identity, retention
claim, and event summary must be complete before the one `Sealed -> Ready`
publication. A segment must never be independently Ready, and an earlier
event-group must block a younger event from bypassing it. Segments replay only
in `segmentIndex` order and may share an `EncodeSession`, but segment edges are
not physical-block, command-buffer, logical-pass, or submission boundaries.
The v2 event DAG is built once over the complete ordered record stream; a pass
may continue over adjacent segment ranges only when membership is contiguous
and its attachment, hazard, action, and logical-pass identity is unchanged.
Missing or inconsistent pass/DAG coverage rejects the whole group before any
Metal effect.

**R-BACK-2.79** Segment completion and resource lifetime must be flattened and
watermark-safe. Each segment `seqId` registers exactly one completion source and
settles exactly once in event/segment FIFO order; final event settlement is
published only after the last segment settles. A shared resource remains
retained and marked until the greatest applicable segment `seqId` and resource
completion watermark have passed. Shared payload pages, publication controls,
and event-lease credits may reclaim only after every segment receipt is
activated, completed, and detached; a segment's earlier completion must not
release storage or resources still needed by a later segment. Stale, duplicate,
or ABA receipts fail before callback, waterline, or release side effects.

**R-BACK-2.80** SegmentSerial must fail closed at the first effect boundary.
Before any receipt activation, child/segment encoder creation, or Metal effect,
a missing identity, stale generation, incomplete record/pass coverage,
unsupported control, resource-mark failure, or capacity overflow must reject
the whole group and route the complete PE event through v2 EventSerial. The
fallback must preserve the same event record order, Presenter ownership,
logical-pass actions, resource marks, and completion semantics. After receipt
activation, encoder/child creation, or any Metal effect, failure is fail-stop;
in particular, a typed `RecoverableFailure` returned by batch publication after
`replayResolvedChunk` has begun is no longer a fallback boundary and must be
converted to fail-stop without recursively replaying EventSerial. This keeps
semantic effects exactly once; partial rollback, orphan segment completion,
mixed event fallback, and a second publication are forbidden. `EventSerial`
remains reachable when SegmentSerial
is unset, unsupported, malformed, or rejected; no fallback may revive v1.

**R-BACK-2.81** Closure of the bounded SegmentSerial lane requires the ordered
evidence bundle from `R-BACK-2.50` and `R-VERIF-2.13`–`R-VERIF-2.15`: a bounded
refinement model must cover group lease/publication/abort, exact flattened
identity assignment, pass/DAG continuity, per-segment completion, final event
settlement, shared resource/page watermarks, and EventSerial fallback; native
truth-table and fake-backend tests must bind those predicates to the production
helpers; and a GT2 identity-v2 capture/replay run must authenticate v2
EventSerial and a non-vacuous SegmentSerial attempt. GT2 r17 proves production
SegmentSerial admission, exact 147-segment capture identity, 43 ordered event
settlements, and a two-segment executable projection; r65/r66 remain historical
single-source and FULL_SNAPSHOT evidence. These runs do not prove performance
or promotion. Promotion additionally requires paired
GT2 visual/locality/no-gputrace results with zero GPU errors, repeated identity
and segment evidence, then the cross-workload gates; until then the Tape and
SegmentSerial selectors remain default-off and EventSerial remains reachable.

**R-BACK-2.82** A capture-era scheduling stall must preserve its exact
production frontier without changing scheduling, capacity, wake, or lifetime
policy. Capture-only PE execution must emit ordered cold breadcrumbs for
bootstrap entry, sealed-overlay completion, closure completion, bootstrap
completion, arm completion, captured-Present reserve entry/return, and pending-
alias flush return with the exact HRESULT disposition. The successful lifecycle
must distinguish arm completion from the following captured Present's reserve,
identity settlement, and publisher closure; a pending alias destroy must remain
pinned until its ordered flush and bootstrap ownership complete.

When `DXMT_PERF_COUNTERS` is enabled, the denied-first-lease production
classifier and its eligibility predicate must expose wait enter/current,
generation retry, admission-pressure-serial, producer-wait-serial, stop,
no-admission-pressure, exhausted-
credit, non-Arena, Present, ordinary-capacity, high-water, and credit-rearm
counts, plus observed/current capacity generation and exact Ready-head `seqId`
and `sourceOrdinal` gauges. The Arena admission gate must expose enter/current
and retry/stop exits. Replay offload must expose current drain/push waiters, raw
in-flight state, and a typed `plan`, `arena_admission`, `encode`, or `done`
stage. A scheduling-watchdog threshold report must include these values without
waiting for a later Present or creating a second watchdog. Every new hot-path
operation must remain behind the existing perf or capture gate, and native
tests must use the production predicates and condition variables without
sleeping or polling.

**R-BACK-2.83** Final WSI quiescence when the CPU-ready session coordinator is
the selected encode worker must be a terminal session drain, not only a
compatibility-writer flush. It must first
publish any final Legacy writer and fix one terminal `seqId` fence, then post a
typed terminal release and wake the session coordinator. Acknowledgement
requires every Ready source through that fence to be represented and any open
session to be submitted. Final quiescence may complete only after the terminal
sequence, its Present ordinal, and Tape residency through the fence have
completed and reclaimed. Admission pressure must not synthesize this release.
The compatibility worker retains its existing writer-publish/sequence-wait
contract. Until DCE lookahead and session release are explicitly composed, a
simultaneous Tape+DCE request must fail closed to the session coordinator so no
unowned terminal latch or Direct-Arena DCE path is reachable. A native
production-loop fixture must pin an open session, a later
Ready suffix, and a final Present without polling Metal or relying on sleeps.

**R-BACK-2.84** Opt-in supply-latency observation must distinguish Legacy and
Arena sources and the two ordered stages `replay entry -> Ready publication`
and `Ready publication -> encode dequeue`. An observation is consumed only by
the same payload kind and exact `(source id, storage generation, control index,
seqId)` identity; a same-kind replay entry that precedes identity assignment
may bind once
to the next admitted source, and SegmentSerial siblings receive source-local
admission entries. Missing attribution and bounded-ledger overflow must be
reported explicitly rather than folded into latency. With performance counters
disabled, this observer must allocate no ledger, read no clock, and perform no
counter atomic operation. Every pre-publication Arena attempt owns a bounded
queue-local token that follows its unbound entry into every exact
SegmentSerial sibling. Failure must cancel only unpublished observations with
that token before retrying or admitting another source; it must not erase an
unrelated attempt, and published exact identities remain live until dequeue.
If token reservation fails, admission must not create unowned exact children;
the later publication reports attribution loss instead.
It is diagnostic evidence only and cannot change admission, publication,
dequeue, release, or completion policy.
