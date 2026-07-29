---
domain: state-churn-encode
workload: 3DMark05 GT1
subcategory: encode-phase
order: 147
title: V003 Direct-Cbuf Visual-Safety Repeat
date: 2026-06-18
type: experiment
status: accepted-local-cpu-win-rejected-correctness-rejected-fps-owner
source: experiments/output/app-d3d9-3dmark05-v003-current-baseline-r1-20260618/result.json, experiments/output/app-d3d9-3dmark05-v003-current-baseline-r1-20260618/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-v003-current-baseline-r1-20260618/actual.png, experiments/output/app-d3d9-3dmark05-v003-direct-cbuf-r1-20260618/result.json, experiments/output/app-d3d9-3dmark05-v003-direct-cbuf-r1-20260618/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-v003-direct-cbuf-r1-20260618/actual.png, traces/app-d3d9-3dmark05-v003-direct-cbuf-r1-20260618/analysis/frame60-perf-counter-comparison.md
related: docs/perfomance/state-churn-encode/state-churn-encode-encode-phase.143.md, docs/perfomance/snapshot-cache/snapshot-cache-visual.01.md
---

# Encode Phase 147 - V003 Direct-Cbuf Visual-Safety Repeat

## Question

After correcting the GT1 visual-safety anchor to `v0.0.3`, can the current
`DXMT9_ARGBUF_DIRECT_CBUF=1` Stage 2b path still be considered a promotable
argbuf cleanup, or is its earlier visual status stale?

## Verdict

The CPU mechanism is real, but the current path fails the `v0.0.3` visual gate
and still does not own average FPS. Direct-cbuf removes the Stage 2 argbuf
table path again, cutting `encode_chunk_cpu_ms_per_present` by `-25.10%` and
`encode_draw_cpu_ms_per_present` by `-31.17%`. However the screenshot is
severely corrupted relative to the same-day baseline: the scene has large black
regions plus overexposed white geometry/bands instead of the normal GT1 frame.

This supersedes the older "normal visual" scout for promotion purposes. Keep
`DXMT9_ARGBUF_DIRECT_CBUF=1` default-off. If direct-cbuf is revisited, debug
runtime PSO/source/bind correctness first, then rerun the `v0.0.3` visual gate
before making any FPS claim.

## Runs

Baseline:

```sh
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix v003-current-baseline-r1-20260618 \
  --no-gputrace \
  --no-encoder-breakdown \
  --frame-sampling \
  --timeout 120 \
  --capture-delay-sec 45 \
  --wait-unlocked-sec 1 \
  --wait-unlocked-interval-sec 1
```

Direct-cbuf:

```sh
DXMT9_ARGBUF_DIRECT_CBUF=1 \
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix v003-direct-cbuf-r1-20260618 \
  --no-gputrace \
  --no-encoder-breakdown \
  --frame-sampling \
  --timeout 120 \
  --capture-delay-sec 45 \
  --wait-unlocked-sec 1 \
  --wait-unlocked-interval-sec 1 \
  --compare-baseline-output experiments/output/app-d3d9-3dmark05-v003-current-baseline-r1-20260618
```

Both runs finished with `status=pass` and timeout finalization, which is the
expected GT1 harness behavior for the final-frame hang.

## A/B Counters

| Metric | Baseline | Direct-cbuf | Delta |
|---|---:|---:|---:|
| `present_encoded` | `1,800` | `1,800` | `0` |
| `draws_per_present` | `738.806` | `739.675` | `+0.12%` |
| `gpu_command_buffer_time_ms_per_present` | `3.183` | `2.962` | `-6.94%` |
| `completion_wait_ms_per_present` | `26.890` | `28.347` | `+5.42%` |
| `completion_wait_without_enqueue_ms_per_present` | `26.839` | `28.250` | `+5.26%` |
| `commit_chunk_replay_cpu_ms_per_present` | `8.039` | `7.996` | `-0.54%` |
| `encode_chunk_cpu_ms_per_present` | `11.311` | `8.471` | `-25.10%` |
| `encode_draw_cpu_ms_per_present` | `8.750` | `6.023` | `-31.17%` |
| `argbuf_setup_cpu_ms_per_present` | `1.899` | `0.000` | `-100.00%` |
| `argbuf_open_cpu_ms_per_present` | `0.771` | `0.000` | `-100.00%` |
| `argbuf_cbuf_update_cpu_ms_per_present` | `0.981` | `0.000` | `-100.00%` |
| `argbuf_table_bind_calls` | non-zero | `0` | removed |

The command-buffer and render-pass shape is stable enough to isolate the CPU
mechanism: command buffers per present are unchanged, render passes rise only
`+0.08%`, and tile preservation changes by `-0.15%`.

## P4 Shape

| No-enqueue stage | Baseline | Direct-cbuf | Delta |
|---|---:|---:|---:|
| `commit entry -> publish` | `15.716` | `16.711` | `+0.995` |
| `publish -> encode dequeue` | `0.246` | `0.244` | `-0.003` |
| `encode dequeue -> command buffer commit` | `12.689` | `9.922` | `-2.768` |
| `wait -> next enqueue` | `32.911` | `31.205` | `-1.707` |

Direct-cbuf removes the encode segment but does not create real overlap:
`completion_wait_without_enqueue_ms_per_present` worsens by `+1.412ms`, and
the run remains `99.658%` no-enqueue wait. This keeps the average-FPS owner in
P4/replay/publish cadence, not in this local argbuf table path.

## Visual Gate

The baseline `actual.png` is a normal GT1 frame with the expected lights/bloom
and no obvious black/translucent vertex artifact. The direct-cbuf `actual.png`
is not a subtle frame-drift mismatch: it shows a mostly black scene with large
overexposed white geometry/bands. Image variance also jumps from `3117.76` to
`10280.42`, consistent with gross corruption rather than a harmless exposure
shift.

Because `v0.0.3` is the current known visual-safe anchor, this rejects
direct-cbuf as a promotable runtime path until the correctness issue is fixed.

## Next Debug Direction

The deterministic MSL/PSO ABI gate from phase 143 already proves that Stage 2b
source variants can omit slot-30 `ArgbufLayout` and use direct cbuf slots. The
current failure is therefore more likely in runtime selection or binding:

- active pass PSO handle versus direct-cbuf source/key mismatch;
- direct cbuf dirty tracking or slot binding across render-encoder boundaries;
- prefetched handle reuse with a stale argbuf selector;
- tile/pass interaction where the pass selector and draw selector disagree.

Do not spend another broad `.gputrace` on this variant first. A cheaper next
probe should attribute the runtime PSO handle/source selector and direct cbuf
slot binds on the corrupted frame, then repeat the same `v0.0.3` visual gate.
