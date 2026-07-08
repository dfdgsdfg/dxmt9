---
domain: index-cache-locality
workload: 3DMark05 GT1
subcategory: cpucost
order: 12
title: Lazy Priority Frontier Rejection
date: 2026-06-05
type: experiment-run
status: rejected
source: experiments/output/app-d3d9-3dmark05-defaultgate-noenc-opaque-depth-lazyfrontier-r1/3dmark05-perf-summary.md; traces/app-d3d9-3dmark05-defaultgate-noenc-opaque-depth-lazyfrontier-r1/analysis/selectvolume-r1-vs-lazyfrontier-r1-run-counters.md
---

# Lazy Priority Frontier Rejection

**Question / hypothesis.** [index-cache-locality-cpucost.11](index-cache-locality-cpucost.11.md) rejected a hard
frontier cap because it reduced active width without reducing select CPU. Can a
heap-backed lazy priority frontier reduce the full-vector rescan cost while
keeping candidate quality acceptable?

**Implementation.** Added diagnostic-only
`DXMT9_INDEX_CACHE_CANDIDATE_LAZY_FRONTIER=1` and wrapper option
`--index-cache-candidate-lazy-frontier`. The selector pushes candidates with a
score epoch and accepts current-epoch heap entries directly; stale entries are
refreshed and reinserted. It reports:

| Counter | Meaning |
|---|---|
| `encode_draw_index_cache_candidate_lazy_heap_pops` | heap entries popped while selecting |
| `encode_draw_index_cache_candidate_lazy_refreshes` | stale entries rescored and reinserted |
| `encode_draw_index_cache_candidate_lazy_stale_drops` | obsolete heap entries discarded |
| `encode_draw_index_cache_candidate_lazy_accepted` | heap entries accepted as selected candidates |

This is intentionally approximate. A stale low-priority entry can become a good
current candidate without bubbling to the top, so the path can change primitive
order and candidate quality.

**Run.**

```sh
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix defaultgate-noenc-opaque-depth-lazyfrontier-r1 \
  --frame 50 --no-gputrace --no-encoder-breakdown --timeout 180 --top 5 \
  --optimize-opaque-depth-index-cache \
  --optimize-opaque-depth-index-cache-min-gain-pct 10 \
  --index-cache-candidate-lazy-frontier
```

The harness passed with timeout finalization and `present_encoded=1440`.

**Result.** Lazy selection reduced score computations, but the heap/refresh path
was slower and produced weaker candidates:

| Metric | uncapped selectvolume-r1 | lazyfrontier-r1 | Delta |
|---|---:|---:|---:|
| `encode_draw_index_cache_candidate_select_cpu_ms` | `91.635` | `111.247` | `+21.40%` |
| `encode_draw_index_cache_candidate_build_cpu_ms` | `124.083` | `143.899` | `+15.97%` |
| `encode_draw_index_cache_candidate_cpu_ms` | `152.117` | `173.576` | `+14.11%` |
| `encode_draw_index_cache_candidate_select_calls` | `302,538` | `302,538` | `0` |
| `encode_draw_index_cache_candidate_select_scored` | `2,061,493` | `392,236` | `-80.97%` |
| `encode_draw_index_cache_candidate_select_candidates_max` | `157` | `442` | `+181.53%` |
| `encode_draw_index_cache_candidate_lazy_heap_pops` | `0` | `392,236` | `n/a` |
| `encode_draw_index_cache_candidate_lazy_refreshes` | `0` | `156,067` | `n/a` |
| `encode_draw_index_cache_candidate_lazy_accepted` | `0` | `236,169` | `n/a` |
| `indexed_cache_opt_candidate_original_miss32` | `530,289` | `530,785` | `+0.09%` |
| `indexed_cache_opt_candidate_miss32` | `418,033` | `434,791` | `+4.01%` |
| `reordered_index_cache_created` | `67` | `63` | `-5.97%` |
| `reordered_index_cache_hits` | `243,495` | `232,907` | `-4.35%` |

No-gputrace GPU proxy moved down (`gpu_command_buffer_time_ms -1.64%`), but this
is not enough to offset the direct CPU regression and quality loss. The run also
changed candidate population (`125→128` candidate draws), so it is not a
drop-in acceleration of the accepted opaque-depth path.

**Verdict.** Rejected. The experiment proved that score computations can be cut,
but a generic `std::priority_queue` lazy refresh path costs more than the vector
scan and degrades LRU32 quality. The next viable direction needs a cheaper
domain-specific frontier, not a general heap:

- bounded per-vertex dirty candidate lists,
- small fixed arrays/buckets keyed by cached-vertex count,
- or candidate construction that avoids revisiting low-value triangles at all.

Do not promote `DXMT9_INDEX_CACHE_CANDIDATE_LAZY_FRONTIER` beyond diagnostic use.

**Related.** [index-cache-locality](../index-cache-locality.md) · prev:
[index-cache-locality-cpucost.11](index-cache-locality-cpucost.11.md) · [index-cache-locality-cpucost.10](index-cache-locality-cpucost.10.md).
