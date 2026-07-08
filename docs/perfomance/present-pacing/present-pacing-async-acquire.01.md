---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: async-acquire
order: 01
title: PRESENT_ASYNC_ACQUIRE=1
date: 2026-06-05
type: ab-test
status: rejected
source: experiments/output/app-d3d9-3dmark05-async-acquire-r1
---

# PRESENT_ASYNC_ACQUIRE=1

**Question / hypothesis.** Step 1 attributed 100% of `completion_wait_ms`
to Present-bearing CBs. Step 2 ruled out the queue-depth axis
(`MAX_FRAME_LATENCY`). The remaining present-side candidate is drawable
acquisition: `DXMT9_PRESENT_ASYNC_ACQUIRE=1` requests the drawable on the
encode thread asynchronously instead of letting `presentDrawable` block
the completion path. If acquire latency is hidden inside completion, the
toggle should reduce per-CB completion wait without disabling vsync.

**Method.** Parent-shell env prefix, same wrapper:

```
DXMT9_PRESENT_ASYNC_ACQUIRE=1 \
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --no-gputrace --suffix async-acquire-r1
```

`DXMT9_PRESENT_ASYNC_ACQUIRE` is resolved in `dxmt9_presenter.hpp`
(`resolveAcquirePolicy`) into the `Async` member of the
`AcquirePolicy` enum. Per
`agents/rules/environment_variables.rules.md`, the three acquire-policy
vars compose with priority `Async > SyncOnSubmit > PreAcquire > Sync`.
With only `Async` set, the runtime takes the async branch and the
encode thread requests the drawable concurrently with the next encode.

**Result.**

| Metric | Baseline | DSync=0 | FrameLatency=3 | AsyncAcquire=1 | Δ vs Baseline |
|---|---:|---:|---:|---:|---:|
| `process_elapsed_sec` | 251.07 | 83.02 | 251.26 | **251.61** | **+0.22%** |
| `completion_wait_ms` | 31,445.8 | 18,717.7 | 30,842.2 | 31,284.0 | −0.5% |
| `completion_present_wait_p50_ms` | 23.978 | 23.688 | 24.933 | 26.253 | +9.5% |
| `completion_present_wait_p95_ms` | 29.465 | 37.220 | 38.750 | 33.360 | +13.2% |
| `completion_present_wait_max_ms` | 47.573 | 46.411 | 53.474 | 52.247 | +9.8% |
| `completion_waits` (CB count) | 1,439 | 839 | 1,319 | 1,379 | −4.2% |
| `gpu_command_buffer_time_ms` | 4,317.5 | 946.7 | 4,259.2 | 4,290.7 | −0.6% |
| `encode_draw_cpu_ms` | 16,476.7 | 9,421.5 | 17,649.3 | 17,848.4 | **+8.3%** |
| **`present_acquire_wait_ms`** | **153.6** | 91.2 | 138.6 | **96.0** | **−37.5%** |
| `present_acquire_wait_max_ms` | (n/a) | n/a | n/a | 4.651 | n/a |
| `present_boundary_wait_ms` | 0.0 | 0.0 | 526.4 | 0.0 | unchanged |

**Mechanism.** The toggle did exactly what its name advertises:
`present_acquire_wait_ms` dropped from 153.6 ms to 96.0 ms (−37.5%),
and the new per-acquire max of 4.651 ms shows acquisitions are no
longer blocking on the completion path. But:

- `present_acquire_wait_ms` was already < 0.5% of the total wait
  budget. 38% of nothing is still nothing.
- `completion_present_wait_ms` (the dominant axis) is *worse*: p50
  23.978 → 26.253 ms (+9.5%).
- `encode_draw_cpu_ms` rose +8.3% — the encode thread now does the
  acquire work additively, which competes with draw-record processing.

The wallclock is functionally unchanged (Δ +0.54 s on 251 s = noise).

**Verdict.** Rejected. Async drawable acquire reduces the counter it is
named for, but the axis is not load-bearing for this workload. The
ceiling stays at the per-CB completion-present wait (≈ 24-26 ms,
i.e., one vsync slot + occasional miss), and the toggle adds small
encode-thread cost without reducing it.

What three steps now confirm together:

1. The 28-31 s `completion_wait_ms` is 100% Present-bearing.
2. Three present-side knobs (`DXMT9_MAX_FRAME_LATENCY`,
   `DXMT9_CAP_FRAME_LATENCY_TO_BACKBUFFERS=0`,
   `DXMT9_PRESENT_ASYNC_ACQUIRE`) cannot recover fps under
   `DXMT9_LAYER_DISPLAY_SYNC=1`.
3. The only knob that does (`DXMT9_LAYER_DISPLAY_SYNC=0`) is a
   diagnostic — it breaks the compositor pacing contract and is not a
   production option.

This forecloses the present-side knob space. The remaining path is
**reducing per-CB encode cost so the frame fits in a single 16.67 ms
vsync slot**. That work lives in [state-churn-encode](../state-churn-encode.md) and the
follow-up [present-pacing-encode-budget.01](present-pacing-encode-budget.01.md).

**Verification that the env var reached the runtime.** Acquire wait
p50 0.067 ms / p95 0.086 ms / max 4.651 ms are non-zero and a strict
subset of the baseline acquire envelope, confirming the acquire policy
moved from sync to async. The toggle path is wired.
