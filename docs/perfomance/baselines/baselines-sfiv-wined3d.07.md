---
domain: baselines
workload: Street Fighter IV Benchmark
subcategory: renderer-comparison
order: 7
title: Street Fighter IV WineD3D Baselines
date: 2026-08-29
type: experiment-run
status: accepted-baseline
source: experiments/output/app-d3d9-sfiv-benchmark-wined3d-sikarugir-warm-20260829; experiments/output/app-d3d9-sfiv-benchmark-wined3d-sikarugir-final-r1-20260829; experiments/output/app-d3d9-sfiv-benchmark-wined3d-heroic-11.16-final-r4-20260829
related: README.md; docs/perfomance/baselines/baselines-d9vk-moltenvk.06.md; docs/perfomance/baselines/baselines-wild-fps-refresh.04.md
---

# Street Fighter IV WineD3D Baselines

## Question

What throughput do the Sikarugir-CX 24.0.7 and Heroic Wine Staging 11.16
builtin WineD3D paths deliver in the SFIV benchmark tracked for dxmt9 and
D9VK/MoltenVK?

## Method

Both accepted runs used `d3d9=b`, no app-local `d3d9.dll`, `--skip-stage`, and
the repository's `perf` profile. The Sikarugir run used Sikarugir-CX 24.0.7
after a 75-second warm-up. The Heroic run used Wine Staging 11.16 in a dedicated
prefix. Both dxmt9 counters and diagnostic renderer HUDs were disabled.

SFIV selected 1280x720 at 60 Hz under Sikarugir and 1280x800 at 60 Hz under
Heroic. Each recorded value is the benchmark overlay's cumulative `AVERAGE` at
the 240-second capture point. It is not the instantaneous `FPS`, the runner's
process-elapsed estimate, or a final result screen. Each process remained in
the benchmark after the useful interval and was terminated by the allowed
30-second post-capture timeout.

Heroic's bundled GStreamer plugin set loads additional MoltenVK copies through
the Vulkan and Apple-media plugins and has a Python plugin without `gi` in this
environment. The accepted run used an isolated view of the same plugin set
excluding only `libgstvulkan.dylib`, `libgstapplemedia.dylib`, and
`libgstpython.dylib`; `libgstlibav.dylib` remained available. This removed the
duplicate Objective-C class and Python initialization crashes without changing
the selected D3D9 renderer. The final `result.json` records status `pass`, no
reported failure, and a 270.8-second process elapsed time.

## Result

| renderer | mode | 240-second overlay average |
|---|---|---:|
| Sikarugir WineD3D/OpenGL | 1280x720 | **`43.80 FPS`** |
| Heroic 11.16 WineD3D/OpenGL | 1280x800 | **`63.34 FPS`** |
| Sikarugir D9VK/MoltenVK | 1280x800 | `46.78 FPS` |
| current dxmt9/Metal | 1280x720 | `44.3 FPS` headline |

Heroic WineD3D is numerically `35.4%` above Sikarugir D9VK at the same mode and
observer, but the Wine runtime also differs, so this is not a renderer-only
A/B. Sikarugir WineD3D and dxmt9 share the 1280x720 mode, but dxmt9's headline
uses renderer frame sampling instead of SFIV's 240-second overlay observer.
The durable evidence is each runtime/provider baseline and its mode, not one
aggregate renderer ordering.

## Limits

Each result is one recorded run, not an interleaved trial. The captured frames
are visually complete, but no pixel-matched oracle was used. Provider identity
follows from builtin `d3d9=b`, skipped dxmt9 staging, and the absence of an
app-local D3D9 provider; the empty D9VK/dxmt9 application logs are consistent
with that selection. The Heroic plugin view is a host-runtime stability
workaround and should be reproduced when refreshing that baseline.
