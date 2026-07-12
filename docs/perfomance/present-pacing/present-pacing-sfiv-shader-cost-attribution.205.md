---
domain: present-pacing
workload: SFIV Benchmark (D3D9Ex)
title: "Present-Pacing #205 - The SFIV 88ms Instances Are Real Shader Work, Not A Wait"
type: leaf
status: current
updated: 2026-07-12
source: experiments/output/app-d3d9-sfiv-benchmark-latency1-probe-r3-quiet-20260712/result.json; experiments/output/app-d3d9-sfiv-benchmark-novsync-probe-r1-20260712/result.json; experiments/output/app-d3d9-sfiv-benchmark-force-frag-color-r1-20260712/result.json; experiments/output/app-d3d9-sfiv-benchmark-texwhite-probe-r1-20260712/result.json; experiments/output/app-d3d9-sfiv-benchmark-shader-attrib-r1-20260712/result.json; traces/app-d3d9-sfiv-benchmark-20260712-gpuintervals
related: docs/perfomance/present-pacing/present-pacing-sfiv-scene-pass-stall.204.md; docs/perfomance/overview-sfiv.md
---

# Present-Pacing #205 - The SFIV 88ms Instances Are Real Shader Work, Not A Wait

## Question

Leaf .204 left the ~88ms scene-pass instances attributed to a GPU-side wait
with cross-frame WAR as the leading hypothesis. Which is it — wait or work —
and what is the wait/work target?

## Probe chain (all cooled, perf profile, ~1,500-present baselines)

| Probe | presents | scene-pass slow cluster | Verdict |
|---|---|---|---|
| baseline (lat 3) | 1,500-1,560 | 26/112 at 88-96ms | reference |
| `DXMT9_MAX_FRAME_LATENCY=1` (quiet desktop, r3) | 1,500 | 36/117, same ~88ms cluster | **WAR-with-in-flight-frame REFUTED** — 1-deep pipeline changes nothing |
| `DXMT9_DISABLE_VSYNC=1` | 1,560 (`present_schedule_immediate=1560`) | — | present pacing REFUTED — fully immediate presents change nothing |
| `DXMT_DEBUG_FORCE_FRAGMENT_COLOR=1` | **5,462 (3.6x)**, CB p50 110→1.2ms | **0/444 slow** — cluster gone | **real fragment shader work CONFIRMED** |
| `DXMT_FORCE_TEXTURE_WHITE=1` | 1,620, CB p50 227ms (worse) | — | texture-fetch cost refuted; cost is ALU/shader-structure (white samples likely defeat an early-out and add work) |

A first latency-1 run (r1) appeared to universalize the stalls; its trace
showed 4.9s of WindowServer fragment bursts (11ms CB trains inside the scene
windows) from the IDE streaming this session's own output — an environmental
confound; the quiet rerun (r3) normalized it. Baseline stall windows contain
only SFIV's own CB spans back-to-back.

## The pass and its shaders

The slow pass (`rt=0x300000100000006`, `depth=0x300000100000005`, `enc=1`) has
an **invariant shape in every frame**: `programmable_draws=11`,
`primitives=22` — eleven fullscreen textured quads (the ink/paper effect
composite), five shader variants (last-draw PS rotates among
`0x26d0eb834f89ee27`, `0x3fff60885678e08d`, `0xd36fb2fb94c44dae`,
`0xdd8e0f36dd0f816b`). Dumped MSL for all scene-pass FS is modest (12-15KB,
1-iteration init loops, 1-5 samples, a few transcendentals) — no static
pathology. Same shaders + same coverage swinging 0.2ms ↔ 88ms (440x) means
the cost is **data-dependent** (effect constants/texture content on pulsing
frames — denormal/special-value arithmetic is the classic suspect).

## Verdict

- SFIV's frame wall is real, data-dependent fragment-shader cost in the
  11-fullscreen-quad effect composite pass, active on 23-31% of frames.
- Proven FPS headroom: **+264% presents** (1,560 → 5,462) with fragment cost
  removed — an upper bound (force-frag-color is correctness-invalid).
- RT versioning/renaming is dead as a candidate (targets a refuted mechanism).

## Next gates

1. Single-frame `.gputrace` of a slow frame (wine capture-layer wrapper +
   `DXMT_METAL_CAPTURE_FRAME`), Xcode per-draw profiling + shader profiler —
   real work reproduces in replay, so this names the exact draw and
   instruction hot spot.
2. Then a shader-translation fix (e.g., clamp/flush denormals, restructure the
   offending pattern) gated by the `v0.0.3`-style visual anchor for SFIV.
