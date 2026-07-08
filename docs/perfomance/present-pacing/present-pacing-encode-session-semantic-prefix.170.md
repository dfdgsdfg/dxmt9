---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: runtime
order: 170
title: EncodeSession Semantic Prefix Selection
date: 2026-06-27
type: no-gputrace
status: diagnostic-safe-runtime-rejected
source: experiments/output/app-d3d9-3dmark05-encode-session-semantic-prefix-r1-20260627/result.json, experiments/output/app-d3d9-3dmark05-encode-session-semantic-prefix-r1-20260627/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-encode-session-semantic-prefix-r1-20260627/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-semantic-prefix-r1-20260627/dxmt9.log, experiments/output/app-d3d9-3dmark05-encode-session-semantic-prefix-r1-20260627/actual.png
related: docs/perfomance/present-pacing/index.md, docs/perfomance/present-pacing/present-pacing-encode-session-ordinary-source-append.169.md, docs/perfomance/present-pacing/present-pacing-encode-session-wait-start-cpuready-publish.168.md, specs/backend/design.md, specs/backend/requirements.md
---

# Present-Pacing H170 - EncodeSession Semantic Prefix Selection

## Question

H169 proved that ordinary non-present ready sources can safely append to an
already-pending carried `EncodeSession`, but that policy is still too late when
pending sessions are sparse. The next implementation slice lets the open-CB
selector pick a tailless non-present ready prefix when the first source is a
`SemanticBoundary`; ordinary sources may append behind that first semantic
source, but still cannot become the first tailless head.

This is closer to the R-BACK-2.43 source-tape model because the selected
non-present suffix can be passed to the encoder as `sessionLookaheadSources`
before the final Present tail is ready.

## Run

```text
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix encode-session-semantic-prefix-r1-20260627 \
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

The code change adds no new environment knob. Native coverage in
`dxmt9-render-backend-batch-contract-spec` now pins that non-semantic head-only
queues are rejected, while semantic-start non-present prefixes are accepted.

## Verdict

Diagnostic safe, runtime promotion rejected.

The no-gputrace smoke is correctness-safe:

- `status=pass`, `failures=[]`, `capture_error=null`
- `returncode=143`, `timed_out=true`, which is the wrapper timeout path
- `present_encoded=840`
- `actual.png` is non-black (`mean_luma=68.986`, `variance=5158.414`)
- no `D3DERR`, `INVALIDCALL`, `0x8876086c`, `DXMT_ASSERT`, `abortOpenCb`, or
  command-buffer error rows in the output logs
- `gpu_command_buffer_errors=0`

The session carrier remains coherent:

- `chunk_publish_reason_semantic_boundary=10498`
- `chunk_publish_reason_present_split_before=809`
- `open_cb_tail_present_pending_started=843`
- `open_cb_tail_present_head_appended=10464`
- `open_cb_tail_present_tail_appended=839`
- `open_cb_tail_present_tail_submitted=839`
- all pending abandon/merge counters are `0`
- `open_cb_tail_present_completion_wait_pending_observed=7`
- `open_cb_tail_present_completion_wait_pending_ready_source=4`
- `open_cb_tail_present_completion_wait_pending_no_ready_source=3`

Same-window work improves only from negligible to negligible:

- `completion_wait_with_enqueue=2`
- `completion_wait_enqueues_during_wait=2`
- `completion_wait_commit_publish=5`
- `completion_wait_encode_dequeue=5`
- `completion_wait_command_buffer_commit=2`
- `completion_wait_with_enqueue_ms_per_present=0.019`
- `completion_wait_without_enqueue_ms_per_present=13.417`
- `completion_wait_overlap_share=0.145%`

The Metal/locality shape does not promote versus H169:

- `command_buffers_per_present`: `4.008 -> 4.011`
- `sub_command_buffers_per_present`: `3.001 -> 3.002`
- `render_pass_begin_per_present`: `10.661 -> 10.664`
- `open_cb_tail_present_head_appended`: `10122 -> 10464`
- `chunk_subcb_count_max=4`
- sampled avg FPS: `9.644 -> 9.651`
- tail600 frame CSV avg/p50/p95: `10.051/9.960/13.496fps -> 10.050/9.907/13.666fps`

## Interpretation

H170 validates the semantic-start source-prefix selector as a safe deterministic
step toward source-tape pass streaming. It expands the selected source suffix
available to `EncodeSession` and increases pending-session observations during
completion wait (`3 -> 7` versus H169), while keeping the fail-open and
completion-source paths clean.

It still does not close R-BACK-2.50. The runtime evidence does not show P4
movement: no-enqueue wait remains the dominant path, command buffers and render
passes are not lower, and tail600 FPS is unchanged within noise. Also, this run
does not include a selector-specific runtime counter, so the semantic-prefix
selection path is proven directly by native coverage and only indirectly by the
runtime session-shape deltas.

The next implementation slice should either add explicit selector-path counters
or move the producer/queue boundary earlier so a semantic-start source prefix is
already attached to an open render encoder before the completion wait opens.
