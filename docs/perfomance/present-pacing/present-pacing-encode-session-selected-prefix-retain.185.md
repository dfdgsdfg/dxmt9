---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: implementation
order: 185
title: EncodeSession Selected-Prefix Retain
date: 2026-06-28
type: no-gputrace
status: implementation-invariant-runtime-smoke
source: experiments/output/app-d3d9-3dmark05-encode-session-selected-prefix-retain-r1-20260628/result.json, experiments/output/app-d3d9-3dmark05-encode-session-selected-prefix-retain-r1-20260628/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-encode-session-selected-prefix-retain-r1-20260628/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-selected-prefix-retain-r1-20260628/actual.png
related: docs/perfomance/present-pacing/index.md, docs/perfomance/present-pacing/present-pacing-encode-session-semantic-attachment-only-rerun.184.md
---

# Present-Pacing H185 - EncodeSession Selected-Prefix Retain

## Question

Can the H183/H184 open-CB `EncodeSession` path make the selected source prefix
explicitly retained as compact completion-source metadata before cross-source
lookahead is exposed to the encoder?

## Implementation

The open-CB encode loop now validates the whole selected prefix with
`retainEncodedSourcesForPendingTail()` before encoding the first source. The
per-source preflight, `EncodeChunkOptions::sessionSource`, and cross-source
`sessionLookaheadSources` are all gated by that retained prefix metadata.

This tightens R-BACK-2.48/R-BACK-2.49 without changing the source payload
ownership model: source records and arenas still live in queue-owned slots, and
the session receives only compact `QueueCompletionSource` refs.

Focused coverage:

- `dxmt9-queue-completion-sources-spec` now covers selected-prefix retention.
- `dxmt9-render-backend-batch-contract-spec` still covers open-CB/session policy.
- `dxmt9-verify-tla` passes for the queue/completion refinement.

## Run

```text
DXMT9_DISABLE_PRESENT_BOUNDARY=1 \
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix encode-session-selected-prefix-retain-r1-20260628 \
  --no-gputrace \
  --no-encoder-breakdown \
  --frame-sampling \
  --pe-recorder-stats \
  --open-cb-preencode-tail-present \
  --open-cb-carry-render-session \
  --open-cb-semantic-boundary-publish \
  --open-cb-draw-attachment-boundary-publish \
  --open-cb-wait-start-cpu-ready-publish \
  --open-cb-active-wait-cpu-ready-append
```

## Verdict

The invariant change is runtime-safe and preserves the H184 shape. It is not a
promotion gate pass by itself.

- `status=pass`, `returncode=143`, `timed_out=true`
- `present_encoded=1,260`
- `gpu_command_buffer_errors=0`
- `sampled_avg_fps=11.622`
- `completion_wait_overlap_share=86.312%`
- `completion_wait_command_buffer_commit=1,686`
- `completion_wait_enqueues_during_wait=1,682`
- `command_buffers_per_present=2.398`
- `sub_command_buffers_per_present=0.064`
- `render_pass_begin_per_present=12.694`
- `encode_session_carry_active_entry_lost_active_before_first_draw=4,506`
- `actual.png` is a normal non-black GT1 frame (`mean_luma=58.224`,
  `variance=6612.755`)

Compared with H184 (`2.404` CB/present, `0.058` sub-CBs/present, `12.748`
passes/present, overlap `85.495%`), the result is same-class noise. The useful
effect is structural: selected source suffix metadata, store-proof lookahead,
and ordered completion expansion now share the same validated prefix.

## Next

This closes a lifecycle/refinement gap, not the locality wall. The remaining
promotion blockers are unchanged: disabled present boundary dependency,
elevated render-pass count versus H180, active-entry loss at clear/present
semantic boundaries, and only broad output-frame visual evidence.
