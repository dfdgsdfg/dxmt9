---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: runtime
order: 180
title: EncodeSession Present-Boundary Disabled Scout
date: 2026-06-28
type: no-gputrace
status: diagnostic-safe-runtime-rejected
source: experiments/output/app-d3d9-3dmark05-encode-session-boundary-disabled-h180-scout-20260628/result.json, experiments/output/app-d3d9-3dmark05-encode-session-boundary-disabled-h180-scout-20260628/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-encode-session-boundary-disabled-h180-scout-20260628/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-boundary-disabled-h180-scout-20260628/actual.png
related: docs/perfomance/present-pacing/index.md, docs/perfomance/present-pacing/present-pacing-encode-session-batch-run-source-boundary.179.md, specs/backend/spec.md, specs/backend/requirements.md
---

# Present-Pacing H180 - EncodeSession Present-Boundary Disabled Scout

## Question

If PE `Present()` is allowed to return immediately, does the already-built
open-CB EncodeSession path create useful N+1 offscreen work during the previous
present-completion wait?

## Run

```text
DXMT9_DISABLE_PRESENT_BOUNDARY=1 \
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix encode-session-boundary-disabled-h180-scout-20260628 \
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

The run is a valid timeout-finalized scout:

- `status=pass`, `returncode=143`
- `present_encoded=1,080`
- `gpu_command_buffer_errors=0`
- `sampled_avg_fps=10.079`
- `actual.png` is visible GT1 output: `60,227` unique RGB colors and
  `32.09%` non-black pixels at RGB `>=16`.

Present-boundary wait is removed, but P4 remains closed:

- `submit_present_boundary_cpu_ms=0.000`
- `completion_wait_with_enqueue_ms=0.000`
- `completion_wait_enqueues_during_wait=0`
- `completion_wait_command_buffer_commit=0`
- `completion_wait_commit_chunk_entries=946`
- `completion_wait_commit_chunk_replay_starts=947`
- `completion_wait_commit_chunk_replay_ends=2`

The Metal shape stays close to the H179 baseline:

- `command_buffers_per_present=4.005`
- `sub_command_buffers_per_present=2.988`
- `render_pass_begin_per_present=11.579`
- `completion_wait_ms_per_present=25.644`

## Interpretation

Disabling the explicit present boundary is not the EncodeSession carrier by
itself. PE `Present()` returns quickly and sampled FPS rises versus H179, but
the queue still does not commit useful command buffers during the active wait.
The source tape is dominated by draw-continuation metadata:
`chunk_publish_reason_draw_continuation=32,381`,
`open_cb_tail_present_selector_ordinary_prefix_sources=32,381`,
and `encode_session_carry_active_entry_lost_active_before_first_draw=0`.

This separates frame-pacing policy from the P4 carrier: the missing piece is
not only "let `Present()` return"; offscreen work must become semantic
CPU-ready work that can be released or appended to an EncodeSession inside the
wait window without creating extra Metal locality cost.
