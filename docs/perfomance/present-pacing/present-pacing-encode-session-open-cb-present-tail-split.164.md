---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: runtime
order: 164
title: EncodeSession Open-CB Present-Tail Split
date: 2026-06-27
type: no-gputrace
status: mechanism-observed-runtime-rejected
source: experiments/output/app-d3d9-3dmark05-encode-session-open-cb-present-tail-split-r1-20260627/result.json, experiments/output/app-d3d9-3dmark05-encode-session-open-cb-present-tail-split-r1-20260627/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-encode-session-open-cb-present-tail-split-r1-20260627/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-open-cb-present-tail-split-r1-20260627/actual.png
related: docs/perfomance/present-pacing/index.md, docs/perfomance/present-pacing/present-pacing-encode-session-producer-cpuready-command-limit.163.md, specs/backend/design.md, specs/backend/requirements.md
---

# Present-Pacing H164 - EncodeSession Open-CB Present-Tail Split

## Question

H163 proved that producer-side CpuReady source publication is active but still
left the final `Present` publish carrying pre-Present draw work. If
`DXMT9_OPEN_CB_PREENCODE_TAIL_PRESENT=1` forces the remaining writing slot to
publish as a `PresentSplitBefore` head before appending the final Present tail,
does the tail-only structure open P4 overlap while preserving correctness?

## Run

```text
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix encode-session-open-cb-present-tail-split-r1-20260627 \
  --no-gputrace --no-encoder-breakdown --timeout 25 \
  --frame-sampling --pe-recorder-stats --dxmt-log-level info \
  --keep-frontmost --keep-frontmost-process 3DMark05.exe \
  --open-cb-preencode-tail-present \
  --open-cb-carry-render-session \
  --open-cb-semantic-boundary-publish \
  --open-cb-cpu-ready-command-limit 48
```

## Verdict

Mechanism observed, runtime promotion rejected.

The run is correctness-safe for a no-gputrace smoke:

- `status=pass`, `failures=[]`, `capture_error=null`
- `present_encoded=840`
- `actual.png` shows normal GT1 content (`mean_luma=73.695`,
  `variance=5535.679`)
- no `D3DERR`, `INVALIDCALL`, `0x8876086c`, `DXMT_ASSERT`, or `abortOpenCb`
  rows in the output logs
- `gpu_command_buffer_errors=0`
- `completion_dequeue_status_error=0`
- `draw_skipped_no_pipeline=0`
- pending timeout, abandon, retain, encode-null, and merge-failed counters all
  remain `0`

The new tail-only split is active:

- `chunk_publish_reason_present_split_before=829`, or `0.987/present`
- `chunk_publish_reason_present=840`
- H163's `chunk_publish_present_pre_present_opportunity_tail_draw_run=884`
  becomes `0`
- the pre-Present work now lands in split heads:
  `chunk_publish_present_split_before_tail_draw_run=822` and
  `chunk_publish_present_split_before_tail_clear=7`
- `open_cb_tail_present_head_appended=6052`
- `open_cb_tail_present_tail_appended=839`
- `open_cb_tail_present_tail_submitted=839`

The useful P4 gate still does not open:

- `open_cb_tail_present_semantic_release_submitted=0`
- `completion_wait_encode_dequeue=0`
- `completion_wait_command_buffer_commit=1`
- `completion_wait_enqueues_during_wait=1`
- `completion_wait_with_enqueue_ms=9.924`, or `0.012ms/present`
- `completion_wait_without_enqueue_ms=10578.157`, or `12.593ms/present`

The Metal shape remains baseline-like:

- `command_buffers=3365`, or `4.006/present`
- `sub_command_buffers=2520`, or `3.000/present`
- `render_pass_begin=8885`, or `10.577/present`
- `chunk_subcb_count_max=4`
- `gpu_command_buffer_time_ms=1256.081`, or `1.495ms/present`
- frame CSV tail600 avg/p50/p95 is `10.177/10.128/13.891fps`, effectively
  unchanged from H163 tail600 avg `10.177fps`

## Interpretation

H164 closes a real structural gap in the open-CB carrier: the final Present
publish is now a drawable/present-only tail under the opt-in path, while the
pre-Present work is a `PresentSplitBefore` source that can be appended into the
same open command buffer/session. This matches the R-BACK-2.37 tail ownership
model better than H163, where the Present source still carried draw work.

It still does not move the frame-rate wall. The new split changes source
shape, but it does not create same-window encode dequeue or Metal command-buffer
commit during the previous present-completion wait. The remaining owner is not
the tail-only split itself; it is still earlier CPU-ready arrival or an
already-dequeued open session that can commit inside the active wait window
without increasing command-buffer, render-pass, tile, or load/store shape.

Keep the implementation default-off. The next candidate should target the
source-tape / pass-streaming ownership below publication, not another
command-limit sweep.
