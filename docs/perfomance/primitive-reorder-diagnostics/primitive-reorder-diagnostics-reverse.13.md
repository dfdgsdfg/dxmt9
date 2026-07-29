---
domain: primitive-reorder-diagnostics
workload: 3DMark05 GT1
subcategory: reverse
order: 13
title: Row/Class 60/4 Large4096 + Alpha + Scissor Reverse
date: 2026-06-02
type: experiment-run
status: inconclusive
outdated: retired-journal
source: specs/perfomance.plan.md#L10874-L11009
---

# Row/Class 60/4 Large4096 + Alpha + Scissor Reverse

> **Outdated — this leaf's only `source:` is the retired `specs/perfomance.plan.md` journal, which was deleted.** The numbers below cannot be re-derived or re-checked. Kept as history; do not cite it as current evidence.

**Question / hypothesis.** Does the narrowest intersection
`large4096 && alpha-blend && scissor` — only 4 of 253 `60/4` draws — own the
positive signal seen at 19 ([primitive-reorder-diagnostics-reverse.11](primitive-reorder-diagnostics-reverse.11.md)) and 16
draws ([primitive-reorder-diagnostics-reverse.12](primitive-reorder-diagnostics-reverse.12.md))?

**Method.** `run_3dmark05_perf_probe.sh --suffix reverse-row-60-4-large4096-alpha-scissor-gputrace-r1
--probe-reverse-indexed-triangles --probe-reverse-indexed-triangles-row 60/4
--probe-reverse-indexed-triangles-classes large4096,alpha-blend,scissor` +
`finalize` with strict gates. Smoke: 4/249 draws (`127,656B`). A
`3dmark05-perf-indexed-probe-draws.csv` draw-sample artifact confirmed the 4
draws (`73/74`, `173/174`, prims `5708`/`4930`) are screen-blend
(`InvDestColor + One + Add`), depth-write off, two near-full overlapping scissor
rects (`0,0,190,553` / `0,0,200,542`). All gates PASSED in the historical capture.

**Result.** (Historical) The 4-draw probe reproduced the full signal: top VS
write `-7.46%`, top GPU `-7.46%`, `60/4` VS write `-22.32%`, bytes/inv `-18.97%`.
Matched rows moved `-109.838MiB` VS write (`-91.731MiB` bytes/inv, `-18.107MiB`
invocation); target `60/4` `-82.652MiB`. Mutating only `4 / 253` draws moved the
whole-frame bucket by the same amount as the 19- and 16-draw probes.

**Verdict.** Inconclusive / historically minimal owner. This made the 4 large
scissored screen-blend draws the best historical candidate — and the
order-independence of screen blend made a production predicate plausible. BUT a
later current-HEAD rerun ([primitive-reorder-diagnostics-reverse.15](primitive-reorder-diagnostics-reverse.15.md)) found the
same 4-draw mutation no longer moves VS write (`+0.00%`). Reclassified as a
shape-sensitive anomaly, not a stable owner. The screen-blend safety predicate
nonetheless seeded [index-cache-locality](../index-cache-locality/index.md).

**Related.** [primitive-reorder-diagnostics](index.md) · from: [primitive-reorder-diagnostics-reverse.12](primitive-reorder-diagnostics-reverse.12.md)
· current rerun: [primitive-reorder-diagnostics-reverse.15](primitive-reorder-diagnostics-reverse.15.md) · safe-subset attempt: [primitive-reorder-diagnostics-reverse.14](primitive-reorder-diagnostics-reverse.14.md)
· [index-cache-locality](../index-cache-locality/index.md) (screen-blend optimization candidate) · [hidden-backend-storage](../hidden-backend-storage/index.md).
