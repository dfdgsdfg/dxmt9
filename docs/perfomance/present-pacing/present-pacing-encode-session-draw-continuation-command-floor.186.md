---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: runtime
order: 186
title: EncodeSession Draw-Continuation Command Floor
date: 2026-06-28
type: no-gputrace
status: diagnostic-safe-runtime-rejected
source: experiments/output/app-d3d9-3dmark05-encode-session-continuation-limit16-20260628/result.json, experiments/output/app-d3d9-3dmark05-encode-session-continuation-limit16-20260628/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-encode-session-continuation-limit16-20260628/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-continuation-limit16-20260628/actual.png
related: docs/perfomance/present-pacing.md, docs/perfomance/present-pacing/present-pacing-encode-session-draw-continuation-source.178.md, docs/perfomance/present-pacing/present-pacing-encode-session-selected-prefix-retain.185.md
---

# Present-Pacing H186 - EncodeSession Draw-Continuation Command Floor

## Question

H178 proved that same-attachment draw-continuation sources can preserve the
active Metal render encoder across source boundaries, but it published a source
at nearly every compatible draw edge. Can a positive
`DXMT9_OPEN_CB_DRAW_CONTINUATION_COMMAND_LIMIT` make that source tape coarse
enough to combine the H178 mechanism proof with the H183/H185 P4 overlap
signal?

## Implementation

`DXMT9_OPEN_CB_DRAW_CONTINUATION_COMMAND_LIMIT=N` is a default-unset numeric
floor for `DXMT9_OPEN_CB_DRAW_CONTINUATION_BOUNDARY_PUBLISH=1`. When set to a
positive value, same-attachment draw-continuation publication waits until the
current non-present writing slot has at least `N` commands. Unset or `0`
preserves the original diagnostic behavior.

Focused coverage was added to the backend batch contract, and the 3DMark05
wrapper forwards `--open-cb-draw-continuation-command-limit N`.

## Run

```text
DXMT9_DISABLE_PRESENT_BOUNDARY=1 \
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix encode-session-continuation-limit16-20260628 \
  --no-gputrace \
  --no-encoder-breakdown \
  --frame-sampling \
  --pe-recorder-stats \
  --keep-frontmost \
  --wait-unlocked-sec 60 \
  --open-cb-preencode-tail-present \
  --open-cb-carry-render-session \
  --open-cb-semantic-boundary-publish \
  --open-cb-draw-attachment-boundary-publish \
  --open-cb-draw-continuation-boundary-publish \
  --open-cb-draw-continuation-command-limit 16 \
  --open-cb-wait-start-cpu-ready-publish \
  --open-cb-active-wait-cpu-ready-append
```

## Verdict

Diagnostic safe, runtime promotion rejected.

The run is a valid no-gputrace smoke:

- `status=pass`, `returncode=143`, `timed_out=true`
- `present_encoded=1,200`
- `gpu_command_buffer_errors=0`
- `actual.png` is a normal non-black GT1 firefight frame (`mean_luma=69.851`,
  `variance=5238.691`)
- `sampled_frames=1,205`, `sampled_avg_fps=10.999`

The command floor is active:

- `chunk_publish_reason_draw_continuation=16,610`
- `chunk_publish_commands_draw_continuation=267,511`
- `completion_no_enqueue_first_publish_slot_commands_p50=16`
- `completion_no_enqueue_first_publish_slot_commands_p95=16`

It creates real P4 overlap, but weaker than H183/H185:

- `completion_wait_with_enqueue_ms=17,702.498`
- `completion_wait_without_enqueue_ms=21,706.972`
- overlap share is `44.920%`
- `completion_wait_command_buffer_commit=702`
- `completion_wait_enqueues_during_wait=698`

The source tape and session carry are heavily exercised:

- `chunk_publish_reason_semantic_boundary=15,012`
- `chunk_publish_reason_present_split_before=834`
- `encode_session_carry_source_entries=33,656`
- `encode_session_carry_source_entry_active_render=31,743`
- `encode_session_carry_active_entry_first_draw_continue_active=20,256`
- `encode_session_carry_active_entry_lost_active_before_first_draw=3,979`

The locality and FPS gates still reject promotion:

- `command_buffers_per_present=2.481`
- `sub_command_buffers_per_present=0.890`
- `render_pass_begin_per_present=11.981`
- `render_pass_tile_preservation=123.236 MiB/present`
- `gpu_command_buffer_time=22.878ms/present`

Compared with H185 r2, the floor lowers render-pass/tile pressure
(`12.640 -> 11.981` passes/present, `128.880 -> 123.236 MiB/present`) and GPU
CB time (`36.105 -> 22.878ms/present`), but it gives up much of the H185 P4
overlap (`86% -> 45%`) and sampled FPS falls (`11.618 -> 10.999`). Compared
with H220 baseline, it is still well behind FPS (`16.267 -> 10.999`) and GPU
CB time (`3.287 -> 22.878ms/present`).

## Interpretation

A command floor makes same-key continuation less pathological than the H178
per-draw source tape, but it is still not the production carrier. It mixes two
rejected shapes: draw-continuation source churn remains large, while the strong
semantic/attachment P4 overlap weakens.

Keep the floor as a diagnostic knob for source-tape shaping. The next
implementation should not sweep command floors as a main path. The remaining
owner is a coarser CPU-ready/source-tape staging or producer/replay cadence
change that keeps source boundaries metadata-only to `EncodeSession`, preserves
baseline-like GPU CB time and render-pass locality, and does not require
`DXMT9_DISABLE_PRESENT_BOUNDARY=1` for useful overlap.
