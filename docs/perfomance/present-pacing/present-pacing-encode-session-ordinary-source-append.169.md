---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: runtime
order: 169
title: EncodeSession Ordinary Source Append
date: 2026-06-27
type: no-gputrace
status: diagnostic-safe-runtime-rejected
source: experiments/output/app-d3d9-3dmark05-encode-session-ordinary-append-r2-20260627/result.json, experiments/output/app-d3d9-3dmark05-encode-session-ordinary-append-r2-20260627/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-encode-session-ordinary-append-r2-20260627/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-ordinary-append-r2-20260627/dxmt9.log, experiments/output/app-d3d9-3dmark05-encode-session-ordinary-append-r2-20260627/actual.png
related: docs/perfomance/present-pacing/index.md, docs/perfomance/present-pacing/present-pacing-encode-session-pending-wait-state.167.md, specs/backend/spec.md, specs/backend/requirements.md
---

# Present-Pacing H169 - EncodeSession Ordinary Source Append

## Question

H168 rejected another reactive wait-window publication gate: the active
completion wait almost never coincides with a publishable writer-owned
non-present slot. The next small policy question was whether an already-pending
carried `EncodeSession` can safely append ordinary non-present ready sources,
rather than restricting appendability to only semantic-boundary sources, without
breaking the final-Present-tail completion model.

## Run

```text
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix encode-session-ordinary-append-r2-20260627 \
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

The code change is not a new environment knob. It widens the append policy only
after a pending carried session already exists. First session start remains
restricted to the existing safe semantic/tail-ready prefix selection; appending
preflights both completion-source capacity and encoder-side session-source
compatibility before mutating the pending record.

The first launched app was closed manually and is not used as evidence. The r2
wrapper run above is the evidence sample.

## Verdict

Diagnostic safe, runtime promotion rejected.

The no-gputrace smoke is correctness-safe:

- `status=pass`, `failures=[]`, `capture_error=null`
- `returncode=143`, `timed_out=true`, which is the wrapper timeout path
- `present_encoded=840`
- `actual.png` is non-black (`mean_luma=66.284`, `variance=5421.722`)
- no `D3DERR`, `INVALIDCALL`, `0x8876086c`, `DXMT_ASSERT`, `abortOpenCb`, or
  command-buffer error rows in the output logs
- `gpu_command_buffer_errors=0`

The widened append path preserves the open-CB session shape and avoids abandon
or merge failures:

- `chunk_publish_reason_semantic_boundary=10163`
- `chunk_publish_reason_present_split_before=801`
- `open_cb_tail_present_pending_started=842`
- `open_cb_tail_present_head_appended=10122`
- `open_cb_tail_present_tail_appended=839`
- `open_cb_tail_present_tail_submitted=839`
- all pending abandon/merge counters are `0`
- `open_cb_tail_present_completion_wait_pending_observed=3`
- `open_cb_tail_present_completion_wait_pending_ready_source=3`
- `open_cb_tail_present_completion_wait_pending_no_ready_source=0`

However, same-window work does not improve:

- `completion_wait_with_enqueue=1`
- `completion_wait_enqueues_during_wait=1`
- `completion_wait_commit_publish=1`
- `completion_wait_encode_dequeue=3`
- `completion_wait_command_buffer_commit=1`
- `completion_wait_with_enqueue_ms_per_present=0.017`
- `completion_wait_without_enqueue_ms_per_present=13.414`
- `completion_wait_overlap_share=0.123%`

The Metal shape moves only slightly versus H168:

- `command_buffers_per_present`: `4.012 -> 4.008`
- `sub_command_buffers_per_present`: `3.002 -> 3.001`
- `render_pass_begin_per_present`: `10.740 -> 10.661`
- `open_cb_tail_present_head_appended`: `10603 -> 10122`
- `chunk_subcb_count_max=4`
- sampled avg FPS: `9.559 -> 9.644`
- tail600 frame CSV avg/p50/p95: `10.067/9.961/13.792fps -> 10.051/9.960/13.496fps`

## Interpretation

H169 validates the narrow ordinary-source append relaxation as safe under the
current opt-in open-CB carrier, but it does not change the owner call. The
slight CB/pass reduction is useful evidence that the preflight/append policy is
not immediately fragmenting locality, yet the active wait still sees too few
pending sessions and too few command-buffer commits to raise FPS.

This keeps R-BACK-2.50 open. The remaining promotable direction is still a
stronger source-tape or open-render-encoder pass-streaming carrier where
CPU-ready source boundaries are metadata to an already-open render encoder and
Metal CB/pass/load-store boundaries are chosen later. A policy tweak that only
changes which ready source can append to a sparse pending session is not enough
to move P4.
