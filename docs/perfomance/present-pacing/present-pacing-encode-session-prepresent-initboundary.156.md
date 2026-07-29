---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: runtime
order: 156
title: EncodeSession Pre-Present Initializer-Wait Boundary
date: 2026-06-21
type: no-gputrace
status: mechanism-observed-runtime-rejected
outdated: knob-removed
source: experiments/output/app-d3d9-3dmark05-encode-session-prepresent128-current-r1-20260621/result.json, experiments/output/app-d3d9-3dmark05-encode-session-prepresent128-current-r1-20260621/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-prepresent128-current-r1-20260621/dxmt9.log, experiments/output/app-d3d9-3dmark05-encode-session-prepresent128-initboundary-r1-20260621/result.json, experiments/output/app-d3d9-3dmark05-encode-session-prepresent128-initboundary-r1-20260621/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-encode-session-prepresent128-initboundary-r1-20260621/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-prepresent128-initboundary-r1-20260621/actual.png
related: docs/perfomance/present-pacing/index.md, docs/perfomance/present-pacing/present-pacing-open-cb-finalizer-limit128.145.md, docs/perfomance/present-pacing/present-pacing-open-cb-bounded-tail-wait.146.md, docs/perfomance/present-pacing/present-pacing-encode-session-ready-preempt-release.155.md, specs/backend/spec.md, specs/backend/requirements.md
---

# Present-Pacing H156 - EncodeSession Pre-Present Initializer-Wait Boundary

> **Outdated — the knob or code path this experiment measured no longer exists in `src/`.** It cannot be re-run. Kept as history; do not cite it as current evidence.

## Question

The draw-heavy `DXMT9_STAGE_PRE_PRESENT_COMMAND_LIMIT=128` diagnostic creates a
large pre-Present head that can be appended into an open `EncodeSession`. A
resource-initializer upload wait is a real Metal boundary: if it is discovered
inside `encodeChunk()` while a carried render encoder is active, the encoder must
be finalized before the event wait can be encoded. Can the queue treat this as a
source-append boundary instead, submit the pending session first, and make the
pre-Present diagnostic safe enough to evaluate P4 overlap?

## Run

Pre-change diagnostic:

```text
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix encode-session-prepresent128-current-r1-20260621 \
  --no-gputrace --no-encoder-breakdown --timeout 25 \
  --frame-sampling --pe-recorder-stats --dxmt-log-level info \
  --keep-frontmost --keep-frontmost-process 3DMark05.exe \
  --open-cb-preencode-tail-present \
  --open-cb-carry-render-session \
  --open-cb-semantic-boundary-publish \
  --stage-pre-present-command-limit 128
```

Post-change diagnostic:

```text
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix encode-session-prepresent128-initboundary-r1-20260621 \
  --no-gputrace --no-encoder-breakdown --timeout 25 \
  --frame-sampling --pe-recorder-stats --dxmt-log-level info \
  --keep-frontmost --keep-frontmost-process 3DMark05.exe \
  --open-cb-preencode-tail-present \
  --open-cb-carry-render-session \
  --open-cb-semantic-boundary-publish \
  --stage-pre-present-command-limit 128
```

## Verdict

Mechanism observed, runtime promotion rejected.

The pre-change run is not a usable performance sample. It times out after only
`9` frame rows, produces a dark early GT1 image (`mean_luma=5.583`), emits no
run-level `dxmt9_perf_counters` in `result.json`, and records
`encode_session_carry_forced_finalize_initializer_wait_active_render=2` in the
frame log. No invalid-call, GPU-command-buffer, or queue-failure string was
found, but the sample does not reach the normal battlefield window.

The post-change run reaches a normal GT1 frame and remains correctness-safe:

- `status=pass`, `failures=[]`, `returncode=143`, `timed_out=true`
- `present_encoded=960`
- output image shows normal battlefield content (`mean_luma=68.564`,
  `variance=5108.204`), not black output or the earlier giant vertical artifact
- `gpu_command_buffer_errors=0`
- log search found no `0x8876086c`, `D3DERR_INVALIDCALL`, `INVALIDCALL`,
  `invalid call`, `commit_chunk_fail`, `device_present_fail`,
  `MTLCommandBufferError`, or command-buffer failure rows
- `encode_session_carry_forced_finalize_initializer_waits=0`
- `encode_session_carry_forced_finalize_initializer_wait_active_render=0`

The diagnostic now exercises the intended carrier heavily:

- `chunk_publish_reason_present_split_before=1143`
- `chunk_publish_present_split_before_tail_draw_run=1143`
- `open_cb_tail_present_pending_started=1083`
- `open_cb_tail_present_head_appended=1950`
- `open_cb_tail_present_tail_appended=919`
- `open_cb_tail_present_tail_submitted=919`
- `encode_session_carry_deferred_active_render_chunks=3033`
- `encode_session_carry_final_chunks=919`
- semantic releases submit `161 / 1667` candidates, with `1469` still blocked
  outside active completion wait

It also moves some wait-window counters relative to H155, but not with an
acceptable shape:

- completion wait: `15981.568ms = 16.647ms/present`
- completion wait with enqueue: `4631.035ms = 4.824ms/present`
- completion wait without enqueue: `11350.533ms = 11.823ms/present`
- completion-wait command-buffer commits: `159`
- enqueues during completion wait: `157`
- command buffers: `4004 / 960 = 4.171/present`
- sub-command buffers: `2878 / 960 = 2.998/present`
- render-pass begins: `10131 / 960 = 10.553/present`
- tile preservation: `107.286MiB/present`
- GPU command-buffer time: `2796.024ms = 2.913ms/present`
- present-boundary wait: `3900.448ms = 4.063ms/present`
- `chunk_subcb_count_max=4`

## Interpretation

This is a good correctness boundary for the opt-in EncodeSession path. A pending
initializer upload is not just another append-compatible source when the pending
session still has an active render encoder; Metal requires the event wait to be
encoded outside that render encoder. Submitting the pending session before
appending the next source preserves the semantic boundary explicitly instead of
letting `encodeChunk()` discover it by force-finalizing an active render encoder.

The result is still not a default policy candidate. The draw-heavy pre-Present
split can now run to a full, visual-safe sample, and same-window commit incidence
is slightly higher than H155. The cost is also visible: command buffers,
render-pass begins, tile preservation, GPU command-buffer time, and
present-boundary wait all rise versus H155. This keeps
`DXMT9_STAGE_PRE_PRESENT_COMMAND_LIMIT=128` diagnostic-only. The promotable
branch still needs earlier CPU-ready staging or already-dequeued wait-window
commits that preserve the baseline CB/sub-CB/render-pass/tile shape instead of
manufacturing more draw-heavy source splits.
