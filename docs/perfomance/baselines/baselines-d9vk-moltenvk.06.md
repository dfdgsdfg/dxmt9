---
domain: baselines
workload: 3DMark05 GT1/GT2/GT3 + 3DMark06 GT1/GT2/HDR1/HDR2 + SFIV
subcategory: renderer-comparison
order: 6
title: Sikarugir D9VK and MoltenVK D3D9 Baseline
date: 2026-08-29
type: experiment-run
status: accepted-baseline
source: experiments/output/conf-d3d9-triangle-d9vk-mvk1210-smoke32-20260829; experiments/output/app-d3d9-3dmark05-d9vk-mvk-{gt1,gt2,gt3}-r1-20260829; experiments/output/app-d3d9-3dmark06-d9vk-mvk-gt1-r5-20260829; experiments/output/app-d3d9-3dmark06-d9vk-mvk-{gt2,hdr1,hdr2}-r1-20260829; experiments/output/app-d3d9-sfiv-benchmark-d9vk-mvk-final-r1-20260829; experiments/output/app-d3d9-sfiv-benchmark-wined3d-sikarugir-final-r1-20260829
related: README.md; docs/perfomance/baselines/baselines-3dmark06-wined3d.05.md; docs/perfomance/baselines/baselines-sfiv-wined3d.07.md; specs/benchmarks/requirements.md
---

# Sikarugir D9VK and MoltenVK D3D9 Baseline

## Question

What throughput does Sikarugir's D3D9-to-Vulkan-to-Metal path deliver on the
wild workloads already tracked for dxmt9?

## Stack Identity

Sikarugir exposes D3D9 through D9VK, while its DXVK toggle is the D3D10/11
path. The measured stack used:

| component | identity |
|---|---|
| Wine | Sikarugir-CX 24.0.7, Wine 9.0 |
| D3D9 provider | [Sikarugir D9VK `v1.10.3-20250511`](https://github.com/Sikarugir-App/d9vk/releases/tag/v1.10.3-20250511), macOS async x86 build |
| Vulkan-to-Metal provider | [Sikarugir MoltenVK `v1.2.10`](https://github.com/Sikarugir-App/MoltenVK/releases/tag/v1.2.10), `macos-1.2.10-cxp20241028-UE4hack-zeroinit` |
| GPU | Apple M1, 8-core GPU |

The x86 D9VK `d3d9.dll` SHA-256 is
`22511d1fbb15cdbc5365dfcb231f028af427533bfb6957505720dac0aca98e3e`.
The locally ad-hoc-codesigned MoltenVK image used for the isolated experiment
has SHA-256
`6c9a2fc73fc307f2e67628e75bb60ea4a8f18f1ade6124273f4ccd3b51d68940`.
The native triangle smoke reached the Vulkan device and passed its green-pixel
oracle before the benchmark runs.

## Method

D9VK was staged app-locally and loaded with `d3d9=n,b`; dxmt9 staging was
skipped. Each recorded lane followed a cache-building run. `DXVK_STATE_CACHE=1`
was enabled, the D9VK HUD and dxmt9 counters were disabled, and the recorded
runs used the repository's `perf` experiment profile.

3DMark05 ran at 1024x768. Its values come from the benchmark-owned `.3dr`
`Result.xml`. 3DMark06 ran at 1280x800 with no anti-aliasing and optimal
filtering. Advanced Edition did not emit a reusable `.3dr`, so its Result
Details screenshots are authoritative. SFIV ran at 1280x800; its value is the
benchmark overlay's cumulative `AVERAGE` at the 240-second capture point, not
an instantaneous FPS or final score-screen value.

## Result

| workload | D9VK + MoltenVK FPS | Sikarugir WineD3D FPS | current dxmt9 FPS |
|---|---:|---:|---:|
| 3DMark05 GT1 | **`28.769`** | `31.4` | `31.2` |
| 3DMark05 GT2 | **`27.041`** | `30.7` | `30.1` |
| 3DMark05 GT3 | **`46.576`** | `61.0` | `66.5` |
| 3DMark06 GT1 | **`16.062`** | `15.976` | `15.7` |
| 3DMark06 GT2 | **`19.161`** | `18.897` | `18.1` |
| 3DMark06 HDR1 | **`46.536`** | `37.620` | `37.0` |
| 3DMark06 HDR2 | **`17.071`** | `15.577` | `16.0` |
| Street Fighter IV Benchmark | **`46.78`** | `43.80` | `44.3` |

The 3DMark05 D9VK path is `8.4-23.6%` below Sikarugir WineD3D and
`7.8-30.0%` below the current dxmt9 values. The 3DMark06 Result Details values
are `0.5-23.7%` above Sikarugir WineD3D, led by HDR1. SFIV D9VK is numerically
`6.8%` above WineD3D, but D9VK used 1280x800 while WineD3D used 1280x720. Its
240-second overlay observer is also not a matched A/B against dxmt9's 1280x720
frame sampling.

## Limits

These are single recorded runs after cache-building runs, not interleaved ABBA
trials. The D9VK build is the Sikarugir macOS async variant, so the result does
not generalize to upstream D9VK/DXVK releases or another MoltenVK build. The
3DMark06 and SFIV mode/observer differences also prevent strict ratios against
the current dxmt9 column; the table records renderer shape and a reproducible
reference lane.
