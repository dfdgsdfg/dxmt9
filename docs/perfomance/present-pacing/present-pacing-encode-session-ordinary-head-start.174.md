---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: runtime
order: 174
title: EncodeSession Ordinary Head-Start Selection
date: 2026-06-28
type: no-gputrace
status: diagnostic-safe-runtime-rejected
source: experiments/output/app-d3d9-3dmark05-encode-session-ordinary-head-start-r2-20260628/result.json, experiments/output/app-d3d9-3dmark05-encode-session-ordinary-head-start-r2-20260628/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-encode-session-ordinary-head-start-r2-20260628/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-ordinary-head-start-r2-20260628/actual.png
related: docs/perfomance/present-pacing.md, docs/perfomance/present-pacing/present-pacing-encode-session-producer-active-wait-publish.173.md, docs/perfomance/present-pacing/present-pacing-encode-session-selector-wait-phase.172.md, specs/backend/design.md, specs/backend/requirements.md
---

# Present-Pacing H174 - EncodeSession Ordinary Head-Start Selection

## Question

H169 allowed ordinary non-present sources to append only after an
`EncodeSession` already existed. H170 allowed a tailless non-present prefix to
start a pending session only when the first source was a `SemanticBoundary`.
H172/H173 then showed that selected work and producer-side reactive publication
still miss the active completion-wait window.

This run tests the remaining selector-policy branch: if carry mode can start a
pending `EncodeSession` from an ordinary non-present source, including a single
head or a head-only prefix, does GT1 create useful P4 overlap without breaking
the baseline command-buffer/sub-CB/pass shape?

## Run

```text
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix encode-session-ordinary-head-start-r2-20260628 \
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

The code under test removes the semantic-only first-head restriction in the
open-CB tail-Present selector. With
`DXMT9_OPEN_CB_CARRY_RENDER_SESSION=1`, an ordinary non-present ready source can
start the pending carried session; the selector also accepts a single ordinary
head and head-only non-present prefixes. The change is still opt-in and does
not alter the default queue path.

## Verdict

Diagnostic safe, runtime promotion rejected.

The rerun is correctness-safe:

- `status=pass`, `failures=[]`, `capture_error=null`
- `returncode=143`, `timed_out=true`, which is the wrapper timeout path
- `present_encoded=840`
- `actual.png` is non-black (`mean_luma=75.829`, `variance=5677.556`)
- `gpu_command_buffer_errors=0`
- no `D3DERR_INVALIDCALL`, `INVALIDCALL`, or `invalid call` strings in the
  direct, dxmt9, or 3DMark logs

The command-buffer shape remains baseline-like:

- `command_buffers_per_present=4.008`
- `sub_command_buffers_per_present=3.001`
- `render_pass_begin_per_present=10.665`
- `chunk_subcb_count_max=4`

The selector is active, but the active-wait opportunity does not improve:

- `open_cb_tail_present_pending_started=842`
- `open_cb_tail_present_pending_started_wait_active=2`
- `open_cb_tail_present_pending_started_wait_inactive=840`
- `open_cb_tail_present_selector_tail_prefix=806` (`1617` sources)
- `open_cb_tail_present_selector_semantic_prefix=290` (`587` sources)
- `open_cb_tail_present_completion_wait_pending_observed=2`
- `open_cb_tail_present_completion_wait_pending_ready_source=2`
- `open_cb_tail_present_completion_wait_pending_release_used=0`

The P4 overlap gate is unchanged:

- `completion_wait_enqueues_during_wait=1`
- `completion_wait_command_buffer_commit=1`
- `completion_wait_encode_dequeue=2`
- `completion_wait_commit_publish=1`
- `completion_wait_with_enqueue_ms_per_present=0.016`
- `completion_wait_without_enqueue_ms_per_present=13.605`

The stable-window FPS remains noise-level against the previous r7/r1 samples:

- H173 r7 tail600 FPS avg/p50/p95: `10.031 / 9.781 / 13.704`
- H174 r1 tail600 FPS avg/p50/p95: `10.019 / 9.773 / 13.442`
- H174 r2 tail600 FPS avg/p50/p95: `10.084 / 9.991 / 14.010`

## Interpretation

H174 rejects the semantic-only first-head restriction as the immediate P4 owner.
Starting a carried session from an ordinary non-present head is safe under the
current opt-in guards, but it does not make useful work appear during the active
completion wait. Pending sessions still overwhelmingly start outside the wait
window, and the completion wait still has only one enqueue/commit event.

The remaining owner is therefore earlier source/session attachment, not another
selector relaxation. A promotable carrier must attach CPU-ready source-tape work
to an open render encoder before the wait opens, or move producer/replay cadence
enough that ready work actually exists while the previous Present completion is
being waited on.
