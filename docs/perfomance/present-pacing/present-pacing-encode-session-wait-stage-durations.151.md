---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: runtime
order: 151
title: EncodeSession Wait-Stage Duration Counters
date: 2026-06-21
type: no-gputrace
status: mechanism-observed-runtime-rejected
source: experiments/output/app-d3d9-3dmark05-encode-session-wait-stage-durations-r1-20260621/result.json, experiments/output/app-d3d9-3dmark05-encode-session-wait-stage-durations-r1-20260621/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-encode-session-wait-stage-durations-r1-20260621/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-wait-stage-durations-r1-20260621/actual.png
related: docs/perfomance/present-pacing.md, docs/perfomance/present-pacing/present-pacing-encode-session-wait-stage-counters.150.md, docs/perfomance/present-pacing/present-pacing-encode-session-semantic-release-gate.149.md, docs/perfomance/present-pacing/present-pacing-encode-session-pass-streaming-runtime.147.md, specs/backend/design.md, specs/backend/requirements.md
---

# Present-Pacing H151 - EncodeSession Wait-Stage Duration Counters

## Question

H150 showed that semantic-release work can publish and dequeue during a
completion wait, but often does not reach Metal command-buffer commit before the
wait ends. Is that because publish-to-dequeue or dequeue-to-commit latency is
large, or because the matching events are too sparse in the wait window?

## Run

```text
DXMT9_OPEN_CB_SEMANTIC_BOUNDARY_PUBLISH=1 \
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix encode-session-wait-stage-durations-r1-20260621 \
  --no-gputrace --no-encoder-breakdown --timeout 25 \
  --frame-sampling \
  --open-cb-preencode-tail-present \
  --open-cb-carry-render-session \
  --pe-recorder-stats --dxmt-log-level info
```

The new counters are observability-only and pair the latest same-wait-window
events, not a source-specific token: `completion_wait_stage_publish_to_encode_*`
measures the time from a publish observed while the completion watcher is inside
`waitUntilCompleted()` to an encode dequeue in that same wait; the
`encode_dequeue_to_command_buffer_commit` row measures the analogous dequeue to
Metal commit span.

## Verdict

Mechanism observed, runtime promotion rejected.

The run is correctness-safe: `status=pass`, `failures=[]`,
`gpu_command_buffer_errors=0`, `present_source_invalid_size=0`,
`commit_chunk_draw_run_break_invalid=0`, and
`shader_decoder_reject_invalid_opcode=0`. Searching the logs found no
`0x8876086c`, `D3DERR_INVALIDCALL`, `INVALIDCALL`, `commit_chunk_fail`,
`device_present_fail`, or `pe_call_return_untracked_failure`. The captured
image is coherent GT1 output with sparks/bloom visible.

The shape remains baseline-style rather than a P4 recovery:

- `present_encoded=900`
- `command_buffers=3644` (`4.049/present`)
- `sub_command_buffers=2697` (`2.997/present`)
- `render_pass_begin=9282` (`10.313/present`)
- `completion_wait_ms=17731.875` (`19.702ms/present`)
- `completion_wait_with_enqueue_ms=1196.049` (`1.329ms/present`)
- `completion_wait_without_enqueue_ms=16535.826` (`18.373ms/present`)

The wait-window event counts improved versus H150 because this run reached more
frames, but the ratio still rejects promotion:

- `open_cb_tail_present_semantic_release_candidates=1424`
- `open_cb_tail_present_semantic_release_submitted=44`
- `open_cb_tail_present_semantic_release_blocked_no_completion_wait=1252`
- `open_cb_tail_present_semantic_release_failed=0`
- `completion_wait_commit_publish=188`
- `completion_wait_encode_dequeue=188`
- `completion_wait_command_buffer_commit=44`
- `completion_wait_enqueues_during_wait=44`

The duration counters rule out a long publish/dequeue path as the primary
owner. When publish and dequeue are both inside the active completion wait, the
latency is small:

- `completion_wait_stage_publish_to_encode_dequeue=188`
- total `14.044ms`, p50 `0.071ms`, p95 `0.110ms`

When dequeue and Metal command-buffer commit both land inside the same wait,
that span is also only about a millisecond:

- `completion_wait_stage_encode_dequeue_to_command_buffer_commit=44`
- total `45.062ms`, p50 `1.074ms`, p95 `1.249ms`

## Interpretation

H151 refines H150: the overlap path is not primarily blocked by a slow
publish-to-dequeue handoff. The encoder sees published work quickly when it
appears during a completion wait, and command-buffer commit is also quick for
the subset that reaches commit before the wait ends.

The failure is window coverage and final commit incidence: most semantic-release
candidates still arrive outside completion wait, and only `44 / 188`
wait-window publish/dequeue samples become a Metal commit in that same wait.
This means the next P4 carrier should not simply loosen the release predicate.
It must either make CPU-ready prefix work arrive earlier, or keep a
locality-preserving encode/session unit open such that already-dequeued work can
be committed inside the wait without increasing CB/pass/tile preservation.
