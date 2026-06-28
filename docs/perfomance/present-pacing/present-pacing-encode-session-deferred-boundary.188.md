---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: runtime
order: 188
title: EncodeSession Deferred Present Boundary Prototype
date: 2026-06-28
type: no-gputrace
status: diagnostic-safe-superseded-runtime-unpromoted
source: experiments/output/app-d3d9-3dmark05-encode-session-deferred-boundary-rerun-20260628/result.json, experiments/output/app-d3d9-3dmark05-encode-session-deferred-boundary-rerun-20260628/3dmark05-perf-summary.md, experiments/output/app-d3d9-3dmark05-encode-session-deferred-boundary-rerun-20260628/3dmark05-perf-frames.csv, experiments/output/app-d3d9-3dmark05-encode-session-deferred-boundary-rerun-20260628/actual.png
related: docs/perfomance/present-pacing.md, docs/perfomance/present-pacing/present-pacing-encode-session-stable-rerun.187.md, specs/backend/design.md, specs/backend/requirements.md
---

# Present-Pacing H188 - EncodeSession Deferred Present Boundary Prototype

## Question

In the initial deferred-boundary prototype, can the stable open-CB
`EncodeSession` flag set recover P4 overlap if the present-completion
frame-latency wait that would have run after the current `Present` tail is
deferred to the next `Present` entry, instead of disabling the present boundary
entirely?

## Run

```text
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix encode-session-deferred-boundary-rerun-20260628 \
  --no-gputrace \
  --no-encoder-breakdown \
  --frame-sampling \
  --pe-recorder-stats \
  --keep-frontmost \
  --wait-unlocked-sec 60 \
  --timeout 120 \
  --present-boundary-deferred \
  --open-cb-preencode-tail-present \
  --open-cb-carry-render-session \
  --open-cb-semantic-boundary-publish \
  --open-cb-draw-attachment-boundary-publish \
  --open-cb-wait-start-cpu-ready-publish \
  --open-cb-active-wait-cpu-ready-append
```

## Verdict

Diagnostic safe, superseded, runtime unpromoted.

This run proves the loose deferred-boundary prototype reaches the runtime and
opens a real run-ahead window:

- `status=pass`, `capture_error=None`
- `returncode=143`, `timed_out=true`; this is a timeout-finalized no-gputrace
  sample with complete artifacts, not an early crash
- `present_encoded=1200`
- `gpu_command_buffer_errors=0`
- `actual.png` is a visible non-black GT1 frame (`mean_luma=61.810`,
  `variance=6625.052`)
- frame sampling reports `1248` nonzero frame rows over `109605.954ms`, or
  `11.386` sampled FPS (`13.015` mean row FPS, `13.018` median)

The boundary path moved as intended:

- `present_boundary_applied=1200`
- `present_boundary_deferred=1199`
- `present_boundary_waits=0`
- `present_boundary_wait_ms=0.000`
- `present_boundary_deferred_waits=0`

`present_boundary_deferred_waits=0` means the deferred target had already
completed by the next `Present` entry in this run. After review, this was
classified as too loose for the intended "N+1 offscreen may run, but N+1
present tail is gated" semantics. The implementation was therefore tightened to
compute the deferred target against the next present tail (`presentSeqId + 1`)
before promotion. A valid runtime sample for the tightened tail-gate semantics
is still pending; the immediate rerun attempts were blocked because the macOS
desktop stayed locked.

The P4 overlap signal is strong versus H187:

- `completion_wait_with_enqueue_ms=35475.400`
- `completion_wait_without_enqueue_ms=5972.141`
- overlap share is `85.591%` (`0.145%` in H187)
- `completion_wait_enqueues_during_wait=1610`
- `completion_wait_command_buffer_commit=1612`
- `encode_session_carry_first_draw_continue_active=11204`
- `encode_session_carry_active_entry_first_draw_continue_active=11204`

However, locality and Metal shape are still not promotable:

- `command_buffers=2919`, or `2.432` per present (`1.009` in H187)
- `sub_command_buffers=115`, or `0.096` per present (`0.002` in H187)
- `render_pass_begin=15288`, or `12.740` passes per present (`11.718` in H187)
- `render_pass_tile_preservation=130.240 MiB/present`
- `gpu_command_buffer_time=36.611ms/present` (`32.471ms/present` in H187)
- `encode_session_carry_first_draw_begin_pass=5933`
- `encode_session_carry_first_draw_split_rt=9229`
- active-entry loss remains semantic-heavy:
  `clear=3144`, `present=1179`

## Interpretation

The experiment answers a diagnostic pacing question: loosening the explicit
present-boundary wait lets PE/unix replay and the encoder produce command
buffers while the previous present is still completing. The large
`completion_wait_with_enqueue_ms` and wait-time commit counts are a clean signal
that this pacing family can open P4 without globally disabling the present
boundary.

It is not a production answer, and it is not a final result for the tightened
tail-gate implementation. The recovered overlap is bought with more Metal
command buffers and more render passes than the stable carrier, and GPU
command-buffer time per present rises. That means this prototype is useful as a
diagnostic upper bound for R-BACK-2.40/R-BACK-2.43 work, but promotion still
requires both a valid tail-gate rerun and open-render-encoder pass streaming or
equivalent source coalescing so the same run-ahead does not recreate the
same-key-reopen load/store wall.

The next owner is therefore not another present-boundary timing knob. It is the
EncodeSession/open-render-encoder implementation that can keep this kind of
run-ahead window while restoring the H187 command-buffer/pass locality.
