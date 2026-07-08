---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: runtime
order: 178
title: EncodeSession Draw-Continuation Source Publish
date: 2026-06-28
type: no-gputrace
status: diagnostic-safe-runtime-rejected
source: experiments/output/app-d3d9-3dmark05-encode-session-draw-continuation-boundary-r1-20260628/result.json, experiments/output/app-d3d9-3dmark05-encode-session-draw-continuation-boundary-r1-20260628/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-encode-session-draw-continuation-boundary-r1-20260628/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-draw-continuation-boundary-r1-20260628/actual.png
related: docs/perfomance/present-pacing/index.md, docs/perfomance/present-pacing/present-pacing-encode-session-active-entry-loss.177.md, specs/backend/design.md, specs/backend/requirements.md
---

# Present-Pacing H178 - EncodeSession Draw-Continuation Source Publish

## Question

H177 showed that the existing semantic selected-source tape was not a useful
draw-to-draw pass-streaming sample: active render state entered the next source,
but `Clear` or final `Present` closed the render encoder before the first draw.

H178 adds a narrower default-off producer-side probe:
`DXMT9_OPEN_CB_DRAW_CONTINUATION_BOUNDARY_PUBLISH=1`. Before appending a draw
whose encoder attachment key matches the current non-present draw tail, the
queue publishes that slot as `chunk_publish_reason_draw_continuation`. This is
a metadata-only source boundary for `EncodeSession`; it must not trigger
semantic-release policy or force the active Metal render encoder to close.

## Run

```text
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix encode-session-draw-continuation-boundary-r1-20260628 \
  --no-gputrace \
  --no-encoder-breakdown \
  --frame-sampling \
  --pe-recorder-stats \
  --open-cb-preencode-tail-present \
  --open-cb-carry-render-session \
  --open-cb-draw-continuation-boundary-publish \
  --keep-frontmost
```

The run was a retry after a manually closed app sample. The manually closed
sample is not evidence; this leaf uses only the supervised pass run above.

## Verdict

Diagnostic safe, runtime promotion rejected.

The run is correctness-safe as a gross smoke:

- `status=pass`, `returncode=0`, `timed_out=false`
- `capture_error=null`
- `present_encoded=877`
- `gpu_command_buffer_errors=0`
- `actual.png` is not a black screen: it is a visible GT1 frame with `64,575`
  unique RGB colors. The scene is dark, so black/near-black pixels are high
  (`60.37%` at RGB `<3`, `65.76%` at RGB `<16`), but non-black pixels still
  cover `34.24%` of the image.

The mechanism is active and proves that the carried render encoder can continue
across a compatible draw-to-draw source boundary:

- `chunk_publish_reason_draw_continuation=26,291`
- `chunk_publish_commands_draw_continuation=27,882`
- `open_cb_tail_present_selector_ordinary_prefix_sources=26,291`
- `encode_session_carry_source_entry_active_render=26,288`
- `encode_session_carry_active_entry_first_draw_continue_active=26,288`
- `encode_session_carry_active_entry_first_draw_begin_pass=0`
- `encode_session_carry_active_entry_lost_active_before_first_draw=0`

The Metal shape stays baseline-like rather than better:

- cumulative `command_buffers_per_present=4.007`
- cumulative `sub_command_buffers_per_present=2.999`
- cumulative `render_pass_begin_per_present=11.735`
- tail600 `command_buffers_per_present=4.000`
- tail600 `sub_command_buffers_per_present=3.000`
- tail600 `render_pass_begin_per_present=12.273`

The P4 overlap wall does not move:

- `completion_wait_enqueues_during_wait=0`
- `completion_wait_with_enqueue_ms=0.000`
- `completion_wait_without_enqueue_ms=22,764.321`
- `completion_wait_command_buffer_commit=0`
- `completion_wait_encode_dequeue=1`
- `completion_wait_stage_encode_dequeue_to_command_buffer_commit=0`

Frame sampling is not a promotion signal:

- `sampled_frames=876`
- `sampled_avg_fps=7.983`
- tail600 average/p50/p95 FPS is `8.052 / 8.685 / 10.967`
- tail100 average/p50/p95 FPS is `8.928 / 9.893 / 11.003`

## Interpretation

H178 accepts the narrow R-BACK-2.43 mechanism: if the source selector creates a
same-attachment draw-to-draw boundary, `EncodeSession` can preserve the active
render encoder and continue the first draw across that source boundary. That
closes the H177 uncertainty about whether continuation itself was broken.

This does not promote the producer-side same-key publication policy. The probe
creates `26k+` logical source publications, but it does not create any enqueue
or Metal commit during the active completion wait, and it does not lower the
CB/pass shape or FPS. Keep `DXMT9_OPEN_CB_DRAW_CONTINUATION_BOUNDARY_PUBLISH`
default-off as a diagnostic/source-selection tool. The next production owner is
still a coarser CPU-ready/source-tape staging design or replay/producer cadence
change that attaches pass-compatible work before the wait opens without
turning every compatible draw edge into an observable scheduling unit.
