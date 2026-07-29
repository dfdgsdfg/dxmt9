---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: runtime
order: 166
title: EncodeSession Combined CpuReady Append
date: 2026-06-27
type: no-gputrace
status: mechanism-safe-runtime-rejected
source: experiments/output/app-d3d9-3dmark05-encode-session-writer-active-plus-active-wait-append-rerun1-20260627/result.json, experiments/output/app-d3d9-3dmark05-encode-session-writer-active-plus-active-wait-append-rerun1-20260627/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-encode-session-writer-active-plus-active-wait-append-rerun1-20260627/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-writer-active-plus-active-wait-append-rerun1-20260627/dxmt9.log, experiments/output/app-d3d9-3dmark05-encode-session-writer-active-plus-active-wait-append-rerun1-20260627/actual.png
related: docs/perfomance/present-pacing/index.md, specs/backend/spec.md, specs/backend/requirements.md
---

# Present-Pacing H166 - EncodeSession Combined CpuReady Append

## Question

H162's reactive writer-active cut can create CPU-ready semantic sources, H163's
producer command-limit cut can create them earlier, and H165's active-wait
append policy can keep compatible ready work inside the pending
`EncodeSession` before release. If all three are enabled together, does GT1 get
useful same-window P4 work while preserving the H164/H165 baseline-like
CB/sub-CB shape?

## Run

```text
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix encode-session-writer-active-plus-active-wait-append-rerun1-20260627 \
  --no-gputrace --no-encoder-breakdown --timeout 25 \
  --frame-sampling --pe-recorder-stats --dxmt-log-level info \
  --keep-frontmost --keep-frontmost-process 3DMark05.exe \
  --open-cb-preencode-tail-present \
  --open-cb-carry-render-session \
  --open-cb-semantic-boundary-publish \
  --open-cb-cpu-ready-command-limit 48 \
  --open-cb-writer-active-cpu-ready-publish \
  --open-cb-active-wait-cpu-ready-append
```

An earlier same-condition sample was discarded because the launched 3DMark05
process was closed manually during the run. Only `rerun1` is used as evidence.

## Verdict

Mechanism safe, runtime promotion rejected.

The rerun is correctness-safe for a no-gputrace smoke:

- `status=pass`, `failures=[]`, `capture_error=null`
- `returncode=143`, `timed_out=true`, which is the wrapper timeout path
- `present_encoded=840`
- `actual.png` is non-black (`mean_luma=62.579`, `variance=5224.633`)
- no `D3DERR`, `INVALIDCALL`, `0x8876086c`, `DXMT_ASSERT`, or `abortOpenCb`
  rows in the output logs
- `gpu_command_buffer_errors=0`
- pending timeout, timeout-submitted, abandon, retain, encode-null, and
  merge-failed counters all remain `0`

The combined source policies are active but still do not open the useful wait
window:

- `chunk_publish_reason_semantic_boundary=10541`
- `chunk_publish_commands_semantic_boundary=240875`
- `chunk_publish_reason_present_split_before=796`
- `open_cb_tail_present_pending_started=843`
- `open_cb_tail_present_head_appended=10494`
- `open_cb_tail_present_tail_appended=839`
- `open_cb_tail_present_tail_submitted=839`
- `open_cb_tail_present_semantic_release_candidates=9791`
- `open_cb_tail_present_semantic_release_submitted=1`
- `open_cb_tail_present_semantic_release_blocked_no_completion_wait=9786`
- `open_cb_tail_present_semantic_release_blocked_no_completion_wait_writer_active=9786`
- writer-active blocks split into `4467` empty-slot and `5319` non-present-slot
  cases
- `open_cb_tail_present_semantic_release_blocked_ready_source_no_completion_wait=6054`

Same-window work is still too sparse to affect FPS:

- `completion_wait_with_enqueue=1`
- `completion_wait_enqueues_during_wait=2`
- `completion_wait_commit_publish=2`
- `completion_wait_encode_dequeue=4`
- `completion_wait_command_buffer_commit=2`
- `completion_wait_with_enqueue_ms=13.950`
- `completion_wait_without_enqueue_ms=11670.966`
- `completion_wait_ms=11684.916`

The Metal shape stays baseline-like, but not better:

- frame CSV `command_buffers_per_present=4.009`
- frame CSV `sub_command_buffers_per_present=3.001`
- frame CSV `render_pass_begin_per_present=10.905`
- `render_pass_begin=8994`
- `gpu_command_buffer_time_ms=1391.753`
- frame CSV tail600 avg/p50/p95 is `9.959/9.748/13.539fps`

## Interpretation

H166 closes the straightforward "enable all current CPU-ready policies
together" branch. The combination is not a correctness failure in the valid
rerun, but it does not promote: it creates many semantic source boundaries and
keeps the command-buffer/sub-CB shape near H165, yet only one semantic release
and two Metal command-buffer commits land inside an active completion wait.

This reinforces H163-H165's diagnosis. Producer-side command-limit publication
and reactive writer-active publication can make more work CPU-ready, and
active-wait append can preserve locality for compatible ready work. The missing
piece is timing and session attachment: most candidates still arrive outside
the active wait, and ready-source/no-wait blocks remain common. The next
promotable design needs earlier source-tape staging or an already-dequeued
open-render-encoder `EncodeSession` that can commit inside the wait without
turning each source boundary into a Metal CB/pass/load-store boundary.

Keep this flag combination diagnostic-only. Do not spend `.gputrace` on it and
do not run more threshold sweeps unless a new carrier first moves
`completion_wait_command_buffer_commit` at non-increasing CB/pass/tile shape.
