---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: runtime
order: 154
title: EncodeSession Deterministic Semantic Release
date: 2026-06-21
type: no-gputrace
status: negative-control-runtime-rejected
source: experiments/output/app-d3d9-3dmark05-encode-session-semantic-deterministic-smoke-r1-20260621/result.json, experiments/output/app-d3d9-3dmark05-encode-session-semantic-deterministic-smoke-r1-20260621/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-encode-session-semantic-deterministic-smoke-r1-20260621/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-semantic-deterministic-smoke-r1-20260621/actual.png
related: docs/perfomance/present-pacing/index.md, specs/backend/spec.md, specs/backend/requirements.md
---

# Present-Pacing H154 - EncodeSession Deterministic Semantic Release

## Question

H153 kept semantic-boundary release tied to active Metal completion-wait windows.
That improved same-window commits, but most candidates still missed the useful
window. If the open-CB carrier releases every semantic-boundary candidate
deterministically, does the higher release coverage turn into an FPS path without
regressing command-buffer or render-pass locality?

## Run

```text
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix encode-session-semantic-deterministic-smoke-r1-20260621 \
  --no-gputrace --no-encoder-breakdown --timeout 25 \
  --frame-sampling --pe-recorder-stats --dxmt-log-level info \
  --keep-frontmost --keep-frontmost-process 3DMark05.exe \
  --open-cb-preencode-tail-present \
  --open-cb-carry-render-session \
  --open-cb-semantic-boundary-publish \
  --open-cb-semantic-boundary-release-mode deterministic
```

The wrapper now exposes the semantic-release knobs directly and rejects release
modes other than `completion_wait` and `deterministic`.

## Verdict

Negative control accepted, runtime promotion rejected.

The short smoke is correctness-safe:

- `status=pass`, `failures=[]`, `returncode=143`, `timed_out=true`
- output image is normal GT1 content, not a black frame
  (`mean_luma=71.713`, `variance=5480.846`)
- `gpu_command_buffer_errors=0`
- log search found no `0x8876086c`, `D3DERR_INVALIDCALL`, `INVALIDCALL`,
  `invalid call`, `commit_chunk_fail`, or `device_present_fail`

Deterministic release removes the H153 coverage blocker:

- semantic release candidates: `1598`
- semantic release submitted: `1598`
- blocked because no completion wait: `0`
- blocked because already used: `0`
- release failures: `0`
- completion-wait command-buffer commits: `232`
- enqueues during completion wait: `229`
- completion wait with enqueue: `5947.460ms` (`6.195ms/present`)

But it does not pass the promotion gates:

- command buffers rise to `6029 / 960` presents (`6.280/present`), above the
  baseline-style `~4.0/present` shape and above H153's `4.124/present`
- sub-command buffers rise to `3468 / 960` (`3.613/present`), above the
  baseline-style `~3.0/present` shape
- GPU command-buffer time rises to `8102.920ms` (`8.441ms/present`), well above
  H153's `2.943ms/present`
- completion wait is still mostly not hidden:
  `25590.903ms` without enqueue (`26.657ms/present`)
- total completion wait is `31538.364ms` (`32.852ms/present`)

Render-pass count itself is not the main regression in this sample
(`10188 / 960 = 10.613/present`, and tile preservation is
`108.382MiB/present`). The failure is that releasing every semantic-boundary
candidate creates too many Metal command-buffer units and does not hide enough
of the remaining no-enqueue wait.

## Interpretation

H154 closes a useful negative-control loop. The sparse-window diagnosis from
H151-H153 is real, but simply dropping the active-wait release predicate is not
a fix. It turns semantic-boundary candidates into work, yet much of that work is
committed outside the only useful wait window and raises command-buffer/GPU-time
pressure.

The next carrier should keep the deterministic mode as a diagnostic only. A
promotable path still needs earlier CPU-ready work or an already-dequeued
session commit inside active completion wait while preserving the baseline
command-buffer/sub-command-buffer shape.
