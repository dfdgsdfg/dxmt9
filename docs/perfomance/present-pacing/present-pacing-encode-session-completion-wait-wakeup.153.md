---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: runtime
order: 153
title: EncodeSession Completion-Wait Wakeup
date: 2026-06-21
type: no-gputrace
status: mechanism-observed-runtime-rejected
source: experiments/output/app-d3d9-3dmark05-encode-session-completion-wait-wakeup-r2-20260621/result.json, experiments/output/app-d3d9-3dmark05-encode-session-completion-wait-wakeup-r2-20260621/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-encode-session-completion-wait-wakeup-r2-20260621/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-completion-wait-wakeup-r2-20260621/actual.png
related: docs/perfomance/present-pacing.md, docs/perfomance/present-pacing/present-pacing-encode-session-current-smoke.152.md, docs/perfomance/present-pacing/present-pacing-encode-session-wait-stage-durations.151.md, specs/backend/design.md, specs/backend/requirements.md
---

# Present-Pacing H153 - EncodeSession Completion-Wait Wakeup

## Question

H151 showed that same-window publish/dequeue and dequeue/commit handoff is fast
when it happens, but many semantic-release candidates still miss the active
completion-wait window. Does waking the encode loop when the completion watcher
enters or exits `waitUntilCompleted()` increase useful same-window commits
without reintroducing invalid-call, GPU, queue, or visual failures?

## Run

```text
DXMT9_OPEN_CB_SEMANTIC_BOUNDARY_PUBLISH=1 \
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix encode-session-completion-wait-wakeup-r2-20260621 \
  --no-gputrace --no-encoder-breakdown --timeout 25 \
  --frame-sampling \
  --open-cb-preencode-tail-present \
  --open-cb-carry-render-session \
  --pe-recorder-stats --dxmt-log-level info \
  --keep-frontmost --keep-frontmost-process 3DMark05
```

Implementation shape:

- `QueueLifecycleController::processOnePendingCompletion()` now notifies the
  encode condition variable when `completionWaitActive_` opens and closes.
- `runOpenCbTailPresentEncodeLoop()` lets a pending open-CB source wake on
  wait-start only when semantic-boundary release is possible and has not already
  been used for that wait.
- The loop also wakes on wait-end to reset the once-per-wait release gate.

The first version of the wake predicate was rejected before promotion because
it woke continuously while the wait stayed active:
`open_cb_tail_present_semantic_release_candidates=597600` and
`open_cb_tail_present_semantic_release_blocked_already_used=595667`. H153 uses
the spin-fixed r2 run.

## Verdict

Mechanism observed, runtime promotion rejected.

The r2 run is correctness-safe for this smoke:

- `status=pass`, `failures=[]`, `timed_out=true`, `returncode=143`
- output image is non-black and coherent enough for smoke purposes
  (`mean_luma=73.040`, `variance=5364.723`)
- `gpu_command_buffer_errors=0`
- `completion_dequeue_status_error=0`
- log search found no `0x8876086c`, `D3DERR_INVALIDCALL`, `INVALIDCALL`,
  `invalid call`, `commit_chunk_fail`, `device_present_fail`, or
  `pe_call_return_untracked_failure`

The wakeup does increase useful same-window activity versus H152:

- semantic releases submitted: `57 -> 126`
- command-buffer commits during completion wait: `57 -> 125`
- enqueues during completion wait: `56 -> 124`
- completion wait with enqueue: `1572.978ms -> 3721.949ms`
- already-used blocks drop despite more candidates: `153 -> 34`

It does not pass the performance/locality promotion gates:

- semantic candidates still mostly arrive outside completion wait:
  `1625 / 1785` blocked with no active wait
- command buffers per present regress slightly: `4.059 -> 4.124`
- render-pass begins per present regress: `10.360 -> 10.843`
- sub-command buffers stay flat at the baseline-style cap:
  `2.997/present -> 2.997/present`
- this was a short no-gputrace smoke, not a 120s locality-gated A/B with an FPS
  uplift

## Interpretation

The invalid-call report is not reproduced by the latest fresh runs; the actual
`0x8876086c` hits remain attached to older rejected intermediate experiments
such as the finalizing-storeproof and no-sub-CB variants.

H153 proves a useful local scheduler fix: the encode loop should not sleep
through a completion-wait window when a semantic-boundary release is already
eligible. That fix moves same-window commit incidence, but it does not solve the
main P4 wall. The remaining blocker is still CPU-ready/window coverage plus
CB/pass locality. The next promotable carrier must make more work CPU-ready
before or during the wait, or merge an already-ready logical tape while
preserving baseline command-buffer, render-pass, and tile load/store shape.
