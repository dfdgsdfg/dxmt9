---
domain: index-cache-locality
workload: 3DMark05 GT1
subcategory: cpucost
order: 07
title: Pool Lookup Single-Scan Rejection
date: 2026-06-05
type: experiment-run
status: rejected
source: traces/app-d3d9-3dmark05-defaultgate-noenc-opaque-depth-fastlookup-r1/analysis/defaultgate-noenc-opaque-depth-r1-vs-fastlookup-r1-run-counters.md; traces/app-d3d9-3dmark05-defaultgate-noenc-opaque-depth-fastlookup-r1/analysis/defaultgate-noenc-baseline-r1-vs-opaque-depth-fastlookup-r1-run-counters.md; experiments/output/app-d3d9-3dmark05-defaultgate-noenc-opaque-depth-fastlookup-r1/result.json
---

# Pool Lookup Single-Scan Rejection

**Question / hypothesis.** Is `reorderedIndexCache` lookup CPU mostly the
`findReorderedIndexBuffer()` double scan (stale-prune pass followed by key
lookup), and can a single-scan hot path reduce the no-encoder default-policy
side-effect?

**Method.** Temporarily changed `Pool::findReorderedIndexBuffer()` so the hot
path scanned entries once, recorded whether pruning was needed, and only ran the
erase/remove stale cleanup when a stale entry was seen and no hit was returned.
Then rebuilt and ran the production opt-in smoke without encoder breakdown:

```sh
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix defaultgate-noenc-opaque-depth-fastlookup-r1 \
  --frame 50 --no-gputrace --no-encoder-breakdown --timeout 180 --top 5 \
  --optimize-opaque-depth-index-cache \
  --optimize-opaque-depth-index-cache-min-gain-pct 10
```

The run passed with `present_encoded=1440`, `returncode=143`, and timeout
finalization. It emitted no encoder/probe rows, as intended for this smoke.

**Result.** Candidate selection and cache behavior stayed stable, but lookup CPU
did not improve:

| Metric | Old no-encoder opt-in | Single-scan lookup | Delta |
|---|---:|---:|---:|
| `present_encoded` | `1,440` | `1,440` | `0` |
| `indexed_cache_opt_candidate_draws` | `125` | `125` | `0` |
| `indexed_cache_opt_candidate_original_miss32` | `530,289` | `530,289` | `0` |
| `indexed_cache_opt_candidate_miss32` | `418,033` | `418,033` | `0` |
| `reordered_index_cache_lookups` | `585,116` | `585,131` | `+15` |
| `reordered_index_cache_hits` | `243,389` | `243,382` | `-7` |
| `reordered_index_cache_rejected_hits` | `341,584` | `341,606` | `+22` |
| `reordered_index_cache_created` | `67` | `67` | `0` |
| `encode_draw_index_cache_lookup_cpu_ms` | `99.368` | `102.799` | `+3.431ms` |
| `encode_draw_index_cache_candidate_cpu_ms` | `160.505` | `161.563` | `+1.058ms` |
| `encode_draw_index_setup_cpu_ms` | `761.186` | `749.002` | `-12.184ms` |
| `encode_draw_cpu_ms` | `16,405.450` | `16,417.318` | `+11.868ms` |
| `gpu_command_buffer_time_ms` | `4,234.749` | `4,317.089` | `+82.340ms` |

The small `index_setup` movement is not attributable to lookup improvement:
the explicit lookup bucket regressed slightly and total encode-draw CPU also
moved worse. The code change was reverted after this measurement.

**Verdict.** Rejected. The lookup CPU bucket is not explained by the simple
stale-prune + key-scan double pass. The remaining default-policy CPU owner is
still candidate build/measure plus repeated lookup/counter/accounting overhead,
not this specific vector scan shape.

**Related.** [[index-cache-locality]] · prev: [[index-cache-locality-cpucost.06]]
· [[index-cache-locality-cpucost.02]] (earlier lookup fast-path rejection) ·
[[index-cache-locality-opaque.07]] (accepted Xcode proof).
