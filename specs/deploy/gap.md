---
type: "Spec Gap"
title: "Deployment Gap"
description: "Implementation and evidence gaps for Wine PE / winemetal deployment."
tags: [specs, gap, deploy, winemetal]
---

# Deployment Gap

Domain-owned implementation and evidence gap tracker. Use the
[root gap index](../gap.md) for cross-domain rollup.

## Wine PE / `winemetal` Deployment Layer

The implementation now owns only the qualified `winemetal_dxmt9.dll` /
`winemetal_dxmt9.so` basenames. Existing runtime evidence predates that rename,
so capability composition, current-Wine WSI, and runtime coexistence evidence
remain open even though deterministic build and staging coverage closes the
module-identity implementation gap.

Loader capability and Metal-surface capability are independent. A runtime that
can load an app-local unixlib is not supported for windowed D3D9 unless it also
provides `extescape-v1` or an exact qualified legacy surface protocol.

| Area | Status | Current evidence and missing work |
|---|---|---|
| C ABI bridge and provider wrappers | ✅ | `include/dxmt9/device_c.h`, generated bridge dispatch, and `src/d3d9/` provider wrappers are present |
| Current unqualified PE/unix modules | ✅ not produced or staged | Meson, installers, package manifests, conformance/experiment staging, and generated sibling/provider lookup use only the suffix-qualified dxmt9 names; upstream-owned unqualified files are never touched |
| Qualified `winemetal_dxmt9.dll` / `.so` pair | ✅ build and deterministic staging evidence | Meson outputs, PE definition/import identity, generated dynamic lookup, install id, locators, package manifests, installers, test staging, and diagnostics are qualified; native/script tests bind the generated and packaged names |
| Builtin loader path | ✅ legacy-name evidence | 2026-05-25 Sikarugir `sikarugir-cx-24.0.7` x64 and WoW64 smoke passed with builtin module pairing, ABI handshake, legacy macdrv acquisition, and present; rerun is required after the qualified rename |
| App-local loader on the Sikarugir legacy fixture | ⚠️ runtime blocked | The native PE chain and locator order were exercised, but that Wine 9-based runtime returns `STATUS_INVALID_INFO_CLASS` for `MemoryWineLoadUnixLibByName`; this says nothing about current upstream Wine's separate WSI support |
| Current upstream Wine loader capability | ⚠️ source-audited, runtime evidence open | Wine master `111e5197390aa008789b002222024229fa2b82cf` (2026-08-24) implements `MemoryWineLoadUnixLibByName` and its WoW64 translation in Wine's `NtQueryVirtualMemory` paths. Add x64/WoW64 app-local runtime evidence and record `by-name-v1` independently from WSI |
| Cold `WineUnixlibLoader` adapter | ❌ | `src/winemetal/winemetal_bridge.cpp` still owns raw information classes and lookup policy directly. Isolate `builtin-pair-v1` / `by-name-v1` probing and remove the spec's former assumption that `__wine_load_unix_lib` is an `ntdll.dll` export |
| Capability-based manifest schema | ❌ | `scripts/tools/package_app_local.py` still emits schema 1 with `min_wine_unixlib_feature`. Implement schema 2 `required_capabilities`, exact legacy runtime identity, observed loader capabilities, and distinct loader/WSI dispositions |
| Stock current-Wine WSI | ❌ | A successful `by-name-v1` load is insufficient: upstream Wine has no accepted Metal-surface ExtEscape protocol, and dxmt9's private-symbol path is not a stock-Wine contract. See `specs/winemetal/gap.md` |
| Provider-load / WSI-fail composition test | ⚠️ bounded wild evidence / formal case still open | The child-current STALKER Day retry records provider factory/ABI activity followed by `layer_acquisition=unavailable` at `stage=query` and clean `CHW::CreateDevice` failure before rendering (`experiments/output/app-d3d9-stkcop-bench-current-head-off-day-20260830-r1`). This is exact accepted-Sikarugir evidence of the failure shape, but the required reusable R-DEPLOY-6.9 test and explicit black-window assertion remain open. |
| DXMT coexistence | ⚠️ namespace proved, runtime open | Deterministic tests prove dxmt9 packages/stages only the qualified pair and never targets upstream-owned basenames; no runtime test yet records upstream artifact checksums while exercising both products sequentially or in one process |
| Legacy unqualified upgrade provenance | ❌ | Installers must identify an old dxmt9-owned unqualified artifact before removing or restoring it; basename-only deletion could damage an upstream DXMT install |
| `dxmt9.dll` / `dxmt9.so` bridge naming | ❌ not selected | The selected target remains the `winemetal_dxmt9` suffix convention |
