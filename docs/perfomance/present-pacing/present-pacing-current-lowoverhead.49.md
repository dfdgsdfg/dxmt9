---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: current-baseline
order: 49
title: Current Low-Overhead Scout After Capture-Layer Repair
date: 2026-06-16
type: experiment
status: accepted-current-baseline
source: experiments/output/app-d3d9-3dmark05-current-lowoverhead-post-capture-r2/result.json, experiments/output/app-d3d9-3dmark05-current-lowoverhead-post-capture-r2/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-current-lowoverhead-post-capture-r2/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-current-lowoverhead-post-capture-r2/actual.png
related: docs/perfomance/present-pacing/present-pacing-current-p2p3.46.md, docs/perfomance/present-pacing/present-pacing-noenqueue-beforepublish.47.md, docs/perfomance/present-pacing/present-pacing-drawchunk-limit.48.md, docs/perfomance/present-pacing.md
---

# Present Pacing 49 - Current Low-Overhead Scout After Capture-Layer Repair

**Question.** After the file `.gputrace` capture route was repaired and Xcode
counter export worked again, does the normal no-gputrace path still show the
same average-FPS owner?

**Verdict.** Yes. The current run is visually normal and still classifies as
`under-pipelined-no-enqueue`. GPU command-buffer work is about
`3.231ms/present`, while completion wait is `27.916ms/present` and `99.288%`
of that wait is not overlapped by later command-buffer enqueue work.

## Run

```sh
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix current-lowoverhead-post-capture-r2 \
  --frame 60 \
  --no-gputrace \
  --no-encoder-breakdown \
  --frame-sampling \
  --timeout 120 \
  --wait-unlocked-sec 5
```

The wrapper completed with `status=pass`, `timed_out=false`, and
`returncode=0`. This is useful because it also rechecks that the current
timeout/supervision path does not require a manual kill for this scout shape.
`actual.png` shows a normal GT1 frame with HUD, glow, and geometry present.
Health counters are clean:

| Counter | Value |
|---|---:|
| `draw_skipped_no_pipeline` | `0` |
| `gpu_command_buffer_errors` | `0` |
| `present_boundary_wait_ms` | `0.000` |
| `completion_pending_depth_max` | `0` |

## Current Shape

| Metric | Value |
|---|---:|
| `present_encoded` | `1,812` |
| `sampled_avg_fps` | `16.557` |
| `gpu_command_buffer_time_ms_per_present` | `3.231` |
| `completion_wait_ms_per_present` | `27.916` |
| `completion_wait_with_enqueue_ms_per_present` | `0.199` |
| `completion_wait_without_enqueue_ms_per_present` | `27.717` |
| `completion_wait_overlap_share` | `0.712%` |
| `completion_wait_no_enqueue_share` | `99.288%` |
| `commit_chunk_replay_cpu_ms_per_present` | `8.519` |
| `commit_chunk_queue_draw_submission_cpu_ms_per_present` | `4.241` |
| `d3d9_snapshot_draw_submission_cpu_ms_per_present` | `3.508` |
| `d3d9_snapshot_cache_lookup_cpu_ms_per_present` | `2.919` |
| `encode_chunk_cpu_ms_per_present` | `11.348` |
| `encode_draw_cpu_ms_per_present` | `8.759` |

The same-cycle exposed no-enqueue stages remain large:

| Stage | total ms/present | p50 ms | p95 ms |
|---|---:|---:|---:|
| wait -> commit chunk entry | `3.869` | `0.821` | `2.672` |
| commit entry -> publish | `15.206` | `13.971` | `27.402` |
| publish -> encode dequeue | `0.243` | `0.336` | `0.473` |
| encode dequeue -> command buffer commit | `12.477` | `17.397` | `24.244` |
| wait -> next enqueue | `32.405` | `12.955` | `50.181` |

Before the first publish after a no-enqueue wait, dxmt9 is not idle. It has
already entered/replayed many chunks:

| Event | total | per publish sample | max | p50 | p95 |
|---|---:|---:|---:|---:|---:|
| entries | `23,761` | `14.340` | `52` | `11` | `20` |
| replay starts | `23,777` | `14.349` | `52` | `11` | `20` |
| replay ends | `22,444` | `13.545` | `51` | `11` | `19` |

Those chunks are draw/const heavy, matching [present-pacing-noenqueue-beforepublish.47](present-pacing-noenqueue-beforepublish.47.md):

| Chunk metric | total | per publish sample | per scanned chunk |
|---|---:|---:|---:|
| scanned chunks | `23,765` | `14.342` | `1.000` |
| chunks with draw | `22,109` | `13.343` | `0.930` |
| chunks with present | `1,656` | `0.999` | `0.070` |
| state/const-only chunks | `0` | `0.000` | `0.000` |

## Decision

Use this as a fresh current-head low-overhead baseline after capture-layer
repair. It does not justify another Xcode `.gputrace` by itself, because the
average-FPS owner is still serialized P2/P3 work plus missing P4 overlap, not
hot-frame GPU counter visibility.

The next average-FPS candidate must either:

- reduce `commit entry -> publish`, snapshot/cache, or backend encode and pass
  the P2/P3 compare gates;
- or recover overlap by increasing `completion_wait_with_enqueue_ms` or
  decreasing `completion_wait_without_enqueue_ms`;
- and keep normal visual output with skipped/error counters clean.

Simple early-publish by draw-count remains rejected by [present-pacing-drawchunk-limit.48](present-pacing-drawchunk-limit.48.md)
because it creates overlap by fragmenting command buffers/render passes and
raises GPU/tiler cost.
