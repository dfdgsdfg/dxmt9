---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: runtime
order: 183
title: EncodeSession Semantic Attachment-Only Overlap Scout
date: 2026-06-28
type: no-gputrace
status: diagnostic-promising-runtime-unpromoted
source: experiments/output/app-d3d9-3dmark05-encode-session-semantic-attachment-only-h183-20260628/result.json, experiments/output/app-d3d9-3dmark05-encode-session-semantic-attachment-only-h183-20260628/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-encode-session-semantic-attachment-only-h183-20260628/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-semantic-attachment-only-h183-20260628/actual.png
related: docs/perfomance/present-pacing/index.md, docs/perfomance/present-pacing/present-pacing-encode-session-semantic-continuation-overlap.182.md, docs/perfomance/present-pacing/present-pacing-encode-session-semantic-attachment-only-rerun.184.md, specs/backend/design.md, specs/backend/requirements.md
---

# Present-Pacing H183 - EncodeSession Semantic Attachment-Only Overlap Scout

## Question

If same-key draw-continuation publication is removed, can semantic/attachment
CPU-ready sources plus open-CB EncodeSession create meaningful P4 overlap while
keeping Metal command-buffer locality acceptable?

## Run

```text
DXMT9_DISABLE_PRESENT_BOUNDARY=1 \
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix encode-session-semantic-attachment-only-h183-20260628 \
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

## Verdict

Promising diagnostic, runtime promotion still unproven.

The run is valid, visible, and error-free:

- `status=pass`, `returncode=143`
- `present_encoded=1,260`
- `gpu_command_buffer_errors=0`
- `sampled_avg_fps=11.562`
- `actual.png` has `72,630` unique RGB colors and `50.61%` non-black pixels at
  RGB `>=16`.

Unlike H180-H182, P4 opens materially:

- `completion_wait_with_enqueue_ms=36,303.685`
- `completion_wait_without_enqueue_ms=6,070.582`
- `completion_wait_overlap_share=85.674%`
- `completion_wait_enqueues_during_wait=1,640`
- `completion_wait_command_buffer_commit=1,642`
- `completion_wait_commit_chunk_entries=6,215`
- `completion_wait_commit_chunk_replay_ends=6,004`

The open-CB source path is now the active numerator:

- `chunk_publish_reason_draw_continuation=0`
- `chunk_publish_reason_present_split_before=1,256`
- `open_cb_tail_present_wait_start_published=11,660`
- `open_cb_tail_present_draw_attachment_boundary_published=9,673`
- `open_cb_tail_present_selector_semantic_prefix=18,678`
- `open_cb_tail_present_selector_semantic_prefix_wait_active=13,499`
- `open_cb_tail_present_completion_wait_pending_releasable=15,048`
- `open_cb_tail_present_completion_wait_pending_release_used=9,564`

The command-buffer count improves, but render-pass locality is not yet a pass:

- `command_buffers_per_present=2.380`
- `sub_command_buffers_per_present=0.074`
- `render_pass_begin_per_present=12.661`
- H180 comparison: `4.005` CB/present, `2.988` sub-CB/present, `11.579`
  passes/present
- `encode_session_carry_active_entry_lost_active_before_first_draw=4,499`,
  mostly `Present` (`1,249` counted in the reason split)

## Interpretation

H183 is the first strong positive P4 signal for the current EncodeSession /
open-render-encoder direction. The decisive difference from H182 is removing
same-key draw-continuation publication: semantic attachment-only sources give
the encoder enough CPU-ready work during completion waits without consuming the
queue with tens of thousands of continuation boundaries.

This is not yet a production promotion. The run requires
`DXMT9_DISABLE_PRESENT_BOUNDARY=1`, has only a run-level screenshot rather than
a same-frame `v0.0.3` visual proof, raises render-pass begins per present, and
still reports active-entry loss before the first draw. The next implementation
target is therefore not more same-key source splitting. It is to turn the H183
shape into a controlled policy: semantic/attachment source publication, bounded
wait-start CPU-ready release, no draw-continuation source flood, and explicit
locality/visual gates before default enablement.

H184 reruns the same flag set after discarding an early aborted launch
and reproduces the P4 signal (`85.495%` overlap share, `1,607` wait-time
command-buffer commits, sampled FPS `11.502`). That strengthens H183 as a
directional signal but does not change the promotion verdict.
