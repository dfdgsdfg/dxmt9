---
domain: index-cache-locality
workload: 3DMark05 GT1
subcategory: cpucost
order: 08
title: Candidate Build Phase Split
date: 2026-06-05
type: experiment-run
status: accepted
source: experiments/output/app-d3d9-3dmark05-defaultgate-noenc-opaque-depth-buildsplit-r1/3dmark05-perf-summary.md; experiments/output/app-d3d9-3dmark05-defaultgate-noenc-opaque-depth-buildsplit-r1/result.json; traces/app-d3d9-3dmark05-defaultgate-noenc-opaque-depth-buildsplit-r1/analysis/defaultgate-noenc-opaque-depth-r1-vs-buildsplit-r1-run-counters.md; traces/app-d3d9-3dmark05-defaultgate-noenc-opaque-depth-buildsplit-r1/analysis/fastlookup-r1-vs-buildsplit-r1-run-counters.md
---

# Candidate Build Phase Split

**Question / hypothesis.** After rejecting the pool lookup single-scan rewrite,
is the remaining opaque-depth index-cache CPU side-effect caused by raw index
read/write, adjacency construction, or the greedy candidate selection loop?

**Instrumentation.** Split the existing
`encode_draw_index_cache_candidate_build_cpu_ms` bucket into four child
counters inside `buildVertexCacheOptimizedTriangleOrderIndexBytes()`:

| Counter | Covered phase |
|---|---|
| `encode_draw_index_cache_candidate_read_cpu_ms` | read source index bytes and build triangle records |
| `encode_draw_index_cache_candidate_adjacency_cpu_ms` | build dense/sparse vertex-to-triangle adjacency |
| `encode_draw_index_cache_candidate_select_cpu_ms` | score candidates, maintain cache, emit triangle order |
| `encode_draw_index_cache_candidate_write_cpu_ms` | write reordered index bytes |

**Run.** Used the default-policy no-encoder smoke so the numbers stay run-level
and do not require Xcode UI export:

```sh
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix defaultgate-noenc-opaque-depth-buildsplit-r1 \
  --frame 50 --no-gputrace --no-encoder-breakdown --timeout 180 --top 5 \
  --optimize-opaque-depth-index-cache \
  --optimize-opaque-depth-index-cache-min-gain-pct 10
```

The log reached `present_encoded=1440` and postprocess artifacts were generated,
but the process was manually TERM'd before the harness' full timeout window
(`capture_delay + --timeout`) elapsed. Therefore this leaf is CPU attribution
evidence only, not a pass/fail correctness or GPU proof.

**Result.** Candidate selection dominates candidate build:

| Metric | Value | Share of build |
|---|---:|---:|
| `encode_draw_index_cache_candidate_build_cpu_ms` | `131.553ms` | `100.0%` |
| `encode_draw_index_cache_candidate_read_cpu_ms` | `0.911ms` | `0.7%` |
| `encode_draw_index_cache_candidate_adjacency_cpu_ms` | `25.128ms` | `19.1%` |
| `encode_draw_index_cache_candidate_select_cpu_ms` | `99.187ms` | `75.4%` |
| `encode_draw_index_cache_candidate_write_cpu_ms` | `0.505ms` | `0.4%` |
| unscoped remainder | `5.822ms` | `4.4%` |

The opt-in workload shape stayed comparable to prior no-encoder opt-in runs:
`indexed_cache_opt_candidate_draws=125`,
`indexed_cache_opt_candidate_original_miss32=530289`,
`indexed_cache_opt_candidate_miss32=418033`, and
`reordered_index_cache_created=67`.

**Verdict.** Accepted as attribution. The hot path is not source-index readback
or writing the reordered bytes. It is the greedy selection loop: repeated
candidate scoring, cache-position scans, remaining-use queries, and candidate
set maintenance. Adjacency still matters, but it is a secondary owner after the
dense-adjacency change.

**Next CPU experiments.**

- Replace per-candidate full rescoring with a bounded active-frontier or heap
  keyed by cached-vertex count / cache distance.
- Avoid repeated `cachePosition()` scans inside every candidate score, or keep a
  small position table for the current cache.
- Keep dense adjacency as the default; raw read/write optimizations are below
  the noise floor for this path.

**Related.** [index-cache-locality](../index-cache-locality.md) · prev:
[index-cache-locality-cpucost.07](index-cache-locality-cpucost.07.md) · next:
[index-cache-locality-cpucost.09](index-cache-locality-cpucost.09.md) · [index-cache-locality-cpucost.03](index-cache-locality-cpucost.03.md)
for the accepted dense-adjacency CPU cut.
