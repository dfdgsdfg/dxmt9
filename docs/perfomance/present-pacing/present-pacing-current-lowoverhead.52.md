---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: current-baseline
order: 52
title: Current Low-Overhead Scout With State-Elision Active
date: 2026-06-16
type: experiment
status: accepted-current-baseline
source: experiments/output/app-d3d9-3dmark05-current-lowoverhead-continuation-r1-20260616/result.json, experiments/output/app-d3d9-3dmark05-current-lowoverhead-continuation-r1-20260616/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-current-lowoverhead-continuation-r1-20260616/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-current-lowoverhead-continuation-r1-20260616/actual.png
related: docs/perfomance/present-pacing/present-pacing-current-lowoverhead.49.md, docs/perfomance/present-pacing/present-pacing-overlap-locality-gates.51.md, docs/perfomance/state-churn-encode.md, docs/perfomance/snapshot-cache.md, docs/perfomance/present-pacing.md
---

# Present Pacing 52 - Current Low-Overhead Scout With State-Elision Active

## Question

After capture-layer repair and the latest state/copy cleanup, does the current
no-gputrace path still point at the same average-FPS owner?

## Verdict

Yes. The run is a normal visual GT1 scout and still classifies as
`under-pipelined-no-enqueue`. GPU command-buffer time is only
`3.020ms/present`, while completion wait is `29.451ms/present` and `99.608%`
of that wait has no later enqueue overlap.

The earlier F1/F2 copy-elision class is no longer the large open waste in this
shape. State materialization is already partly elided (`417,901 / 891,872`
submissions, `46.86%`), same-generation/lane compatibility covers nearly all
compatible pairs (`415,586 / 421,822`, `98.52%`), and materialized states later
discarded by append are only `3,921` records (`0.44%` of submissions).

## Run

```sh
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix current-lowoverhead-continuation-r1-20260616 \
  --no-gputrace \
  --no-encoder-breakdown \
  --frame-sampling \
  --timeout 120 \
  --capture-delay-sec 45 \
  --wait-unlocked-sec 1 \
  --wait-unlocked-interval-sec 1 \
  --min-free-mb 256
```

The wrapper completed with `status=pass`, `timed_out=false`, and
`returncode=0`. The captured `actual.png` is a normal GT1 scene with HUD,
geometry, engine glow, and muzzle/bloom visible; it is not a black/yellow-frame
regression scout.

## Current Shape

| Metric | Value |
|---|---:|
| `present_encoded` | `1,823` |
| `sampled_avg_fps` | `16.666` |
| `gpu_command_buffer_time_ms_per_present` | `3.020` |
| `completion_wait_ms_per_present` | `29.451` |
| `completion_wait_with_enqueue_ms_per_present` | `0.115` |
| `completion_wait_without_enqueue_ms_per_present` | `29.336` |
| `completion_wait_overlap_share` | `0.392%` |
| `completion_wait_no_enqueue_share` | `99.608%` |
| `commit_chunk_replay_cpu_ms_per_present` | `8.395` |
| `commit_chunk_queue_draw_submission_cpu_ms_per_present` | `4.209` |
| `d3d9_snapshot_draw_submission_cpu_ms_per_present` | `3.496` |
| `d3d9_snapshot_cache_lookup_cpu_ms_per_present` | `2.925` |
| `encode_chunk_cpu_ms_per_present` | `11.110` |
| `encode_draw_cpu_ms_per_present` | `8.573` |

The exposed no-enqueue stage shape is still serial P2/P3 work feeding a late
enqueue:

| Stage | total ms/present | p50 ms | p95 ms |
|---|---:|---:|---:|
| wait -> commit chunk entry | `3.674` | `0.758` | `2.341` |
| commit entry -> publish | `13.672` | `6.576` | `26.735` |
| publish -> encode dequeue | `0.244` | `0.288` | `0.468` |
| encode dequeue -> command buffer commit | `12.498` | `12.469` | `24.088` |
| wait -> next enqueue | `30.482` | `13.480` | `48.298` |

Before the first publish after a no-enqueue wait, replay is active and mostly
draw/const work:

| Metric | Value |
|---|---:|
| scanned chunks | `22,253` |
| chunks with draw | `20,528` |
| chunks with present | `1,725` |
| state/const-only chunks | `0` |
| draw records | `552,699` |
| const records | `515,625` |

## State-Elision Check

| Metric | Value |
|---|---:|
| `d3d9_snapshot_state_materialized` | `473,971` |
| `d3d9_snapshot_state_elided` | `417,901` |
| `d3d9_snapshot_state_materialized_bytes` | `4,849,671,272` |
| `d3d9_snapshot_state_elided_bytes` | `4,275,963,032` |
| `d3d9_snapshot_state_copy_cpu_ms_per_present` | `0.079` |
| `submit_draw_run_batch_compat_scan_cpu_ms_per_present` | `0.031` |
| `submit_draw_run_batch_discarded_state_records` | `3,921` |
| `submit_draw_run_batch_discarded_state_bytes` | `40,119,672` |

This keeps the old "copy and discard N-1 states" critique in the accepted/fixed
bucket for the current tree. The residual state-copy and compatibility-scan
costs are too small to be the next direct FPS owner.

## Encode Residual

The top local encode buckets are still worth CPU cleanup, but the accepted
direct-cbuf and uniform-storage work already show that local encode wins do not
automatically move average FPS unless P4 overlap/no-enqueue also moves.

| Rank | Counter | ms/present |
|---:|---|---:|
| 1 | `encode_draw_argbuf_setup_cpu_ms` | `1.827` |
| 2 | `encode_draw_stream_bind_cpu_ms` | `1.239` |
| 3 | `encode_slot_pso_prefetch_cpu_ms` | `1.169` |
| 4 | `encode_draw_binding_packet_cpu_ms` | `1.032` |
| 5 | `encode_draw_argbuf_cbuf_update_cpu_ms` | `0.958` |
| 6 | `encode_draw_argbuf_open_cpu_ms` | `0.730` |

## Decision

Use this as the current low-overhead baseline after capture-layer recovery and
state/copy cleanup. The next average-FPS candidate should target one of two
measurable outcomes:

- reduce `commit entry -> publish` or `encode dequeue -> command buffer commit`
  enough that `wait -> next enqueue` shrinks;
- or recover `completion_wait_with_enqueue_ms` without increasing command
  buffers, render passes, or tile-preservation bytes.

Do not re-open broad F1/F2 state-copy work as the next priority unless a future
run shows `d3d9_snapshot_state_copy_cpu_ms`, compatibility scan, or discarded
materialized states growing again.
