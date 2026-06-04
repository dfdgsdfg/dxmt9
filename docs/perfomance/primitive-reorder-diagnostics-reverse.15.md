---
domain: primitive-reorder-diagnostics
subcategory: reverse
order: 15
title: Current Diagnostic 60/4 4-Draw Rerun
date: 2026-06-02
type: validation
status: rejected
source: specs/perfomance.plan.md#L11652-L11809
---

# Current Diagnostic 60/4 4-Draw Rerun

**Question / hypothesis.** Re-run the historical 4-draw
`large4096 && alpha-blend && scissor` diagnostic
([[primitive-reorder-diagnostics-reverse.13]]) on current HEAD, using the
diagnostic path (not the env-gated optimization), to test whether the `-7.46%`
VS-write win is a stable property of reversing those 4 screen-blend draws.

**Method.** `run_3dmark05_perf_probe.sh --suffix
reverse-row-60-4-large4096-alpha-scissor-current-gputrace-r1
--probe-reverse-indexed-triangles --probe-reverse-indexed-triangles-row 60/4
--probe-reverse-indexed-triangles-classes large4096,alpha-blend,scissor` +
`finalize` with strict gates. Scope exact: 4 reordered / 255 skipped,
`127,656B`. All gates PASSED.

**Result.** Total GPU `34.391 -> 34.533ms` (`+0.41%`); hot GPU `+0.54%`; hot VS
buffer write `1472.747 -> 1472.767MiB` (`+0.00%`); hot unexplained write
`-0.01%`; VS bytes/inv `+0.54%`. `60/4` invocations `-0.97%` but bytes/inv
`+1.00%`, net matched-row `+0.020MiB`. The historical `-7.46%` does NOT reproduce.
A draw-sample diff vs the historical capture shows identical draw membership,
primitive sizes, blend/depth state and stream offsets — only the **scissor
rectangles drift** (`0,0,190,553` -> `0,0,196,551`), i.e. tile-coverage shape.

**Verdict.** Rejected (anomaly confirmed). The historical 4-draw win was a
shape-sensitive anomaly, not a stable owner. The real variable is the surrounding
`60/4` row shape: the historical capture happened to be a lower-churn,
lower-vertex `60/4` instance (1,068,372 vs ~1.1M vertices; 477B/inv vs ~590B/inv).
Motivated the scissor-rectangle probe ([[primitive-reorder-diagnostics-reverse.16]]).

**Related.** [[primitive-reorder-diagnostics]] · reruns: [[primitive-reorder-diagnostics-reverse.13]]
· next: [[primitive-reorder-diagnostics-reverse.16]] · [[hidden-backend-storage]]
· [[index-cache-locality]] (screen-blend opt also only moves GPU time, not VS write).
