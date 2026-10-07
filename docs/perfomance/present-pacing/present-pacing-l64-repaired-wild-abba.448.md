---
domain: present-pacing
workload: STALKER CoP Day high max-threads 2 snd_targets 16
title: "Repaired-image L0/L64 wild ABBA: no established FPS advantage"
type: experiment
status: measured-no-promotion
updated: 2026-10-07
source: traces/stkcop-l64-h448/{run.py,analyze.py,summary.json}; traces/stkcop-l64-h448/{a1,b1,b2,a2}/{invocation.json,completion.json,loading-ui.json,loading.png,scene-later-ui.json,scene-later.png}; experiments/output/app-d3d9-stkcop-bench-l64-h448-{a1,b1,b2,a2}/{result.json,daybenchmark.result,stkcop-requested.ltx,stkcop-after-run.ltx,stkcop-controls.txt}
related: docs/perfomance/present-pacing/present-pacing-l64-depth-publication-fix.447.md; docs/perfomance/present-pacing/present-pacing-v4-final-grouping.443.md
---

# Repaired L0/L64 wild ABBA

Commit `34e57cb052c885a5e841f76ce17793e331d478d6` fixes depth/stencil
live-out preservation before measuring either policy. This is a policy
comparison on the repaired image, not a pre/post repair comparison against
the corrupted H444–H446 output.

## Conditions and receipts

- Sikarugir `sikarugir-cx-24.0.7`; Day/high, 1280x720 windowed,
  `-max-threads 2`, `snd_targets=16`.
- Order: limit 0, 64, 64, 0; 20-second pauses between runs.
- Perf profile, perf counters and frame sampling OFF. No supply/query observer,
  Metal capture or validation. No concurrent build, test or other benchmark
  launched by this session. Background OS activity is not controlled.
- Every catalogue run completed successfully without timeout. All five staged
  image receipts match across runs and match H447's verified image; provider
  SHA-256 prefix `7f45561ddf0fd22f`. Requested/final/control configuration bytes
  also match, asserted by `analyze.py`.
- Computer-use restored the window during a visually verified loading screen.
  The scheduled +65-second images were still loading; they do not qualify
  scene correctness. All +125-second images show an actual scene with normal
  ground/buildings. This is sampled qualitative evidence, not full-frame exact
  equality, continuous foreground monitoring, or full-route visual coverage.
  The same small screenshot schedule applies to each run; its cost is not
  subtracted from engine FPS.

## Results

Average FPS comes directly from `daybenchmark.result` / `[general] average`.
Frame-time percentiles invert the engine's ordered `per_frame_stats` values;
they are not independently measured Present or GPU latency. The final ten
seconds use the reconstructed frame-time timeline, fractionally clipping its
first overlapping frame. Reconstructed durations span 118.34–119.13 seconds.

| Run | Limit | Average FPS | p95 ms | p99 ms | Max ms | Frames >100 ms | Last 10 s FPS |
|---|---:|---:|---:|---:|---:|---:|---:|
| A1 | 0 | 31.4102 | 59.69 | 79.06 | 266.70 | 23 | 18.31 |
| B1 | 64 | 31.2682 | 63.50 | 88.03 | 697.34 | 23 | 15.85 |
| B2 | 64 | 30.8377 | 64.64 | 87.80 | 294.78 | 29 | 17.69 |
| A2 | 0 | 30.2034 | 62.67 | 89.95 | 340.84 | 27 | 17.26 |

The arithmetic mean of the two engine-average values is **30.8068 FPS** for
L0 and **31.0529 FPS** for L64, a **+0.80%** difference. Forward B1/A1 is
**−0.45%**; reverse B2/A2 is **+2.10%**. Baseline A2/A1 itself changes
**−3.84%**. With only two samples per policy and opposite-signed paired
outcomes, this is not an established FPS gain.

L64 p95 is worse in both paired comparisons. p99 and final-ten-second FPS
have mixed paired signs, and B1 contains a 697 ms outlier. The outlier is
retained; no frame is removed after observing the result, and its owner is
not attributable from this observer-OFF experiment.

## Decision

Keep limit 0 as the default. The correctness repair remains necessary
independently of this near-flat policy result. Do not revive H444's apparent
gain or promote L64 based on the +0.80% mean. H443's old-image cold CPU result
does not establish current-image CPU savings or explain the wild frame tail.

The next CPU-focused comparison, if pursued, is to refresh fixed-input
image-bound CPU/GPU qualification on this repaired image and isolate which
replay/encode work L64 actually saves. More wild FPS repeats alone will not
identify the cost owner. No runtime, default or README FPS change is made here.
