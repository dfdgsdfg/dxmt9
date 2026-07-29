---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: runtime
order: 168
title: EncodeSession Wait-Start CpuReady Publish
date: 2026-06-27
type: no-gputrace
status: diagnostic-safe-runtime-rejected
outdated: knob-removed
source: experiments/output/app-d3d9-3dmark05-encode-session-wait-start-cpuready-publish-r2-20260627/result.json, experiments/output/app-d3d9-3dmark05-encode-session-wait-start-cpuready-publish-r2-20260627/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-encode-session-wait-start-cpuready-publish-r2-20260627/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-wait-start-cpuready-publish-r2-20260627/dxmt9.log, experiments/output/app-d3d9-3dmark05-encode-session-wait-start-cpuready-publish-r2-20260627/actual.png
related: docs/perfomance/present-pacing/index.md, docs/perfomance/present-pacing/present-pacing-encode-session-pending-wait-state.167.md, docs/perfomance/present-pacing/present-pacing-encode-session-combined-cpuready-append.166.md, specs/backend/spec.md, specs/backend/requirements.md
---

# Present-Pacing H168 - EncodeSession Wait-Start CpuReady Publish

> **Outdated — the knob or code path this experiment measured no longer exists in `src/`.** It cannot be re-run. Kept as history; do not cite it as current evidence.

## Question

H167 showed that active completion waits almost never overlap an existing
pending open `EncodeSession`. The remaining narrow branch was whether the
encoder loop can create the first pending session at wait start by publishing
the current non-present writer slot as a semantic CpuReady source while the
completion thread is already inside `waitUntilCompleted()`.

## Run

```text
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix encode-session-wait-start-cpuready-publish-r2-20260627 \
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

The default-off knob is `DXMT9_OPEN_CB_WAIT_START_CPU_READY_PUBLISH=1`. The r1
sample was used only to check that the flag combination still ran; r2 adds the
wait-start classification counters and is the evidence sample.

## Verdict

Diagnostic safe, runtime promotion rejected.

The smoke is correctness-safe for a no-gputrace timeout sample:

- `status=pass`, `failures=[]`, `capture_error=null`
- `returncode=143`, `timed_out=true`, which is the wrapper timeout path
- `present_encoded=840`
- `actual.png` is non-black (`mean_luma=75.953`, `variance=5421.289`)
- no `D3DERR`, `INVALIDCALL`, `0x8876086c`, `DXMT_ASSERT`, or `abortOpenCb`
  rows in the output logs
- `gpu_command_buffer_errors=0`

The new wait-start path does not actually publish:

- `open_cb_tail_present_wait_start_publish_candidates=1`
- `open_cb_tail_present_wait_start_publish_slot_empty=1`
- `open_cb_tail_present_wait_start_publish_slot_present=0`
- `open_cb_tail_present_wait_start_publish_blocked_headroom=0`
- `open_cb_tail_present_wait_start_published=0`

The existing combined source policy remains active, but same-window work stays
negligible:

- `chunk_publish_reason_semantic_boundary=10647`
- `chunk_publish_reason_present_split_before=800`
- `open_cb_tail_present_pending_started=844`
- `open_cb_tail_present_head_appended=10603`
- `open_cb_tail_present_tail_submitted=839`
- `open_cb_tail_present_completion_wait_pending_observed=9`
- `open_cb_tail_present_completion_wait_pending_ready_source=6`
- `completion_wait_with_enqueue=2`
- `completion_wait_enqueues_during_wait=3`
- `completion_wait_encode_dequeue=7`
- `completion_wait_command_buffer_commit=3`

The Metal shape is still baseline-like rather than improved:

- `command_buffers_per_present=4.012`
- `sub_command_buffers_per_present=3.002`
- `render_pass_begin_per_present=10.740`
- `chunk_subcb_count_max=4`
- tail600 frame CSV avg/p50/p95 is `10.067/9.961/13.792fps`
- `completion_wait_without_enqueue_ms_per_present=13.510`
- `completion_wait_overlap_share=0.141%`

## Interpretation

H168 rejects the narrow "create the first pending session exactly at wait
start" branch. The encode loop observed the required top-level state only once
in the valid sample, and that one observation had an empty writer slot, so no
semantic CpuReady source was published. The path is error-safe, but it does not
move P4, FPS, command-buffer count, render-pass count, or same-window commit
incidence.

This strengthens H167 rather than replacing it. The issue is not merely that a
pending session is missing when the wait begins; the encoder-observable wait
window also does not reliably coincide with writer-owned non-present work that
can be cut into a first pending session. More reactive wait-window gates are
therefore unlikely to promote. The remaining production branch is still a
stronger CpuReady/source-tape carrier where source boundaries are metadata-only
to an already-open render encoder, or a larger replay/producer cadence change
that creates enqueue-during-wait without increasing CB/pass/tile/load-store
shape.
