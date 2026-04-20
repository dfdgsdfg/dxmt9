# DXVK D3D9 Quirk Checklist

This checklist compares `dxmt9` against the specific `dxvk` D3D9 behavior that is most likely to affect parity work.

It is not a full spec. It is a focused implementation checklist seeded from:

- `~/workspaces/dxvk/src/d3d9/d3d9_fixed_function.*`
- `~/workspaces/dxvk/src/d3d9/d3d9_device.cpp`

Status meanings:

- `match`: `dxmt9` already appears to implement the same behavior.
- `unknown`: relevant logic exists, but parity is not established yet.
- `likely gap`: current `dxmt9` behavior looks different or underspecified.

| Area | DXVK reference | dxmt9 reference | Status | Current coverage | Next step |
| --- | --- | --- | --- | --- | --- |
| Fog defaults | `d3d9_device.cpp` default states: `FOGSTART=0.0`, `FOGEND=1.0`, `FOGDENSITY=1.0` | `src/d3d9/core.cpp` resets `RS_FOG_START` to `1.0f`, `RS_FOG_END` to `1.0f`, `RS_FOG_DENSITY` to `1.0f` | `likely gap` | none | add a targeted fog-default sanity app or shader/device test |
| Fog mode split and constant derivation | `d3d9_device.cpp` `UpdateFogModeSpec`, `UpdateFogConstants`, `FOGVERTEXMODE` vs `FOGTABLEMODE`; `d3d9_fixed_function.cpp` fog lowering | `src/d3d9/core.cpp` fixed-function keys only read `RS_FOG_TABLE_MODE`, `RS_FOG_FROM_VERTEX`, `RS_RANGE_FOG`; `src/dxmt9/backend_metal.mm` derives fog uniforms from `RS_FOG_*` | `likely gap` | none | add focused fog mode coverage before trusting fixed-function parity |
| Alpha test enable/func/ref | `d3d9_device.cpp` alpha-test state updates and render-target-format sensitivity | `src/d3d9/core.cpp` fixed-function pixel key hashes alpha-test state; `src/dxmt9/backend_metal.mm` lowers alpha test into shader uniforms | `unknown` | partial via `d9vk-d3d9-triangle` draw path only | add explicit alpha-test format-sensitive regression target |
| Texture stage state clamping | `d3d9_device.cpp: SetStateTextureStageState` clamps stage/type and notes “Matches tests” | `src/d3d9/core.cpp: Device::setTextureStageState` returns `D3DERR_INVALIDCALL` only on out-of-range stage and does not clamp stage/type | `likely gap` | none | add a narrow device-state test for out-of-range texture-stage writes |
| Fixed-function shader dirties on texture changes | `d3d9_device.cpp` marks FF state dirty for mip-count changes, format changes, border color cases | `src/d3d9/core.cpp` rebuilds fixed-function keys from state maps; explicit texture-driven dirty tracking is not obvious | `unknown` | none | inspect texture binding path and add a targeted fixed-function material swap test |
| Vertex/index buffer lock flag normalization | `d3d9_device.cpp` normalizes `DISCARD`, `NOOVERWRITE`, `READONLY`, `DONOTWAIT` by pool, usage, and device-lost state | `src/d3d9/core.cpp: Buffer::lock` only special-cases `DISCARD + DYNAMIC`; `src/d3d9/device_c_resources.cpp` forwards lock flags directly | `likely gap` | basic coverage via `d9vk-d3d9-buffer` permutations | extend with pool/lost-device corner cases |
| Texture/surface lock flag normalization | `d3d9_device.cpp` has more detailed resource-lock handling than just discard-zeroing | `src/d3d9/core.cpp: Texture::lockRect` and `Surface::lockRect` only zero on `UsageDiscard` | `likely gap` | none | add texture/surface lock corner-case tests |
| FPU / float behavior setup | `d3d9_device.cpp: SetupFPU()` explicitly matches D3D9 float behavior | no analogous `SetupFPU` or process-level float-mode setup found in current `dxmt9` D3D9 path | `likely gap` | none | evaluate whether host FPU control is required for parity-sensitive shader/device tests |
| Shader-source fixed-function lowering | `d3d9_fixed_function.cpp` and fixed-function shaders are the behavioral baseline | `src/dxmt9/backend_metal.mm` owns fixed-function shader-source generation; `src/dxmt9/dxmt9_shader_service_exports.cpp` mirrors fog/alpha-test compile inputs | `unknown` | indirect only | compare generated shader semantics row-by-row as fixed-function bring-up continues |

Fast sanity suite coverage map:

- `d9vk-d3d9-clear`
  - covers device create/reset/present/backbuffer copy path
- `d9vk-d3d9-buffer`
  - covers basic `D3DPOOL_DEFAULT` vertex-buffer lock permutations
- `d9vk-d3d9-triangle`
  - covers shader compile/create, texture upload, render target, depth stencil, draw, and stretch/present path

What remains intentionally uncovered by the fast suite:

- fog behavior
- texture-stage out-of-range semantics
- alpha-test precision/format sensitivity
- lost-device lock behavior
- non-default-pool lock-flag quirks
- FPU behavior
