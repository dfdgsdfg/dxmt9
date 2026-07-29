---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: runtime
order: 157
title: EncodeSession Strict Semantic Tailless Start
date: 2026-06-21
type: no-gputrace
status: mechanism-observed-runtime-rejected
source: experiments/output/app-d3d9-3dmark05-encode-session-strict-semantic-smoke-r1-20260621/result.json, experiments/output/app-d3d9-3dmark05-encode-session-strict-semantic-smoke-r1-20260621/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-encode-session-strict-semantic-smoke-r1-20260621/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-strict-semantic-smoke-r1-20260621/actual.png
related: docs/perfomance/present-pacing/index.md, docs/perfomance/present-pacing/present-pacing-encode-session-ready-preempt-release.155.md, specs/backend/spec.md, specs/backend/requirements.md
---

# Present-Pacing H157 - EncodeSession Strict Semantic Tailless Start

## Question

After H156, the queue separates tailless carried-session starts from complete
tail-ready prefixes: before the final Present tail is selected in the same ready
prefix, only `SemanticBoundary` sources may start or append to a pending carried
`EncodeSession`. Does this stricter semantic-only path preserve the visual gate,
remove the draw-count `PresentSplitBefore` cost, and move P4 enough to promote?

## Run

```text
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix encode-session-strict-semantic-smoke-r1-20260621 \
  --no-gputrace --no-encoder-breakdown --timeout 25 \
  --frame-sampling --pe-recorder-stats --dxmt-log-level info \
  --keep-frontmost --keep-frontmost-process 3DMark05.exe \
  --open-cb-preencode-tail-present \
  --open-cb-carry-render-session \
  --open-cb-semantic-boundary-publish
```

## Verdict

Mechanism observed, runtime promotion rejected.

The run is correctness-safe:

- `status=pass`, `failures=[]`, `returncode=143`, `timed_out=true`
- `present_encoded=960`
- output image shows normal GT1 battlefield content (`mean_luma=72.938`,
  `variance=5433.012`)
- `gpu_command_buffer_errors=0`
- log search found no `0x8876086c`, `D3DERR_INVALIDCALL`, `INVALIDCALL`,
  `invalid call`, `commit_chunk_fail`, `device_present_fail`,
  `MTLCommandBufferError`, or command-buffer failure rows
- `encode_session_carry_forced_finalize_initializer_waits=0`

The strict start policy removes the H156 draw-count path:

- `chunk_publish_reason_present_split_before=0`
- `chunk_publish_present_split_before_tail_draw_run=0`
- `chunk_publish_reason_semantic_boundary=1704`
- `open_cb_tail_present_pending_started=1038`
- `open_cb_tail_present_head_appended=666`
- `open_cb_tail_present_tail_submitted=872`
- `encode_session_carry_deferred_active_render_chunks=1704`
- `open_cb_tail_present_semantic_release_submitted=163 / 1581`
- semantic release misses remain high: `1359` no-active-wait blocks and `59`
  already-used blocks

Compared with H155, the same-window counters move in the right direction but do
not clear the promotion gate:

- semantic release submissions: `141 -> 163`
- completion-wait command-buffer commits: `141 -> 162`
- enqueues during completion wait: `140 -> 162`
- completion wait with enqueue: `4.502 -> 5.123ms/present`
- completion wait without enqueue: `15.863 -> 15.634ms/present`
- total completion wait: `20.365 -> 20.757ms/present`
- command buffers: `4.147 -> 4.173/present`
- sub-command buffers: `2.997 -> 2.998/present`
- render-pass begins: `10.404 -> 10.332/present`
- tile preservation: `104.892 -> 103.769MiB/present`
- GPU command-buffer time: `2.487 -> 2.642ms/present`
- present-boundary wait: `0.787 -> 0.588ms/present`
- `chunk_subcb_count_max=4`

## Interpretation

H157 is a cleaner policy point than H156. It proves the latest source no longer
needs draw-count `PresentSplitBefore` heads to exercise the carried
`EncodeSession`; the path is semantic-only, visual-safe, and still increases
same-window enqueue/commit activity. It also removes H156's large
present-boundary and tile/GPU regression from the command-limit diagnostic.

It is still not the R-BACK-2.50 production carrier. The P4 improvement is small,
most semantic candidates still arrive outside completion wait, and command
buffers remain above H155 and above the non-increasing locality gate. The next
candidate needs either earlier CPU-ready arrival for semantic sources or a way
to commit already-dequeued session work during active wait without increasing
command-buffer count.
