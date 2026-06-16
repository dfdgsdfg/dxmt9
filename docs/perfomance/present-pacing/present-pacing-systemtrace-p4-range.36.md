---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: xctrace-cpu-summary
order: 36
title: Seq-Range P4 System Trace Sidecar
date: 2026-06-15
type: experiment
status: negative-scout
source: traces/app-d3d9-3dmark05-current-p4-sidecar-range-r1-20260615/analysis/xctrace-cpu-thread-summary.md, traces/app-d3d9-3dmark05-current-p4-sidecar-range-r1-20260615/analysis/xctrace-cpu-thread-verdict.json, traces/app-d3d9-3dmark05-current-p4-sidecar-range-r1-20260615/analysis/xctrace-metal-gpu-intervals-summary.md, experiments/output/app-d3d9-3dmark05-current-p4-sidecar-range-r1-20260615/result.json, experiments/output/app-d3d9-3dmark05-current-p4-sidecar-range-r1-20260615/actual.png
related: docs/perfomance/present-pacing/present-pacing-systemtrace-p4-current.35.md, docs/perfomance/state-churn-encode/state-churn-encode-encode-phase.91.md
---

# Present-Pacing 36 - Seq-Range P4 System Trace Sidecar

## Question

Can the current P4 System Trace fallback use `--encoder-breakdown-seq-range`
instead of all-frame encoder breakdown, reducing probe overhead while still
joining xctrace rows and selecting the real producer thread?

## Run

```sh
bash scripts/tools/run_3dmark05_system_trace_sidecar.sh \
  --record-delay-sec 70 \
  --time-limit-sec 2 \
  --summary-top 10 \
  --encoder-breakdown-seq-range 1000:1125 \
  --export-cpu-summary \
  --cpu-producer-from-pe-log \
  -- \
  --suffix current-p4-sidecar-range-r1-20260615 \
  --frame 60 \
  --no-gputrace \
  --timeout 120 \
  --frame-sampling
```

The wrapper completed successfully. `result.json` reports `status=pass`,
`returncode=143`, and `timed_out=true`; that is acceptable for this workload
because 3DMark05 final-frame timeout finalization is expected and the sidecar
wrapper itself reported `system_trace_wrapper_status=0`.

| Field | Value |
|---|---:|
| `system_trace_xctrace_status` | `0` |
| `system_trace_wrapper_status` | `0` |
| Encoder breakdown scope | `seq 1000:1125` |
| Captured seq range | `1037..1073` |
| Joined encoder rows | `395 / 395` |
| dxmt join coverage | `100%` |
| Probe output size | `130MiB` |
| Trace size | `54MiB` |
| Frame sampling rows | `1,693` |

The size reduction is material: the prior all-frame current sidecar produced
about `553MiB` of probe output and `60MiB` of trace output. The seq-range run
keeps the same proof shape while cutting probe output to `130MiB`.

`actual.png` is visually normal for this capture point: scene geometry, bloom,
muzzle/impact light, and dense particles are all present.

## CPU Selector Result

The same-run native selector works and the producer verdict returns to a strict
negative scout:

| Field | Value |
|---|---:|
| Producer selector | `0x668652` |
| Selector source | `native-log-commit-chunk-entry` |
| Producer profile weight | `2506.000ms` |
| Producer sample rows | `2,515` |
| Producer running rows | `2,515` |
| Producer blocked rows | `0` |
| Producer wait keyword hits | `0` |
| Non-producer wait keyword hits | `1` |

```json
{
  "status": "producer-running-negative-scout",
  "producer_selection": "0x668652",
  "producer_selection_source": "native-log-commit-chunk-entry",
  "producer_wait_keyword_hits": "0",
  "producer_sample_running_rows": "2515",
  "producer_sample_blocked_rows": "0",
  "nonproducer_wait_keyword_hits": "1"
}
```

The one non-producer keyword hit is on another thread. The selected producer has
no `OnMainThread` / `kevent` / macdrv wait-stack sample in this 2-second window.

## Metal Timing Context

System Trace timing remains vertex-stage dominated:

| Metric | Value |
|---|---:|
| Stage sum | `1863.524ms` |
| Vertex stage sum | `1745.504ms` |
| Fragment stage sum | `118.019ms` |
| Vertex share | `93.67%` |
| Top-10 vertex ms/Mvertex p50 / p95 | `15.984 / 16.977` |

By route verdict:

| Group | Stage share | Vertex share |
|---|---:|---:|
| `needs-programmable-color-route` | `59.69%` | `95.39%` |
| `needs-programmable-textured-route` | `31.89%` | `90.72%` |
| `candidate-depth-only-route` | `6.71%` | `93.34%` |
| `mixed-programmable-route` | `1.71%` | `89.58%` |

This is still not an Xcode replay counter export; it does not expose
`VS Buffer Device Memory Bytes Written`.

## Pacing Context

The range-limited run is closer to the low-overhead timing shape than the
all-frame current sidecar, but it still proves no overlap:

| Metric | Total | Per present |
|---|---:|---:|
| `present_encoded` | `1,680` | n/a |
| `gpu_command_buffer_time_ms` | `5273.876ms` | `3.139ms` |
| `completion_wait_ms` | `46378.125ms` | `27.606ms` |
| `completion_wait_with_enqueue_ms` | `0.000ms` | `0.000ms` |
| `commit_chunk_replay_cpu_ms` | `14307.330ms` | `8.516ms` |
| `commit_chunk_queue_draw_submission_cpu_ms` | `7140.000ms` | `4.250ms` |
| `d3d9_snapshot_draw_submission_cpu_ms` | `5932.418ms` | `3.531ms` |
| `d3d9_snapshot_cache_lookup_cpu_ms` | `5020.362ms` | `2.988ms` |
| `encode_chunk_cpu_ms` | `18269.111ms` | `10.874ms` |
| `encode_draw_cpu_ms` | `14810.637ms` | `8.816ms` |

Same-cycle p50/p95 after no-enqueue waits:

| Stage | p50 | p95 |
|---|---:|---:|
| `commit_entry -> publish` | `5.820ms` | `32.950ms` |
| `publish -> encode_dequeue` | `0.235ms` | `0.482ms` |
| `encode_dequeue -> command_buffer_commit` | `12.318ms` | `24.010ms` |
| `wait -> next_enqueue` | `18.514ms` | `56.133ms` |

Clean gates remain clean: `draw_skipped_no_pipeline=0`,
`gpu_command_buffer_errors=0`, and `render_split_hazard=0`.

Frame sampling is not a promotion gate for this run, but it is in the expected
sidecar band: avg/p50/p95/tail600 p50 `17.458 / 17.151 / 25.396 / 14.823fps`.

## Decision

Accepted as the preferred current System Trace fallback shape while `.gputrace`
is blocked:

- It preserves xctrace/dxmt join coverage and native producer selection.
- It lowers artifact size and avoids all-frame encoder-breakdown overhead.
- It returns the P4 producer verdict to `producer-running-negative-scout`.
- It still shows `completion_wait_with_enqueue_ms=0`, so P4 is not hidden by
  producer run-ahead.

This reinforces the current target split: average-FPS work should reduce the
serial P2/P3 replay/snapshot/encode path or implement a larger overlap design.
A future P4 claim needs a targeted positive producer wait-stack sample or
direct movement in `completion_wait_with_enqueue_ms` on a visual-normal
low-overhead run.

**Related.** [[present-pacing-systemtrace-p4-current.35]] ·
[[state-churn-encode-encode-phase.91]] · [[present-pacing]].
