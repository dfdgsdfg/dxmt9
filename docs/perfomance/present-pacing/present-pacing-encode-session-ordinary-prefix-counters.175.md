---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: runtime
order: 175
title: EncodeSession Ordinary-Prefix Counter Rerun
date: 2026-06-28
type: no-gputrace
status: diagnostic-safe-runtime-rejected
source: experiments/output/app-d3d9-3dmark05-encode-session-ordinary-prefix-counters-r1-20260628/result.json, experiments/output/app-d3d9-3dmark05-encode-session-ordinary-prefix-counters-r1-20260628/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-encode-session-ordinary-prefix-counters-r1-20260628/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-ordinary-prefix-counters-r1-20260628/actual.png
related: docs/perfomance/present-pacing/index.md, docs/perfomance/present-pacing/present-pacing-encode-session-ordinary-head-start.174.md, specs/backend/design.md, specs/backend/requirements.md
---

# Present-Pacing H175 - EncodeSession Ordinary-Prefix Counter Rerun

## Question

H174 allowed an ordinary non-present head to start a carried `EncodeSession`,
but the selector counters still only classified tail-ready and semantic-start
prefixes. After adding explicit ordinary-start counters, this rerun asks whether
GT1 actually selects ordinary/head-only session prefixes, or whether the
remaining source-tape numerator is still semantic-boundary dominated.

## Run

```text
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix encode-session-ordinary-prefix-counters-r1-20260628 \
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

The code under test adds
`open_cb_tail_present_selector_ordinary_prefix*` counters and classifies
single-source carried-session prefixes instead of counting only multi-source
selector prefixes. The counter definition change means H175 semantic-prefix
totals include single semantic heads that H174 did not count.

## Verdict

Diagnostic safe, runtime promotion rejected.

The run is correctness-safe:

- `status=pass`, `failures=[]`, `capture_error=null`
- `returncode=143`, `timed_out=true`, which is the wrapper timeout path
- `present_encoded=840`
- `actual.png` is non-black (`mean_luma=71.848`, `variance=5098.550`)
- `gpu_command_buffer_errors=0`
- no `D3DERR_INVALIDCALL`, `INVALIDCALL`, or `invalid call` strings in the
  direct, dxmt9, or 3DMark logs

The ordinary-start numerator is zero:

- `open_cb_tail_present_selector_ordinary_prefix=0`
- `open_cb_tail_present_selector_ordinary_prefix_sources=0`
- wait-active and wait-inactive ordinary-prefix rows are all `0`

The selected source-tape work is semantic-boundary dominated, but still misses
the active wait:

- `open_cb_tail_present_selector_semantic_prefix=10170`
- `open_cb_tail_present_selector_semantic_prefix_sources=10437`
- `open_cb_tail_present_selector_semantic_prefix_wait_active=4`
- `open_cb_tail_present_selector_semantic_prefix_wait_active_sources=6`
- `open_cb_tail_present_selector_semantic_prefix_wait_inactive=10166`
- `open_cb_tail_present_selector_semantic_prefix_wait_inactive_sources=10431`
- `open_cb_tail_present_selector_tail_prefix=804`
- `open_cb_tail_present_selector_tail_prefix_wait_active=0`

The pending-session wait view is slightly less inert than H174 but still too
sparse for promotion:

- `open_cb_tail_present_pending_started=843`
- `open_cb_tail_present_pending_started_wait_active=3`
- `open_cb_tail_present_pending_started_wait_inactive=840`
- `open_cb_tail_present_completion_wait_pending_observed=6`
- `open_cb_tail_present_completion_wait_pending_releasable=6`
- `open_cb_tail_present_completion_wait_pending_release_used=2`
- `open_cb_tail_present_completion_wait_pending_active_render=6`
- `open_cb_tail_present_completion_wait_pending_ready_source=3`
- `open_cb_tail_present_completion_wait_pending_no_ready_source=3`

The command-buffer shape remains baseline-like:

- `command_buffers_per_present=4.011`
- `sub_command_buffers_per_present=3.002`
- `render_pass_begin_per_present=10.662`
- `chunk_subcb_count_max=4`

The P4 overlap gate still does not move materially:

- `completion_wait_enqueues_during_wait=2`
- `completion_wait_command_buffer_commit=2`
- `completion_wait_encode_dequeue=4`
- `completion_wait_commit_publish=5`
- `completion_wait_with_enqueue_ms_per_present=0.020`
- `completion_wait_without_enqueue_ms_per_present=13.247`
- tail600 FPS avg/p50/p95 is `10.057 / 9.911 / 13.785`

## Interpretation

H175 rejects ordinary/head-only prefix selection as a GT1 numerator. The H174
policy relaxation is safe, but GT1 does not actually select ordinary-start
prefixes under the current source stream. Once single-prefix sessions are
classified, the available tailless session starts are almost entirely
semantic-boundary starts, and those still overwhelmingly arrive outside the
active completion wait.

The next owner is therefore not another ordinary selector relaxation. A
promotable implementation must make the semantic-boundary source stream attach
to an open render encoder before the wait opens, or move producer/replay cadence
enough that the already-safe semantic session starts become active during the
previous Present completion wait.
