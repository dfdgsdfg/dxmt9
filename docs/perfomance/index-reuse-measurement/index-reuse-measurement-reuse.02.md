---
domain: index-reuse-measurement
workload: 3DMark05 GT1
subcategory: reuse
order: 02
title: Order-Preserving Vertex Payload Canonicalization Check
date: undated
type: measurement
status: rejected
source: specs/perfomance.plan.md#L3142-L3187
---

# Order-Preserving Vertex Payload Canonicalization Check

**Question / hypothesis.** Could VS invocations be cut without reordering
triangles, by canonicalizing equal vertex payloads to the same index? That would
reduce post-transform work while preserving depth-read/color-write primitive
ordering — the safest locality idea after primitive reorder was rejected.

**Method.** One-off offline dump analysis over the 19-draw `50/2` window. Loaded
every index plus every captured stream payload, built a per-index vertex key from
all stream bytes (stream0 + stream1), then remapped identical payloads to
canonical indices while preserving the original index sequence, and simulated
LRU32 before/after.

**Result.** Draws checked `19`; aggregate unique indices `29,268`; aggregate
canonical vertex payloads `29,268`; canonical vertex delta `0`. LRU32 before
`42,853` = after `42,853` = delta `0`. Per draw, every draw had
`unique_indices == canonical_vertices`.

**Verdict.** Rejected. There are no duplicate vertex payloads to merge in this
material window, so order-preserving canonicalization gives zero locality benefit
here. The remaining locality lever is therefore primitive order (semantically
constrained for depth-read/color-write draws) or a backend-shape variant that
changes AGX storage without changing submitted primitive order.

**Related.** [index-reuse-measurement](index.md) · follows
[index-reuse-measurement-reuse.01](index-reuse-measurement-reuse.01.md) · pushes work back to
[primitive-reorder-diagnostics](../primitive-reorder-diagnostics/index.md) and [index-cache-locality](../index-cache-locality/index.md) · width owner
remains [hidden-backend-storage](../hidden-backend-storage/index.md).
