---
domain: primitive-reorder-diagnostics
workload: 3DMark05 GT1
subcategory: minindex
order: 05
title: Screen-Blend Run 71..188 Min-Index Rerun R2
date: undated
type: experiment-run
status: rejected
outdated: retired-journal
source: specs/perfomance.plan.md#L16248-L16340
---

# Screen-Blend Run 71..188 Min-Index Rerun R2

> **Outdated — this leaf's only `source:` is the retired `specs/perfomance.plan.md` journal, which was deleted.** The numbers below cannot be re-derived or re-checked. Kept as history; do not cite it as current evidence.

**Question / hypothesis.** Rerun the full-frame min-index / address-locality
probe with real disk headroom and a complete Xcode export (`result.json`,
performance-embedded `.gputrace`, counters after draw-counter profiling). Is the
large VS-write drop a verified optimization, or only a backend
address/density classifier that fails the stable-frame geometry gates?

**Method.** Suffix `screen-blend-run-71-188-sort-gputrace-r2`, full-frame
min-index sort over run 71..188. Compared to the clean current-head baseline
with `compare_xcode_dxmt_bottlenecks.py --require-stable-frame-proof` (top
row-key match + GPU/VS/unexplained-write decrease + `0.05` draw/vertex/triangle
drift gates).

**Result.** Aggregate looks like a win: total GPU `35.456 → 28.394ms`
(`-19.92%`); top VS buffer write `1627.240 → 1120.059MiB` (`-31.17%`);
unexplained write `-31.26%`; top VS B/inv `1447.741 → 740.925B` (`-48.82%`);
expected VSOut still `184B`; top rows matched `60/0,60/1,60/2`. **But** top draw
calls `385 → 616` (`+60.00%`), top vertices `+24.71%`, triangles `+24.71%`,
stream handle changes `+62.70%`, PSO handle changes `+222.45%`. Row attribution:
`60/2` improves via bytes/inv (`1602.6 → 444.9B`, `-72.24%`); `60/0` likewise;
but `60/1` *regresses* via invocation growth (`+48.99%`) and becomes the new
largest writer. Stable-frame gate fails: draw `+60.00%`, vertex/triangle
`+24.71%` exceed the 5% limit.

**Verdict.** Rejected as an optimization proof — address/backend classifier
only, not a verified fix. The only useful signal is narrow: row identity stayed
stable and hidden backend write density dropped while source-visible VSOut layout
stayed `0xfff` / `184B`, reinforcing the hidden Apple vertex/tiler/backend
storage hypothesis. Do not rerun this exact full-frame min-index sort as a proof
candidate; the geometry-locked direction has stronger evidence.

**Related.** [primitive-reorder-diagnostics](index.md) · prior:
[primitive-reorder-diagnostics-minindex.04](primitive-reorder-diagnostics-minindex.04.md) (geometry-locked rerun that
reclassifies this) · [hidden-backend-storage](../hidden-backend-storage/index.md) (bytes/inv drop at fixed 184B
VSOut → hidden storage) · [vsout-layout](../vsout-layout/index.md) (184B source vs ~1448B/inv) ·
[mini-replay-bisection](../mini-replay-bisection/index.md) (the 113-draw `60/2` prefix/window bisection preferred
over this) · [index-cache-locality](../index-cache-locality/index.md) (semantic-safe successor) · [baselines](../baselines/index.md).
