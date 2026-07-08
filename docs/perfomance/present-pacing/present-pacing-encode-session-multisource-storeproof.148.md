---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: runtime
order: 148
title: EncodeSession Multi-Source Store-Proof Runtime
date: 2026-06-21
type: no-gputrace
status: mechanism-accepted-runtime-rejected
source: experiments/output/app-d3d9-3dmark05-encode-session-multisource-storeproof-r1-20260621/result.json, experiments/output/app-d3d9-3dmark05-encode-session-multisource-storeproof-r1-20260621/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-encode-session-multisource-storeproof-r1-20260621/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-multisource-storeproof-r1-20260621/actual.png
related: docs/perfomance/present-pacing/index.md, docs/perfomance/present-pacing/present-pacing-encode-session-pass-streaming-runtime.147.md, specs/backend/design.md, specs/backend/requirements.md
---

# Present-Pacing H148 - EncodeSession Multi-Source Store-Proof Runtime

## Question

After the H147 session-wide sub-CB cap restored the baseline command-buffer
chain shape, can R-BACK-2.48 improve tile/load-store locality by letting the
encoder prove stores across the already selected `EncodeSession` source suffix?

## Run

```text
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix encode-session-multisource-storeproof-r1-20260621 \
  --no-gputrace --no-encoder-breakdown --timeout 45 \
  --frame-sampling \
  --open-cb-preencode-tail-present \
  --open-cb-carry-render-session \
  --draw-chunk-command-limit 128 \
  --pe-recorder-stats --dxmt-log-level info
```

The implementation passes a call-local `sessionLookaheadSources` span through
`EncodeChunkOptions`. The span is consumed synchronously by the render-pass
store-proof scanner and is never stored in `EncodeSession`. The queue only
provides it when the ready prefix already contains the final Present tail; a
pending head without a known tail remains conservative.

## Verdict

Mechanism accepted, runtime promotion rejected.

The run is correctness-safe for this smoke: `status=pass`, `failures=[]`,
`gpu_command_buffer_errors=0`, and the captured image is normal/non-black
(`mean_luma=73.657`, `variance=5460.760`). Log search found no
`commit_chunk_fail`, `0x8876086c`, `D3DERR_INVALIDCALL`, or `INVALIDCALL`
string. The process timed out as expected for the short smoke
(`returncode=143`, `timed_out=true`) after producing complete artifacts.

The locality/performance result is negative. Command-buffer shape stays near
the H147 session-cap shape (`command_buffers_per_present=4.004`,
`sub_command_buffers_per_present=2.999`, `render_passes_per_present=11.628`),
but tile preservation does not improve:
`tile_preservation_mib_per_present=125.859`, still above the h220 baseline
(`120.222`) and essentially flat/slightly worse than H147 session-cap
(`125.638`). P4 overlap also does not move:
`completion_wait_with_enqueue=0`,
`completion_wait_without_enqueue_ms_per_present=16.961`.

The direct reason is visible in the proof counters: the selected suffix path
does not expose usable clear-dead stores for this workload. Both
`render_pass_depth_proof_allow_next_clear` and
`render_pass_color_proof_allow_next_clear` remain `0`; most counted sources are
still defensive (`depth block_no_lookahead=6685`, `depth block_draw_depth=3672`,
`color block_no_lookahead=6865`, `color block_present=4567`,
`color block_draw_target=2522`).

## Interpretation

This closes the narrow R-BACK-2.48 gap for already selected session suffixes:
the implementation can reason across source boundaries without storing borrowed
spans or guessing future writer output. It does not break invalid-call,
completion, or visual correctness in the GT1 smoke.

It is not the wall-breaking change. GT1's remaining overhead is not a missed
next-clear proof across selected sources; it is still the same P4/no-enqueue
shape plus render-pass/tile locality floor. The next candidate must either
change the logical stream enough to create real producer/encode overlap, or
reduce replay/encode/materialization cost directly, while keeping this
baseline-style CB/pass/tile shape and the `v0.0.3` visual gate.
