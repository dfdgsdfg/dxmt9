---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: runtime
order: 182
title: EncodeSession Semantic Plus Draw-Continuation Overlap Scout
date: 2026-06-28
type: no-gputrace
status: diagnostic-safe-runtime-rejected
source: experiments/output/app-d3d9-3dmark05-encode-session-semantic-attachment-waitstart-h182-20260628/result.json, experiments/output/app-d3d9-3dmark05-encode-session-semantic-attachment-waitstart-h182-20260628/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-encode-session-semantic-attachment-waitstart-h182-20260628/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-semantic-attachment-waitstart-h182-20260628/actual.png
related: docs/perfomance/present-pacing.md, docs/perfomance/present-pacing/present-pacing-encode-session-boundary-disabled.180.md, docs/perfomance/present-pacing/present-pacing-encode-session-semantic-attachment-only.183.md
---

# Present-Pacing H182 - EncodeSession Semantic Plus Draw-Continuation Overlap Scout

## Question

Does the strongest existing source-publication combination open P4 when the
present boundary is disabled: semantic boundary publish, attachment-boundary
publish, wait-start publish, active-wait append, and same-key draw-continuation
metadata?

## Run

```text
DXMT9_DISABLE_PRESENT_BOUNDARY=1 \
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix encode-session-semantic-attachment-waitstart-h182-20260628 \
  --no-gputrace \
  --no-encoder-breakdown \
  --frame-sampling \
  --pe-recorder-stats \
  --open-cb-preencode-tail-present \
  --open-cb-carry-render-session \
  --open-cb-semantic-boundary-publish \
  --open-cb-draw-attachment-boundary-publish \
  --open-cb-draw-continuation-boundary-publish \
  --open-cb-wait-start-cpu-ready-publish \
  --open-cb-active-wait-cpu-ready-append \
  --keep-frontmost
```

## Verdict

Diagnostic safe, runtime promotion rejected.

The run is visible and error-free:

- `status=pass`, `returncode=143`
- `present_encoded=1,020`
- `gpu_command_buffer_errors=0`
- `sampled_avg_fps=9.506`
- `actual.png` has `56,340` unique RGB colors and `30.00%` non-black pixels at
  RGB `>=16`.

This is the first scout in this chain with nonzero same-window work, but the
effect is too small:

- `completion_wait_with_enqueue_ms=18.233`
- `completion_wait_without_enqueue_ms=29,892.899`
- `completion_wait_enqueues_during_wait=2`
- `completion_wait_command_buffer_commit=2`
- `completion_wait_overlap_share=0.061%`

The cost is a worse source/CB shape:

- `command_buffers_per_present=6.162`
- `sub_command_buffers_per_present=3.786`
- `render_pass_begin_per_present=11.500`
- `chunk_publish_reason_draw_continuation=70,965`
- `open_cb_tail_present_draw_attachment_boundary_published=197`
- `open_cb_tail_present_draw_attachment_boundary_blocked_headroom=7,735`
- `encode_session_carry_active_entry_lost_active_before_first_draw=1,389`

## Interpretation

Combining same-key draw-continuation publication with semantic attachment
publication opens only a token P4 window while over-fragmenting the logical
source stream. Same-key draw-continuation remains useful as an R-BACK-2.43
mechanism proof, but it is the wrong production carrier for average FPS: it
creates tens of thousands of metadata boundaries, consumes headroom, increases
command-buffer pressure, and introduces active-entry loss without enough overlap
to move the frame limit.

The next scout should remove same-key draw-continuation and keep only semantic
attachment boundaries.
