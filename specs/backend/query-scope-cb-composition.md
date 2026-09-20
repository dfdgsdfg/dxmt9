---
type: "Spec"
title: "Query-scope command-buffer composition"
description: "FIFO command-buffer sharing without merging occlusion render passes."
tags: [backend, queries, verification]
---

# Query-scope command-buffer composition

## Contract (R-CORE-8.2(b))

The reference encodes each immutable scoped source independently. The candidate
may compose consecutive sources into one bounded EncodeSession command-buffer
chain, but must preserve source order, scope identity, render-pass boundaries,
attachment contents, resource ownership and per-pass visibility contributions.
This is CB composition, not render-pass coalescing or delayed query END.

Every scoped source must flush pending clear work and close its render/blit
encoders before returning a deferred submission. A following source must not
reuse that visibility attachment, including when both sources have the same
scope. A membership change must close any predecessor render encoder before
encoding the successor. Framegraph reorder/DCE, queried parallel passes and
Arena admission remain excluded. PE wire format and publication are unchanged.

Each contribution retains its immutable result generation, visibility storage
and originating CB. Exactly one successful-origin callback adds its samples;
an unsuccessful origin fails its scope. Callbacks must run before the ordered
completion waterline. A later successful tail cannot redeem a failed origin.
Reissue, release and reset must not redirect contributions to a newer result.

Query END fixes the existing ordinary and initializer prefixes. FLUSH must
request submission of a pending session even when no further source arrives;
it must not wait for GPU completion. Until actual successful completion and
zero outstanding contributions, polling must preserve caller bytes and must
not return S_OK. Existing capacity, initializer, Present, shutdown and failure
release rules continue to bound the session. No pin or completion obligation
may be removed merely because sources share a CB.

The semantic source/work cap and the physical lease vector are independent
bounds. A pre-effect candidate that exceeds any physical lease dimension must
remain Ready and cause the admitted prefix to be encoded/submitted first.
It must not consume successor headroom or be treated as a post-effect fatal
error. Invalid identity and a charge without an owned prefix still fail closed.

## Ownership

The query core owns membership and result generations. Source admission owns
the opt-in eligibility predicate; the encoder lifecycle owns pass closure and
contributions. The Tape coordinator retains ordered source receipts and owns
submission release. The finish path runs callbacks before completion/reclaim.
The candidate selector is DXMT9_QUERY_SCOPE_CB_JOIN, default OFF.

## Verification and promotion

1. Exhaustively check bounded scope sequences and CB partitions against the
   independent-source sum, including overlap, repeated scopes and failed origins.
   Bind admission and pass closure to production pure predicates.
2. Retain EncodeSessionCompletion and CpuReadySessionProgress TLA checks for
   ordered completion, bounded ownership and submission wake obligations.
3. Real Metal tests must prove CB sharing actually occurred, preserve separate
   passes, compare exact sample totals and framebuffer bytes, and exercise
   FLUSH without a later source, gated S_FALSE, reissue, release and reset.
4. Run observer-OFF STALKER OFF/ON/repeat with identical binaries. Compare
   frame-time windows separately from a diagnostic run measuring CB/pass and
   query publication populations. A diagnostic FPS is not a performance verdict.

The bounded value model does not prove runtime allocation behavior, Metal
driver execution, attachment bytes or pixels. Existing session TLA models do
not independently prove visibility accumulation. Missing evidence belongs in
the backend gap; no default promotion follows from only a model or clean API
validation. Pre-fragmentation wait attribution is a separate investigation.
