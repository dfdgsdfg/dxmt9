---
type: "Spec Gap"
title: "Deployment Gap"
description: "Implementation and evidence gaps for Wine PE / winemetal deployment."
tags: [specs, gap, deploy, winemetal]
---

# Deployment Gap

Domain-owned implementation and evidence gap tracker. Use the [root gap index](../gap.md) for cross-domain rollup.

## Wine PE / `winemetal` Deployment Layer

The spec now matches upstream DXMT's deployment shape:
`d3d9.dll` imports `winemetal.dll`, and `winemetal.dll` dispatches to the
paired Wine unix module `winemetal.so`. The older `dxmt9.dll` / `dxmt9.so`
bridge naming is no longer part of the target spec.

| Area | Status | Notes |
|---|---|---|
| C ABI bridge header `device_c.h` | ✅ | All factory / device / resource types |
| Provider-side C ABI wrappers in `src/d3d9/` | ✅ | `dxmt9c_*` provider + bridge sources are present |
| `winemetal.so` unix module | ✅ | `src/winemetal/unix/meson.build` builds `winemetal.so` and links Wine `winemac.so` / `ntdll.so` when configured |
| `d3d9.dll` as user-facing PE DLL | ✅ (builtin x64 + WoW64 x86) | Built via `build-win32-x64-builtin` / `build-win32-x86-builtin` (llvm-mingw cross + `winebuild --builtin` postprocess), staged to `lib/wine/{x86_64-windows,i386-windows}/d3d9.dll` + prefix `system32`/`syswow64`. **2026-05-25 end-to-end Wine WSI smoke PASS on BOTH x64 and WoW64/x86** — see thunk row. Native (app-local) variant: PE side validated, runtime-gated — see thunk row. |
| `winemetal.dll` as shared Wine builtin PE bridge | ✅ (builtin x64 + WoW64 x86) | Built + `winebuild --builtin` postprocessed, staged to `lib/wine/{x86_64-windows,i386-windows}/winemetal.dll`. Loaded under `WINEDLLOVERRIDES="d3d9,winemetal=n,b"`; `abi-hash handshake OK` logged for both the 64-bit and 32-bit bridge (32-bit dispatcher proven via 32-bit pointer widths, e.g. `dispatcher=7BF2801D`). |
| PE bridge ↔ unix module thunk mechanism | ✅ (builtin, x64 + WoW64 x86); ⚠️ (app-local runtime-gated) | **2026-05-25 end-to-end Wine WSI smoke PASS (builtin, Sikarugir `sikarugir-cx-24.0.7`).** Two binaries exercised both arch lanes: (a) `conf-d3d9-triangle` x64 + x86 (`--binary <abs path to ...-x86.exe>`), (b) the dedicated **`conf-d3d9-wsi-present`** (its missing `build/wsi_present/wsi_present_x64.exe` was built from `tests/integration/wsi_present/main.cpp` via `x86_64-w64-mingw32-clang++ -O2 ... -ld3d9 -luser32 -lgdi32`; **180/180 `device_present hr=0x0`, ssim 1.0**). Evidence (all lanes): `[winemetal-abi] abi-hash handshake OK (0x29886309da4f648d)` in BOTH PE + unix logs (64-bit) and the 32-bit equivalent (PE↔unix lockstep at runtime); `builtin unixlib lookup: info=1000 status=0x0` (the supported path); `[dxmt9-wsi] layer_acquisition=macdrv_functions`; `CreateDeviceEx → non-null`; `device_present hr=0x0`; `returncode=0`. **App-local (native) lane — PARTIAL, host-runtime-gated (NOT a dxmt9 defect):** native non-builtin DLLs built (`build-win32-x64`, `wine_builtin_dll=false`, plain `PE32+`, no static `winemetal.dll` import), packaged via `scripts/tools/package_app_local.py`, and **the dxmt9 PE chain is correct end-to-end** — `+loaddll` confirms the app-dir native `d3d9.dll`/`winemetal.dll` load (not the runtime builtins), the provider locator runs the spec R-DEPLOY-3.4 order (`env → module-dir → exe-dir → runtime-by-name`), NT-path conversion succeeds, and a missing provider surfaces as a clean `Direct3DCreate9` failure (R-DEPLOY-3.8). It is blocked at the unix provider load because the app-local locator uses `NtQueryVirtualMemory(info=1002 = MemoryWineLoadUnixLibByName)` (`winemetal_bridge.cpp:648`) and **Sikarugir CX 24.0.7 (wine-9.0 base) does not implement info class 1002** → `status=0xc0000003` → `abi-hash unix-call failed; refusing to attach`. This is exactly the `min_wine_unixlib_feature: MemoryWineLoadUnixLibByName` precondition the deploy manifest pre-declares and the `test_wild.rules.md` checklist-5 / CrossOver "class 1002" note. App-local end-to-end validation (R-DEPLOY-6.2/6.3) needs a newer Wine that implements info=1002 with a non-stripped `winemac.so`; the builtin lane works on Sikarugir via info=1000. |
| `dxmt9.dll` / `dxmt9.so` legacy bridge naming | ❌ | Removed from target spec; stale references should be treated as documentation drift |

---
