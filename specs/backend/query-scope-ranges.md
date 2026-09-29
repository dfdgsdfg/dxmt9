---
type: "Spec"
title: "Source-local immutable query membership ranges"
description: "Candidate contract separating query membership from source publication."
tags: [backend, queries, verification, dod]
---

# Source-local query membership ranges

## Contract (R-CORE-8.2(c))

The reference publishes each populated writer before changing occlusion
membership. The candidate may retain ordered command ranges in one source,
but may not change which issue-generation owns any passing-sample contribution.
Range compression is metadata compression, not draw/pass reordering or pass
coalescing. Existing preserving attachment policy remains required.

Each range is a half-open command interval and a source-table scope identity.
Ranges cover the command stream exactly once, in order, without gaps or overlap.
Adjacent equal identities may share metadata; distinct issue generations must
not merge. Empty scopes use the canonical zero identity. Empty ranges are not
stored. Mandatory clear/blit/pass boundaries remain in the command stream.

## Ownership and admission

When ranges are enabled, an already validated non-capture raw containing only
ordinary Draw/DrawIndexed/APPLY_STATE and at least one draw may select the
existing compatibility transaction before Arena capacity planning. Replay must
cancel its speculative next-source intent before effects and retain the same
resource-marking and serial replay owner. State-only, UP, controls, capture
(including incomplete token/event pairs) and unlisted families keep the full
planner. `queryRangeCompatibilityShortcut` and the CPU-ready plan opcode truth
table bind this pre-effect routing restriction; no new publication, completion
or lifetime transition is introduced. `OcclusionScopeUnsupported` is an
expected representation fallback, not a warning-level admission failure.

Replay/queue construction owns bounded flat range storage and a frozen scope
table retaining the existing immutable `OcclusionScopeRef` / `OcclusionResult`
generations. Encode reads these owned tables, never mutable Query/COM state.
Scope identity is table-local; writer queue/index/generation/storageGeneration
qualifies the table and excludes recycled storage. An integer identity alone
does not pin a result or authorize table access.

`render::planQueryRangeAppend` is the candidate arithmetic reducer: check
identity and command/range dimensions before any write; extend the final
range or append one. Invalid input refuses unchanged. Capacity pressure returns
SealRequired unchanged. Physical storage, scope-table and retain admission
must succeed atomically before consuming this witness. On refusal seal and
retry with a newly owned writer, or use the old path before effects; never
drop a command or reuse a partially written range. The reducer is not itself
a storage/retention capability or an allocation-failure proof.

### Bounded metadata owner

`render::QueryScopeRangeOwner<S,R,E>` owns fixed arrays of scope snapshots,
ranges and END offsets. It is noncopyable and move-constructible; moving
invalidates the draft. Scope snapshots copy only active retained result refs,
not the caller's mutable scope container. Membership is interned by exact
result ownership regardless of input ordering. Distinct result storage with
the same object/issue/epoch is rejected rather than splitting one identity's
accumulator. Unqueried ranges use no scope-table entry.

The command, range, scope and END capacities are independent. Append admission
performs all checks before the noexcept reference-copy/metadata commit. The
first nonempty END allocates one shared state per writer; later ENDs reuse it.
Allocation exception/null result preserves the draft and output receipt. Empty
END is deliberately rejected by this metadata component: the queue must handle
it with the preceding-prefix rule rather than creating a fake deferred marker.

Phases are Writing -> Sealed -> Published, with Writing/Sealed -> Cancelled.
Only sealed/published owners expose const borrowed range/table views; the owner
must not move or die while a consumer holds a view. Seal freezes metadata.
`bindPublished` is called under the queue lock only after actual publication;
it does not itself prove publication or enqueue work. Published cannot roll
back. Actual queue failure continues through the queue's terminal state.

Typed `QueryRangeEndReceipt` retains cancellation state together with the
existing EndMarker and its per-END logical/initializer prefix. Unpublished
owner abandonment marks cancellation, so its receipts fail rather than remain
unbound forever. Wrong-queue receipts are invalid. Cancellation is local to
this writer receipt and does not fail unrelated shared result generations.
The production `EndReceipt` adapter aliases the retained EndState's marker;
the marker itself owns the cancellation bit. No raw pointer or cancellation
authority is discarded. Only `RetainedCancellable` markers authorize immutable
snapshot polling before publication. Ordinary coalescing markers retain their
existing legacy-query fallback. An independent FLUSH still validates the exact
live writer and publishes it under the queue mutex; eligibility never implies
completion or bypasses reset/initializer/result checks.

This owner reserves metadata against a command count, not physical command
storage. Queue integration must compose payload allocation/append with this
transaction and retain the published owner through consumer/completion lifetime.
Fixed-array zero-initialization, bounded table scans and reference copies have
not yet been cost-qualified. Template capacities are not production defaults.

### Physical append seam

`appendWithPayload` is a single-writer draft transaction: tentatively admit
metadata, prepare physical storage without changing payload logical lengths,
then perform a statically noexcept commit. Preparation failure restores the
cursor, last extended range and new scope refs. It leaves END metadata intact.
The callback is compile-time bound, not a type-erased runtime observer. It may
not publish, re-enter the owner, or introduce an externally visible effect.
This seam is not a general rollback mechanism for arbitrary callbacks.

The initial `QueryRangeClearSource` owns a real `core::ChunkSlot` plus this
metadata owner and admits only Clear. It stages the nested rect copy and
reserves both header/clear vectors before calling the actual rvalue Clear
appender. No payload is exposed while Writing. Failed preparation may retain
larger vector capacity but does not change semantic sizes or references;
future queue credit accounting must cover that physical capacity explicitly.
It remains the narrow native Clear fixture. `appendQueryRangeDraw` additionally
composes the actual DrawRun appender with metadata. All record/state/uniform/
byte and lookup vector capacities are reserved before any lookup rebuild or
logical commit. Existing batch reservation and interning are then reused in
the noexcept commit. Failure preserves every schema length, old lookup chain,
scope retain and END offset; surviving physical capacity remains priced.

### Experimental queue and consumer binding

`DXMT9_QUERY_SCOPE_RANGES=1` requires actual Metal occlusion and asynchronous
fences. It defaults OFF. The initial route uses compatibility Slot storage;
Direct, Arena, session joining, framegraph DCE/reordering and parallel encoding
are excluded. This is not an all-family promotion.

Membership replacement freezes the preceding physical command prefix in the
bounded owner instead of publishing it. DrawRun append uses the transaction
adapter after the ordinary segment resource admission. Clear/control suffixes
are frozen at membership/publication edges; failure stops publication. A free
range/scope entry is kept for the next suffix; pressure publishes before the
next membership. END capacity uses eager publication. An already existing
ordinary deferred marker is published before adopting a range owner, never
overwritten. Published source ownership remains in the real CpuReadyTape.

`prepareSlotForPublish` seals metadata and installs a conservative source-wide
query guard for PSO prefetch. This guard must cover queries anywhere in a source,
even when its final scope is empty. It is not the draw's membership. The queue
binds the owner's marker only after successful `sealAndPublish` with the actual
queue/source/storage identity and sequence. The existing locked poll path
publishes an unbound owner and posts the sticky submission request.

Encode selects frozen scope membership by command index. Membership and recorded
END boundaries close pending clears/render passes before switching scope. The
existing preserving attachment policy and origin-CB callbacks are unchanged.
Callback scope refs alias the retained table; result generations survive payload
retirement. Failed encoding marks every retained scope failed, including scopes
not represented by the source-wide prefetch guard.

## END, FLUSH and completion

### Serialized ordered-control release

For a validated BEGIN or END of a resolved GPU occlusion query only, the
range candidate may retain the unpublished compatibility writer across the
replay ordered-control release. `orderedControlWriter` admits exact flags
1 (END) or 2 (BEGIN); other query kinds, unknown flags and invalid locators
keep the publication cut. `retainWriterForSessionRelease` additionally
requires DirectObservation/ClosePass and the enabled range policy.

The release still posts one FIFO event, wakes Encode and waits for its exact
acknowledgement. Its fixed sequence fence covers only previously published
sources. The acknowledgement is a pass-close receipt for that prefix, not
completion or publication of the retained writer. The following query dispatch
freezes membership at the physical command boundary; encode applies its pass
cut when consuming that command range. END captures the initializer prefix
and a retained writer marker; FLUSH publishes/binds/submits through the existing
owner path. Capacity may still force earlier publication.

Readback, UpdateTexture, non-occlusion queries, reset and terminal submission
retain their existing obligations. A late failure after replay effects is
fail-stop, never replayed through a second fallback. The bounded release test
composes production release/ack predicates and END polling over the old-prefix,
END, publication, submission, completion and initializer event permutations;
it does not prove arbitrary concurrent reset or Metal callback behavior.

Membership replacement alone need not publish. A nonempty END records an
immutable logical command prefix and initializer prefix and retains an unbound,
writer-qualified marker. An END must not seal merely to assign a sequence:
otherwise moving the scope boundary saves little or no publication work.
Empty END preserves the existing completed-prefix semantics.

The candidate reuses `EndMarker::bind` / `classifyEnd` once it is integrated:
bind only after successful actual publication, only for the admitted writer,
once. Multiple ENDs may conservatively wait for the same containing-source
sequence. This may delay availability, never authorize earlier success or
change result membership. Intermediate END offsets remain available to audit
the logical prefix; the initial candidate does not promise sub-source retirement.

FLUSH must seal an unbound END's owner and post a sticky submission obligation
even without later commands. It must not wait for GPU completion. A source
waiting on its own result cannot hold an unsubmitted suffix indefinitely.
Present, non-membership ordered controls and any bounded-capacity escape also
seal; initializer and inflight pressure obligations remain intact. Progress requires eventual
worker execution/completion, not merely an arithmetic boundary predicate.

Each contribution remains associated with its actual originating CB and frozen
issue generation. Failed origin cannot be redeemed by a later successful tail.
Callbacks precede the ordered completion/reclaim waterline; zero pending before
encode does not imply completion. Reset, reissue and public release must not
redirect older contributions. Scope/table/storage ownership survives until all
consumers and completion obligations retire.

## Verification and performance gates

This is a stateful R-VERIF-1.5–1.8 lane. Required evidence is:

1. Bounded native range refinement: independent per-command membership versus
   candidate intervals, exact order/coverage, overlaps, generations, failed
   origins and every capacity dimension. Shared candidate reducer is exercised,
   but this alone is not production queue/encoder binding.
2. Existing QueryEndCoalescing, QueryOcclusionResults and QueryInitializerFence
   models remain prerequisites. Their composition with command-prefix ranges,
   FLUSH without successors, capacity retry and reset needs explicit refinement;
   isolated model success is not that composition proof.
3. Native actual receipt checks bind existing EndMarker/classify predicates.
   Storage/retain fault injection and asynchronous queue interleavings require
   the integrated ownership implementation.
4. A real Metal fixture must execute the range consumer and match serial
   visible/occluded/overlapping result values, pixels and failed-origin behavior.
   Existing source-based GPU tests are reference evidence only.
5. Measure identical captured input, with separate planning, publication and
   encode CPU scopes. Compare full END-to-FLUSH burst lengths and source counts:
   immediate FLUSH after every END permits much less batching than delayed
   polls. Metadata packing speed is not a proxy for production CPU/FPS savings.
6. Only then qualify wild visuals and observer-OFF repeats. No default promotion
   follows from native value checks or H408's small FPS difference.

PE ABI and default behavior remain unchanged. Candidate scheduling is explicitly
restricted as above; no wild-performance or composed end-to-end proof follows
from the bounded owner checks and prerequisite TLA models alone.
Implementation/evidence status lives in [gap.md](gap.md).
