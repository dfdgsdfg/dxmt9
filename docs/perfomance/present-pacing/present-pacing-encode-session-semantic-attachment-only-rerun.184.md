---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: runtime
order: 184
title: EncodeSession Semantic Attachment-Only Rerun
date: 2026-06-28
type: no-gputrace
status: reproduction-runtime-unpromoted
source: experiments/output/app-d3d9-3dmark05-encode-session-semantic-attachment-only-h185-rerun-20260628/result.json, experiments/output/app-d3d9-3dmark05-encode-session-semantic-attachment-only-h185-rerun-20260628/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-encode-session-semantic-attachment-only-h185-rerun-20260628/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-semantic-attachment-only-h185-rerun-20260628/actual.png
related: docs/perfomance/present-pacing/index.md, docs/perfomance/present-pacing/present-pacing-encode-session-semantic-attachment-only.183.md
---

# Present-Pacing H184 - EncodeSession Semantic Attachment-Only Rerun

## Question

Does the H183 semantic attachment-only EncodeSession signal reproduce after an
early aborted launch is discarded?

## Run

```text
DXMT9_DISABLE_PRESENT_BOUNDARY=1 \
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix encode-session-semantic-attachment-only-h185-rerun-20260628 \
  --no-gputrace \
  --no-encoder-breakdown \
  --frame-sampling \
  --pe-recorder-stats \
  --open-cb-preencode-tail-present \
  --open-cb-carry-render-session \
  --open-cb-semantic-boundary-publish \
  --open-cb-draw-attachment-boundary-publish \
  --open-cb-wait-start-cpu-ready-publish \
  --open-cb-active-wait-cpu-ready-append \
  --keep-frontmost
```

The first retry used output suffix
`encode-session-semantic-attachment-only-h184-rerun-20260628`, but exited before
GT1 data collection (`process_exit=1`, no frame samples, no capture). Treat that
run as discarded launch hygiene, not performance evidence. The valid sample for
this leaf is the later `h185-rerun` output listed in `source`.

## Verdict

The valid rerun reproduces H183's P4 overlap signal, but it does not promote
the runtime policy.

The run is valid and error-free:

- `status=pass`, `returncode=143`, `timed_out=true`
- `present_encoded=1,200`
- `gpu_command_buffer_errors=0`
- `sampled_avg_fps=11.502`
- `actual.png` is non-black by the runner's image metrics (`mean_luma=61.662`,
  `variance=6913.955`)

The P4 overlap shape matches H183:

- `completion_wait_with_enqueue_ms=34,593.513`
- `completion_wait_without_enqueue_ms=5,869.001`
- `completion_wait_overlap_share=85.495%`
- `completion_wait_enqueues_during_wait=1,597`
- `completion_wait_command_buffer_commit=1,607`
- `completion_wait_commit_chunk_entries=5,803`
- `completion_wait_commit_chunk_replay_ends=5,600`

The source path remains the semantic/attachment numerator, without
draw-continuation flooding:

- `chunk_publish_reason_draw_continuation=0`
- `chunk_publish_reason_present_split_before=1,197`
- `open_cb_tail_present_wait_start_published=10,802`
- `open_cb_tail_present_draw_attachment_boundary_published=9,240`
- `open_cb_tail_present_completion_wait_pending_releasable=14,098`
- `open_cb_tail_present_completion_wait_pending_release_used=8,757`

The shape is still not a production gate pass:

- `command_buffers_per_present=2.404`
- `sub_command_buffers_per_present=0.058`
- `render_pass_begin_per_present=12.748`
- `encode_session_carry_active_entry_lost_active_before_first_draw=4,345`
- H183 comparison: `2.379` CB/present, `0.077` sub-CBs/present, `12.663`
  passes/present, active-entry loss `4,499`

## Interpretation

The rerun confirms that H183 was not a one-off: semantic/attachment CPU-ready
source publication plus bounded wait-time release can make work available while
the completion thread is waiting. The rejected H182 continuation flood remains
the wrong carrier; the useful numerator is the coarser semantic/attachment
source stream.

This still needs promotion work. The run depends on
`DXMT9_DISABLE_PRESENT_BOUNDARY=1`, has only output-frame visual smoke, and
keeps render-pass begins above the H180 boundary-disabled comparison. The next
implementation should make the H183/H184 shape explicit and bounded, then prove
visual correctness and locality before any default enablement.
