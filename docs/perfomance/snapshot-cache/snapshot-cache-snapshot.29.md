---
domain: snapshot-cache
workload: 3DMark05 GT1
subcategory: snapshot
order: 29
title: Batch Miss Semantic Reuse Probe Rejects Small Recent-Key Cache
date: 2026-06-18
type: opportunity-probe
status: rejected-next-lever
source: include/dxmt9/core_snapshots.hpp; src/d3d9/core_draw.cpp; src/dxmt9/dxmt9_perf_counters.cpp; experiments/output/app-d3d9-3dmark05-batch-miss-semantic-reuse-probe-r1-20260617/3dmark05-perf-summary.md
related: docs/perfomance/snapshot-cache/snapshot-cache-snapshot.28.md, docs/perfomance/snapshot-cache.md, docs/perfomance/present-pacing/present-pacing-current-lowoverhead.71.md
---

# Snapshot Cache 29 - Batch Miss Semantic Reuse Probe Rejects Small Recent-Key Cache

**Question / hypothesis.** After [snapshot-cache-snapshot.28](snapshot-cache-snapshot.28.md), batch misses
still own about `~2ms/present`. Are those misses recurring in a short window
such that a small multi-entry cache or interner could recover them?

**Verdict.** No. A recent-key cache over the previous eight binding-agnostic
miss keys would hit only `1.95%` of sampled batch misses. That is too small to
justify hot-path cache/interner complexity as the next implementation target.

## Probe

The opt-in probe is guarded by
`DXMT9_PERF_BATCH_MISS_SEMANTIC_REUSE_PROBE=1`. On each
`cachedBaseDrawStateForSubmissionBatch()` miss, it hashes the cleared
`FlatDrawStateKey` and exact-compares it against the previous eight miss keys.
It records hit/miss and hit distance. It does not change rendering behavior, but
it adds key hash/equality work and should only be used for no-gputrace scouts.

```sh
DXMT9_PERF_BATCH_MISS_SEMANTIC_REUSE_PROBE=1 \
DXMT_3DMARK05_FOCUS_KEEPALIVE_SEC=140 \
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix batch-miss-semantic-reuse-probe-r1-20260617 \
  --no-gputrace \
  --no-encoder-breakdown \
  --frame-sampling \
  --timeout 120 \
  --wait-unlocked-sec 1 \
  --wait-unlocked-interval-sec 1 \
  --min-free-mb 256
```

## Result

| Counter | Value |
|---|---:|
| `d3d9_draw_state_cache_batch_hits` | `462,616` |
| `d3d9_draw_state_cache_batch_misses` | `419,703` |
| `d3d9_snapshot_cache_batch_miss_semantic_reuse_probe_samples` | `419,703` |
| `d3d9_snapshot_cache_batch_miss_semantic_reuse_probe_hits` | `8,172` |
| `d3d9_snapshot_cache_batch_miss_semantic_reuse_probe_misses` | `411,531` |
| Hit rate | `1.95%` |

Hit distance distribution:

| Distance bucket | Hits |
|---|---:|
| 1 | `1` |
| 2 | `3,697` |
| 3-4 | `3,671` |
| 5-8 | `803` |

Run health and current owner shape:

| Metric | Value |
|---|---:|
| `draw_skipped_no_pipeline` | `0` |
| `gpu_command_buffer_errors` | `0` |
| `sampled_avg_fps` | `16.591` |
| `completion_wait_ms_per_present` | `27.336` |
| `commit_chunk_replay_cpu_ms_per_present` | `8.389` |
| `commit_chunk_queue_draw_submission_cpu_ms_per_present` | `4.085` |
| `d3d9_snapshot_cache_batch_miss_cpu_ms_per_present` | `2.052` |
| `encode_chunk_cpu_ms_per_present` | `11.221` |

## Decision

Reject a short recent-key cache/interner as the next snapshot-cache lever. The
residual batch-miss cost should instead be treated as real state churn and
serialization pressure unless a broader semantic interning design proves a much
larger recurrence window without adding comparable hot-path lookup cost.

Average-FPS work should remain focused on the P2/P3/P4 owner split in
[present-pacing-current-lowoverhead.71](../present-pacing/present-pacing-current-lowoverhead.71.md): replay/snapshot/encode reduction
paired with overlap/locality gates.
