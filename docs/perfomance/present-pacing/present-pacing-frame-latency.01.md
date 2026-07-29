---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: frame-latency
order: 01
title: MAX_FRAME_LATENCY=3 + CAP_FRAME_LATENCY_TO_BACKBUFFERS=0
date: 2026-06-05
type: ab-test
status: rejected
outdated: evidence-missing
source: experiments/output/app-d3d9-3dmark05-frame-latency-3-r1
---

# MAX_FRAME_LATENCY=3 + CAP_FRAME_LATENCY_TO_BACKBUFFERS=0

> **Outdated — every artifact this leaf cites in `source:` is gone from disk.** The numbers below cannot be re-derived or re-checked. Kept as history; do not cite it as current evidence.

**Question / hypothesis.** Display sync (`DXMT9_LAYER_DISPLAY_SYNC=0`)
disabled gave +199% scene throughput in
[present-pacing-display-sync.01](present-pacing-display-sync.01.md), but tearing it isn't a production
option. The hypothesis: if the runtime allows more frames in-flight,
short per-frame slowdowns are amortised across multiple vsync windows
instead of forcing the current frame to slip a slot. Tested by raising
`DXMT9_MAX_FRAME_LATENCY` to 3 and lifting the backbuffer cap with
`DXMT9_CAP_FRAME_LATENCY_TO_BACKBUFFERS=0`.

**Method.** Parent-shell env prefix, same wrapper as Step 1:

```
DXMT9_MAX_FRAME_LATENCY=3 \
DXMT9_CAP_FRAME_LATENCY_TO_BACKBUFFERS=0 \
bash scripts/tools/run_3dmark05_perf_probe.sh \
  --no-gputrace --suffix frame-latency-3-r1
```

`DXMT9_MAX_FRAME_LATENCY` is consumed in `dxmt9_device.cpp:104`
(stored as `maxFrameLatency_`, default `core::kDefaultFrameLatency`).
`DXMT9_CAP_FRAME_LATENCY_TO_BACKBUFFERS` is read in
`dxmt9_command_queue.cpp:856`. Both are read once at process init.

Verification that the toggle reached the runtime: `present_boundary_wait_ms`
went from `0` (baseline) to `526.4 ms`, confirming a behaviour change in
the present-policy code path. (The baseline `0` indicates the present
boundary wait was previously inactive; raising the latency cap activated
it.)

**Result.**

| Metric | Baseline | DSync=0 (Step 1) | FrameLatency=3 (this) | Δ vs Baseline |
|---|---:|---:|---:|---:|
| `process_elapsed_sec` | 251.07 | 83.02 | **251.26** | **+0.07%** |
| Implied fps | 1.00× | 2.99× | 1.00× | **unchanged** |
| `completion_wait_ms` | 31,445.8 | 18,717.7 | 30,842.2 | −1.9% |
| `completion_present_wait_p50_ms` | 23.978 | 23.688 | 24.933 | +4.0% |
| `completion_present_wait_p95_ms` | 29.465 | 37.220 | 38.750 | +31.5% |
| `completion_present_wait_max_ms` | 47.573 | 46.411 | 53.474 | +12.4% |
| `completion_waits` (CB count) | 1,439 | 839 | 1,319 | −8.3% |
| `gpu_command_buffer_time_ms` | 4,317.5 | 946.7 | 4,259.2 | −1.4% |
| `encode_draw_cpu_ms` | 16,476.7 | 9,421.5 | 17,649.3 | +7.1% |
| `present_acquire_wait_ms` | 153.6 | 91.2 | 138.6 | −9.8% |
| `present_acquire_wait_p50_ms` | (not captured) | (not captured) | 0.113 | n/a |
| `present_boundary_wait_ms` | 0.0 | 0.0 | **526.4** | **NEW** |

**Mechanism.** With `DXMT9_LAYER_DISPLAY_SYNC=1` (default), Apple's
`CAMetalLayer` paces `presentDrawable` to the compositor's refresh rate
regardless of how many CBs are queued ahead of the GPU. Adding more
in-flight frames just lengthens the buffer queue; the actual present
rate is the display refresh.

Concretely the p50 wait moved from 23.978 ms to 24.933 ms (slightly
*worse*), the p95 from 29.465 ms to 38.750 ms (significantly worse), and
`process_elapsed_sec` is functionally identical (Δ +0.19 s on a 251 s
scene, well inside the run-to-run noise).

The new `present_boundary_wait_ms = 526 ms` is the indicator that the
runtime *did* take a different code path — the boundary policy that was
previously short-circuited is now contributing. But the additional
boundary wait is dwarfed by the unchanged completion-present wait, so
the net effect on wallclock is null.

**Verdict.** Rejected. `MAX_FRAME_LATENCY=3` plus
`CAP_FRAME_LATENCY_TO_BACKBUFFERS=0` does not move wallclock under
`DXMT9_LAYER_DISPLAY_SYNC=1`. The display compositor's vsync pacing is
a stronger constraint than the runtime's in-flight frame limit; queuing
more frames does not increase the actual present rate.

What this tells us:

- The in-flight frame count is *not* the bottleneck axis. The
  vsync-pacing wait happens *per Present command buffer*, not *per
  pipeline depth*.
- The actual ceiling sits in `encode_draw_cpu_ms` (~11 ms/CB at this
  workload, well above the budget headroom). Owned by
  [state-churn-encode](../state-churn-encode/index.md).
- p95 / max getting worse with this knob suggests the deeper queue
  occasionally introduces tail latency without a throughput win.

**Next.** [present-pacing-async-acquire.01](present-pacing-async-acquire.01.md) —
`DXMT9_PRESENT_ASYNC_ACQUIRE=1` tests whether async drawable acquire
helps independently. After that, attention shifts to per-CB encode cost
reduction since that's the budget axis the data points to.
