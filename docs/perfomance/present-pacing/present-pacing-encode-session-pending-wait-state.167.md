---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: runtime
order: 167
title: EncodeSession Pending Wait State
date: 2026-06-27
type: no-gputrace
status: diagnostic-observed-runtime-rejected
source: experiments/output/app-d3d9-3dmark05-encode-session-pending-wait-state-r1-20260627/result.json, experiments/output/app-d3d9-3dmark05-encode-session-pending-wait-state-r1-20260627/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-encode-session-pending-wait-state-r1-20260627/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-pending-wait-state-r1-20260627/dxmt9.log, experiments/output/app-d3d9-3dmark05-encode-session-pending-wait-state-r1-20260627/actual.png
related: docs/perfomance/present-pacing/index.md, docs/perfomance/present-pacing/present-pacing-encode-session-combined-cpuready-append.166.md, specs/backend/design.md, specs/backend/requirements.md
---

# Present-Pacing H167 - EncodeSession Pending Wait State

## Question

H166 proved that the combined writer-active CpuReady cut, producer command-limit
cut, Present-tail split, and active-wait append path is visual/error safe but
does not promote. The remaining ambiguity was whether the active completion
wait usually has no pending `EncodeSession`, or whether a pending open render
session exists but is not releasable/appendable.

## Run

```text
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix encode-session-pending-wait-state-r1-20260627 \
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

The run adds cumulative counters for pending `EncodeSession` state observed
while `completionWaitActive()` is true. It does not change the runtime policy.

## Verdict

Diagnostic observed; runtime promotion remains rejected.

The rerun is valid as a no-gputrace smoke:

- `status=pass`, `failures=[]`, `capture_error=null`
- `returncode=143`, `timed_out=true`, which is the wrapper timeout path
- `present_encoded=840`
- `actual.png` is non-black (`mean_luma=69.048`, `variance=5155.995`)
- no `D3DERR`, `INVALIDCALL`, `0x8876086c`, `DXMT_ASSERT`, `abortOpenCb`, or
  command-buffer error rows in the output logs
- `gpu_command_buffer_errors=0`

The combined source policy remains active:

- `chunk_publish_reason_semantic_boundary=10220`
- `chunk_publish_commands_semantic_boundary=237090`
- `chunk_publish_reason_present_split_before=804`
- `open_cb_tail_present_pending_started=843`
- `open_cb_tail_present_head_appended=10181`
- `open_cb_tail_present_tail_appended=839`
- `open_cb_tail_present_tail_submitted=839`
- `open_cb_tail_present_semantic_release_candidates=9524`
- `open_cb_tail_present_semantic_release_submitted=1`
- `open_cb_tail_present_semantic_release_blocked_no_completion_wait=9521`
- all no-wait blocks are writer-active (`9521 / 9521`)
- writer-active blocks split into `4403` empty-slot and `5118`
  non-present-slot observations
- `open_cb_tail_present_semantic_release_blocked_ready_source_no_completion_wait=5800`

The new wait-state counters close the ambiguity:

- `open_cb_tail_present_completion_wait_pending_observed=3`
- `open_cb_tail_present_completion_wait_pending_releasable=3`
- `open_cb_tail_present_completion_wait_pending_release_used=0`
- `open_cb_tail_present_completion_wait_pending_active_render=3`
- `open_cb_tail_present_completion_wait_pending_ready_source=3`
- `open_cb_tail_present_completion_wait_pending_no_ready_source=0`

Same-window work is still too sparse:

- `completion_wait_with_enqueue=1`
- `completion_wait_enqueues_during_wait=2`
- `completion_wait_commit_publish=1`
- `completion_wait_encode_dequeue=3`
- `completion_wait_command_buffer_commit=2`
- `completion_wait_with_enqueue_ms=13.395`
- `completion_wait_without_enqueue_ms=11370.853`
- `completion_wait_ms=11384.249`

The Metal shape remains baseline-like, not improved:

- frame CSV `command_buffers_per_present=4.009`
- frame CSV `sub_command_buffers_per_present=3.001`
- frame CSV `render_pass_begin_per_present=10.882`
- frame CSV tail600 avg/p50/p95 is `10.049/9.911/13.604fps`

## Interpretation

H167 shows that the failure mode is not "a pending open render session is often
available during the wait but blocked by a missing readiness bit." The active
completion wait sees a pending carried session only three times in the valid
sample. Those observations are releasable, active-render, and have a ready
source, but the total opportunity is effectively zero compared with `840`
presents and `9524` semantic-release candidates.

This reinforces the H166 conclusion: current CpuReady cuts create many source
boundaries, but they do not attach enough already-open `EncodeSession` work to
the completion-wait window. More threshold sweeps on the same source-cut family
are unlikely to move FPS. The remaining promotable branch is a stronger source
tape / pass-streaming carrier where CPU-ready source boundaries stay metadata
to an already-open render encoder, and Metal CB/pass/load-store boundaries are
chosen later without increasing CB/pass/tile preservation shape.
