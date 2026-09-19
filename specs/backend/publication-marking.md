---
type: "Design"
title: "Bounded publication stamp reuse"
description: "Exact per-Slot texture and surface stamp reuse without changing buffer effects."
tags: [backend, lifetime, verification]
---

# Bounded publication stamp reuse

This is an experimental implementation of resource lifetime preservation under
R-BACK-2.51 and R-VERIF-1.5–1.8, selected by
`DXMT9_PUBLISH_RESOURCE_DEDUP=1`; the reference remains default.

`markSlotResourcesUnlocked` holds the queue ownership boundary and visits an
immutable Slot at one fixed sequence. `Pool::markTextureUse` and
`Pool::markSurfaceUse` only update the corresponding generation-qualified
record's last-use sequence by `max(old, sequence)`. Repeating that update is
idempotent. A 64-entry direct-mapped stack cache suppresses only an exact
kind/full-handle match already visited in this publication. Collisions replace
the entry and cause another real mark; they never imply membership.

The cache must not outlive this publication. Resource creation/replacement and
reclaim cannot interleave inside the locked walk. Source retention is unchanged;
this optimization never substitutes an earlier ingress stamp for final use.
Buffer active-backing selection and captured-backing stamps are unchanged.
Captured-buffer batch flushes still occur before every noncaptured reference,
even if a texture/surface stamp is skipped, and at every command boundary.
The shared resource visitor and Arena publication path are unchanged.

The reference is a stamp at every visited reference. The native bounded
refinement enumerates length-seven traces over six identities at capacities
1, 2 and 64, comparing every prefix's stamp state. It includes kind aliases,
generation changes, collisions, nulls, buffers, and a fresh publication.
This binds to the production cache predicate, not a second cache algorithm.
It does not prove the queue's locking contract or concrete GPU residency;
existing queue/lifetime TLA and integration tests remain required. Promotion
also requires actual pool/GPU lifetime evidence, wild correctness and measured
publication cost; a passing value test alone does not authorize promotion.
