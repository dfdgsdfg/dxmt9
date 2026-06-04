---
domain: mini-replay-bisection
workload: 3DMark05 GT1
subcategory: replay
order: 02
title: Per-Draw Scissor Fix
date: undated
type: experiment-run
status: inconclusive
source: specs/perfomance.plan.md#L14460-L14549
---

# Per-Draw Scissor Fix

**Question / hypothesis.** The full16 replay's fragment-dominated shape
([[mini-replay-bisection-replay.01]]) was suspected to be a replay artifact: the
original 16-draw `60/2` window has 10 scissored draws (`0,268..97,768`), but the
runner applied only the first draw's non-scissored state to the whole encoder.
Does fixing per-draw scissor restore the original shape — and does it move VS
buffer write?

**Method.** `run_3dmark05_mini_replay.py` was updated to emit per-draw scissor
rects and call `setScissorRect` before each draw (`scissor_draw_count=10`). Same
16-draw manifest recaptured as `mini-replay-full16-scissor.gputrace` with Xcode
counters.

**Result.**

| Metric | Original `60/2` | Mini full16 | Mini scissor | Scissor / orig |
|---|---:|---:|---:|---:|
| GPU time | 20.327ms | 3.710ms | **1.498ms** | 0.074x |
| VS buffer write | 981.171MiB | 31.974MiB | **31.978MiB** | 0.033x |
| VS invocations | 642,001 | 54,104 | 54,104 | 0.084x |
| FS invocations | 3,296,064 | 22,057,376 | **2,963,392** | 0.899x |
| Pixels rasterized | 14,020,864 | 21,270,944 | 2,176,960 | 0.155x |
| Vertex stage time | 96.06% | 26.11% | 64.37% | 0.670x |

**Verdict.** INCONCLUSIVE (but a real fidelity fix). Per-draw scissor removed the
artificial fragment-dominated shape: GPU `3.710→1.498ms`, FS inv `22.1M→3.0M`,
pixels rasterized ~9.8x lower, vertex-stage time `26.11%→64.37%`. But
`VS Buffer Device Memory Bytes Written` did **not** move (`31.974→31.978MiB`, same
54,104 VS inv, 619.8B/VS inv). This cleanly separates scissor pollution from the
real gap: the missing owner is wider-pass vertex/tiler amplification, not fragment
overdraw. Suspicion shifts to depth attachment content.

**Related.** [[mini-replay-bisection]] · [[mini-replay-bisection-replay.01]] ·
[[mini-replay-bisection-depth.01]] · [[hidden-backend-storage]] · [[overview]]
