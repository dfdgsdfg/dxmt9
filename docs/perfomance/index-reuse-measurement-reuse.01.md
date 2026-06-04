---
domain: index-reuse-measurement
subcategory: reuse
order: 01
title: Indexed Unique Vertex / Index Reuse Probe
date: 2026-06-02
type: measurement
status: accepted
source: specs/perfomance.plan.md#L2320-L2467
---

# Indexed Unique Vertex / Index Reuse Probe

**Question / hypothesis.** Does the ~1.6 GiB top-3 VS-buffer-write bucket come
from raw indexed-reference count, or from the smaller post-transform vertex
work? Establish whether Xcode `VS Invocations` tracks submitted references,
draw-local unique vertices, or finite vertex-cache misses.

**Method.** `DXMT9_MEASURE_INDEX_REUSE=1` with `DXMT9_PERF_ENCODER_BREAKDOWN=1`
(wrapper `--measure-index-reuse`). Diagnostic-only scan of indexed draw buffers
that counts raw indexed references, a draw-local unique-index estimate, and
LRU 16/32/64-entry cache-miss estimates. Does not change draw submission.
Run `app-d3d9-3dmark05-measure-index-reuse-gputrace-r1` with matched Xcode
counter export.

**Result.** Probe is GPU-neutral: top-3 VS buffer write `1627.240MiB → 1627.285MiB`
(`+0.00%`), top-3 GPU time `34.837ms → 35.239ms` (`+1.16%`). Top-3 aggregate:
indexed references `2,146,185`; draw-local unique estimate `951,736` (reuse
`2.255x`); Xcode VS invocations `1,178,584` = `0.549x` references but `1.238x`
unique; VS buffer write `1627.285MiB` (`1792.9B/unique`, `1447.8B/VS invocation`);
`385/0` measured/skipped draws. Per hot encoder VS-inv/unique: `60/2 = 1.292x`,
`60/1 = 1.160x`, `60/0 = 1.232x`. Finite-cache validation run (722-draw, not a
matched Xcode frame): refs `3,121,914`, unique `1,523,119`, cache-miss 16/32/64 =
`2,037,449`/`1,944,132`/`1,847,341` (`1.338x`/`1.276x`/`1.213x` unique). Matched
Xcode cache run (723 draws): top-4 VS-inv/cache64 = `0.976x`, VS-inv/unique =
`1.184x`, `856.3B/invocation` vs `184B` visible VSOut.

**Verdict.** Accepted as measurement. VS invocations follow the post-transform
**cache-miss estimate** (`VS-inv/cache64 ≈ 0.976x`), not raw references (`0.549x`)
and not raw unique (`1.18x` gap is finite-cache locality). This is the result
that makes index-cache locality the promising lever. It does NOT explain the
hidden write width: `~836-879B`/invocation vs `184B` VSOut stays unexplained.

**Related.** [[index-reuse-measurement]] · baseline geometry from
[[index-reuse-measurement-geometry.03]] · width owner [[hidden-backend-storage]] ·
motivates [[index-cache-locality]] and [[primitive-reorder-diagnostics]] ·
refutes visible-width ownership [[vsout-layout]].
