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
The new typed receipt is not yet interchangeable with the production
`EndReceipt` polling API; discarding its cancellation authority is forbidden.

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
It is a native integration component, not an enabled replay source. No Draw,
resource-marking, queue submission or GPU-consumer support is implied.

## END, FLUSH and completion

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
Present, ordered control and any bounded-capacity escape also seal; initializer
and inflight pressure obligations remain intact. Progress requires eventual
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

PE ABI, runtime defaults and encoder scheduling are unchanged by the prototype.
Implementation/evidence status lives in [gap.md](gap.md).
