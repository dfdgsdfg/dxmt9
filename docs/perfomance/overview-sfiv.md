---
domain: root
workload: SFIV Benchmark (D3D9Ex)
title: "SFIV Benchmark Performance — Investigation Map"
type: root-overview
status: current
updated: 2026-07-12
source: docs/perfomance/index.md
related: docs/perfomance/log.md; docs/perfomance/overview.md; docs/perfomance/overview-3dmark05-gt1.md
---

# SFIV Benchmark Performance — Investigation Map

SFIV Benchmark (`app-d3d9-sfiv-benchmark`, D3D9Ex, windowed 1280x720,
interval-one, sikarugir Wine runtime) is the first D3D9Ex workload validated
and profiled on the post-cleanup engine-default runtime (H216-H221 era).
This map is workload-level only; detailed rows and leaves live in the owning
domains, mirroring the [3DMark05 GT1 map](overview-3dmark05-gt1.md).

## Settled

- **D3D9Ex validation on the engine-default trio: pass.** Healthy rendering,
  zero GPU errors, `present_boundary_skipped` exactly 1.0/present —
  `PresentEx` rides the chunk-replay path and the offload's per-present
  ordinal pacing replaces the inline boundary identically to GT1
  (R-BACK-2.51(g)/(h)). Offload replay 0.95-2.0ms/present. The opaque-depth
  index-cache predicate never matches SFIV (`reordered_index_cache_lookups=0`
  — no benefit, no overhead). Evidence:
  `experiments/output/app-d3d9-sfiv-benchmark-cleanup-arc-validation-r{1,2*}-20260712`.
- **Reported black/glyph flicker is not in the rendered frames.** 242 dense
  backbuffer captures across offload on/off are black-frame-free with
  identical UI-band deltas (the benchmark's own fades); offload on/off
  presents are identical (1,560 = 1,560). The observation window coincided
  with a concurrent agent session force-frontmosting a 3DMark05 window every
  4-5s over the windowed SFIV — environmental explanation; screen-level
  re-verification stayed inconclusive (SFIV window on another Space/display
  than the recorded one). Re-check visually on a quiet desktop if it recurs.
  Evidence: `experiments/output/app-d3d9-sfiv-benchmark-flicker-{A-offload-on,B-offload-off}-20260712`,
  `experiments/output/sfiv-flicker-{A-offload-on,B-offload-off}` capture sets.
- **The frame wall (~11-16fps) is a periodic frame-period GPU stall in the
  scene pass, not render-pass fragmentation, not desktop contention, and not
  the engine-default trio.** SFIV's own GPU execution is ~20ms/frame; the
  scene pass (rt=`006`+depth=`005`) is bimodal 0.2ms vs 88-96ms (= one frame
  period) every 3-10 frames with split-and-resumed encoder intervals, while
  SFIV's CBs occupy the fragment channel 80.6% (~5.9s/10s open-but-stalled).
  Leading hypothesis: cross-frame WAR hazard on the scene RT (frame N's write
  waits for frame N-1's post-chain/composite reads). Owning row/leaf:
  [present-pacing H222](present-pacing/log.md) /
  [present-pacing-sfiv-scene-pass-stall.204](present-pacing/present-pacing-sfiv-scene-pass-stall.204.md).
- **Frame anatomy: fixed 25-pass shape** (23 render + blit + present,
  44 draws): a 13-pass ink/bloom RAW post chain (unmergeable), the scene
  pass, HUD/composite, full-surface StretchRect, and 5 clear-only passes of
  which 3 are dead clears (`004`, `01c/01d`, `016/012`) plus a duplicate
  same-attachment clear pair — pass floor ≈16/23, all second-order for FPS
  (<2ms/frame GPU outside the scene pass). Owning note:
  [render-pass-store log — SFIV cross-workload note](render-pass-store/log.md).
  Evidence: `experiments/output/sfiv-dag-20260712` (framegraph DAG dumps,
  frames 799-801).

## Open gates

1. **Stall mechanism confirm**: `DXMT9_MAX_FRAME_LATENCY=1` A/B with a 10s
   Metal System Trace — a 1-deep pipeline should erase the 88ms cluster if
   the cross-frame WAR hypothesis is right.
2. **RT versioning/renaming opportunity sizing** once (1) confirms; then a
   prototype that rotates the scene RT allocation per frame or narrows the
   hazard scope.
3. Dead-clear DCE / clear folding as a framegraph follow-up (second-order).

## Measurement notes

- `offload_commit_app_cpu_ms` is pacing-dominated for vsync-paced apps (it
  includes the present-ordinal wait) — do not compare to GT1's uncapped
  values.
- `DXMT9_PERF_ENCODER_GPU_TIME=1` degenerated to CB-granular windows on this
  workload (per-encoder p50 71.7ms, sums 22x the CB window) and cannot
  attribute within the CB here; use xctrace `metal-gpu-intervals` joined to
  `metal-application-encoders-list` by `encoder-id` instead. Local artifacts:
  `traces/app-d3d9-sfiv-benchmark-20260712-gpuintervals/` (trace, XML
  exports, `analysis/gpu-intervals-summary.md`).
