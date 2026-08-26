---
domain: baselines
workload: 3DMark06 GT1/GT2/HDR1/HDR2
subcategory: renderer-comparison
order: 5
title: 3DMark06 Sikarugir and Heroic WineD3D Graphics Baselines
date: 2026-08-26
type: experiment-run
status: accepted-baseline
source: experiments/output/app-d3d9-3dmark06-wined3d-sikarugir-graphics-r9-20260826; experiments/output/app-d3d9-3dmark06-wined3d-sikarugir-identity-r5-20260826; experiments/output/app-d3d9-3dmark06-wined3d-heroic-11.16-graphics-r7-20260826; experiments/output/app-d3d9-3dmark06-wined3d-heroic-11.16-identity-r2-20260826; experiments/output/app-d3d9-3dmark06-wined3d-heroic-11.16-renderer-r3-20260826
related: README.md; docs/perfomance/baselines/baselines-wild-fps-refresh.04.md; specs/experiments/gap.md
---

# 3DMark06 Sikarugir and Heroic WineD3D Graphics Baselines

## Question

What throughput do pristine Sikarugir-CX 24.0.7 and Heroic Wine Staging 11.16
builtin WineD3D paths deliver on the four 3DMark06 graphics scenes used by the
dxmt9 baseline?

## Method

One supervised graphics-only Advanced Edition run per runtime used separate
fresh prefixes and pristine Wine roots. `d3d9=b`, native `d3dx9_28`, and
native `openal32` were selected; dxmt9 staging was skipped. The test selector
kept GT1, GT2, HDR1, and HDR2 enabled and disabled both CPU tests. No
anti-aliasing or optional rendering override was enabled.

WineD3D did not enumerate the dxmt9 baseline's 1280x720 mode on this host. The
run therefore retained WineD3D's nearest default mode, 1280x800. Results are
the benchmark-owned values shown by the Result Details dialog, not dxmt9 frame
sampling. This makes the run a renderer baseline, not a matched A/B.

The companion identity runs loaded builtin `opengl32.dll`, `wined3d.dll`, and
`d3d9.dll`. The Heroic renderer probe additionally named `GL_RENDERER "Apple
M1"`. Neither runtime loaded the dxmt9 PE bridge or unix provider. Both primary
runs returned normally with runner status `pass`; each primary artifact stores
the Result Details screenshot and a manually transcribed JSON sidecar.

## Result

| scene | Sikarugir FPS | Heroic 11.16 FPS | Heroic vs. Sikarugir |
|---|---:|---:|---:|
| GT1 - Return to Proxycon | **`15.976`** | **`14.945`** | `-6.5%` |
| GT2 - Firefly Forest | **`18.897`** | **`16.542`** | `-12.5%` |
| HDR1 - Canyon Flight | **`37.620`** | **`29.688`** | `-21.1%` |
| HDR2 - Deep Freeze | **`15.577`** | **`13.636`** | `-12.5%` |

Sikarugir reported SM2.0 score `2092` and HDR/SM3.0 score `2660`; Heroic 11.16
reported `1889` and `2176`. CPU score and aggregate 3DMark score are
intentionally unavailable because the CPU tests were excluded. A preliminary
Heroic run reported `14.984`, `16.555`, `29.686`, and `13.875` FPS; the accepted
run differs by only `0.26%`, `0.08%`, `0.01%`, and `1.72%` respectively.

## Interpretation

The four scenes do not share one dxmt9/WineD3D renderer-wide ordering:
WineD3D is close to the current dxmt9 GT1 scene average, substantially below
dxmt9 on GT2 and HDR2, and Sikarugir WineD3D is above dxmt9 on HDR1. Resolution
and observer differences prevent assigning strict percentages to those gaps.
The two WineD3D columns do share resolution, settings, and observer; Heroic is
`6.5-21.1%` below Sikarugir across the four scenes, with the largest gap in
HDR1. The durable result is the per-scene shape, not a claim that one aggregate
score represents the renderer.

3DMark06 Advanced Edition again emitted no reusable per-test `.3dr`; the
benchmark UI screenshot remains the authoritative per-scene result artifact.
