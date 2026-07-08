---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: runtime
order: 159
title: EncodeSession No-Wait Writer Split
date: 2026-06-21
type: no-gputrace
status: diagnostic-observed-runtime-rejected
source: experiments/output/app-d3d9-3dmark05-encode-session-no-wait-writer-split-r1-20260621/result.json, experiments/output/app-d3d9-3dmark05-encode-session-no-wait-writer-split-r1-20260621/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-encode-session-no-wait-writer-split-r1-20260621/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-no-wait-writer-split-r1-20260621/actual.png
related: docs/perfomance/present-pacing/index.md, docs/perfomance/present-pacing/present-pacing-encode-session-ready-source-miss-counter.158.md, specs/backend/spec.md, specs/backend/requirements.md
---

# Present-Pacing H159 - EncodeSession No-Wait Writer Split

## Question

H158 showed that ready-source/no-wait append-locality misses are real but
secondary to the older no-active-wait semantic-release blocker. Is that
dominant no-active-wait blocker an inactive-writer drain problem, or is the
writer still active when the semantic prefix is observed?

## Run

```text
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix encode-session-no-wait-writer-split-r1-20260621 \
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
- output image shows normal GT1 battlefield content (`mean_luma=90.487`,
  `variance=6866.559`)
- no giant vertical black artifact
- `gpu_command_buffer_errors=0`
- `completion_dequeue_status_error=0`
- `draw_skipped_no_pipeline=0`
- `encode_session_carry_forced_finalize_initializer_waits=0`

The new split classifies every dominant no-active-wait miss as writer-active:

- `open_cb_tail_present_semantic_release_blocked_no_completion_wait=1398`
- `open_cb_tail_present_semantic_release_blocked_no_completion_wait_writer_active=1398`
- `open_cb_tail_present_semantic_release_blocked_no_completion_wait_writer_inactive=0`
- `open_cb_tail_present_semantic_release_blocked_ready_source_no_completion_wait=196`
- `open_cb_tail_present_semantic_release_blocked_already_used=63`
- `open_cb_tail_present_semantic_release_submitted=132`
- pending timeout, abandon, retain, encode-null, and merge-failed counters all
  remain `0`
- `chunk_publish_reason_present_split_before=0`
- `chunk_publish_reason_semantic_boundary=1732`
- `chunk_subcb_count_max=4`

Compared with H158, this is not an FPS or P4 promotion:

- command buffers: `4.148 -> 4.141/present`
- sub-command buffers: `2.998 -> 2.998/present`
- render-pass begins: `10.358 -> 10.365/present`
- GPU command-buffer time: `2.414 -> 2.408ms/present`
- total completion wait: `20.307 -> 20.082ms/present`
- completion wait with enqueue: `4.302 -> 4.191ms/present`
- completion wait without enqueue: `16.004 -> 15.891ms/present`
- completion-wait command-buffer commits: `138 -> 131`
- enqueues during completion wait: `137 -> 131`

## Interpretation

The missing inactive-writer drain hypothesis is rejected for this GT1 shape.
The empty-ready/no-wait blocker is not waiting on a stale inactive writer that
the encoder could drain with a simple policy tweak. In this sample, all `1398`
dominant no-active-wait blocks happen while the writer is still active.

That narrows the next owner. Broad release outside active completion waits is
still rejected by H154's locality fragmentation, and ready-source preempt is
secondary per H158. A promotable carrier now needs earlier CPU-ready semantic
sources or a logical source/tape merge that can make writer-active work
commit-ready during the active wait while preserving command-buffer,
render-pass, tile, and load/store shape.
