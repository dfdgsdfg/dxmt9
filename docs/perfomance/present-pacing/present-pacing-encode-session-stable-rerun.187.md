---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: runtime
order: 187
title: EncodeSession Stable Rerun
date: 2026-06-28
type: no-gputrace
status: diagnostic-safe-runtime-rejected
source: experiments/output/app-d3d9-3dmark05-encode-session-stable-rerun-20260628b/result.json, experiments/output/app-d3d9-3dmark05-encode-session-stable-rerun-20260628b/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-encode-session-stable-rerun-20260628b/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-stable-rerun-20260628b/actual.png
related: docs/perfomance/present-pacing/index.md, specs/backend/spec.md, specs/backend/requirements.md
---

# Present-Pacing H187 - EncodeSession Stable Rerun

## Question

After the producer sequence-wait release fix, does the stable open-CB
`EncodeSession` flag set become a promotable pass-streaming shape without
disabling the present boundary or enabling same-key draw-continuation source
flooding?

## Run

```text
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix encode-session-stable-rerun-20260628b \
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
  --open-cb-wait-start-cpu-ready-publish \
  --open-cb-active-wait-cpu-ready-append
```

## Verdict

Diagnostic safe, runtime promotion rejected.

The rerun is a clean no-gputrace smoke:

- `status=pass`, `returncode=0`, `timed_out=false`
- `present_encoded=861`
- `gpu_command_buffer_errors=0`
- `actual.png` is a visible non-black GT1 frame (`mean_luma=67.518`,
  `variance=5063.399`)
- frame sampling reports `860` frames over `109486.880ms`, or
  `7.855` sampled FPS

The command-buffer carrier is healthy:

- `command_buffers=869`, or `1.009` per present
- `sub_command_buffers=2`, or `0.002` per present
- `open_cb_tail_present_tail_appended=861`
- `open_cb_tail_present_tail_submitted=861`
- pending abandon / nonappendable / merge failures are all `0`

But the run is not true open-render-encoder pass streaming:

- `encode_session_carry_deferred_chunks=10096`
- `encode_session_carry_deferred_active_render_chunks=10089`
- `encode_session_carry_source_entries=10957`
- `encode_session_carry_source_entry_active_render=10086`
- `encode_session_carry_first_draw_continue_active=0`
- `encode_session_carry_active_entry_first_draw_continue_active=0`
- `encode_session_carry_first_draw_begin_pass=3352`
- `encode_session_carry_first_draw_split_rt=6737`
- `encode_session_carry_active_entry_lost_active_before_first_draw=3349`
- active-entry loss is still semantic: `clear=2495`, `present=854`

P4 also stays closed:

- `present_boundary_wait_ms=32966.656` across `860` waits
- `completion_wait_with_enqueue_ms=41.303`
- `completion_wait_without_enqueue_ms=28403.505`
- overlap share is only `0.145%`
- `completion_wait_enqueues_during_wait=2`
- `completion_wait_command_buffer_commit=2`
- wait-start CpuReady publication has no usable source
  (`candidates=1`, `published=0`)

Locality is mixed rather than promotable:

- `render_pass_begin=10089`, or `11.718` passes per present
- `render_pass_tile_preservation=115.558 MiB/present`
- `gpu_command_buffer_time=32.471ms/present`
- `chunk_publish_reason_draw_continuation=0`
- `chunk_publish_reason_semantic_boundary=9235`
- draw attachment publication sees many same-key opportunities
  (`same=269600`) but publishes only changed attachment sources (`6739`)

## Interpretation

This separates two concepts that previous experiments could conflate:

1. The open-CB tail-present carrier can collapse Metal command buffers to about
   one per present.
2. That does not imply R-BACK-2.43 open-render-encoder pass streaming.

In the stable flag set, active render state reaches source entry, but the first
draw after each source boundary either begins a new pass or splits on render
target change. When active state is lost before the next draw, the reason is a
real semantic boundary (`Clear` or final `Present`). Therefore the current
stable path is a good safety/current-state sample, not a promotion result.

The next implementation owner remains the same as H186: create a coarser
source-tape or producer/pacing carrier that attaches compatible draw work to an
already open `EncodeSession` before the wait window, without requiring
`DXMT9_DISABLE_PRESENT_BOUNDARY=1`, draw-continuation flooding, or extra
Metal render-pass load/store boundaries.
