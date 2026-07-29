---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: runtime
order: 179
title: EncodeSession Batch-And-Run Source Boundary
date: 2026-06-28
type: no-gputrace
status: diagnostic-safe-runtime-rejected
outdated: knob-removed
source: experiments/output/app-d3d9-3dmark05-encode-session-batch-run-source-boundary-r1-20260628/result.json, experiments/output/app-d3d9-3dmark05-encode-session-batch-run-source-boundary-r1-20260628/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-encode-session-batch-run-source-boundary-r1-20260628/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-batch-run-source-boundary-r1-20260628/actual.png
related: docs/perfomance/present-pacing/index.md, docs/perfomance/present-pacing/present-pacing-encode-session-draw-continuation-source.178.md, specs/backend/spec.md, specs/backend/requirements.md
---

# Present-Pacing H179 - EncodeSession Batch-And-Run Source Boundary

> **Outdated — the knob or code path this experiment measured no longer exists in `src/`.** It cannot be re-run. Kept as history; do not cite it as current evidence.

## Question

H178 proved that `DXMT9_OPEN_CB_DRAW_CONTINUATION_BOUNDARY_PUBLISH=1` can create
same-attachment draw-continuation sources and that `EncodeSession` can preserve
the active render encoder across those source boundaries.

H179 fixes one policy mismatch found after that run: the leading batch segment
inside `submitDrawRunBatchAndRunImpl()` still used only the attachment-change
boundary helper, while standalone draw submission and `submitDrawRunBatchImpl()`
used the full draw-source boundary helper. The change makes batch-and-run use
the same source-boundary policy, so same-key draw-continuation publication is
not silently skipped on that path.

## Run

```text
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix encode-session-batch-run-source-boundary-r1-20260628 \
  --no-gputrace \
  --no-encoder-breakdown \
  --frame-sampling \
  --pe-recorder-stats \
  --open-cb-preencode-tail-present \
  --open-cb-carry-render-session \
  --open-cb-draw-continuation-boundary-publish \
  --keep-frontmost
```

## Verdict

Diagnostic safe, runtime promotion rejected.

The run is a valid supervised smoke:

- `status=pass`, `returncode=0`, `timed_out=false`
- `capture_error=null`
- `present_encoded=877`
- `gpu_command_buffer_errors=0`
- `actual.png` is a visible dark GT1 frame, not a black-screen failure:
  `53,275` unique RGB colors, `63.60%` RGB `<3`, `69.49%` RGB `<16`, and
  `30.51%` non-black pixels at RGB `>=16`.

The policy change does not materially change the H178 mechanism numerator:

- `chunk_publish_reason_draw_continuation=26,291`
- `chunk_publish_commands_draw_continuation=27,886`
- `open_cb_tail_present_selector_ordinary_prefix_sources=26,291`
- `encode_session_carry_source_entry_active_render=26,288`
- `encode_session_carry_active_entry_first_draw_continue_active=26,288`
- `encode_session_carry_active_entry_lost_active_before_first_draw=0`

The Metal shape remains baseline-like:

- cumulative `command_buffers_per_present=4.007`
- cumulative `sub_command_buffers_per_present=2.999`
- cumulative `render_pass_begin_per_present=11.725`

The P4 gate remains closed:

- `completion_wait_enqueues_during_wait=0`
- `completion_wait_command_buffer_commit=0`
- `completion_wait_with_enqueue_ms=0.000`
- `completion_wait_without_enqueue_ms=22,859.337`
- `completion_wait_without_enqueue_ms_per_present=26.065`

Frame sampling is also unchanged in kind:

- `sampled_frames=876`
- `sampled_avg_fps=7.992`
- tail600 wall-FPS/p50/p95 is `8.055 / 8.703 / 11.026`

## Interpretation

H179 is a coverage and consistency fix, not a performance promotion. It makes
the batch-and-run producer path obey the same draw-source boundary policy as
the other draw submission paths, but GT1's observed H178 numerator was already
dominated by paths covered before the fix. The run therefore repeats the same
R-BACK-2.43 mechanism proof and the same R-BACK-2.50 rejection: the carried
session can continue across compatible logical sources, but no useful Metal
commit happens during the active completion wait.

Keep `DXMT9_OPEN_CB_DRAW_CONTINUATION_BOUNDARY_PUBLISH` diagnostic-only. The
remaining owner is still coarser CPU-ready/source-tape staging, producer/replay
cadence movement, or another design that makes pass-compatible work attach to
an open session before the wait window without creating more CB/pass/tile
load-store work.
