---
domain: state-churn-encode
workload: 3DMark05 GT1
subcategory: encode-phase
order: 124
title: Argbuf Open Completed-Seq Snapshot
date: 2026-06-15
type: experiment
status: accepted-local-cleanup-rejected-fps-owner
source: src/dxmt9/dxmt9_command_queue.cpp, src/dxmt9/dxmt9_command_queue.hpp, src/dxmt9/dxmt9_draw_encoder.hpp, src/dxmt9/dxmt9_draw_encoder.mm, src/dxmt9/dxmt9_argbuf_hybrid.cpp, src/dxmt9/dxmt9_argbuf_hybrid.hpp, experiments/output/app-d3d9-3dmark05-uniform-backend-materialize-reuse-base-r1/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-uniform-backend-materialize-reuse-base-r1/result.json, experiments/output/app-d3d9-3dmark05-argbuf-open-completed-snapshot-r1/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-argbuf-open-completed-snapshot-r1/result.json, experiments/output/app-d3d9-3dmark05-argbuf-open-completed-snapshot-r1/actual.png
related: docs/perfomance/state-churn-encode/state-churn-encode-encode-phase.122.md, docs/perfomance/state-churn-encode/state-churn-encode-encode-phase.123.md, docs/perfomance/state-churn-encode.md
---

# Encode Phase 124 - Argbuf Open Completed-Seq Snapshot

**Question.** Can per-draw `openArgbuf()` table reservation avoid rereading
`CommandQueue::completedSeqId_` under the queue mutex on every draw?

**Implementation.** `EncodeContext` now carries one chunk-local
`transientCompletedSeqId` snapshot. `openArgbufWithCompletedSeqId()` and the
draw encoder's direct transient upload helpers pass that snapshot to the
transient arena. This is lifetime-safe because a stale lower completion
watermark can only delay reclaim; it cannot free storage earlier than the GPU
completion waterline.

```mermaid
sequenceDiagram
  participant Q as CommandQueue
  participant E as encodeChunk
  participant A as transient arena
  participant G as GPU

  Q->>Q: snapshot completedSeqId once
  Q->>E: EncodeContext.transientCompletedSeqId
  loop per draw that reopens argbuf
    E->>A: reserve argbuf table with snapshot
    A->>A: reclaim only up to snapshot
    E->>G: bind fresh slot-30 table
  end
  Note over A,G: stale snapshot is conservative; live slabs are retained longer, never shorter
```

**Probe.**

```sh
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix argbuf-open-completed-snapshot-r1 \
  --frame 60 \
  --no-gputrace \
  --no-encoder-breakdown \
  --frame-sampling \
  --timeout 120 \
  --wait-unlocked-sec 1 \
  --wait-unlocked-interval-sec 1
```

The run completed with `status=pass`, normal GT1 visual output, and clean
health counters: `draw_skipped_no_pipeline=0`, `gpu_command_buffer_errors=0`,
and `render_split_hazard=0`.

## Result

Baseline is `uniform-backend-materialize-reuse-base-r1`; candidate is
`argbuf-open-completed-snapshot-r1`.

| Metric | Baseline | Candidate | Delta |
|---|---:|---:|---:|
| `encode_draw_argbuf_open_reserve_cpu_ms_per_present` | `0.196` | `0.158` | `-19.04%` |
| `encode_draw_argbuf_open_call_cpu_ms_per_present` | `0.341` | `0.328` | `-3.77%` |
| `argbuf_open_cpu_ms_per_present` | `0.779` | `0.862` | `+10.60%` |
| `argbuf_setup_cpu_ms_per_present` | `1.845` | `2.076` | `+12.55%` |
| `argbuf_cbuf_update_cpu_ms_per_present` | `0.924` | `1.050` | `+13.63%` |
| `encode_chunk_cpu_ms_per_present` | `10.820` | `12.054` | `+11.41%` |
| `commit_chunk_replay_cpu_ms_per_present` | `8.110` | `9.297` | `+14.64%` |
| `completion_wait_without_enqueue_ms_per_present` | `26.695` | `25.414` | `-4.80%` |
| `sampled_avg_fps` | prior noisy band | `14.830` | not promoted |

## Interpretation

The queue-mutex snapshot does what it was meant to do only at the narrow child
level: `open_reserve` falls by about `0.037ms/present`. That is too small to
move the parent. The larger `argbuf_open`, `argbuf_setup`, encode, and replay
buckets do not improve in this run, so this is not an FPS or P2/P3/P4 fix.

Treat this as a bounded hot-path cleanup, not a bottleneck answer. The next
argbuf candidate should not spend another iteration on completed-seq snapshot
plumbing. It must either reduce actual fresh table frequency, reduce true cbuf
dirty upload frequency, or move to a broader serial-stage/P4 overlap design.

**Related.** [state-churn-encode-encode-phase.122](state-churn-encode-encode-phase.122.md) ·
[state-churn-encode-encode-phase.123](state-churn-encode-encode-phase.123.md) · [state-churn-encode](../state-churn-encode.md).
