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
- **The frame wall (~11-16fps) is real, data-dependent fragment-shader cost
  in the 11-fullscreen-quad effect composite pass** (rt=`006`+depth=`005`) —
  not render-pass fragmentation, not desktop contention, not the
  engine-default trio, not cross-frame hazards, and not present pacing. The
  pass is bimodal 0.2ms vs 88-96ms on 23-31% of frames with an invariant
  shape (11 draws, 22 primitives, 5 PS variants; dumped MSL statically
  unremarkable) — a 440x data-dependent swing (denormal/special-value
  arithmetic on effect-pulse frames is the classic suspect). Wait hypotheses
  were refuted by quiet-desktop `DXMT9_MAX_FRAME_LATENCY=1` (cluster
  unchanged) and `DXMT9_DISABLE_VSYNC=1` (no movement);
  `DXMT_DEBUG_FORCE_FRAGMENT_COLOR=1` erases the cluster and proves
  **+264% presents headroom** (1,560 → 5,462; CB p50 110 → 1.2ms);
  `DXMT_FORCE_TEXTURE_WHITE=1` does not help (ALU/shader-structure cost, not
  texture fetch). Owning rows/leaves:
  [present-pacing H222-H223](present-pacing/log.md) /
  [.204](present-pacing/present-pacing-sfiv-scene-pass-stall.204.md) /
  [.205](present-pacing/present-pacing-sfiv-shader-cost-attribution.205.md).
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

1. **Residual cbuf-load hoisting** (H224 direction 2): the alpha-test/fog
   variant fix landed (`0b82f69c`, H225 — SFIV CB GPU p50 `110 -> 82.9ms`
   cooled, `68.2ms` in a counters-only run, near the `60.5ms` strip bound);
   the remaining per-fragment `PsConsts` loads (and the enabled-variant
   `FfpPsConsts` loads) are still serialized dependent loads that the
   compiler cannot constant-preload. Hoist/vectorize or move to verified
   preload bindings if a further slice is worth it.
2. After the GPU fix, re-attribute the wall (app CPU under Rosetta is the
   expected next owner at ~60-68ms/frame; SFIV presents did not move with
   the CB drop — the frame cadence is no longer GPU-owned).
3. Dead-clear DCE / clear folding as a framegraph follow-up (second-order).

Closed gates: `DXMT9_MAX_FRAME_LATENCY=1` mechanism probe (WAR refuted), RT
versioning/renaming (killed), `DXMT9_ARGBUF_DIRECT_CBUF` (no change — the
argbuf-table indirection per se is not the cost). Measurement caveat: Xcode
replay profiling on an active desktop interleaves "External Process" GPU work
into encoder windows — trust per-line shape, not absolute replay numbers.

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
