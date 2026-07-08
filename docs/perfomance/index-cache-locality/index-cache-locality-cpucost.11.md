---
domain: index-cache-locality
workload: 3DMark05 GT1
subcategory: cpucost
order: 11
title: Candidate Frontier Cap Rejection
date: 2026-06-05
type: experiment-run
status: rejected
source: experiments/output/app-d3d9-3dmark05-defaultgate-noenc-opaque-depth-frontier64-r1/3dmark05-perf-summary.md; experiments/output/app-d3d9-3dmark05-defaultgate-noenc-opaque-depth-frontier32-r1/3dmark05-perf-summary.md; traces/app-d3d9-3dmark05-defaultgate-noenc-opaque-depth-frontier64-r1/analysis/selectvolume-r1-vs-frontier64-r1-run-counters.md; traces/app-d3d9-3dmark05-defaultgate-noenc-opaque-depth-frontier32-r1/analysis/selectvolume-r1-vs-frontier32-r1-run-counters.md
---

# Candidate Frontier Cap Rejection

**Question / hypothesis.** [index-cache-locality-cpucost.10](index-cache-locality-cpucost.10.md) proved that the
candidate select bucket is dominated by rescoring volume. Can a simple hard cap
on the active candidate frontier cut CPU without damaging LRU32 candidate
quality?

**Implementation.** Added diagnostic-only
`DXMT9_INDEX_CACHE_CANDIDATE_FRONTIER_CAP` and wrapper option
`--index-cache-candidate-frontier-cap`. `0` keeps the uncapped builder. Positive
values stop enqueueing new neighbor candidates once the active vector reaches the
cap and count those drops through
`encode_draw_index_cache_candidate_frontier_dropped`.

**Runs.** Both probes used the same default-policy no-encoder production opt-in
smoke as [index-cache-locality-cpucost.10](index-cache-locality-cpucost.10.md):

```sh
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix defaultgate-noenc-opaque-depth-frontier64-r1 \
  --frame 50 --no-gputrace --no-encoder-breakdown --timeout 180 --top 5 \
  --optimize-opaque-depth-index-cache \
  --optimize-opaque-depth-index-cache-min-gain-pct 10 \
  --index-cache-candidate-frontier-cap 64

bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix defaultgate-noenc-opaque-depth-frontier32-r1 \
  --frame 50 --no-gputrace --no-encoder-breakdown --timeout 180 --top 5 \
  --optimize-opaque-depth-index-cache \
  --optimize-opaque-depth-index-cache-min-gain-pct 10 \
  --index-cache-candidate-frontier-cap 32
```

Both runs passed under timeout finalization with `present_encoded=1440`.

**Result.** The cap changes volume, but does not reduce the measured select
bucket:

| Metric | uncapped selectvolume-r1 | cap 64 | cap 32 |
|---|---:|---:|---:|
| `encode_draw_index_cache_candidate_select_cpu_ms` | `91.635` | `98.752` | `96.232` |
| `encode_draw_index_cache_candidate_build_cpu_ms` | `124.083` | `131.087` | `128.451` |
| `encode_draw_index_cache_candidate_cpu_ms` | `152.117` | `159.203` | `156.390` |
| `encode_draw_index_cache_candidate_select_calls` | `302,538` | `302,538` | `302,538` |
| `encode_draw_index_cache_candidate_select_slots` | `2,061,493` | `1,983,427` | `1,642,444` |
| `encode_draw_index_cache_candidate_select_candidates_max` | `157` | `64` | `32` |
| `encode_draw_index_cache_candidate_frontier_dropped` | `0` | `8,937` | `37,305` |
| `indexed_cache_opt_candidate_original_miss32` | `530,289` | `530,289` | `530,289` |
| `indexed_cache_opt_candidate_miss32` | `418,033` | `418,009` | `418,231` |
| `reordered_index_cache_created` | `67` | `67` | `67` |
| `gpu_command_buffer_time_ms` | `4,253.877` | `4,272.639` | `4,197.270` |

Cap 64 lowers scored slots only `-3.79%` and cap 32 lowers them `-20.33%`, but
both increase `candidate_select_cpu_ms` versus the uncapped baseline. The cap
itself adds a branch in the enqueue path and, more importantly, it does not
reduce `select_calls` at all: every emitted triangle still performs a full best
candidate selection. Cap 32 also slightly weakens candidate quality
(`candidate_miss32 +198`, `+0.05%`), while cap 64 is effectively quality-neutral.
No-gputrace GPU proxies are mixed (`+0.44%` for cap 64, `-1.33%` for cap 32) and
are not strong enough to promote the cap.

**Verdict.** Rejected as a CPU optimization. A fixed vector-width cap is useful
as a diagnostic because it proves frontier width can be bounded without changing
candidate draw counts or reordered-cache creation. It is not the right
algorithmic fix: the remaining owner is the number of selection iterations and
full rescans, not just the worst-case candidate vector width.

**Next CPU direction.**

- Replace full vector rescans with lazy score invalidation or a priority/frontier
  structure whose update cost is tied to touched vertices.
- Keep the dropped-frontier counter as an experiment-only guard when testing
  lower-width heuristics.
- Do not ship `DXMT9_INDEX_CACHE_CANDIDATE_FRONTIER_CAP` as a production default;
  it is a diagnostic knob.
- Follow-up: [index-cache-locality-cpucost.12](index-cache-locality-cpucost.12.md) tested a generic heap-backed
  lazy frontier and rejected it, so any remaining frontier work must be cheaper
  and more domain-specific than a standard priority queue.

**Related.** [index-cache-locality](../index-cache-locality.md) · prev:
[index-cache-locality-cpucost.10](index-cache-locality-cpucost.10.md) · [index-cache-locality-cpucost.09](index-cache-locality-cpucost.09.md).
