---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: runtime
order: 155
title: EncodeSession Ready-Source Preemptive Semantic Release
date: 2026-06-21
type: no-gputrace
status: mechanism-observed-runtime-rejected
source: experiments/output/app-d3d9-3dmark05-encode-session-ready-preempt-smoke-r1-20260621/result.json, experiments/output/app-d3d9-3dmark05-encode-session-ready-preempt-smoke-r1-20260621/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-encode-session-ready-preempt-smoke-r1-20260621/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-ready-preempt-smoke-r1-20260621/actual.png
related: docs/perfomance/present-pacing.md, docs/perfomance/present-pacing/present-pacing-encode-session-completion-wait-wakeup.153.md, docs/perfomance/present-pacing/present-pacing-encode-session-deterministic-semantic-release.154.md, specs/backend/design.md, specs/backend/requirements.md
---

# Present-Pacing H155 - EncodeSession Ready-Source Preemptive Semantic Release

## Question

H153 only released an open-CB semantic-boundary pending prefix when the ready
queue was empty. If a Metal completion-wait window opens while another ready
source is already queued, the encoder can append that source to the pending
session instead of submitting the already-dequeued semantic prefix. Does
preempting ready-source append during the active wait raise useful P4 overlap
without reproducing H154's deterministic command-buffer fragmentation?

## Run

```text
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix encode-session-ready-preempt-smoke-r1-20260621 \
  --no-gputrace --no-encoder-breakdown --timeout 25 \
  --frame-sampling --pe-recorder-stats --dxmt-log-level info \
  --keep-frontmost --keep-frontmost-process 3DMark05.exe \
  --open-cb-preencode-tail-present \
  --open-cb-carry-render-session \
  --open-cb-semantic-boundary-publish
```

## Verdict

Mechanism observed, runtime promotion rejected.

The short smoke is correctness-safe:

- `status=pass`, `failures=[]`, `returncode=143`, `timed_out=true`
- output image is normal GT1 content, not a black frame
  (`mean_luma=78.240`, `variance=5607.387`)
- `gpu_command_buffer_errors=0`
- log search found no `0x8876086c`, `D3DERR_INVALIDCALL`, `INVALIDCALL`,
  `invalid call`, `commit_chunk_fail`, `device_present_fail`,
  `MTLCommandBufferError`, or command-buffer failure rows

The ready-source preempt path raises same-window activity over H153:

- semantic release submissions: `126 -> 141`
- completion-wait command-buffer commits: `125 -> 141`
- enqueues during completion wait: `124 -> 140`
- completion wait with enqueue: `3.649 -> 4.502ms/present`

The locality shape avoids H154's deterministic fragmentation:

- command buffers: `3981 / 960 = 4.147/present`
- sub-command buffers: `2877 / 960 = 2.997/present`
- render-pass begins: `9988 / 960 = 10.404/present`
- tile preservation: `104.892MiB/present`
- GPU command-buffer time: `2387.536ms = 2.487ms/present`
- `chunk_subcb_count_max=4`

But it still does not move the wall enough to promote:

- command buffers are slightly worse than H153 (`4.147` vs `4.124/present`)
- total completion wait is slightly worse than H153 (`20.365` vs
  `19.626ms/present`)
- no-enqueue completion wait remains effectively flat (`15.863` vs
  `15.977ms/present`)
- most release candidates still miss the active wait window:
  `1396 / 1602` blocked because no completion wait is active, plus `65`
  already-used blocks

## Interpretation

H155 is a useful scheduler correction for the opt-in EncodeSession carrier. It
prevents the encoder from appending more ready work over an already releasable
semantic prefix when that release can land inside the current completion-wait
window. It is also a better policy point than H154: the sub-CB cap and
render-pass shape stay baseline-like instead of exploding command-buffer count.

The result still rejects FPS promotion. The remaining bottleneck is not slow
publish/dequeue handoff or a missing ready-source preempt branch; it is still
window coverage and final commit incidence. A promotable carrier needs earlier
CPU-ready arrival or an already-dequeued session unit that can commit during
active wait without raising command-buffer, sub-command-buffer, render-pass, or
tile-preservation shape.
