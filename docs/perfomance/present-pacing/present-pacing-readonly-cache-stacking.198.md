---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: producer-attribution
order: 198
title: Readonly Cache Stacks With The Promoted Pair For +26% Cumulative
date: 2026-07-08
type: no-gputrace
status: accepted-stacking-confirm
source: experiments/output/app-d3d9-3dmark05-h197-stacking-r1-20260708/result.json; experiments/output/app-d3d9-3dmark05-h196-readonly-cache-r1-20260708/result.json; docs/perfomance/present-pacing/present-pacing-readonly-managed-buffer-cache.197.md
related: docs/perfomance/present-pacing/index.md; docs/perfomance/present-pacing/present-pacing-producer-sampling-attribution.196.md
---

# Present-Pacing H211 - Readonly cache + promoted pair stacking confirm

## Question

H197 confirmed the mechanism but refused an FPS claim. Review of its
`result.json` found the run was config-confounded in the useful direction:
the probe wrapper pins the promoted pair off without explicit caller env, so
H197 measured the cache **without** offload/index-cache — and its `2,002`
presents against the no-offload baseline of `1,800` is an offload-class
`+11.2%` from the cache alone. Does the cache stack with the promoted pair?

## Run

Same recipe with the pair env exported, no frame sampling:

```sh
DXMT9_OFFLOAD_COMMIT_REPLAY=1 DXMT9_OPTIMIZE_OPAQUE_DEPTH_INDEX_CACHE=1 \
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix h197-stacking-r1-20260708 --frame 60 --no-gputrace \
  --timeout 120 --keep-frontmost --no-encoder-breakdown
```

## Verdict

Accepted stacking confirm: **`2,271` presents** (`status=pass`, zero GPU
errors) vs the pair-on pre-cache population `1,980-2,040` (median `~2,010`)
= **`+13.0%`**, and vs the no-offload/no-cache baseline `1,800` =
**`+26.2%` cumulative**. Well outside the `±5%` noise band. All three
mechanisms verified simultaneously live in one run:

| Mechanism | Counter | /present |
|---|---|---:|
| readonly cache | `d3d9_buffer_lock_calls` | `54.4` (was `1,478.7` pre-cache) |
| offload | `offload_commit_app_cpu_ms` / `offload_replay_cpu_ms` | `1.108` / `8.961` |
| index-cache | `reordered_index_cache_hits` | `169.5` |

Worker idle drops `44.3 -> 39.0ms/present` (the producer got faster; the
pipeline stays producer-bound). Map mutex wait stays collapsed
(`0.207ms/present`). Time-based `actual.png` metrics (`luma 41.9`,
`variance 3,248`) are in the same class as the promoted-pair smoke
(`45.2` / `3,924`); a same-frame or `v0.0.3`-anchor visual check remains
the promotion gate, together with an H194-style full-demo long confirm.

## Remaining before promoting the cache

- Visual gate (anchor comparison or same-frame proof).
- Long-window confirm pair (H194 pattern, `--timeout 150`).
- Recommended hardening from the H197 review: an env kill-switch
  (readonly-lock writes by non-conforming apps now diverge silently) and
  PE-side cache hit/miss/invalidate counters under `DXMT9_PE_RECORDER_STATS`.
