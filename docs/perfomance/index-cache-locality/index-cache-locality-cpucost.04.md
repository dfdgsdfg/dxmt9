---
domain: index-cache-locality
workload: 3DMark05 GT1
subcategory: cpucost
order: 04
title: Index Setup Counter Scope Correction
date: 2026-06-05
type: measurement
status: accepted
source: specs/perfomance.plan.md#L675-L787
---

# Index Setup Counter Scope Correction

**Question / hypothesis.** Is `encode_draw_index_setup_cpu_ms` the cost of resolving the
*original* index buffer, or an outer indexed-draw-path scope? The `+309ms` full-run
opt-in delta must not be mis-read as a base index-lookup regression.

**Method.** Found `encode_draw_index_setup_cpu_ms` is an outer scope enclosing source/IB
resolve, reordered-IB prelookup, candidate measure/build/gate, indexed probe telemetry,
IB bind, and the indexed draw issue. Added a narrow timer
`encode_draw_index_source_resolve_cpu_ms` (user-index upload reuse/fallback, pool IB
lookup, shadow span selection, shadow fallback transient upload). Counter-smoke runs
via `run_3dmark05_perf_probe.sh ... --no-gputrace --timeout 180` (with and without
`--optimize-opaque-depth-index-cache`).

**Result.** New narrow bucket is small relative to the outer movement. Seq50 directional
check (workload differed, `1295` vs `1440` presents): `encode_draw_index_setup_cpu_ms
408.868→1,042.989` (`+634.121ms`) while `encode_draw_index_source_resolve_cpu_ms
105.858→116.823` (`+10.965ms` only); lookup + candidate + apply account for `~541ms`.
Normalized by draw, source resolve is flat (`0.114us→0.111us/draw`). First unscoped
baseline (`status=fail`, `1080` presents) was a counter-output smoke only, confirming
`encode_draw_index_source_resolve_cpu_ms=83.960ms` exists.

**Verdict.** Accepted (scope corrected). Base index-buffer lookup/span selection is NOT
the remaining CPU owner; the owner is candidate/lookup + draw-path/bind work. Do not
rewrite the per-source reordered-IB vector cache (already beaten `unordered_map`/last-hit).

**Related.** [index-cache-locality](../index-cache-locality.md) · prev: [index-cache-locality-cpucost.03](index-cache-locality-cpucost.03.md)
· [index-cache-locality-cpucost.01](index-cache-locality-cpucost.01.md) (the original split) · [state-churn-encode](../state-churn-encode.md).
