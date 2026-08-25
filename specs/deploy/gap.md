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

The implementation and existing runtime evidence still use upstream DXMT's
unqualified `winemetal.dll` / `winemetal.so` basenames. The target contract
reserves `winemetal_dxmt9.dll` / `winemetal_dxmt9.so` for dxmt9. Existing smoke
evidence proves portions of bridge behavior, but it does not close the rename,
capability composition, current-Wine WSI, or coexistence requirements.

Loader capability and Metal-surface capability are independent. A runtime that
can load an app-local unixlib is not supported for windowed D3D9 unless it also
provides `extescape-v1` or an exact qualified legacy surface protocol.

| Area | Status | Current evidence and missing work |
|---|---|---|
| C ABI bridge and provider wrappers | ✅ | `include/dxmt9/device_c.h`, generated bridge dispatch, and `src/d3d9/` provider wrappers are present |
| Current unqualified PE/unix modules | ⚠️ superseded names | `src/winemetal/meson.build` and `src/winemetal/unix/meson.build` still build `winemetal.dll` / `winemetal.so`, violating R-DEPLOY-2.11 and R-DEPLOY-5.6 |
| Qualified `winemetal_dxmt9.dll` / `.so` pair | ❌ | Rename Meson outputs, PE definition/import identity, generated dynamic lookup, install name, locators, package manifests, installers, test staging, and diagnostics |
| Builtin loader path | ✅ legacy-name evidence | 2026-05-25 Sikarugir `sikarugir-cx-24.0.7` x64 and WoW64 smoke passed with builtin module pairing, ABI handshake, legacy macdrv acquisition, and present; rerun is required after the qualified rename |
| App-local loader on the Sikarugir legacy fixture | ⚠️ runtime blocked | The native PE chain and locator order were exercised, but that Wine 9-based runtime returns `STATUS_INVALID_INFO_CLASS` for `MemoryWineLoadUnixLibByName`; this says nothing about current upstream Wine's separate WSI support |
| Current upstream Wine loader capability | ⚠️ source-audited, runtime evidence open | Wine master `111e5197390aa008789b002222024229fa2b82cf` (2026-08-24) implements `MemoryWineLoadUnixLibByName` and its WoW64 translation in Wine's `NtQueryVirtualMemory` paths. Add x64/WoW64 app-local runtime evidence and record `by-name-v1` independently from WSI |
| Cold `WineUnixlibLoader` adapter | ❌ | `src/winemetal/winemetal_bridge.cpp` still owns raw information classes and lookup policy directly. Isolate `builtin-pair-v1` / `by-name-v1` probing and remove the spec's former assumption that `__wine_load_unix_lib` is an `ntdll.dll` export |
| Capability-based manifest schema | ❌ | `scripts/tools/package_app_local.py` still emits schema 1 with `min_wine_unixlib_feature`. Implement schema 2 `required_capabilities`, exact legacy runtime identity, observed loader capabilities, and distinct loader/WSI dispositions |
| Stock current-Wine WSI | ❌ | A successful `by-name-v1` load is insufficient: upstream Wine has no accepted Metal-surface ExtEscape protocol, and dxmt9's private-symbol path is not a stock-Wine contract. See `specs/winemetal/gap.md` |
| Provider-load / WSI-fail composition test | ❌ | Add the R-DEPLOY-6.9 case: ABI handshake succeeds, `layer_acquisition=unavailable`, and windowed creation fails without black-window success |
| DXMT coexistence | ❌ | No installer audit or runtime test proves that upstream DXMT's unqualified artifacts remain byte-identical while dxmt9 loads the qualified pair, sequentially in one prefix and, where available, in one process |
| Legacy unqualified upgrade provenance | ❌ | Installers must identify an old dxmt9-owned unqualified artifact before removing or restoring it; basename-only deletion could damage an upstream DXMT install |
| `dxmt9.dll` / `dxmt9.so` bridge naming | ❌ not selected | The selected target remains the `winemetal_dxmt9` suffix convention |
