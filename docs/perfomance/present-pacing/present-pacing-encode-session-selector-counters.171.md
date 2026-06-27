---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: runtime
order: 171
title: EncodeSession Selector Prefix Counters
date: 2026-06-27
type: no-gputrace
status: diagnostic-safe-runtime-rejected
source: experiments/output/app-d3d9-3dmark05-encode-session-selector-counters-r1-20260627/result.json, experiments/output/app-d3d9-3dmark05-encode-session-selector-counters-r1-20260627/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-encode-session-selector-counters-r1-20260627/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-selector-counters-r1-20260627/dxmt9.log, experiments/output/app-d3d9-3dmark05-encode-session-selector-counters-r1-20260627/actual.png
related: docs/perfomance/present-pacing.md, docs/perfomance/present-pacing/present-pacing-encode-session-semantic-prefix.170.md, docs/perfomance/present-pacing/present-pacing-encode-session-ordinary-source-append.169.md, specs/backend/design.md, specs/backend/requirements.md
---

# Present-Pacing H171 - EncodeSession Selector Prefix Counters

## Question

H170 made the open-CB selector accept tailless non-present prefixes when the
first source is a `SemanticBoundary`, but the runtime proof was indirect. This
run adds explicit selector-path counters and reruns the same no-gputrace flag
set to answer two questions:

- Does GT1 actually select tail-ready and semantic-start prefixes at runtime?
- If yes, does that selected-prefix work reach Metal command-buffer commit
  during the previous Present completion wait?

## Run

```text
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix encode-session-selector-counters-r1-20260627 \
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

The code under test is `fe742554` (`backend: count open-cb selector prefixes`).
That commit adds cumulative and frame-sampled counters for selected tail-ready
prefixes and selected semantic-start prefixes.

## Verdict

Diagnostic safe, runtime promotion rejected.

The no-gputrace smoke is correctness-safe:

- `status=pass`, `failures=[]`, `capture_error=null`
- `returncode=143`, `timed_out=true`, which is the wrapper timeout path
- `present_encoded=840`
- `actual.png` is non-black (`mean_luma=68.159`, `variance=5312.769`)
- no `D3DERR`, `INVALIDCALL`, `0x8876086c`, `DXMT_ASSERT`, `abortOpenCb`, or
  command-buffer error rows in the output logs
- `gpu_command_buffer_errors=0`

The selector path is active:

- `open_cb_tail_present_selector_tail_prefix=804`
- `open_cb_tail_present_selector_tail_prefix_sources=1613`
- `open_cb_tail_present_selector_semantic_prefix=277`
- `open_cb_tail_present_selector_semantic_prefix_sources=562`

The carried session remains coherent:

- `open_cb_tail_present_pending_started=842`
- `open_cb_tail_present_head_appended=10395`
- `open_cb_tail_present_tail_appended=839`
- `open_cb_tail_present_tail_submitted=839`
- all pending abandon/merge counters are `0`
- `open_cb_tail_present_completion_wait_pending_observed=3`
- `open_cb_tail_present_completion_wait_pending_ready_source=2`
- `open_cb_tail_present_completion_wait_pending_no_ready_source=1`

The selected prefixes still do not become useful P4 overlap:

- `completion_wait_enqueues_during_wait=1`
- `completion_wait_command_buffer_commit=1`
- `completion_wait_encode_dequeue=2`
- `completion_wait_commit_publish=1`
- `completion_wait_with_enqueue_ms_per_present=0.017`
- `completion_wait_without_enqueue_ms_per_present=13.409`
- `completion_wait_no_enqueue_share=99.873%`
- Pacing verdict remains `under-pipelined-no-enqueue`

Metal/locality shape remains baseline-like but not better:

- `command_buffers_per_present=4.008`
- `sub_command_buffers_per_present=3.001`
- `render_pass_begin_per_present=10.675`
- `chunk_subcb_count_max=4`
- sampled avg FPS: `9.627`
- tail600 frame CSV avg/p50/p95: `10.059/9.871/13.713fps`

## Comparison

Versus H169, H171 keeps the same command-buffer shape and same negligible
same-window work:

- command buffers: `4.008 -> 4.008/present`
- sub command buffers: `3.001 -> 3.001/present`
- render passes: `10.661 -> 10.675/present`
- same-window command-buffer commits: `1 -> 1`
- no-enqueue wait: `13.414 -> 13.409ms/present`
- tail600 avg/p50/p95: `10.051/9.960/13.491 -> 10.059/9.871/13.713fps`

Versus H170, the explicit selector counters prove that the selected-prefix
path was not merely a native-test artifact:

- tail-ready selected prefixes: `804`
- semantic-start selected prefixes: `277`
- same-window command-buffer commits: `2 -> 1`
- no-enqueue wait: `13.417 -> 13.409ms/present`
- command buffers: `4.011 -> 4.008/present`

## Interpretation

H171 closes H170's observability gap. GT1 really does select many open-CB
prefixes, including hundreds of semantic-start prefixes, and those sources are
successfully carried through ordered completion without merge or retain
failures.

That still does not close R-BACK-2.50. The bottleneck is not "the selector does
not choose a prefix"; it is that the chosen source-tape work is not attached to
an open render encoder early enough to produce command-buffer commits inside
the previous Present completion wait. This keeps the next implementation owner
on producer/queue boundary timing or a stronger `EncodeSession` source-tape
merge, not another selector-shape relaxation.
