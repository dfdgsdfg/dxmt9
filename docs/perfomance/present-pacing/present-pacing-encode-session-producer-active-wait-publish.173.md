---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: runtime
order: 173
title: EncodeSession Producer Active-Wait CpuReady Publish
date: 2026-06-28
type: no-gputrace
status: diagnostic-safe-runtime-rejected
source: experiments/output/app-d3d9-3dmark05-encode-session-producer-active-wait-publish-r2-after-manual-close-20260628/result.json, experiments/output/app-d3d9-3dmark05-encode-session-producer-active-wait-publish-r2-after-manual-close-20260628/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-encode-session-producer-active-wait-publish-r2-after-manual-close-20260628/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-producer-active-wait-publish-r2-after-manual-close-20260628/actual.png
related: docs/perfomance/present-pacing.md, docs/perfomance/present-pacing/present-pacing-encode-session-selector-wait-phase.172.md, docs/perfomance/present-pacing/present-pacing-encode-session-wait-start-cpuready-publish.168.md, specs/backend/design.md, specs/backend/requirements.md
---

# Present-Pacing H173 - EncodeSession Producer Active-Wait CpuReady Publish

## Question

H172 showed that selected `EncodeSession` prefixes and pending-session starts
almost always happen outside `completionWaitActive()`. H168 also showed that an
encode-thread wait-start observation rarely sees publishable writer work.

This run tests the remaining reactive timing variant: if the producer appends a
draw run after the completion wait has already opened, can that producer path
cut the current non-present writing slot as a semantic CpuReady source and give
the encoder useful work inside the same wait window?

## Run

```text
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix encode-session-producer-active-wait-publish-r2-after-manual-close-20260628 \
  --no-gputrace --no-encoder-breakdown --timeout 25 \
  --frame-sampling --pe-recorder-stats --dxmt-log-level info \
  --keep-frontmost --keep-frontmost-process 3DMark05.exe \
  --open-cb-preencode-tail-present \
  --open-cb-carry-render-session \
  --open-cb-semantic-boundary-publish \
  --open-cb-cpu-ready-command-limit 48 \
  --open-cb-writer-active-cpu-ready-publish \
  --open-cb-active-wait-cpu-ready-append \
  --open-cb-wait-start-cpu-ready-publish
```

The code under test extends `DXMT9_OPEN_CB_WAIT_START_CPU_READY_PUBLISH=1` to
producer-side draw append. It adds producer-specific counters so the ordinary
wait-start path and the producer-after-wait-start path can be separated.

## Verdict

Diagnostic safe, runtime promotion rejected.

The no-gputrace smoke is correctness-safe:

- `status=pass`, `failures=[]`, `capture_error=null`
- `returncode=143`, `timed_out=true`, which is the wrapper timeout path
- `present_encoded=840`
- `actual.png` is non-black (`mean_luma=68.378`, `variance=5117.047`)
- `gpu_command_buffer_errors=0`

The new producer path is completely inert in GT1:

- `open_cb_tail_present_wait_start_producer_publish_candidates=0`
- `open_cb_tail_present_wait_start_producer_published=0`
- the existing encode-thread wait-start path is also zero:
  `open_cb_tail_present_wait_start_publish_candidates=0` and
  `open_cb_tail_present_wait_start_published=0`

The P4 overlap gate still does not move:

- `completion_wait_enqueues_during_wait=1`
- `completion_wait_command_buffer_commit=1`
- `completion_wait_encode_dequeue=2`
- `completion_wait_commit_publish=1`
- `completion_wait_with_enqueue_ms_per_present=0.018`
- `completion_wait_without_enqueue_ms_per_present=13.501`
- `completion_wait_overlap_share=0.132%`

Shape remains baseline-like rather than better:

- `command_buffers_per_present=4.008`
- `sub_command_buffers_per_present=3.001`
- `render_pass_begin_per_present=10.680`
- `chunk_subcb_count_max=4`

## Interpretation

H173 rejects the "producer appended after wait-start wakeup but before wait
end" variant. In the valid rerun, the producer-side candidate counter is zero,
so GT1 is not delivering non-present draw work to the queue while the previous
Present completion wait is active.

This means the next owner is still earlier than reactive wait-window policy.
Either source/session attachment must happen before the wait opens, or the
Present/completion pacing must change enough that producer work can actually
exist during the wait. More wait-start hooks are unlikely to move FPS without
that earlier source-tape or pacing change.
