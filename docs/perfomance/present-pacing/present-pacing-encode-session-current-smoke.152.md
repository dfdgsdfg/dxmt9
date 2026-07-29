---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: runtime
order: 152
title: EncodeSession Fresh-Build Current Smoke
date: 2026-06-21
type: no-gputrace
status: mechanism-observed-runtime-rejected
outdated: knob-removed
source: experiments/output/app-d3d9-3dmark05-encode-session-current-smoke-r1-20260621/result.json, experiments/output/app-d3d9-3dmark05-encode-session-current-smoke-r1-20260621/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-encode-session-current-smoke-r1-20260621/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-current-smoke-r1-20260621/actual.png
related: docs/perfomance/present-pacing/index.md, docs/perfomance/present-pacing/present-pacing-encode-session-wait-stage-durations.151.md, specs/backend/spec.md, specs/backend/requirements.md
---

# Present-Pacing H152 - EncodeSession Fresh-Build Current Smoke

> **Outdated — the knob or code path this experiment measured no longer exists in `src/`.** It cannot be re-run. Kept as history; do not cite it as current evidence.

## Question

After rebuilding the native, unix-provider, and PE builtin staging outputs, does
the current opt-in `EncodeSession` / open-render-encoder path still run without
the reported D3D9 invalid-call, Metal command-buffer, or queue-completion error?

## Run

```text
DXMT9_OPEN_CB_SEMANTIC_BOUNDARY_PUBLISH=1 \
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix encode-session-current-smoke-r1-20260621 \
  --no-gputrace --no-encoder-breakdown --timeout 25 \
  --frame-sampling \
  --open-cb-preencode-tail-present \
  --open-cb-carry-render-session \
  --pe-recorder-stats --dxmt-log-level info \
  --keep-frontmost --keep-frontmost-process 3DMark05
```

The run reinstalled the latest `build-win32-x64-builtin`,
`build-win32-x86-builtin`, and `build-x86_64-builtin` outputs into the 3DMark05
prefix/runtime before launch.

## Verdict

Fresh-build smoke passed, runtime promotion still rejected.

The run completed as a normal timeout-finalized GT1 smoke:

- `status=pass`, `failures=[]`, `timed_out=true`, `returncode=143`
- output image is non-black and coherent enough for smoke purposes
  (`mean_luma=63.028`, `variance=5268.824`)
- `gpu_command_buffer_errors=0`
- `completion_dequeue_status_error=0`
- log search found no `0x8876086c`, `D3DERR_INVALIDCALL`, `INVALIDCALL`,
  `invalid call`, `commit_chunk_fail`, `device_present_fail`, or
  `pe_call_return_untracked_failure`

The active `EncodeSession` mechanism was exercised:

- `present_encoded=960`
- `command_buffers=3897` (`4.059/present`)
- `sub_command_buffers=2877` (`2.997/present`)
- `render_pass_begin=9946` (`10.360/present`)
- `open_cb_tail_present_pending_started=1008`
- `open_cb_tail_present_head_appended=717`
- `open_cb_tail_present_tail_appended=950`
- `open_cb_tail_present_tail_submitted=950`
- `open_cb_tail_present_pending_merge_failed=0`
- `encode_session_carry_deferred_chunks=1725`
- `encode_session_carry_deferred_active_render_chunks=1725`
- `encode_session_carry_final_chunks=950`

The semantic-release gate also opened in the current build:

- `open_cb_tail_present_semantic_release_candidates=1528`
- `open_cb_tail_present_semantic_release_submitted=57`
- `open_cb_tail_present_semantic_release_blocked_no_completion_wait=1318`
- `completion_wait_enqueues_during_wait=56`
- `completion_wait_command_buffer_commit=57`

This does not change the H151 conclusion. The path is visual/error safe in this
fresh-build smoke, but it is still not a promotable P4/FPS result: most
semantic-release candidates remain outside the active completion wait, and the
run was a short no-gputrace smoke rather than a locality-gated 120s A/B.
