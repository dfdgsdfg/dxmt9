---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: runtime
order: 158
title: EncodeSession Ready-Source Miss Counter
date: 2026-06-21
type: no-gputrace
status: diagnostic-observed-runtime-rejected
source: experiments/output/app-d3d9-3dmark05-encode-session-ready-source-miss-counter-r1-20260621/result.json, experiments/output/app-d3d9-3dmark05-encode-session-ready-source-miss-counter-r1-20260621/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-encode-session-ready-source-miss-counter-r1-20260621/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-ready-source-miss-counter-r1-20260621/result-perf-counter-comparison.md, experiments/output/app-d3d9-3dmark05-encode-session-ready-source-miss-counter-r1-20260621/actual.png
related: docs/perfomance/present-pacing/index.md, docs/perfomance/present-pacing/present-pacing-encode-session-strict-semantic-start.157.md, specs/backend/spec.md, specs/backend/requirements.md
---

# Present-Pacing H158 - EncodeSession Ready-Source Miss Counter

## Question

H157 showed that most semantic-boundary release candidates still miss the active
completion-wait window. Are those misses mostly caused by the queue already
having a next ready source and preserving append locality, or are they still
primarily caused by the pending prefix being observed while no completion wait
is active and no useful ready-source preempt is available?

## Run

```text
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix encode-session-ready-source-miss-counter-r1-20260621 \
  --no-gputrace --no-encoder-breakdown --timeout 25 \
  --frame-sampling --pe-recorder-stats --dxmt-log-level info \
  --keep-frontmost --keep-frontmost-process 3DMark05.exe \
  --open-cb-preencode-tail-present \
  --open-cb-carry-render-session \
  --open-cb-semantic-boundary-publish
```

## Verdict

Diagnostic observed, runtime promotion rejected.

The run is correctness-safe:

- `status=pass`, `failures=[]`, `returncode=143`, `timed_out=true`
- `present_encoded=960`
- output image shows normal GT1 battlefield content (`mean_luma=75.956`,
  `variance=5667.399`)
- no giant vertical black artifact
- `gpu_command_buffer_errors=0`
- log search found no `D3DERR_INVALIDCALL`, `invalid call`, fatal assertion,
  queue error, or command-buffer failure rows
- `draw_skipped_no_pipeline=0`
- `encode_session_carry_forced_finalize_initializer_waits=0`

The new counter records a real but secondary miss class:

- `open_cb_tail_present_semantic_release_blocked_ready_source_no_completion_wait=188`
- legacy no-active-wait blocks: `1391`
- semantic releases submitted: `139`
- already-used blocks: `47`
- semantic release failures: `0`
- `chunk_publish_reason_present_split_before=0`
- `chunk_publish_reason_semantic_boundary=1724`
- `open_cb_tail_present_pending_started=1015`
- `open_cb_tail_present_head_appended=709`
- `open_cb_tail_present_tail_submitted=873`
- `encode_session_carry_deferred_active_render_chunks=1724`

Compared with H157, this instrumentation run is not an FPS or P4 promotion:

- command buffers: `4.173 -> 4.148/present`
- sub-command buffers: `2.998 -> 2.998/present`
- render-pass begins: `10.332 -> 10.358/present`
- tile preservation: `103.769 -> 104.216MiB/present`
- GPU command-buffer time: `2.642 -> 2.414ms/present`
- total completion wait: `20.757 -> 20.307ms/present`
- completion wait with enqueue: `5.123 -> 4.302ms/present`
- completion wait without enqueue: `15.634 -> 16.004ms/present`
- completion-wait command-buffer commits: `162 -> 138`
- enqueues during completion wait: `162 -> 137`
- publish-to-dequeue during wait p50/p95: `0.069/0.107 -> 0.070/0.105ms`
- dequeue-to-commit during wait p50/p95: `1.077/1.266 -> 1.121/1.303ms`
- `chunk_subcb_count_max=4`

## Interpretation

The ready-source miss class exists, but it is not the dominant wall. The new
counter is an observation counter for the ready-source path, not a replacement
for the legacy semantic-release candidate denominator. In H158 it reports
`188` ready-source/no-wait observations, while the older empty-ready/no-wait
blocker still reports `1391` events. This means the queue sometimes preserves
append locality when a pending semantic prefix could otherwise be released, but
most missed opportunities are still simply outside an active completion wait.

This rejects a broad "release before every ready source" policy. H154 already
showed that deterministic release can submit all candidates but fragments CB
shape badly, and H158 shows the ready-source no-wait class is too small to be
the sole owner. The next carrier still needs earlier CPU-ready arrival or a
session object that is already dequeued and can be committed inside an active
wait without adding command buffers, render passes, or tile preservation.
