---
domain: state-churn-encode
workload: 3DMark05 GT1
subcategory: encode-phase
order: 146
title: Current Direct-Cbuf Repeat Against State-Elision Baseline
date: 2026-06-16
type: experiment
status: accepted-local-cpu-win-rejected-fps-owner-visual-open
source: experiments/output/app-d3d9-3dmark05-current-lowoverhead-continuation-r1-20260616/result.json, experiments/output/app-d3d9-3dmark05-current-lowoverhead-continuation-r1-20260616/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-direct-cbuf-current-continuation-r1-20260616/result.json, experiments/output/app-d3d9-3dmark05-direct-cbuf-current-continuation-r1-20260616/3dmark05-perf-summary.md, traces/app-d3d9-3dmark05-direct-cbuf-current-continuation-r1-20260616/analysis/frame60-perf-counter-comparison.md, experiments/output/app-d3d9-3dmark05-direct-cbuf-current-continuation-r1-20260616/actual.png
related: docs/perfomance/state-churn-encode/state-churn-encode-encode-phase.144.md, docs/perfomance/present-pacing/present-pacing-current-lowoverhead.52.md, docs/perfomance/present-pacing/index.md
---

# Encode Phase 146 - Current Direct-Cbuf Repeat Against State-Elision Baseline

## Question

After the current state/copy cleanup and capture-layer recovery, does
`DXMT9_ARGBUF_DIRECT_CBUF=1` still remove the local argbuf table path, and does
that now move the average-FPS/P4 owner?

## Verdict

The local CPU mechanism still works, but it is not the current average-FPS
owner. Compared with [present-pacing-current-lowoverhead.52](../present-pacing/present-pacing-current-lowoverhead.52.md), direct-cbuf cuts
`encode_chunk_cpu_ms_per_present` by `-23.49%` and `encode_draw_cpu_ms_per_present`
by `-29.90%`, while all argbuf table/open/cbuf-update counters drop to zero.

It still does not recover meaningful P4 overlap. `wait -> next enqueue` is flat
to slightly worse (`30.482 -> 30.703ms/present`), and the largest exposed p50
row moves to `commit entry -> publish`. This makes replay/snapshot/publish
cadence the next average-FPS target after argbuf removal.

The visual smoke is also not strong enough for default promotion. The run is not
black and has HUD, but `actual.png` is much darker than the baseline
(`mean_luma 25.07` versus `60.52`) and shows a large white band. Treat this as
visual-open until an oracle or repeated same-frame smoke proves it is expected.

## Run

```sh
DXMT9_ARGBUF_DIRECT_CBUF=1 \
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix direct-cbuf-current-continuation-r1-20260616 \
  --no-gputrace \
  --no-encoder-breakdown \
  --frame-sampling \
  --timeout 120 \
  --capture-delay-sec 45 \
  --wait-unlocked-sec 1 \
  --wait-unlocked-interval-sec 1 \
  --min-free-mb 256 \
  --compare-baseline-output experiments/output/app-d3d9-3dmark05-current-lowoverhead-continuation-r1-20260616 \
  --require-encode-draw-cpu-decrease
```

The wrapper completed with `status=pass`, `timed_out=false`, and
`returncode=0`. The compare gate passed for encode CPU reduction.

## A/B Counters

| Metric | Baseline | Direct-cbuf | Delta |
|---|---:|---:|---:|
| `present_encoded` | `1,823` | `1,835` | `+0.66%` |
| `sampled_avg_fps` | `16.666` | `16.774` | flat/noisy |
| `gpu_command_buffer_time_ms_per_present` | `3.020` | `2.927` | `-2.47%` |
| `completion_wait_ms_per_present` | `29.451` | `28.848` | `-2.05%` |
| `completion_wait_with_enqueue_ms_per_present` | `0.115` | `0.138` | `+0.022ms` |
| `completion_wait_without_enqueue_ms_per_present` | `29.336` | `28.711` | `-2.13%` |
| `completion_wait_no_enqueue_share` | `99.608%` | `99.522%` | flat |
| `commit_chunk_replay_cpu_ms_per_present` | `8.395` | `8.264` | `-1.56%` |
| `d3d9_snapshot_cache_lookup_cpu_ms_per_present` | `2.925` | `2.836` | `-3.03%` |
| `encode_chunk_cpu_ms_per_present` | `11.110` | `8.500` | `-23.49%` |
| `encode_draw_cpu_ms_per_present` | `8.573` | `6.010` | `-29.90%` |
| `argbuf_setup_cpu_ms_per_present` | `1.827` | `0.000` | `-100.00%` |
| `argbuf_open_cpu_ms_per_present` | `0.730` | `0.000` | `-100.00%` |
| `argbuf_cbuf_update_cpu_ms_per_present` | `0.958` | `0.000` | `-100.00%` |
| `argbuf_table_bind_calls` | `994,684` | `0` | `-100.00%` |

The locality gates are stable enough for this CPU test: command buffers and
sub-command buffers per present are unchanged, render passes rise only `+0.09%`,
and tile preservation rises `+0.58%`. That means the result is not hiding behind
the known bad draw-count fragmentation carrier.

## P4 Shape

| No-enqueue stage | Baseline | Direct-cbuf | Delta |
|---|---:|---:|---:|
| wait -> commit chunk entry | `3.674` | `4.015` | `+0.341` |
| commit entry -> publish | `13.672` | `16.260` | `+2.588` |
| publish -> encode dequeue | `0.244` | `0.244` | flat |
| encode dequeue -> command buffer commit | `12.498` | `9.769` | `-2.729` |
| wait -> next enqueue | `30.482` | `30.703` | `+0.221` |

This is the important shape change: direct-cbuf removes the encode segment, but
the exposed wait shifts into the replay/publish segment rather than becoming
overlap. Any follow-up that targets `stream_bind`, PSO prefetch, or binding
packet work must now prove `wait -> next enqueue` moves, not only its local
encode counter.

## Residual Encode Ranking

After argbuf table removal, encode is no longer argbuf-led:

| Rank | Counter | ms/present |
|---:|---|---:|
| 1 | `encode_draw_stream_bind_cpu_ms` | `1.198` |
| 2 | `encode_slot_pso_prefetch_cpu_ms` | `1.158` |
| 3 | `encode_draw_binding_packet_cpu_ms` | `1.016` |
| 4 | `encode_draw_pipeline_lookup_cpu_ms` | `0.606` |
| 5 | `encode_draw_issue_cpu_ms` | `0.505` |

These are still valid local cleanup targets, but they are now second-order for
average FPS unless paired with a P4 gate.

## Decision

Keep `DXMT9_ARGBUF_DIRECT_CBUF=1` default-off:

- CPU mechanism: accepted.
- Average-FPS owner: rejected for direct promotion.
- Visual correctness: open for the current repeat.

The next bottleneck work should move to replay/snapshot/publish cadence or a
true overlap design. If direct-cbuf is revisited, first capture a same-frame
visual oracle or run a controlled repeated smoke before claiming correctness.
