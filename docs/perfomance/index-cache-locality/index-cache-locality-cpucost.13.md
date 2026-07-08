---
domain: index-cache-locality
workload: 3DMark05 GT1
subcategory: cpucost
order: 13
title: Bucketed Candidate Selector Rejection
date: 2026-06-05
type: experiment-run
status: rejected
source: experiments/output/app-d3d9-3dmark05-defaultgate-noenc-opaque-depth-bucketed-r1/3dmark05-perf-summary.md; traces/app-d3d9-3dmark05-defaultgate-noenc-opaque-depth-bucketed-r1/analysis/selectvolume-r1-vs-bucketed-r1-run-counters.md
---

# Bucketed Candidate Selector Rejection

**Question / hypothesis.** [index-cache-locality-cpucost.12](index-cache-locality-cpucost.12.md) rejected a
generic heap-backed lazy frontier. Can a more domain-specific selector reduce
full-vector rescans by keeping active candidates in four cached-vertex-count
buckets and updating only candidates adjacent to vertices that enter/leave the
simulated post-transform cache?

**Implementation.** Added diagnostic-only
`DXMT9_INDEX_CACHE_CANDIDATE_BUCKETED_SELECT=1` and wrapper option
`--index-cache-candidate-bucketed-select`. The path is mutually exclusive with
`DXMT9_INDEX_CACHE_CANDIDATE_LAZY_FRONTIER`. It reports:

| Counter | Meaning |
|---|---|
| `encode_draw_index_cache_candidate_bucket_vertex_visits` | active candidate neighbor visits during cache residency updates |
| `encode_draw_index_cache_candidate_bucket_moves` | candidate moves between cached-vertex-count buckets |
| `encode_draw_index_cache_candidate_bucket_selected` | selections served by the bucketed path |

The selector scans only the highest non-empty cached-vertex-count bucket and
uses the existing score within that bucket. This is diagnostic-only because a
lower cached-vertex-count candidate can no longer win on the secondary
cache-distance / remaining-use / min-index terms.

**Run.**

```sh
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix defaultgate-noenc-opaque-depth-bucketed-r1 \
  --frame 50 --no-gputrace --no-encoder-breakdown --timeout 180 --top 5 \
  --optimize-opaque-depth-index-cache \
  --optimize-opaque-depth-index-cache-min-gain-pct 10 \
  --index-cache-candidate-bucketed-select
```

The harness passed with `present_encoded=1440`.

**Result.** The bucketed path reduced scored candidates substantially, but its
touched-vertex bucket maintenance cost more than the scan it replaced:

| Metric | uncapped selectvolume-r1 | bucketed-r1 | Delta |
|---|---:|---:|---:|
| `encode_draw_index_cache_candidate_select_cpu_ms` | `91.635` | `121.378` | `+32.46%` |
| `encode_draw_index_cache_candidate_build_cpu_ms` | `124.083` | `153.906` | `+24.03%` |
| `encode_draw_index_cache_candidate_cpu_ms` | `152.117` | `181.433` | `+19.27%` |
| `encode_draw_index_setup_cpu_ms` | `746.219` | `764.743` | `+2.48%` |
| `encode_draw_index_cache_candidate_select_calls` | `302,538` | `302,538` | `0` |
| `encode_draw_index_cache_candidate_select_scored` | `2,061,493` | `564,710` | `-72.61%` |
| `encode_draw_index_cache_candidate_bucket_vertex_visits` | `0` | `191,339` | `n/a` |
| `encode_draw_index_cache_candidate_bucket_moves` | `0` | `190,647` | `n/a` |
| `encode_draw_index_cache_candidate_bucket_selected` | `0` | `236,169` | `n/a` |
| `indexed_cache_opt_candidate_original_miss32` | `530,289` | `530,289` | `0` |
| `indexed_cache_opt_candidate_miss32` | `418,033` | `418,018` | `-15` |
| `reordered_index_cache_created` | `67` | `67` | `0` |
| `reordered_index_cache_hits` | `243,495` | `243,504` | `+9` |

Candidate quality stayed effectively neutral (`miss32 -15`, same `67`
created buffers), but CPU regressed. The no-gputrace GPU command-buffer proxy
also moved down (`-1.46%`), but this is not a hardware proof and does not
override the direct candidate CPU regression.

**Verdict.** Rejected as a CPU optimization. This is a useful negative result:
the remaining CPU owner is not solved by replacing full rescans with a generic
candidate-maintenance structure. Even a domain-shaped four-bucket selector pays
too much in per-cache-mutation neighbor updates for the current GT1 candidate
sets. The next CPU direction must reduce candidate construction/selection calls
or avoid building candidates for low-value draws, not maintain a more elaborate
active frontier.

Do not promote `DXMT9_INDEX_CACHE_CANDIDATE_BUCKETED_SELECT` beyond diagnostic
use.

**Related.** [index-cache-locality](index.md) · prev:
[index-cache-locality-cpucost.12](index-cache-locality-cpucost.12.md) · [index-cache-locality-cpucost.10](index-cache-locality-cpucost.10.md).
