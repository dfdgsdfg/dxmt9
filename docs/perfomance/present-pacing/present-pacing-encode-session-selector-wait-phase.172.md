---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: runtime
order: 172
title: EncodeSession Selector Wait-Phase Counters
date: 2026-06-28
type: no-gputrace
status: diagnostic-safe-runtime-rejected
source: experiments/output/app-d3d9-3dmark05-encode-session-selector-wait-phase-r1-20260628-001947/result.json, experiments/output/app-d3d9-3dmark05-encode-session-selector-wait-phase-r1-20260628-001947/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-encode-session-selector-wait-phase-r1-20260628-001947/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-selector-wait-phase-r1-20260628-001947/dxmt9.log, experiments/output/app-d3d9-3dmark05-encode-session-selector-wait-phase-r1-20260628-001947/actual.png
related: docs/perfomance/present-pacing.md, docs/perfomance/present-pacing/present-pacing-encode-session-selector-counters.171.md, docs/perfomance/present-pacing/present-pacing-encode-session-semantic-prefix.170.md, specs/backend/design.md, specs/backend/requirements.md
---

# Present-Pacing H172 - EncodeSession Selector Wait-Phase Counters

## Question

H171 proved the open-CB selector is active, but it did not distinguish when the
selected prefixes are dequeued. This run adds wait-phase counters for selected
tail-ready prefixes, selected semantic-start prefixes, and pending-session
starts.

The discriminator is:

- if selected prefixes mostly happen while completion wait is active, the next
  owner is session attach/finalization after dequeue;
- if selected prefixes mostly happen after the wait is inactive, the next owner
  is producer/queue boundary timing or earlier CPU-ready source attachment.

## Run

```text
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix encode-session-selector-wait-phase-r1-20260628-001947 \
  --no-gputrace --no-encoder-breakdown --timeout 25 \
  --frame-sampling --pe-recorder-stats --dxmt-log-level info \
  --keep-frontmost --keep-frontmost-process 3DMark05.exe \
  --open-cb-preencode-tail-present \
  --open-cb-carry-render-session \
  --open-cb-semantic-boundary-publish \
  --open-cb-cpu-ready-command-limit 48 \
  --open-cb-writer-active-cpu-ready-publish \
  --open-cb-active-wait-cpu-ready-append \
  --open-cb-wait-start-cpu-ready-publish
```

The code under test adds cumulative and frame-sampled counters for
`*_wait_active` and `*_wait_inactive` selector/pending-start phases. It does
not change queue behavior.

## Verdict

Diagnostic safe, runtime promotion rejected.

The no-gputrace smoke is correctness-safe:

- `status=pass`, `failures=[]`, `capture_error=null`
- `returncode=143`, `timed_out=true`, which is the wrapper timeout path
- `present_encoded=840`
- `actual.png` is non-black (`mean_luma=72.590`, `variance=5211.567`)
- no invalid-call/assert/black-screen error rows in the output logs

The wait-phase split is decisive:

- pending session starts: `3` wait-active, `840` wait-inactive
- tail-ready selector prefixes: `1` wait-active (`2` sources), `805`
  wait-inactive (`1615` sources)
- semantic-start selector prefixes: `2` wait-active (`4` sources), `274`
  wait-inactive (`558` sources)
- completion-wait pending observations remain sparse: `7` total, with `4`
  ready-source and `3` no-ready-source observations

The P4 overlap gate still does not move:

- `completion_wait_enqueues_during_wait=2`
- `completion_wait_command_buffer_commit=2`
- `completion_wait_encode_dequeue=5`
- `completion_wait_commit_publish=5`
- `completion_wait_with_enqueue_ms_per_present=0.019`
- `completion_wait_without_enqueue_ms_per_present=13.673`
- `completion_wait_no_enqueue_share=99.860%`

Shape remains baseline-like but not better:

- `command_buffers_per_present=4.011`
- `sub_command_buffers_per_present=3.002`
- `render_pass_begin_per_present=10.682`
- `chunk_subcb_count_max=4`

## Interpretation

H172 narrows the H171 conclusion. The selector is not merely choosing the wrong
prefix class; it is choosing almost all useful prefixes after the previous
Present completion wait is already inactive. The session path therefore has no
material work to attach to an open render encoder during the P4 window.

The next implementation branch should move source readiness earlier than the
wait observation point, or reduce producer/replay cadence enough that real
ready sources arrive during the wait. Another selector relaxation or
post-dequeue append policy is unlikely to move FPS by itself.
