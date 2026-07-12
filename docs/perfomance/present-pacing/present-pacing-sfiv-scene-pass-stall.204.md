---
domain: present-pacing
workload: SFIV Benchmark (D3D9Ex)
title: "Present-Pacing #204 - SFIV Scene-Pass Frame-Period Stall Owns The Frame Wall"
type: leaf
status: current
updated: 2026-07-12
source: traces/app-d3d9-sfiv-benchmark-20260712-gpuintervals/analysis/gpu-intervals-summary.md; experiments/output/app-d3d9-sfiv-benchmark-gpu-intervals-r1-20260712/result.json; experiments/output/sfiv-dag-20260712
related: docs/perfomance/overview-sfiv.md; docs/perfomance/present-pacing/log.md
---

# Present-Pacing #204 - SFIV Scene-Pass Frame-Period Stall Owns The Frame Wall

## Question

SFIV (D3D9Ex) runs ~11-16fps while `gpu_command_buffer_time_p50` reads
~110-118ms per CB, which first suggested "GPU-bound". Which encoder owns the
GPU time, and is it execution or waiting?

## Method

10s `xcrun xctrace record --template "Metal System Trace" --all-processes`
during the benchmark fight (113 presents in window), exported
`metal-gpu-intervals` + `metal-application-encoders-list`, joined depth-1 GPU
intervals to dxmt9 encoder labels by `encoder-id`, and computed per-channel
busy as the union of depth-0 spans per process.

## Results

- Fragment channel 84.9% busy; **SFIV owns 80.6%** (union of its CB spans),
  IDE/compositor 0.2% — desktop contention rejected.
- Labeled encoder execution inside those spans totals only ~2.2s/10s
  (RenderPass 2,096ms = 18.55ms/present, Clear 16.2ms, Present 10.6ms) →
  **~5.9s of SFIV CB channel occupancy is stalled, not executing**
  (head-of-line blocking).
- The scene pass (rt=`0x300000100000006`, depth=`0x300000100000005`,
  12 draws, 720p) is bimodal across 112 instances: 86 at ~0.2ms; 26 at
  16-96ms hard-clustered at **88-96ms ≈ one frame period** (88.5ms avg frame
  in window), recurring every 3-10 frames (seq gaps 0-10, no content
  clustering), several split-and-resumed (29.9+60.1, 73.2+16.2, 55.0+34.0ms
  for one seq) — the signature of a wait, not shader work.
- Everything else is cheap: all other render passes sum <2ms/frame; vertex
  0.33ms/present. Frame anatomy and the dead-clear inventory live in the
  [render-pass-store SFIV note](../render-pass-store/log.md) and the
  [SFIV map](../overview-sfiv.md).

## Reading

The scene pass periodically blocks on a GPU-side dependency for ~one frame
period while holding the fragment channel. Leading hypothesis: **cross-frame
WAR hazard on the scene RT** — frame N's write to `006` waits for frame N-1's
post-process/composite reads of `006`'s texture to retire; with ~2 CBs
pipelined the write periodically catches the in-flight previous frame, stalls
~one frame, drains the pipeline, runs fast for a few frames, and repeats
(sawtooth). SFIV's genuine GPU work is ~20ms/frame, so breaking the stall has
large FPS headroom (bounded next by app CPU under Rosetta + PE recording).

## Next gates

1. Mechanism probe: `DXMT9_MAX_FRAME_LATENCY=1` A/B — a 1-deep pipeline
   completes frame N-1 before frame N's scene pass encodes, so the 88ms
   cluster should vanish if the WAR hypothesis is right.
2. If confirmed: size and prototype RT versioning/renaming (rotate the scene
   RT allocation per frame) or narrower hazard scope.
3. `.gputrace` dependency inspection only if (1) is ambiguous.
