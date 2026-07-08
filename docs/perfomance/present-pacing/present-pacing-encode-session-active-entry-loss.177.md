---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: runtime
order: 177
title: EncodeSession Active-Entry Loss Reasons
date: 2026-06-28
type: no-gputrace
status: diagnostic-safe-runtime-rejected
source: experiments/output/app-d3d9-3dmark05-encode-session-active-entry-loss-reason-r2-20260628/result.json, experiments/output/app-d3d9-3dmark05-encode-session-active-entry-loss-reason-r2-20260628/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-encode-session-active-entry-loss-reason-r2-20260628/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-active-entry-loss-reason-r2-20260628/actual.png
related: docs/perfomance/present-pacing/index.md, docs/perfomance/present-pacing/present-pacing-encode-session-source-class-counters.176.md, specs/backend/spec.md, specs/backend/requirements.md
---

# Present-Pacing H177 - EncodeSession Active-Entry Loss Reasons

## Question

H176 showed that GT1 source/session attachment is mostly semantic and
wait-inactive. H177 asks the narrower pass-streaming question: when
`encodeChunk()` enters a new selected source with an active carried render
encoder, does the first draw continue that encoder, or is the active encoder
closed before the first draw?

This matters because R-BACK-2.43 only preserves locality when compatible draw
work crosses the source boundary without a semantic Metal encoder boundary.
`Clear` and final `Present` are semantic boundaries unless the clear can be
folded into a pass-open load action; they cannot be treated as ordinary
draw-to-draw continuation points.

## Run

```text
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix encode-session-active-entry-loss-reason-r2-20260628 \
  --no-gputrace \
  --no-encoder-breakdown \
  --frame-sampling \
  --pe-recorder-stats \
  --open-cb-preencode-tail-present \
  --open-cb-carry-render-session \
  --open-cb-semantic-boundary-publish
```

The code under test adds frame-sampled and cumulative counters for active-entry
first-draw decisions and for the reason an active render encoder is closed
before that first draw.

## Verdict

Diagnostic safe, runtime promotion rejected.

The rerun is correctness-safe:

- `status=pass`, `failures=[]`, `capture_error=null`
- `returncode=143`, `timed_out=true`, which is the wrapper timeout path
- `present_encoded=1260`
- `actual.png` is non-black (`mean_luma=70.072`, `variance=5263.079`)
- `gpu_command_buffer_errors=0`

The command-buffer shape is baseline-like but not better:

- cumulative `command_buffers_per_present=4.228`
- cumulative `sub_command_buffers_per_present=2.998`
- cumulative `render_pass_begin_per_present=11.470`
- tail600 FPS avg/p50/p95 is `12.292 / 12.730 / 17.601`
- tail600 frame-local `command_buffers_per_present=4.223`
- tail600 frame-local `sub_command_buffers_per_present=3.000`
- tail600 frame-local `render_pass_begin_per_present=12.638`

The active render encoder does reach source entry, but never survives to a
first-draw continuation:

- `encode_session_carry_source_entries=5903`
- `encode_session_carry_source_entry_active_render=4350`
- `encode_session_carry_first_draw_continue_active=0`
- `encode_session_carry_first_draw_begin_pass=4636`
- `encode_session_carry_active_entry_first_draw_continue_active=0`
- `encode_session_carry_active_entry_first_draw_begin_pass=3091`
- `encode_session_carry_active_entry_lost_active_before_first_draw=4350`

Every active-entry loss is explained by semantic `Clear` or `Present`, not by
source finalization, render-target mismatch, hazards, or copy/readback/colorfill
commands:

- `encode_session_carry_active_entry_lost_active_before_first_draw_clear=3097`
- `encode_session_carry_active_entry_lost_active_before_first_draw_present=1253`
- `encode_session_carry_active_entry_lost_active_before_first_draw_final=0`
- `encode_session_carry_active_entry_lost_active_before_first_draw_rt=0`
- `encode_session_carry_active_entry_lost_active_before_first_draw_hazard=0`
- `encode_session_carry_active_entry_lost_active_before_first_draw_surface_copy=0`
- `encode_session_carry_active_entry_lost_active_before_first_draw_stretch_rect=0`
- `encode_session_carry_active_entry_lost_active_before_first_draw_readback=0`
- `encode_session_carry_active_entry_lost_active_before_first_draw_color_fill=0`
- `encode_session_carry_active_entry_lost_active_before_first_draw_present_acquire=0`

The tail600 frame-local counters show the same shape:

- active source entries: `2635`
- active-entry first-draw continuations: `0`
- active-entry losses before first draw: `2635`
- clear-caused losses: `2035`
- present-caused losses: `600`

## Interpretation

H177 rejects the idea that this GT1 scout is currently blocked by a broken
draw-to-draw source-boundary continuation. The selected sources do carry an
active render encoder into the next source entry, but the next semantic command
closes it before the first draw every time. In this run, the sampled source
tape does not expose a compatible draw-to-draw continuation for
open-render-encoder pass streaming to exploit.

The remaining implementation owner is below the current selected-source shape:
either produce deterministic native/fake-backend coverage that proves a
compatible draw-to-draw source boundary is streamed correctly, or change
CPU-ready/source selection so it creates pass-compatible draw-continuation
sources instead of mostly clear/present semantic boundaries. Crossing `Clear`
or final `Present` with the same Metal render encoder is not a valid shortcut
under the current R-BACK-2.43/R-BACK-2.47 contract.
