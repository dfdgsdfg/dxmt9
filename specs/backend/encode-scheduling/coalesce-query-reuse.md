# Graph-qualified coalescing query reuse

The reference is the pass-coalescing algorithm at `bba48fd8`. Optimization
must preserve ascending pair selection, the first safe merge, ordered command
and draw ranges, resource access remapping, load/store output, active-seed
witnesses, and enabled diagnostic counters. Semantic flags alone are not
hard boundaries.

Reachability and pass summaries belong to one immutable graph incarnation.
A successful merge destroys that query owner before the next search. Cached
reachability is reflexive, supports cycles, and ignores out-of-range edges
for valid endpoint queries. Storage admission is bounded; oversize graphs
use the reference traversal, not a reduced candidate search.

Hard-boundary prefix rejection is permitted only without return diagnostics
and without an active-seed left endpoint. Otherwise preserve the original
sequential rejection classification and partial diagnostic accounting.

Return diagnostics require explicit opt-in independently of generic counters.
Disabled counters do not establish that no return candidates exist.

Verification uses the frozen reference in test/probe executables only, full
graph comparison on bounded generated cases and captured inputs, and separate
native CPU measurements. These checks establish graph refinement, not GPU
pixels or end-to-end FPS. Wild/GPU evidence must be reported separately.

The bounded native domain enumerates every four-pass forward DAG with binary
attachment assignments, and every three-pass directed graph including cycles
and self edges, under ten command/boundary/seed variants and both diagnostic
settings. It compares every optimizer statistic and active-seed witness in
addition to full graph output. Directed 65/129-pass cases cross bitset words;
pass-count and edge-count overflow cases exercise exact traversal fallback.

Malformed ranges are tested only in non-merging A-B-A rejection windows.
The reference algorithm assumes that unrelated passes copied during a merge
have valid ranges; this optimization does not introduce a graph validator or
claim arbitrary malformed-input safety. Allocation failure timing, unbounded
graphs, driver behavior, and pixel contents are outside the exhaustive domain.
