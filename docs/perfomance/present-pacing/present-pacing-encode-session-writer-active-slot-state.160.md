---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: runtime
order: 160
title: EncodeSession Writer-Active Slot State
date: 2026-06-21
type: no-gputrace
status: diagnostic-observed-runtime-rejected
source: experiments/output/app-d3d9-3dmark05-encode-session-writer-active-slot-state-r1-20260621/result.json, experiments/output/app-d3d9-3dmark05-encode-session-writer-active-slot-state-r1-20260621/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-encode-session-writer-active-slot-state-r1-20260621/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-writer-active-slot-state-r1-20260621/actual.png
related: docs/perfomance/present-pacing/index.md, docs/perfomance/present-pacing/present-pacing-encode-session-no-wait-writer-split.159.md, specs/backend/design.md, specs/backend/requirements.md
---

# Present-Pacing H160 - EncodeSession Writer-Active Slot State

## Question

H159 showed that the dominant empty-ready/no-wait semantic-release misses are
writer-active, not inactive-writer drain misses. When those misses happen, is
the writer still waiting for first useful work, or does the writing slot already
contain non-present work that has not become a CPU-ready source?

## Run

```text
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix encode-session-writer-active-slot-state-r1-20260621 \
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
- output image shows normal GT1 battlefield content (`mean_luma=72.313`,
  `variance=5254.395`)
- no giant vertical black artifact
- `gpu_command_buffer_errors=0`
- `completion_dequeue_status_error=0`
- `draw_skipped_no_pipeline=0`
- `encode_session_carry_forced_finalize_initializer_waits=0`

The new slot-state split is decisive:

- `open_cb_tail_present_semantic_release_blocked_no_completion_wait=1365`
- `open_cb_tail_present_semantic_release_blocked_no_completion_wait_writer_active=1365`
- `open_cb_tail_present_semantic_release_blocked_no_completion_wait_writer_active_slot_empty=0`
- `open_cb_tail_present_semantic_release_blocked_no_completion_wait_writer_active_slot_nonpresent=1365`
- `open_cb_tail_present_semantic_release_blocked_no_completion_wait_writer_active_slot_present=0`
- `open_cb_tail_present_semantic_release_blocked_no_completion_wait_writer_inactive=0`
- `open_cb_tail_present_semantic_release_blocked_ready_source_no_completion_wait=191`
- `open_cb_tail_present_semantic_release_submitted=162`
- pending timeout, abandon, retain, encode-null, and merge-failed counters all
  remain `0`
- `chunk_publish_reason_present_split_before=0`
- `chunk_publish_reason_semantic_boundary=1724`
- `chunk_subcb_count_max=4`

Compared with H159, this instrumentation run is not an FPS or locality
promotion:

- command buffers: `4.141 -> 4.172/present`
- sub-command buffers: `2.998 -> 2.998/present`
- render-pass begins: `10.365 -> 10.358/present`
- GPU command-buffer time: `2.408 -> 2.605ms/present`
- total completion wait: `20.082 -> 20.405ms/present`
- completion wait with enqueue: `4.191 -> 4.972ms/present`
- completion wait without enqueue: `15.891 -> 15.433ms/present`
- completion-wait command-buffer commits: `131 -> 160`
- enqueues during completion wait: `131 -> 159`

## Interpretation

This rejects the "writer-active but empty" version of the CPU-ready gap. The
writer has already accumulated non-present commands when every dominant
empty-ready/no-wait semantic miss is observed. The problem is therefore not
waiting for first-record arrival and not inactive-writer drain. It is that
non-present work remains inside the active writing slot and has not crossed a
locality-safe CPU-ready/session boundary before the completion-wait window
closes.

The next production-shaped carrier should focus on making that already-existing
non-present writing-slot work available to the `EncodeSession` path without
turning the source boundary into an extra Metal command-buffer or render-pass
boundary. That points back to logical source/tape merge or a semantic
CPU-ready cutoff that preserves open-render-encoder pass streaming, not another
ready-drain or deterministic release policy.
