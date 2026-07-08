---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: runtime
order: 165
title: EncodeSession Active-Wait CpuReady Append
date: 2026-06-27
type: no-gputrace
status: mechanism-safe-runtime-rejected
source: experiments/output/app-d3d9-3dmark05-encode-session-active-wait-cpuready-append-r1-20260627/result.json, experiments/output/app-d3d9-3dmark05-encode-session-active-wait-cpuready-append-r1-20260627/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-encode-session-active-wait-cpuready-append-r1-20260627/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-active-wait-cpuready-append-r1-20260627/actual.png
related: docs/perfomance/present-pacing/index.md, docs/perfomance/present-pacing/present-pacing-encode-session-open-cb-present-tail-split.164.md, specs/backend/spec.md, specs/backend/requirements.md
---

# Present-Pacing H165 - EncodeSession Active-Wait CpuReady Append

## Question

H164 fixed the final Present tail shape but still submitted no semantic
pending prefixes during the active completion-wait window. If
`DXMT9_OPEN_CB_ACTIVE_WAIT_CPU_READY_APPEND=1` keeps appendable ready sources
inside a pending `EncodeSession` before active-wait semantic release, and cuts
current writer work as a semantic CPU-ready source when no ready source exists,
does that create useful P4 work without breaking visual correctness or CB/pass
locality?

## Run

```text
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix encode-session-active-wait-cpuready-append-r1-20260627 \
  --no-gputrace --no-encoder-breakdown --timeout 25 \
  --frame-sampling --pe-recorder-stats --dxmt-log-level info \
  --keep-frontmost --keep-frontmost-process 3DMark05.exe \
  --open-cb-preencode-tail-present \
  --open-cb-carry-render-session \
  --open-cb-semantic-boundary-publish \
  --open-cb-cpu-ready-command-limit 48 \
  --open-cb-active-wait-cpu-ready-append
```

## Verdict

Mechanism safe, runtime promotion rejected.

The run is correctness-safe for a no-gputrace smoke:

- `status=pass`, `failures=[]`, `capture_error=null`
- `present_encoded=840`
- `actual.png` shows normal GT1 content
- no `D3DERR`, `INVALIDCALL`, `0x8876086c`, `DXMT_ASSERT`, or `abortOpenCb`
  rows in the output logs
- `gpu_command_buffer_errors=0`
- pending timeout, abandon, retain, encode-null, and merge-failed counters all
  remain `0`

The active-wait append policy does not materially open the P4 window:

- `open_cb_tail_present_semantic_release_candidates=5358`
- `open_cb_tail_present_semantic_release_submitted=0`
- `open_cb_tail_present_semantic_release_blocked_no_completion_wait=5358`
- `open_cb_tail_present_semantic_release_blocked_no_completion_wait_writer_active=5358`
- `open_cb_tail_present_semantic_release_blocked_no_completion_wait_writer_active_slot_nonpresent=5098`
- `open_cb_tail_present_semantic_release_blocked_ready_source_no_completion_wait=784`
- `completion_wait_commit_publish=1`
- `completion_wait_encode_dequeue=0`
- `completion_wait_command_buffer_commit=1`
- `completion_wait_enqueues_during_wait=1`
- `completion_wait_with_enqueue_ms=9.863`, or `0.012ms/present`
- `completion_wait_without_enqueue_ms=10671.766`, or `12.704ms/present`

The source and Metal shape remain baseline-like:

- `chunk_publish_reason_semantic_boundary=6147`
- `chunk_publish_reason_present_split_before=830`
- `open_cb_tail_present_head_appended=6135`
- `open_cb_tail_present_tail_appended=839`
- `open_cb_tail_present_tail_submitted=839`
- frame CSV `command_buffers_per_present=4.006`
- frame CSV `sub_command_buffers_per_present=3.000`
- frame CSV `render_pass_begin_per_present=10.880`
- `render_pass_begin=8913`
- `gpu_command_buffer_time_ms=1273.493`
- frame CSV tail600 avg/p50/p95 is `10.140/10.045/13.881fps`

## Interpretation

H165 is a useful negative control for the "append inside active wait" policy.
The new flag is structurally safe: it does not introduce invalid-call, Metal
error, pending-merge, or broad visual failures, and it keeps the command-buffer
and sub-CB shape at the H163/H164 level.

It also proves that the next blocker is not merely the H155
release-before-ready ordering. The pending semantic candidates still happen
outside the active completion-wait window, and the active-wait path almost never
gets a chance to publish or append current writer work before release. The
remaining owner is active-wait coverage: either CPU-ready/session work must be
created earlier by the producer, or the design needs a stronger logical tape
merge that can keep already-existing writer-active work attached to an
`EncodeSession` before the completion wait opens.

Keep `DXMT9_OPEN_CB_ACTIVE_WAIT_CPU_READY_APPEND` default-off. Do not promote
or sweep it without a new source-tape carrier that first proves
`completion_wait_encode_dequeue` or `completion_wait_command_buffer_commit`
movement at non-increasing CB/pass/tile/load-store shape.
