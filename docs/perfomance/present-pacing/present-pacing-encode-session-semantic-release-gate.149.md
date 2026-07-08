---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: runtime
order: 149
title: EncodeSession Semantic Release Gate Runtime
date: 2026-06-21
type: no-gputrace
status: mechanism-observed-runtime-rejected
source: experiments/output/app-d3d9-3dmark05-encode-session-semantic-release-counters-r1-20260621/result.json, experiments/output/app-d3d9-3dmark05-encode-session-semantic-release-counters-r1-20260621/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-encode-session-semantic-release-counters-r1-20260621/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-semantic-release-counters-r1-20260621/actual.png
related: docs/perfomance/present-pacing/index.md, docs/perfomance/present-pacing/present-pacing-encode-session-pass-streaming-runtime.147.md, docs/perfomance/present-pacing/present-pacing-encode-session-multisource-storeproof.148.md, specs/backend/design.md, specs/backend/requirements.md
---

# Present-Pacing H149 - EncodeSession Semantic Release Gate Runtime

## Question

After adding explicit counters around the default-off open-CB semantic-boundary
release gate, does the carried `EncodeSession` path actually publish a visible
prefix while a completion wait is active?

## Run

```text
DXMT9_OPEN_CB_SEMANTIC_BOUNDARY_PUBLISH=1 \
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix encode-session-semantic-release-counters-r1-20260621 \
  --no-gputrace --no-encoder-breakdown --timeout 45 \
  --frame-sampling \
  --open-cb-preencode-tail-present \
  --open-cb-carry-render-session \
  --pe-recorder-stats --dxmt-log-level info
```

This run uses the same timeout-finalized no-gputrace smoke class as H147/H148.
The new counters are observability-only: they do not change the release
predicate. A pending source prefix may release at a semantic boundary only when
the source was published for a semantic boundary, it is not the final Present
tail, a completion wait is currently active, and the current wait window has
not already consumed a semantic release.

## Verdict

Mechanism observed, runtime promotion rejected.

The run is correctness-safe for this smoke: `status=pass`, `failures=[]`,
`gpu_command_buffer_errors=0`, `completion_dequeue_status_error=0`, and the
captured image is normal GT1 output rather than a black or frozen frame.
Searching the run artifacts found no `D3DERR_INVALIDCALL`, `INVALIDCALL`,
`0x8876086c`, `commit_chunk_fail`, `device_present_fail`, or
`pe_call_return_untracked_failure` string. The short run timed out as expected
after producing complete artifacts (`returncode=143`, `timed_out=true`).

The release gate is not opening in this workload shape. The run records
`open_cb_tail_present_semantic_release_candidates=1054`, but all candidates are
blocked by `open_cb_tail_present_semantic_release_blocked_no_completion_wait=1054`.
No prefix is submitted through that gate:
`open_cb_tail_present_semantic_release_submitted=0`,
`open_cb_tail_present_semantic_release_blocked_already_used=0`, and
`open_cb_tail_present_semantic_release_failed=0`.

The aggregate pacing matches that explanation. There is no enqueue-during-wait
work in this run: `completion_wait_enqueues_during_wait=0`,
`completion_wait_with_enqueue_ms=0.000`, and
`completion_wait_without_enqueue_ms=8172.049` over `540` encoded presents.
The command-buffer shape is baseline-like rather than a P4 recovery:
`command_buffers_per_present=4.000`,
`sub_command_buffers_per_present=2.994`, and
`render_passes_per_present=11.643`.

## Interpretation

This closes a measurement gap in the H147/H148 runtime story. Semantic-boundary
prefixes are being created and observed, but in GT1 they arrive outside the
queue completion-wait window. The policy therefore preserves locality and
visual correctness, but it cannot recover P4 overlap by itself.

H150 refines this result with stage counters. The gate can occasionally open on
current head, but most candidates still arrive outside the wait window and only
a subset of wait-window publish/dequeue events reach command-buffer commit
before the wait ends.

The next overlap candidate must change one of the timing owners, not just the
release predicate. Either the producer/encoder must make a semantic prefix
ready while the previous Present completion wait is active, or the project
should return to serial replay/encode/materialization reductions that directly
move the exposed no-enqueue wait and CPU rows.
