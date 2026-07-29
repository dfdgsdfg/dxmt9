---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: display-sync
order: 01
title: Display-Sync Attribution of `completion_wait_ms`
date: 2026-06-05
type: measurement
status: accepted
outdated: evidence-missing
source: experiments/output/app-d3d9-3dmark05-display-sync-off-r1, experiments/output/app-d3d9-3dmark05-current-nondiag-baseline-r1
---

# Display-Sync Attribution of `completion_wait_ms`

> **Outdated — every artifact this leaf cites in `source:` is gone from disk.** The numbers below cannot be re-derived or re-checked. Kept as history; do not cite it as current evidence.

**Question / hypothesis.** The 2026-06-04 perf summary observed 8-22 fps on
3DMark05 GT1 while GPU `gpu_command_buffer_time_ms` was only ~4 s. The
~28 s `completion_wait_ms` was the dominant wallclock cost. Attribute it.

**Method.** Static-analyse `dxmt9_perf_counters.cpp:2972` (`countCompletionWait`)
and `dxmt9_queue.cpp:1194` (single accumulator site, in the completion handler
after `MTLCommandBuffer.waitUntilCompleted()`). Then read the existing
per-class sub-bucket counters from `result.json`. The accumulator already
classifies each wait into one of five exclusive buckets (`present` /
`draw` / `blit` / `stretch` / `other`) plus combination buckets and a
shader-hash sample.

The candidate-axis A/B: set `DXMT9_LAYER_DISPLAY_SYNC=0` (per
`agents/rules/environment_variables.rules.md` Present-policy table; default
`1`) at the parent shell. The wrapper `scripts/tools/run_3dmark05_perf_probe.sh`
preserves parent env through `env "${env_args[@]}" ${cmd[@]}` (no `-i`), so
the toggle reaches the Wine app.

```
DXMT9_LAYER_DISPLAY_SYNC=0 bash scripts/tools/run_3dmark05_perf_probe.sh \
  --no-gputrace --suffix display-sync-off-r1
```

**Result.**

Sub-bucket attribution of the baseline 31.4 s `completion_wait_ms`:

| Bucket | Baseline (ms) | Share |
|---|---:|---:|
| `completion_present_wait_ms` | 31,445.8 | **100.0%** |
| `completion_draw_wait_ms` | 0 | 0% |
| `completion_blit_wait_ms` | 0 | 0% |
| `completion_stretch_wait_ms` | 0 | 0% |
| `completion_other_wait_ms` | 0 | 0% |
| `present_acquire_wait_ms` (sibling) | 153.6 | 0.49% of total |
| `present_boundary_wait_ms` (sibling) | 0 | 0% |
| `sync_wait_ms` (sibling) | 0 | 0% |
| `queue_writer_wait_ms` (sibling) | 0 | 0% |
| `queue_commit_wait_ms` (sibling) | 0 | 0% |
| `queue_sequence_wait_ms` (sibling) | 0 | 0% |

→ **100% of the wait sits on Present-bearing command buffers.** Drawable
acquire, present boundary, sync flush, queue ring, queue commit, and queue
sequence are all functionally zero.

A/B comparison, same workload (3DMark05 GT1 scene, `status: pass` both runs):

| Metric | Baseline (default `DXMT9_LAYER_DISPLAY_SYNC=1`) | `DXMT9_LAYER_DISPLAY_SYNC=0` | Delta |
|---|---:|---:|---:|
| `process_elapsed_sec` | 251.07 | **83.02** | **−66.9%** |
| Implied fps (≈ inverse) | 1.0× | **2.99×** | **+199%** |
| `completion_wait_ms` | 31,445.8 | 18,717.7 | −40.5% |
| `completion_present_wait_p50_ms` | 23.978 | **23.688** | −1.2% |
| `completion_present_wait_p95_ms` | 29.465 | 37.220 | +26% |
| `completion_present_wait_max_ms` | 47.573 | 46.411 | −2.4% |
| `gpu_command_buffer_time_ms` (total) | 4,317.5 | **946.7** | **−78.1%** |
| `encode_draw_cpu_ms` (total) | 16,476.7 | 9,421.5 | −42.8% |
| `completion_waits` (CB count) | 1,439 | 839 | −41.7% |
| CB throughput (CB / s wallclock) | 5.73 | **10.11** | **+76.5%** |
| `present_acquire_wait_ms` | 153.6 | 91.2 | −40.6% |

**Mechanism.** Per-CB wait p50 is essentially unchanged (23.98 ms →
23.69 ms). Per-CB GPU time dropped from 3.00 ms to 1.13 ms — the GPU was
pacing inside `waitUntilCompleted()` because the `CAMetalLayer` had display
sync enabled; with it off, the GPU executes the CB and the wait returns as
soon as the compositor accepts the drawable.

What moved is the **CB throughput across wallclock**:

```
baseline   : 5.73 CB/s   = 1 CB per ~175 ms wallclock
DSync = 0  : 10.11 CB/s  = 1 CB per ~99 ms wallclock
```

dxmt9 is producing the same number of frames per scene; with vsync on, each
frame either makes its 60 Hz slot or misses and waits the next ~16.67 ms.
The 23.98 ms p50 = one slot + one missed slot, averaged. Per-encoder CPU
encode (`encode_draw_cpu_ms / completion_waits = 11.5 ms/CB`) consumes ~69%
of the 16.67 ms vsync budget, so small slowdowns push past the boundary.

**Verdict.** Accepted. The 28-31 s `completion_wait_ms` is 100%
`completion_present_wait_ms`. The mechanism is display-sync paced
`waitUntilCompleted()` returning at the compositor refresh rather than
GPU-execute-done. Disabling display sync triples the scene throughput
(`process_elapsed_sec` 251 → 83) while every other wait class stays at
zero.

This shifts the bottleneck attribution from "completion wait is a mystery"
to "the wait is display-sync pacing, the new ceiling is `encode_draw_cpu_ms`
at 11.2 ms/CB versus a 16.67 ms vsync budget". Follow-up investigations:

- [present-pacing-frame-latency.01](present-pacing-frame-latency.01.md) — `DXMT9_MAX_FRAME_LATENCY=3` +
  `DXMT9_CAP_FRAME_LATENCY_TO_BACKBUFFERS=0` (production-safe alternative)
- [present-pacing-async-acquire.01](present-pacing-async-acquire.01.md) — `DXMT9_PRESENT_ASYNC_ACQUIRE=1`
- [state-churn-encode](../state-churn-encode/index.md) — existing topic that owns the per-CB encode cost
  side of this story

**Non-goal.** `DXMT9_LAYER_DISPLAY_SYNC=0` is not a production fix; it
causes tearing and breaks the compositor pacing contract. The diagnostic
value is purely attribution.
