---
domain: index-cache-locality
workload: 3DMark05 GT1
subcategory: cpucost
order: 09
title: Candidate Cache-Position Table
date: 2026-06-05
type: experiment-run
status: accepted
outdated: evidence-missing
source: experiments/output/app-d3d9-3dmark05-defaultgate-noenc-opaque-depth-cachepos-r1/3dmark05-perf-summary.md; experiments/output/app-d3d9-3dmark05-defaultgate-noenc-opaque-depth-cachepos-r1/result.json; traces/app-d3d9-3dmark05-defaultgate-noenc-opaque-depth-cachepos-r1/analysis/buildsplit-r1-vs-cachepos-r1-run-counters.md; traces/app-d3d9-3dmark05-defaultgate-noenc-opaque-depth-cachepos-r1/analysis/defaultgate-noenc-opaque-depth-r1-vs-cachepos-r1-run-counters.md
---

# Candidate Cache-Position Table

> **Outdated — every artifact this leaf cites in `source:` is gone from disk.** The numbers below cannot be re-derived or re-checked. Kept as history; do not cite it as current evidence.

**Question / hypothesis.** Since [index-cache-locality-cpucost.08](index-cache-locality-cpucost.08.md) showed
`encode_draw_index_cache_candidate_select_cpu_ms` owns most of candidate build,
does replacing repeated linear `cachePosition()` scans with a small position
table reduce the select bucket without changing candidate choice?

**Implementation.** Inside
`buildVertexCacheOptimizedTriangleOrderIndexBytes()`:

- keep dense `index -> cache slot` storage for dense referenced ranges;
- keep a tiny sparse map for sparse ranges;
- clear/rebuild positions after each cache mutation so candidate scoring can
  query positions directly;
- preserve the old first-match behavior when duplicate cache entries exist, so
  candidate order remains comparable.

**Run.** Used the same no-gputrace production opt-in smoke:

```sh
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix defaultgate-noenc-opaque-depth-cachepos-r1 \
  --frame 50 --no-gputrace --no-encoder-breakdown --timeout 240 --top 5 \
  --optimize-opaque-depth-index-cache \
  --optimize-opaque-depth-index-cache-min-gain-pct 10
```

The harness passed with timeout finalization:
`status=pass`, `timed_out=true`, `returncode=143`,
`process_elapsed_sec=311.771`, and `present_encoded=1440`.

**Result.** Candidate selection stayed stable and the target select bucket
dropped, but the overall draw encode bucket did not materially improve:

| Metric | Build split r1 | Cache-position r1 | Delta |
|---|---:|---:|---:|
| `present_encoded` | `1,440` | `1,440` | `0` |
| `indexed_cache_opt_candidate_draws` | `125` | `125` | `0` |
| `indexed_cache_opt_candidate_original_miss32` | `530,289` | `530,289` | `0` |
| `indexed_cache_opt_candidate_miss32` | `418,033` | `418,033` | `0` |
| `reordered_index_cache_created` | `67` | `67` | `0` |
| `encode_draw_index_cache_candidate_cpu_ms` | `160.056` | `153.934` | `-6.122ms` |
| `encode_draw_index_cache_candidate_build_cpu_ms` | `131.553` | `124.861` | `-6.692ms` |
| `encode_draw_index_cache_candidate_select_cpu_ms` | `99.187` | `92.121` | `-7.066ms` |
| `encode_draw_index_cache_candidate_adjacency_cpu_ms` | `25.128` | `25.501` | `+0.373ms` |
| `encode_draw_index_cache_lookup_cpu_ms` | `106.875` | `100.137` | `-6.738ms` |
| `encode_draw_index_setup_cpu_ms` | `754.472` | `743.352` | `-11.120ms` |
| `encode_draw_cpu_ms` | `16,523.031` | `16,532.676` | `+9.645ms` |

Against the older no-encoder opt-in r1, GPU command-buffer time was effectively
flat (`4234.749ms -> 4233.985ms`, `-0.764ms`) and `encode_draw_cpu_ms` was
still higher (`16405.450ms -> 16532.676ms`). The cache-position table is a real
local bucket reduction, not a full CPU-side fix.

**Verdict.** Accepted as a micro-optimization and attribution confirmation. It
proves the repeated cache-position scan was part of the select cost, but the
remaining `~92ms` select bucket is still dominated by rescoring too many
candidates. The next meaningful CPU reduction needs to reduce candidate scoring
work itself, not only make each score cheaper.

**Next CPU experiments.**

- Fix or explicitly test the local LRU cache miss-update shape; the current
  preserved first-match behavior may be leaving candidate quality on the table.
- Replace full candidate-vector rescans with a bounded active frontier.
- Add candidate-set size / score-scan counters before larger algorithm changes.

**Related.** [index-cache-locality](index.md) · prev:
[index-cache-locality-cpucost.08](index-cache-locality-cpucost.08.md) · next:
[index-cache-locality-cpucost.10](index-cache-locality-cpucost.10.md) · [index-cache-locality-cpucost.07](index-cache-locality-cpucost.07.md).
