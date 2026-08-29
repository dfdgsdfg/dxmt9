---
domain: baselines
workload: 3DMark05 + 3DMark06 + SFIV
title: "Baselines — the reference captures every other experiment compares against - Current Overview"
type: domain-overview
status: current
updated: 2026-08-29
source: docs/perfomance/baselines/log.md; docs/perfomance/overview-3dmark05-gt1.md; docs/perfomance/baselines/baselines-wild-fps-refresh.04.md; docs/perfomance/baselines/baselines-3dmark06-wined3d.05.md; docs/perfomance/baselines/baselines-d9vk-moltenvk.06.md; docs/perfomance/baselines/baselines-sfiv-wined3d.07.md
related: docs/perfomance/baselines/index.md; docs/perfomance/baselines/log.md
---

# Baselines — the reference captures every other experiment compares against - Current Overview

> Current, compact view for this performance domain. Historical detail from the former
> top-level `baselines.md` overview is preserved in [log](log.md). Domain landing: [index](index.md).

## Scope

This domain owns the reference 3DMark05 GT1 captures that define the bottleneck
shape and serve as the A/B denominators for every other domain. It holds four
capture regimes: the historical **frame120** Xcode snapshot that first showed
the shape, the current canonical **frame50** normal-source gputrace/Xcode
replay plus its no-gputrace sanity/timeout/watchdog-cleanup scouts, the
mid-investigation **frame60** validation capture with full finalizer
attribution gates, and the post-visualfix **frame60** refresh that confirms the
same owner after the latest correctness path. It also
keeps the whole-run counter shape that contextualizes the single-frame
captures. The latest frame60 no-gputrace visual-coupling scout keeps the
wrong-path counter interpretation separate from the Xcode GPU proof. The
latest low-overhead recovery scout separates transient/heavy-instrumentation
FPS-zero observations from the current normal renderer path. Almost
every A/B delta elsewhere is measured against baselines-frame50.01,
baselines-frame60.01, or the refreshed baselines-frame60.02
— all three now marked `outdated:` (`retired-journal`, `retired-journal`, and
`evidence-missing` respectively). Deltas already expressed against them stand as
recorded, but a new A/B cannot be re-based on these denominators; use the
current whole-run reference in [overview-3dmark05-gt1](../overview-3dmark05-gt1.md).

## Latest Conclusions

| # | Hypothesis | Verdict | Evidence |
|---|---|---|---|
| H11 | The 120s no-gputrace timeout policy still produces standard `result.json` evidence when the wrapper watchdog includes capture delay | accepted | baselines-frame50.05 *(removed: evidence-missing; in git history)* |
| H12 | The current renderer is stuck in an FPS-zero state after the latest sidecar/visual work | rejected-current | baselines-frame60.04 *(removed: evidence-missing; in git history)* (`--no-encoder-breakdown` scout p50/p95 FPS `18.081`/`26.648`, last sample `24.798fps`, visual state observed normal; heavy sidecar FPS tails remain instrumentation caveat) |
| H13 | Current `.gputrace` / System Trace preflight is operational for file/sidecar routes, while Xcode `developerTools` attach remains blocked | accepted current preflight plus file export | baselines-gputrace-preflight.02 *(removed: evidence-missing; in git history)* now records `~168GiB` free, full Xcode, Developer Mode enabled, file `.gputrace` dry-run passing through `--with-wine-capture-layer`, System Trace sidecar dry-run passing the `4096MiB` guard, and a real `capture-layer-current-r2-20260619` file-route export finalized into Xcode/dxmt reports. The old `~605MiB < 2048MiB` disk block is historical only. The Xcode `developerTools` route is still unavailable because attach preflight stays at `process-list-loading` / `Getting Process List...`; use file `.gputrace` or System Trace until that preflight passes. |
| H14 | Current capture-layer file route can produce Xcode encoder counters again | accepted | baselines-gputrace-capture.02 (`frame60.gputrace` written, performance export and encoder counters exported; latest current-worktree `--with-wine-capture-layer` wrapper run has `10` encoder rows, Xcode reports `35.919ms`, `10` render encoders, `396` draw calls, top-three `98.26%`) |
| H15 | A latest black-geometry / transparent-weapon report invalidates the current performance direction | rejected as a wall; accepted as a baseline gate | [snapshot-cache-visual.02](../snapshot-cache/snapshot-cache-visual.02.md) keeps `v0.0.3` as the visual-safe anchor. For the sampled black-foreground firefight window, H169 rejects full-cbuf as the owner and H172 shows the broad dark class in `v0.0.3`; separate weapon/lighting artifacts still need same-frame or draw-local proof before perf runs are promoted. |
| H16 | The PE/DOD refactor series changed the current wild-workload ceiling | rejected as a measurable single-run shift | [baselines-wild-fps-refresh.04](baselines-wild-fps-refresh.04.md) records GT1 `30.646`, GT2 `28.311`, GT3 `64.875`, and SFIV `43.252` at `e32da591`. Every delta from the 2026-08-23 sweep is inside the approximately `+/-3%` ambient band, with normal captures and zero GPU errors. |
| H17 | One WineD3D result can stand in for the current 3DMark06 renderer comparison | rejected as an aggregate; accepted as per-runtime, per-scene baselines | [baselines-3dmark06-wined3d.05](baselines-3dmark06-wined3d.05.md) records Sikarugir at GT1 `15.976`, GT2 `18.897`, HDR1 `37.620`, HDR2 `15.577`, and Heroic 11.16 at `14.945`, `16.542`, `29.688`, `13.636` official FPS. Both WineD3D runs use 1280x800 and the same official observer; Heroic is `6.5-21.1%` lower. Their mode and observer still differ from dxmt9's 1280x720 frame sampling, so no strict dxmt9 percentage is assigned. |
| H18 | A D3D9 Vulkan-to-Metal reference can be represented by generic Sikarugir DXVK | rejected; accepted as a D9VK-specific reference lane | [baselines-d9vk-moltenvk.06](baselines-d9vk-moltenvk.06.md) identifies Sikarugir D9VK `v1.10.3-20250511` plus patched MoltenVK `v1.2.10`, proves the x86 path with a pixel oracle, and records 3DMark05, 3DMark06, and SFIV. D9VK trails on all three 3DMark05 scenes but leads the Sikarugir WineD3D 3DMark06 scenes by `0.5-23.7%`; observer and mode differences keep this a per-workload reference rather than one aggregate ordering. |
| H19 | SFIV lacks runtime-qualified WineD3D references | rejected | [baselines-sfiv-wined3d.07](baselines-sfiv-wined3d.07.md) records `43.80 FPS` under Sikarugir at 1280x720 and `63.34 FPS` under Heroic 11.16 at 1280x800 from the 240-second SFIV overlay. D9VK records `46.78` at 1280x800 under Sikarugir; runtime, mode, and observer differences prevent one aggregate renderer ordering. |

## Current Navigation

- [Domain index](index.md)
- [Historical log](log.md)
- [Root 3DMark05 GT1 map](../overview-3dmark05-gt1.md)

## Recent Leaf Documents

- [baselines-sfiv-wined3d.07](baselines-sfiv-wined3d.07.md) — current
  Sikarugir and Heroic 11.16 WineD3D SFIV references, including the Heroic
  GStreamer plugin-view stability condition.
- [baselines-d9vk-moltenvk.06](baselines-d9vk-moltenvk.06.md) — current
  Sikarugir D9VK/MoltenVK D3D9 reference across 3DMark05, 3DMark06, and SFIV.
- [baselines-3dmark06-wined3d.05](baselines-3dmark06-wined3d.05.md) —
  current pristine Sikarugir and Heroic 11.16 WineD3D 3DMark06 graphics
  baselines.
- [baselines-wild-fps-refresh.04](baselines-wild-fps-refresh.04.md) — current
  dxmt9 3DMark05/SFIV whole-run throughput surface.

Older and non-recheckable baseline history is retained in [log](log.md).
