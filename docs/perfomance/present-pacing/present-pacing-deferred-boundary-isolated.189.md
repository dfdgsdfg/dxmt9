---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: runtime
order: 189
title: Isolated Deferred Present Boundary On The Baseline Shape
date: 2026-07-04
type: no-gputrace
status: rejected-isolated-p4-noop
source: experiments/output/app-d3d9-3dmark05-p4-deferred-iso-baseline-r0-20260704/result.json; experiments/output/app-d3d9-3dmark05-p4-deferred-iso-baseline-r0-20260704/3dmark05-perf-summary.md; experiments/output/app-d3d9-3dmark05-p4-deferred-iso-candidate-r1-20260704-retry1/result.json; experiments/output/app-d3d9-3dmark05-p4-deferred-iso-candidate-r1-20260704-retry1/3dmark05-perf-summary.md; experiments/output/app-d3d9-3dmark05-p4-deferred-iso-candidate-r1-20260704/result.json; traces/app-d3d9-3dmark05-p4-deferred-iso-baseline-r0-20260704/analysis/captures/frame000920.bmp; traces/app-d3d9-3dmark05-p4-deferred-iso-candidate-r1-20260704-retry1/analysis/captures/frame000920.bmp; scripts/tools/compare_3dmark05_p4_pair.py; docs/superpowers/specs/2026-07-04-gt1-p4-deferred-boundary-design.md
related: docs/perfomance/present-pacing.md; docs/perfomance/present-pacing/present-pacing-encode-session-deferred-boundary.188.md; docs/perfomance/present-pacing/present-pacing-encode-session-stable-rerun.187.md
---

# Present-Pacing H189 - Isolated deferred present boundary on the baseline shape

## Question

H188 tested `DXMT9_PRESENT_BOUNDARY_DEFERRED=1` only on top of the open-CB
`EncodeSession` carrier stack, whose own cost halves FPS, so the pacing win was
never measured in isolation. Does the tightened tail-gate implementation
(`presentSeqId + 1` target, `9c0960f5`), run standalone on the plain baseline
shape, open the P4 window and raise FPS above a paired baseline while
preserving baseline CB/pass/tile shape?

## Run

Paired 120 s supervised no-gputrace scouts on current HEAD, identical flags
except the candidate env:

```text
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix p4-deferred-iso-baseline-r0-20260704 \
  --no-gputrace --no-encoder-breakdown --frame-sampling \
  --timeout 120 --keep-frontmost \
  --capture-range 880:960:10 --capture-delay-sec 45

bash scripts/tools/run_3dmark05_perf_probe.sh \
  --suffix p4-deferred-iso-candidate-r1-20260704-retry1 \
  ... same flags ... \
  --present-boundary-deferred
```

The first candidate attempt (`...-candidate-r1-20260704`) failed at 3DMark05
startup (`returncode=1`, 54 factory bridge calls, zero draws/presents, no
dxmt9 error lines) and is kept as a startup-flake sample; the immediate retry
ran clean, so the failure is not attributed to the env.

Judged with `scripts/tools/compare_3dmark05_p4_pair.py` (gates from the design
doc).

## Verdict

Rejected as an isolated P4/FPS lever: the policy engages but is a near-no-op
on the baseline shape.

| Gate | Baseline R0 | Candidate R1 | Result |
|---|---|---|---|
| fps presents / 110 s window | `1800` | `1860` | `+3.33%`, inside the `±5%` noise band |
| p4 `completion_wait_without_enqueue_ms/present` | `26.546` | `26.400` | FAIL (needs `<=50%`) |
| p4 `completion_wait_with_enqueue_ms/present` | `0.023` | `0.000` | window did not open |
| locality CB / sub-CB / passes per present | `4.009 / 2.998 / 11.808` | `4.009 / 2.998 / 11.783` | PASS (flat) |
| locality tile preservation MiB/present | `120.760` | `120.681` | PASS (flat) |
| correctness | `status=pass`, `gpu_command_buffer_errors=0`, non-black | same | PASS |
| semantics | `present_boundary_waits=0`, `wait_ms=0.0` | `applied=1860`, `deferred=1857`, `deferred_waits=0` | gate engaged, never had to wait |

Visual spot-check: internal captures `frame000920.bmp` from both runs show the
same fully rendered firefight frame (HUD `Frame: 912`) with no current-only
artifact class; candidate HUD FPS `19` vs baseline `17`.

The structural finding is in the baseline row: **the plain baseline never
waits at the present boundary at all** (`present_boundary_waits=0`,
`present_boundary_wait_ms=0.0`, GPU command-buffer time `3.130 ms/present`
against a `~61 ms` frame). Deferring a wait that never fires cannot open P4.
The `~26.5 ms/present` no-enqueue window is owned by the producer/replay
serial path, consistent with the H225/H226 attribution
(`commit_chunk_replay_cpu_ms/present=8.424`,
`encode_chunk_cpu_ms/present=11.249`) and the H68-H72 between-call gap splits.

## Interpretation

H188's P4 opening was a carrier phenomenon, not a boundary phenomenon: under
the open-CB carrier the present boundary waited `32966.656 ms` per run (H187)
because the carrier holds all work to the present tail, so deferring that wait
mattered. On the baseline shape there is no boundary wait to defer.

Consequences for the average-FPS program:

- Present-pacing knobs are exhausted as isolated levers on the baseline
  shape; do not spend further runs on boundary-timing variants without a
  carrier that first makes the boundary wait real.
- The remaining average-FPS owners are (a) shrinking the producer/replay
  serial path itself, and (b) an overlap carrier that lets recorded work
  encode/submit during producer gaps **without adding producer-side cost** —
  H187 shows the current carrier family fails that precondition before the
  boundary policy even matters.
- Phase B (session locality restoration) should treat "does the carrier slow
  the producer?" as its first gate, ahead of pass-streaming shape recovery.
