---
domain: primitive-reorder-diagnostics
workload: 3DMark05 GT1
subcategory: minindex
order: 02
title: Cache-Aware Triangle Reorder Scout
date: undated
type: scout
status: rejected
outdated: retired-journal
source: specs/perfomance.plan.md#L13324-L13520
---

# Cache-Aware Triangle Reorder Scout

> **Outdated — this leaf's only `source:` is the retired `specs/perfomance.plan.md` journal, which was deleted.** The numbers below cannot be re-derived or re-checked. Kept as history; do not cite it as current evidence.

**Question / hypothesis.** The min-index scout proved index-range order is the
wrong axis. Does a greedy vertex-cache-aware triangle order — same draw count,
better software cache locality — reduce the hidden VS/backend write bucket when
promoted to an Xcode capture?

**Method.** `run_3dmark05_perf_probe.sh --suffix cacheopt-hotrows-span600k-scout-r1
... --probe-optimize-indexed-triangles-vertex-cache`
(`DXMT9_PROBE_OPTIMIZE_INDEXED_TRIANGLES_VERTEX_CACHE`), reusing the same
`--probe-reverse-indexed-triangles-rows 60/0..60/4 --classes large4096
--stream0-span-min 600000 --measure-index-reuse`. Heuristic: 64-entry MRU cache,
prefer candidate triangles reusing more cached vertices, fall back to original
order. No-gputrace scout promoted to gputrace candidate
(`cacheopt-hotrows-span600k-gputrace-r1`) with strict finalizer gates (top-row,
PSO, Xcode-counter/dxmt-join, `0.01` draw / `0.05` vertex/triangle drift);
baseline `current-normal-gputrace-r1`.

**Result.** Scout (software): `7` draws / `158,354` prims unchanged; cache miss
16 `300,027 → 204,771` (`-31.75%`), miss 32 `-27.67%`, miss 64
`255,598 → 195,706` (`-23.43%`); adjacent delta `-8.58%`. Promoted to Xcode —
**rejected**: Xcode GPU `35.46 → 46.98ms` (`+32.49%`); total buffer write
`1628.04 → 2033.02MiB` (`+24.88%`); top row set changed `60/0,1,2 → 60/0,3,4`;
top draws `385 → 867` (`+125.19%`), top vertices `+87.64%`, stream handle
changes `+132.72%`, IB `+111.66%`. Shared row `60/0`: VS invocations
`152,895 → 527,065` (`+244.72%`) while bytes/inv fell `1542.6 → 666.5B` —
invocation-count-driven, not a per-invocation width win.

**Verdict.** Rejected by strict finalizer. The software cache scout predicted a
cache64 win, but the Xcode replay changed top-row shape, exploded draw/vertex
volume and state churn, and inflated whole-frame write traffic — invalidating
the cache-locality signal. Not evidence that production reordering reduces the
hidden VS/backend bucket; at most it shows the per-draw transient-IB diagnostic
perturbs encoder shape too much.

**Related.** [primitive-reorder-diagnostics](index.md) · prior:
[primitive-reorder-diagnostics-minindex.01](primitive-reorder-diagnostics-minindex.01.md) · next:
[primitive-reorder-diagnostics-minindex.03](primitive-reorder-diagnostics-minindex.03.md) (geometry-locked min-index reruns)
· [index-cache-locality](../index-cache-locality/index.md) (the semantic-safe, geometry-preserving cache path
this motivates) · [index-reuse-measurement](../index-reuse-measurement/index.md) (LRU miss model) ·
[hidden-backend-storage](../hidden-backend-storage/index.md) (invocation-count vs bytes/inv attribution).
