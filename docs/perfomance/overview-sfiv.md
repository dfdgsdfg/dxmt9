---
domain: root
workload: SFIV Benchmark (D3D9Ex)
title: "SFIV Benchmark Performance — Investigation Map"
type: root-overview
status: current
updated: 2026-07-25
source: experiments/output/app-d3d9-sfiv-benchmark-solo-clean-r1-20260712; experiments/output/app-d3d9-sfiv-benchmark-at-immediate-sfiv-r2-20260714; experiments/output/app-d3d9-sfiv-benchmark-default-passcoalesce-r1-20260725; experiments/output/app-d3d9-sfiv-benchmark-default-passcoalesce-perf-r1-20260725; experiments/output/app-d3d9-sfiv-benchmark-final-release-r1-20260725
related: docs/perfomance/log.md; docs/perfomance/overview.md; docs/perfomance/overview-3dmark05-gt1.md; docs/perfomance/present-pacing/present-pacing-sfiv-scene-pass-stall.204.md; docs/perfomance/present-pacing/present-pacing-sfiv-shader-cost-attribution.205.md
---

# SFIV Benchmark Performance — Investigation Map

SFIV Benchmark (`app-d3d9-sfiv-benchmark`, D3D9Ex, windowed 1280x720,
interval-one, Sikarugir-CX 24.0.7 Wine) is the primary D3D9Ex performance and
visual-effects workload. The July 2026 shader-stall investigation remains
valuable history, but its `~11-16 FPS` conclusion is no longer the current
runtime state.

## Current Measured Baseline

> **Re-measured 2026-07-31 at `890d78b1`: `44.624` sampled FPS**
> (`43.836` / `45.413` over two duration-matched runs), wall p50/p95
> `16.703 / 49.521ms`, GPU-CB p50/p95 `2.779 / 8.137ms`, zero GPU errors — see
> the [root table](overview.md#current-multi-workload-baseline). This confirms
> the `44.668` below rather than replacing it: `-0.1%`, and the game's own
> overlay reads `AVERAGE: 44.94` on the `r2` capture. SFIV is unaffected by
> `d63f7a65`, consistent with its cause already having been fixed for its pixel
> shaders by H226.
>
> This also closes the open question
> [defselect.03](shader-codegen/shader-codegen-defselect.03.md) raised against
> this figure. Its `~11.3 fps` SFIV lanes were **debug**-profile runs
> (`619-653 MB` logs of per-call `debug:` tracing); the `perf` pair from the
> same investigation reads `43.020`. Nothing here needed re-measuring on that
> account, and the instruction not to trust `45.416` was withdrawn in
> `9a577ce8`.

Two duration-matched runs on 2026-07-19 used the V2-only command wire, the
`perf` profile, frame sampling, a 25-second capture delay, and a 110-second run
timeout. This gives about 135.7 seconds of process observation, matching the
historical V1-era sample. Both captures render the expected characters,
lighting, ink/bloom, and post effects; the current `r3` image reports a
benchmark overlay average of `47.27` FPS at the capture point. GPU errors are
zero.

| Metric | Two-run median | Run range |
|---|---:|---:|
| sampled average FPS | `44.668` | `44.415-44.922` |
| sampled frames | `5,654` | `5,610-5,698` |
| sampled wall time | `126.576s` | `126.310-126.843s` |
| wall p50 | `16.751ms` | `16.732-16.770ms` |
| wall p95 | `49.728ms` | `48.797-50.658ms` |
| GPU CB p50 | `2.725ms` | `2.678-2.771ms` |
| GPU CB p95 | `8.005ms` | `7.806-8.204ms` |
| encoded presents | `5,610` | `5,580-5,640` |

A separate long run observed 260.6 seconds and 10,740 presents. It sustained
`42.684` sampled FPS with GPU CB p50/p95 `3.233/7.703ms` and zero GPU errors.
Because its scene/loop coverage differs, it is stability evidence rather than
a third member of the duration-matched median.

The 2026-07-25 release-default spot check rebuilt commit `5dc7ca01` as
release/O3 and repeated the duration-matched window. It emits `5,654` positive
frame samples over `124.493s`, or `45.416` sampled FPS (`+1.67%` versus the
two-run median), with wall p50/p95 `16.702/47.793ms` and GPU-CB p50/p95
`3.810/8.633ms`. The capture shows Ryu, lighting, bloom/blur, glyphs, and
logos; its overlay reports `47.94` instantaneous and `46.06` average FPS.
Chunk/V2 rejects, GPU errors, pipeline failures, missing-pipeline draws, and
DCE activity are all zero. This single run confirms release health without
replacing the repeated baseline.

The 2026-07-25 env-clean default-renderer regression closes the SFIV
scene-level gate for promoted `framegraph + progressive + passcoalesce`. Its
35-second window capture reaches the rendered benchmark with Ryu, lighting,
logos, and post effects intact. A separate `perf` run records `7,320` encoded
Presents, `1,674,130` draws, GPU CB p50/p95 `3.532/6.392ms`, and zero chunk
rejects, skipped Presents, GPU command-buffer errors, pipeline-build failures,
or missing-pipeline draws. The late perf screenshot failed window-title lookup
and is not visual evidence. The debug capture's `14.45` instantaneous and
`11.41` average overlay is also not a performance baseline because verbose
logging generated about 1.5 GB before compression. These runs therefore add
default-policy correctness/stability evidence without replacing the
duration-matched `44.668` sampled-FPS baseline. Since 2026-07-29 the experiment
profile defaults to `perf` and every run records the profile it resolved in
`result.json:profile`, so a debug-profile figure like the `11.41` above is
identifiable from the artifact itself rather than re-derived from its log size.

The 2026-07-20 direct-cbuf generality gate
adds a quiet same-build ABBA pair. Sampled FPS is flat-positive
(`45.544 -> 45.694`, `+0.33%`), draw/chunk CPU falls `32.57%/12.57%`, argbuf
setup/binds become zero, and the character/effect captures plus error counters
remain clean. Per-frame GPU-time p50/p95 increases `8.95%/4.34%`, however, so
SFIV is the main reason not to interpret the default-on CPU-path promotion as
a universal GPU or FPS win. Its GPU-phase behavior remains a post-promotion
monitoring item, with explicit value `0` available as the rollback lane.

## Performance Improvement Versus the V1 Era

The closest duration-matched historical sample is
`solo-clean-r1-20260712`: 135.36 seconds, 1,500 presents, and GPU CB p50/p95
`110.117/126.235ms`. Compared with the current two-run medians:

| Metric | V1-era sample | Current V2-only runtime | Change |
|---|---:|---:|---:|
| encoded presents | `1,500` | `5,610` | `+274%` |
| process throughput | `11.082 presents/s` | `41.344 presents/s` | `+273%` |
| GPU CB p50 | `110.117ms` | `2.725ms` | `-97.5%` |
| GPU CB p95 | `126.235ms` | `8.005ms` | `-93.7%` |

This is a real cumulative performance improvement, but it is not a controlled
command-wire comparison. A 2026-07-14 pre-V2 `at-immediate-sfiv-r2` run had
already reached `40.344` presents/s and GPU CB p50/p95 `3.167/8.932ms`.
Therefore most of the SFIV gain predates V2; the current result shows that the
V2-only runtime preserves that gain and remains stable, not that V2 alone
caused it.

## Current Interpretation

- **The former 88-96ms scene-pass cluster is no longer the current owner.**
  The July 12 traces correctly attributed the old build's `~11-16 FPS` wall to
  a data-dependent 11-fullscreen-quad effect-composite fragment path. Current
  GPU CB p50/p95 is only `2.725/8.005ms`; the old cluster is absent from the
  run-level distribution.
- **The D3D9Ex/offload path remains healthy.** `PresentEx` continues through
  chunk replay and ordinal pacing, the opaque-depth index-cache predicate has
  no useful SFIV matches, and the new runs have no GPU or queue errors.
- **The visual effect path is active.** Current captures show the expected
  post-processing and lighting rather than achieving the FPS gain by dropping
  shaders, bloom, or scene rendering.
- **The frame anatomy remains useful.** Historical DAG evidence describes a
  stable 25-pass frame (23 render, one blit, one present), including the
  13-pass ink/bloom RAW chain and several clear-only passes. That shape remains
  a regression map, but clear DCE is second-order against the current baseline.

The prior black/glyph flicker report was not reproduced in 242 internal
backbuffer captures and was linked to concurrent frontmost manipulation of a
different benchmark. The current quiet runs also show no such rendering
failure.

## Open Gates

1. Preserve `44.668` sampled FPS and the current effects capture as the default
   SFIV regression gate. A candidate must not trade away bloom, ink, lighting,
   glyphs, or benchmark progression.
2. If exact ownership of the July 12-to-14 jump becomes important, use a
   preserved-binary or commit bisect with the same 135.7-second window. The
   present artifacts prove when the gain existed, not which single change
   caused it.
3. Investigate the remaining wall p95 (`49.728ms`) only with phase-aligned
   frame samples. The old residual-cbuf-hoisting item is no longer a primary
   target unless a fresh trace shows the former scene-pass cluster returning.

## Measurement Notes

- Use frame-sampled FPS for current A/B work. Process-wide presents/elapsed is
  retained only for comparison with older artifacts that lack frame samples.
- `offload_commit_app_cpu_ms` includes interval-one ordinal pacing and cannot
  be compared directly with uncapped GT1.
- Historical `DXMT9_PERF_ENCODER_GPU_TIME=1` rows degenerated to CB-granular
  windows on SFIV. Use xctrace `metal-gpu-intervals` joined by encoder ID when
  a new GPU attribution is required.
