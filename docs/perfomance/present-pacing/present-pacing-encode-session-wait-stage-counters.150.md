---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: runtime
order: 150
title: EncodeSession Wait-Stage Counters
date: 2026-06-21
type: no-gputrace
status: mechanism-observed-runtime-rejected
source: experiments/output/app-d3d9-3dmark05-encode-session-wait-stage-counters-r1-20260621/result.json, experiments/output/app-d3d9-3dmark05-encode-session-wait-stage-counters-r1-20260621/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-encode-session-wait-stage-counters-r1-20260621/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-wait-stage-counters-r1-20260621/actual.png
related: docs/perfomance/present-pacing/index.md, docs/perfomance/present-pacing/present-pacing-encode-session-semantic-release-gate.149.md, docs/perfomance/present-pacing/present-pacing-encode-session-pass-streaming-runtime.147.md, specs/backend/spec.md, specs/backend/requirements.md
---

# Present-Pacing H150 - EncodeSession Wait-Stage Counters

## Question

After adding explicit wait-window stage counters, where does the open-CB
semantic-release path miss useful P4 overlap: producer publish, encoder dequeue,
command-buffer commit, or completion enqueue?

## Run

```text
DXMT9_OPEN_CB_SEMANTIC_BOUNDARY_PUBLISH=1 \
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix encode-session-wait-stage-counters-r1-20260621 \
  --no-gputrace --no-encoder-breakdown --timeout 25 \
  --frame-sampling \
  --open-cb-preencode-tail-present \
  --open-cb-carry-render-session \
  --pe-recorder-stats --dxmt-log-level info
```

The run is a short timeout-finalized smoke to validate the counters and refine
H149. The new counters are observability-only:
`completion_wait_commit_publish`, `completion_wait_encode_dequeue`, and
`completion_wait_command_buffer_commit`.

## Verdict

Mechanism observed, runtime promotion rejected.

The smoke is correctness-safe: `status=pass`, `failures=[]`,
`gpu_command_buffer_errors=0`, `present_source_invalid_size=0`,
`commit_chunk_draw_run_break_invalid=0`, and
`shader_decoder_reject_invalid_opcode=0`. Searching the direct logs found no
`hr=0x8876086c`, `D3DERR_INVALIDCALL`, `INVALIDCALL`, `commit_chunk_fail`,
`device_present_fail`, or `pe_call_return_untracked_failure`. The captured image
is normal GT1 output, not a black or frozen frame.

The H149 all-blocked result is not a hard impossibility: on this current-head
short run, semantic release does sometimes land inside a completion wait.
However, it is sparse and too late to be a promotable overlap carrier:

- `open_cb_tail_present_semantic_release_candidates=763`
- `open_cb_tail_present_semantic_release_submitted=17`
- `open_cb_tail_present_semantic_release_blocked_no_completion_wait=737`
- `open_cb_tail_present_semantic_release_failed=0`

The wait-stage counters explain the remaining loss:

- `completion_wait_commit_chunk_entries=175`
- `completion_wait_commit_chunk_replay_starts=175`
- `completion_wait_commit_chunk_replay_ends=165`
- `completion_wait_commit_publish=42`
- `completion_wait_encode_dequeue=42`
- `completion_wait_command_buffer_commit=17`
- `completion_wait_enqueues_during_wait=17`

Producer replay can enter the wait window, and publish/dequeue can occur during
that window, but only `17 / 42` wait-window publish/dequeue cases reach Metal
command-buffer commit before the wait ends. The actual completion enqueue count
matches that final commit count.

The command-buffer shape remains baseline-like rather than a recovered P4 shape:
`present_encoded=480`, `command_buffers=1937`
(`4.035/present`), `sub_command_buffers=1437` (`2.994/present`), and
`render_pass_begin=4966` (`10.346/present` in this short scene slice).

## Interpretation

H150 refines H149: the semantic-release gate is not merely blocked by the
predicate. Current GT1 timing can occasionally publish and dequeue prefix work
while the previous Present completion wait is active, but encode-to-commit often
misses the same wait window, and most semantic candidates still arrive after
the wait has ended.

The next carrier should therefore target earlier CPU-ready production or a
shorter publish/dequeue-to-commit path while preserving the current
baseline-style CB/sub-CB shape. Merely loosening the semantic-release predicate
would submit more prefixes outside the useful window and risks returning to the
known locality-fragmenting class.
