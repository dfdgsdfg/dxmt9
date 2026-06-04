---
domain: hidden-backend-storage
workload: 3DMark05 GT1
subcategory: model
order: 02
title: External Apple GPU Model Refresh
date: 2026-06-04
type: conceptual-model
status: model
source: specs/perfomance.plan.md#L3011-L3072
---

# External Apple GPU Model Refresh

**Question / hypothesis.** Does public Apple / Asahi / Mesa / MoltenVK material
support the hidden-backend-storage model from
[[hidden-backend-storage-model.01]], and what can each source actually prove?

**Method.** Literature survey of external GPU architecture documentation,
mapped onto the five-component model and the runtime gate
(`VS Buffer Device Memory Bytes Written` must move).

**Result.** Supporting evidence:

- **Asahi AGX hardware notes** describe a tiler buffer system storing vertex
  attribute and primitive data in driver-provided fixed buffers plus a
  firmware-managed heap, and list TVB tile/list/heap structures for a 3D frame.
  Directly supports treating the large VS-write bucket as vertex/primitive
  backend storage, not a dxmt CPU upload.
- **Mesa Asahi glossary** names `UVS` (buffers VS outputs/varyings), `VDM`
  (VS dispatch), `PPP` (primitive assembly), `ISP` (rasterization). Matches the
  split between stage-out/varying storage and primitive/binning/tiler storage.
- **Apple Metal TBDR guidance** reinforces the render-pass side: tile memory
  avoids system-memory round trips while a pass stays intact, so attachment
  store/load traffic remains a real secondary target.
- **MoltenVK** is useful only for API-layer expectations (translation overhead,
  limited fine-grained Metal control); less useful than Asahi/Mesa for AGX
  hidden TVB/UVS/parameter allocation details.

References: `asahilinux.org/docs/hw/soc/agx/#tiler-buffer-management`,
`docs.mesa3d.org/drivers/asahi.html#hardware-glossary`, Apple TBDR doc,
MoltenVK runtime user guide, Vulkanised 2023 MoltenVK talk.

**Verdict.** model (corroborated). External architecture sources validate the
UVS (varying) / PPP (primitive) split of the hidden bucket and keep
render-pass store/load as a documented secondary target. Layering caution from
MoltenVK: prefer the smallest dump-first probe before broad runtime changes.

**Related.** [[hidden-backend-storage-model.01]] · [[hidden-backend-storage]] ·
[[render-pass-store]] · [[tvb-mechanism-proof]] · [[overview]]
