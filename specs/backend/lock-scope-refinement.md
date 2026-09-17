---
type: "Spec"
title: "Bounded queue/arena lock-scope refinement"
description: "Reference-preserving upload, captured-stamp and immutable-preparation lock scopes."
tags: [specs, backend, synchronization, verification]
---

# Bounded queue/arena lock-scope refinement

This implementation addresses the queue/arena scope mismatches in
[H307](../../docs/perfomance/present-pacing/present-pacing-lock-contention-audit.307.md).
Range-upload arena serialization, bounded captured-stamp batches and immutable
preparation are the sole runtime path. The former
`DXMT9_LOCK_SCOPE_REFINEMENT` selector and redundant outer-lock/scalar/preflight
alternatives are retired. Safety, progress, lock work and tail latency remain
separate evidence dimensions; this is not a new rendering provider.

Under R-VERIF-1.5–1.8 this is a stateful synchronization refinement, not a
pure byte-layout cleanup. The Unix device owns range admission, the resource
Pool owns arena mutation, and CommandQueue owns final sequence/publication.
The PE wire ABI, encoder policy and completion waterline protocol are unchanged.

## Reference and permitted changes

**R-BACK-SYNC-1 — range upload.** A DEFAULT+DYNAMIC range upload keeps its
caller-validated offset/extent, NOOVERWRITE or already-rotated DISCARD
admission, synchronous byte visibility and retained object lifetime. The
entire record mutation remains inside the buffer arena's exclusive lock.
It consumes no queue state or completion watermark, so it need not acquire
the queue scheduling mutex. Full upload, managed rotation, map waits and
reclaim are unchanged by this candidate.

**R-BACK-SYNC-2 — captured-backing stamps.** A bounded block may combine
captured-buffer stamps only for the same generation-qualified handle and
concrete backing, using the block's final sequence. Scalar and batched
execution must produce equal logical and backing waterlines at block flush.
Missing handles remain no-ops; a missing captured backing must preserve the
reference logical-waterline update. No backing may be confused with the
currently active backing. A logical/active-backing mark or other resource
operation flushes the block before executing; no dedup crosses that boundary.
Every pending stamp must be flushed before publication or release of its
resource/sequence ownership. Batches are bounded to at most 64 entries and
must not allocate. Failure/overflow must not silently discard work.

**R-BACK-SYNC-3 — immutable preparation.** While callers retain the source,
entry validity, payload extent calculation and pairwise immutable draw-state
compatibility may be prepared before the queue mutex is acquired. This does
not reserve a slot, select a live backing, fix a sequence, publish a prefix,
or mutate source bytes. Queue stop/active-destination state is revalidated
after acquisition before any final-slot effect. Live binding capture and
slot-relative compatibility/capacity, reserve, append and publication remain
under the reference synchronization protocol. If live capture changes payload
shape, its exact byte requirement must be settled before reserve. When capture
only fills previously empty snapshot fields, the immutable input is counted
once and newly appended snapshot records provide a checked byte increment.
Already-captured fields and other payload roles must not be counted twice;
overflow must preserve the reference rejection sentinel. A new capture path
that replaces or removes payload bytes cannot reuse this additive contract.

**R-BACK-SYNC-4 — refinement evidence.** `LockScopeRefinement.tla` must cover
range-write arena serialization, exact captured backing qualification,
flush-before-publish and retained immutable preparation with destination
revalidation. Negative configurations must distinguish wrong backing collapse,
missing flush and stale destination effects. Native tests must exercise shared
production predicates, actual batch mutation and the real range-upload entry,
including a contended unrelated queue mutex. Model progress is conditional on
fair scheduling; bounded dedup cannot hold a lock waiting for future input.
The bounded model does not replace generation-reuse/reclaim models or prove
OS mutex fairness. Stale-generation behavior must additionally be pinned in
the actual Pool; native predicate tables are explicit correspondences, not
an automatically generated proof of the entire C++ implementation.

**R-BACK-SYNC-5 — consolidation.** Preserve the bounded formal/native,
deterministic byte/readback and supervised wild correctness evidence when
making this synchronization-work reduction unconditional. Reference scalar
effects and conservative comparisons belong in tests, not a runtime opt-out.
Queue holds contain arena work and are not additive. A larger batch must not
trade lower acquisition count for an unbounded hold. This consolidation is an
explicit implementation-policy decision based on bounded correctness and
reduced redundant work; it is not a statistical CPU, latency or FPS
non-regression result. Keep unresolved performance and arbitrary-MT coverage
in the owning gap. A subsequently demonstrated material regression must be
investigated rather than dismissed because lock counts decreased. Formal/native
success does not prove driver behavior, allocator economics or all interleavings.

The conservative full-upload watermark read, complete off-slot reserve/copy/
commit, per-resource lock sharding, lock primitive replacement, and publication
mark suppression are outside this candidate. Existing `ProducerMarkReclaim`,
`BufferBackingVersioning`, `NoOverwriteByteRange` and queue lifecycle contracts
continue to apply.
