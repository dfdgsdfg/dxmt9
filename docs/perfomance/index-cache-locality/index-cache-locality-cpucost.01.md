---
domain: index-cache-locality
workload: 3DMark05 GT1
subcategory: cpucost
order: 01
title: Opaque-Depth CPU Attribution Split
date: 2026-06-05
type: measurement
status: accepted
source: specs/perfomance.plan.md#L251-L368
---

# Opaque-Depth CPU Attribution Split

**Question / hypothesis.** The opt-in's `+25%` `encode_draw_stream_bind_cpu_ms` rise —
is it real stream/IB/texture binding churn, or is it the indexed reorder/cache work?
Split the broad bucket into narrow timers to find the owner.

**Method.** Added counters `encode_draw_raster_state_cpu_ms`,
`encode_draw_vertex_stream_bind_cpu_ms`, `encode_draw_texture_sampler_bind_cpu_ms`,
`encode_draw_index_setup_cpu_ms`, `encode_draw_index_cache_{lookup,candidate,apply}_cpu_ms`.
Paired baseline / `DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE=1` (`_MIN_GAIN_PCT=10`)
`run_experiment.py` runs, `--timeout 180`. Follow-up opt-in run added global
reordered-cache hit/miss counters.

**Result.** The aggregate rise is the index block, not bind churn:
`encode_draw_index_setup_cpu_ms 318.430→779.659` (`+144.84%`, the owner);
`encode_draw_raster_state +2.84%`, `vertex_stream_bind +3.87%`,
`texture_sampler_bind +0.42%` (all flat). Cache breakdown:
`candidate 386.824ms` (cold LRU/build/gate owner), `lookup 104.615ms`, `apply 3.400ms`;
`125` candidates evaluated, `67` reordered IBs created, `530,289→418,033` LRU32
(`-21.17%`); `585,263` cache lookups, `243,470` hits, `341,650` rejected hits, `143` misses.

**Verdict.** Accepted (CPU owner identified). The opt-in pays a few cold candidate
builds plus many cheap cached decisions — it is not rebuilding candidates every draw.
Candidate work is useful (LRU32 `-21.17%` on evaluated keys). GPU proof already exists;
this turn only reclassified CPU accounting. Next CPU target: lookup/rejected fast path
or candidate prewarm.

**Related.** [index-cache-locality](index.md) · prev: [index-cache-locality-opaque.05](index-cache-locality-opaque.05.md)
· next: [index-cache-locality-cpucost.02](index-cache-locality-cpucost.02.md) · [state-churn-encode](../state-churn-encode/index.md) (stream-bind context).
