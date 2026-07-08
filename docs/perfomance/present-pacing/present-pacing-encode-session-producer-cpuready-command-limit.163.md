---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: runtime
order: 163
title: EncodeSession Producer CpuReady Command Limit
date: 2026-06-27
type: no-gputrace
status: mechanism-observed-runtime-rejected
source: experiments/output/app-d3d9-3dmark05-encode-session-cpuready-command-limit48-r2-20260627/result.json, experiments/output/app-d3d9-3dmark05-encode-session-cpuready-command-limit48-r2-20260627/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-encode-session-cpuready-command-limit48-r2-20260627/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-cpuready-command-limit48-r2-20260627/actual.png
related: docs/perfomance/present-pacing/index.md, docs/perfomance/present-pacing/present-pacing-encode-session-writer-active-cpuready-publish.162.md, docs/perfomance/present-pacing/present-pacing-encode-session-writer-active-slot-shape.161.md, specs/backend/design.md, specs/backend/requirements.md
---

# Present-Pacing H163 - EncodeSession Producer CpuReady Command Limit

## Question

H162 proved that cutting writer-active work reactively from the encode thread is
safe but too late. If the producer publishes a non-present writing slot as a
`SemanticBoundary` source when it reaches a deterministic command threshold,
does that make CPU-ready work arrive early enough to recover P4 overlap while
keeping the baseline command-buffer shape?

## Run

```text
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix encode-session-cpuready-command-limit48-r2-20260627 \
  --no-gputrace --no-encoder-breakdown --timeout 25 \
  --frame-sampling --pe-recorder-stats --dxmt-log-level info \
  --keep-frontmost --keep-frontmost-process 3DMark05.exe \
  --open-cb-preencode-tail-present \
  --open-cb-carry-render-session \
  --open-cb-semantic-boundary-publish \
  --open-cb-cpu-ready-command-limit 48
```

The earlier `r1` run is not evidence: the launched app was closed manually and
the harness failed with `missing_capture`. The `r2` rerun is the valid sample.

## Verdict

Mechanism observed, runtime promotion rejected.

The run is correctness-safe for a no-gputrace smoke:

- `status=pass`, `failures=[]`, `capture_error=null`
- `present_encoded=900`
- `actual.png` shows normal GT1 content (`mean_luma=15.540`,
  `variance=2120.826`)
- no `D3DERR`, `INVALIDCALL`, `DXMT_ASSERT`, `abortOpenCb`, or assertion rows in
  the output logs
- `gpu_command_buffer_errors=0`
- `completion_dequeue_status_error=0`
- `draw_skipped_no_pipeline=0`
- pending timeout, abandon, retain, encode-null, and merge-failed counters all
  remain `0`

The producer-side source cut is active:

- `DXMT9_OPEN_CB_CPU_READY_COMMAND_LIMIT=48` publishes
  `chunk_publish_reason_semantic_boundary=6576`, or `7.307/present`
- first no-enqueue publish slots hit the configured threshold:
  `completion_no_enqueue_first_publish_slot_commands_p50=48`,
  `p95=48`
- `encode_session_carry_deferred_active_render_chunks=6576`
- `open_cb_tail_present_head_appended=5675`
- `open_cb_tail_present_tail_submitted=898`
- `chunk_publish_reason_present_split_before=0`

But the useful P4 gate does not open:

- `open_cb_tail_present_semantic_release_submitted=0`
- `completion_wait_encode_dequeue=0`
- `completion_wait_command_buffer_commit=1`
- `completion_wait_enqueues_during_wait=1`
- `completion_wait_with_enqueue_ms=9.743`, only `0.011ms/present`
- `completion_wait_without_enqueue_ms=11773.836`, or `13.082ms/present`

The command-buffer locality shape is baseline-like, not better:

- `command_buffers=3605`, or `4.006/present`
- `sub_command_buffers=2700`, or `3.000/present`
- `render_pass_begin=9600`, or `10.667/present`
- `gpu_command_buffer_time_ms=1570.870`, or `1.745ms/present`
- frame CSV FPS is weak for this short smoke: all-frame avg/p50/p95
  `11.797/11.102/19.624`, tail600 avg/p50/p95 `10.177/9.900/13.763`

## Interpretation

H163 is the stronger version of H162's lesson. The source-publication timing is
now deterministic and producer-side, and it successfully turns many small
non-present writing-slot units into `SemanticBoundary` sources without visual or
GPU errors. That closes the "reactive encode-thread cut is simply too late"
implementation gap.

It still does not raise the FPS ceiling. The new sources become visible to the
queue, but they do not become same-window Metal commits: the active completion
wait sees essentially no encode dequeue or command-buffer commit. The queue ends
up with more logical source boundaries while `EncodeSession` still emits about
the same `4` Metal command buffers and `3` sub-command buffers per Present.

Therefore command-limit CPU-ready publication is not a production path by
itself. It is useful diagnostic evidence that the remaining problem is below
the source-publication layer: source boundaries must be metadata-only to an open
render encoder, or the encoder must be able to stream across staged sources
without turning each source into a separate CB/sub-CB scheduling unit. The next
R-BACK-2.40/R-BACK-2.43 shape should focus on source-tape or pass-streaming
encode ownership, not more command-limit threshold sweeps.
