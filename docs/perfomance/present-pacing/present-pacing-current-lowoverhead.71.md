---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: current-baseline
order: 71
title: Current Low-Overhead Baseline After Uniform ABI-Prefix Fix
date: 2026-06-18
type: experiment
status: accepted-current-baseline
source: experiments/output/app-d3d9-3dmark05-current-lowoverhead-after-uniform-prefix-r1-20260618/result.json, experiments/output/app-d3d9-3dmark05-current-lowoverhead-after-uniform-prefix-r1-20260618/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-current-lowoverhead-after-uniform-prefix-r1-20260618/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-current-lowoverhead-after-uniform-prefix-r1-20260618/actual.png
related: docs/perfomance/present-pacing/present-pacing-current-lowoverhead.49.md, docs/perfomance/snapshot-cache.md, docs/perfomance/state-churn-encode.md, docs/perfomance/present-pacing.md
---

# Present Pacing 71 - Current Low-Overhead Baseline After Uniform ABI-Prefix Fix

**Question.** After restoring compact-uniform ABI-prefix correctness, does the
normal no-gputrace path still show the same average-FPS owner?

**Verdict.** Yes. The current run is clean and still classifies as
`under-pipelined-no-enqueue`. The uniform correctness fix does not change the
wallclock owner: GPU command-buffer work remains small relative to completion
wait, and almost all completion wait is not overlapped by later command-buffer
enqueue work.

## Run

```sh
DXMT_3DMARK05_FOCUS_KEEPALIVE_SEC=140 \
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix current-lowoverhead-after-uniform-prefix-r1-20260618 \
  --no-gputrace \
  --no-encoder-breakdown \
  --frame-sampling \
  --timeout 120 \
  --wait-unlocked-sec 1 \
  --wait-unlocked-interval-sec 1 \
  --min-free-mb 256
```

The wrapper completed with `status=pass`, `timed_out=false`, and
`returncode=0`. Health counters are clean:

| Counter | Value |
|---|---:|
| `draw_skipped_no_pipeline` | `0` |
| `gpu_command_buffer_errors` | `0` |

## Current Shape

| Metric | Value |
|---|---:|
| `present_encoded` | `1,793` |
| `sampled_avg_fps` | `16.395` |
| `completion_wait_ms_per_present` | `28.287` |
| `completion_wait_with_enqueue_ms_per_present` | `0.362` |
| `completion_wait_without_enqueue_ms_per_present` | `27.925` |
| `completion_wait_overlap_share` | `1.279%` |
| `completion_wait_no_enqueue_share` | `98.721%` |
| `commit_chunk_replay_cpu_ms_per_present` | `8.195` |
| `commit_chunk_queue_draw_submission_cpu_ms_per_present` | `3.812` |
| `d3d9_snapshot_draw_submission_cpu_ms_per_present` | `3.062` |
| `d3d9_snapshot_cache_lookup_cpu_ms_per_present` | `2.470` |
| `encode_chunk_cpu_ms_per_present` | `11.403` |
| `encode_draw_cpu_ms_per_present` | `8.710` |

The same-cycle exposed no-enqueue stages are still large:

| Stage | total ms/present | p50 ms | p95 ms |
|---|---:|---:|---:|
| wait -> commit chunk entry | `4.183` | `0.763` | `2.727` |
| commit entry -> publish | `15.065` | `14.736` | `26.706` |
| publish -> encode dequeue | `0.249` | `0.346` | `0.479` |
| encode dequeue -> command buffer commit | `12.513` | `17.558` | `24.415` |
| wait -> next enqueue | `32.644` | `12.899` | `48.600` |

The replay/snapshot rank is unchanged in shape: replay is the largest aggregate
CPU owner, queue draw submission is mostly snapshot work, and snapshot lookup is
mostly batch miss:

| Metric | Value |
|---|---:|
| `commit_chunk_replay_cpu_ms_per_present` | `8.195` |
| `commit_chunk_queue_draw_submission_snapshot_cpu_ms_per_present` | `3.126` |
| `d3d9_snapshot_cache_batch_miss_cpu_ms_per_present` | `1.757` |
| `commit_chunk_replay_pending_flush_cpu_ms_per_present` | `1.720` |
| `commit_chunk_draw_batch_submit_cpu_ms_per_present` | `1.706` |
| `submit_draw_run_batch_append_cpu_ms_per_present` | `1.324` |

Encode remains the other serialized stage owner:

| Metric | Value |
|---|---:|
| `encode_draw_argbuf_setup_cpu_ms_per_present` | `1.915` |
| `encode_draw_stream_bind_cpu_ms_per_present` | `1.274` |
| `encode_slot_pso_prefetch_cpu_ms_per_present` | `1.185` |
| `encode_draw_binding_packet_cpu_ms_per_present` | `1.058` |

## Decision

Use this as the current low-overhead baseline after the uniform ABI-prefix fix.
The next average-FPS candidate must move at least one load-bearing P2/P3 stage
and also pass the P4 gates: completion wait should fall, useful overlap should
rise, or the same-cycle no-enqueue stage deltas should shrink without increasing
command buffers, render passes, tile-preservation traffic, or visual errors.

Do not spend another `.gputrace` on this CPU-only baseline. Xcode captures stay
appropriate for GPU-hot-frame or backend-storage candidates; the current
average-FPS owner is still replay/snapshot/encode serialization plus missing
overlap.
