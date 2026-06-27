---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: runtime
order: 176
title: EncodeSession Source-Class Counters
date: 2026-06-28
type: no-gputrace
status: diagnostic-safe-runtime-rejected
source: experiments/output/app-d3d9-3dmark05-encode-session-source-class-counters-r2-20260628/result.json, experiments/output/app-d3d9-3dmark05-encode-session-source-class-counters-r2-20260628/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-encode-session-source-class-counters-r2-20260628/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-source-class-counters-r2-20260628/actual.png
related: docs/perfomance/present-pacing.md, docs/perfomance/present-pacing/present-pacing-encode-session-ordinary-prefix-counters.175.md, specs/backend/design.md, specs/backend/requirements.md
---

# Present-Pacing H176 - EncodeSession Source-Class Counters

## Question

H175 proved GT1 does not use ordinary sources to start tailless carried
`EncodeSession` prefixes. H176 asks the next narrower question: once a pending
session starts or a source is appended to the current head, which source class
actually owns the source-tape attachment? If the numerator is already semantic
and already appended before the wait, the remaining wall is timing/pacing rather
than selector relaxation.

## Run

```text
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix encode-session-source-class-counters-r2-20260628 \
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

The code under test adds pending-start counters split into tail-ready,
semantic, and ordinary source classes, with wait-active and wait-inactive
sub-buckets. It also splits `open_cb_tail_present_head_appended` into semantic
and ordinary append classes.

## Verdict

Diagnostic safe, runtime promotion rejected.

The rerun is correctness-safe:

- `status=pass`, `failures=[]`, `capture_error=null`
- `returncode=143`, `timed_out=true`, which is the wrapper timeout path
- `present_encoded=840`
- `actual.png` is non-black (`mean_luma=72.195`, `variance=5145.315`)
- `gpu_command_buffer_errors=0`
- no `D3DERR_INVALIDCALL`, `INVALIDCALL`, or `invalid call` strings in the
  direct, dxmt9, or result logs

The command-buffer shape remains baseline-like:

- `command_buffers_per_present=4.008`
- `sub_command_buffers_per_present=3.001`
- `render_pass_begin_per_present=10.696`
- `chunk_subcb_count_max=4`

Pending starts are not ordinary, and they almost never happen while completion
wait is active:

- `open_cb_tail_present_pending_started=842`
- `open_cb_tail_present_pending_started_wait_active=2`
- `open_cb_tail_present_pending_started_wait_inactive=840`
- `open_cb_tail_present_pending_started_tail_ready=2`
- `open_cb_tail_present_pending_started_tail_ready_wait_active=1`
- `open_cb_tail_present_pending_started_tail_ready_wait_inactive=1`
- `open_cb_tail_present_pending_started_semantic=840`
- `open_cb_tail_present_pending_started_semantic_wait_active=1`
- `open_cb_tail_present_pending_started_semantic_wait_inactive=839`
- `open_cb_tail_present_pending_started_ordinary=0`

Once a carried session exists, head append is also mostly semantic, with a
smaller ordinary tail behind it:

- `open_cb_tail_present_head_appended=10250`
- `open_cb_tail_present_head_appended_semantic=9447`
- `open_cb_tail_present_head_appended_ordinary=803`
- `open_cb_tail_present_tail_appended=839`
- `open_cb_tail_present_tail_submitted=839`

The selector-side classification agrees with H175's conclusion:

- `open_cb_tail_present_selector_tail_prefix=804`
- `open_cb_tail_present_selector_tail_prefix_sources=1614`
- `open_cb_tail_present_selector_tail_prefix_wait_active=0`
- `open_cb_tail_present_selector_semantic_prefix=10000`
- `open_cb_tail_present_selector_semantic_prefix_sources=10282`
- `open_cb_tail_present_selector_semantic_prefix_wait_active=1`
- `open_cb_tail_present_selector_semantic_prefix_wait_inactive=9999`
- `open_cb_tail_present_selector_ordinary_prefix=0`

The P4 overlap gate still does not move:

- `completion_wait_enqueues_during_wait=1`
- `completion_wait_command_buffer_commit=1`
- `completion_wait_with_enqueue_ms_per_present=0.016`
- `completion_wait_without_enqueue_ms_per_present=13.475`
- tail600 FPS avg/p50/p95 is `10.023 / 9.888 / 13.678`

## Interpretation

H176 rejects both remaining selector-shape branches for GT1. Starting a pending
session from an ordinary source is not used, and appendability is not the main
blocker: the source tape already appends many sources into carried sessions, but
that attachment is overwhelmingly semantic and wait-inactive.

The remaining owner is therefore timing. A promotable carrier must either make
semantic source/session attachment happen before the previous Present completion
wait opens, keep that attached work in an open render encoder until it can be
committed inside the wait, or change producer/replay cadence enough to create
real enqueue-during-wait activity. Another ordinary-prefix or appendability
relaxation is not expected to move GT1 FPS.
