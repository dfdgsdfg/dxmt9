---
domain: index-cache-locality
workload: 3DMark05 GT1
subcategory: cpucost
order: 10
title: Candidate Select Volume Counters
date: 2026-06-05
type: experiment-run
status: accepted
source: experiments/output/app-d3d9-3dmark05-defaultgate-noenc-opaque-depth-selectvolume-r1/3dmark05-perf-summary.md; experiments/output/app-d3d9-3dmark05-defaultgate-noenc-opaque-depth-selectvolume-r1/result.json; traces/app-d3d9-3dmark05-defaultgate-noenc-opaque-depth-selectvolume-r1/analysis/cachepos-r1-vs-selectvolume-r1-run-counters.md; traces/app-d3d9-3dmark05-defaultgate-noenc-opaque-depth-selectvolume-r1/analysis/defaultgate-noenc-opaque-depth-r1-vs-selectvolume-r1-run-counters.md
---

# Candidate Select Volume Counters

**Question / hypothesis.** After [[index-cache-locality-cpucost.09]] reduced
cache-position lookup cost but left total CPU flat, is the remaining select
bucket explained by candidate rescoring volume?

**Instrumentation.** Added run-level volume counters around
`chooseBestCandidate()`:

| Counter | Meaning |
|---|---|
| `encode_draw_index_cache_candidate_select_calls` | number of best-candidate selections |
| `encode_draw_index_cache_candidate_select_slots` | total candidate vector slots scanned |
| `encode_draw_index_cache_candidate_select_scored` | non-emitted candidates actually scored |
| `encode_draw_index_cache_candidate_select_skipped` | emitted/invalid candidate slots skipped |
| `encode_draw_index_cache_candidate_select_candidates_max` | largest candidate vector size observed |

The counters are accumulated locally per candidate build and flushed once to
avoid changing the per-slot hot path with atomic writes.

**Run.** Used the default-policy no-encoder production opt-in smoke:

```sh
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix defaultgate-noenc-opaque-depth-selectvolume-r1 \
  --frame 50 --no-gputrace --no-encoder-breakdown --timeout 240 --top 5 \
  --optimize-opaque-depth-index-cache \
  --optimize-opaque-depth-index-cache-min-gain-pct 10
```

The harness passed with timeout finalization:
`status=pass`, `timed_out=true`, `returncode=143`,
`process_elapsed_sec=311.488`, and `present_encoded=1440`.

**Result.** Candidate choice stayed stable, and the select bucket volume is now
quantified:

| Metric | Value |
|---|---:|
| `indexed_cache_opt_candidate_draws` | `125` |
| `indexed_cache_opt_candidate_original_miss32` | `530,289` |
| `indexed_cache_opt_candidate_miss32` | `418,033` |
| `reordered_index_cache_created` | `67` |
| `encode_draw_index_cache_candidate_select_cpu_ms` | `91.635ms` |
| `encode_draw_index_cache_candidate_select_calls` | `302,538` |
| `encode_draw_index_cache_candidate_select_slots` | `2,061,493` |
| `encode_draw_index_cache_candidate_select_scored` | `2,061,493` |
| `encode_draw_index_cache_candidate_select_skipped` | `0` |
| `encode_draw_index_cache_candidate_select_candidates_max` | `157` |
| average slots per select call | `6.814` |
| select calls per candidate draw | `2,420.304` |
| scored candidates per candidate draw | `16,491.944` |
| select ns per scored candidate | `44.451ns` |

Against [[index-cache-locality-cpucost.09]], the new counter-only build stayed
close: `select_cpu_ms=92.121 -> 91.635`, `candidate_build_cpu_ms=124.861 ->
124.083`, candidate miss32 unchanged, and `reordered_index_cache_created=67`.

**Verdict.** Accepted as attribution. The per-candidate score is already cheap
after the cache-position table. The remaining CPU owner is the product of
`302k` selection calls and `2.06M` scored candidates, not skipped stale slots
(`skipped=0`) or raw index read/write. A meaningful CPU cut needs to reduce
selection calls/scored candidates or reuse candidate scores across cache
updates.

**Next CPU experiments.**

- Add an explicit candidate frontier cap experiment and measure miss32 loss vs
  select volume drop.
- Try lazy-score invalidation keyed by touched vertices instead of full vector
  rescans.
- Fix/test local cache duplicate behavior separately, because this measurement
  preserved current first-match semantics.

**Related.** [[index-cache-locality]] · prev:
[[index-cache-locality-cpucost.09]] · [[index-cache-locality-cpucost.08]].
