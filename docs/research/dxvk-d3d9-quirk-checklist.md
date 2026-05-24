# DXVK D3D9 Quirk Checklist

This checklist tracks the DXVK D3D9 behavior that is worth converting into
`dxmt9` regression coverage.

DXVK does not carry a local D3D9 runtime test tree in `~/workspaces/dxvk`; the
usable material is implementation behavior and comments in `src/d3d9`. For
`dxmt9`, the right shape is therefore oracle-style micro-apps that run against
both `dxmt9` and builtin D3D9/Wine, not direct test imports.

Status meanings:

- `covered`: current fast sanity already has meaningful coverage.
- `partial`: current coverage exists but misses the DXVK quirk dimension.
- `gap`: no targeted coverage exists yet.
- `implementation-risk`: current `dxmt9` code looks underspecified or different.

## Implementation Order

1. Lock matrix tests: buffer plus texture/surface.
2. Fixed-function micro-tests: TSS clamp, texcoord transform, DOTPRODUCT3, Dref clamp.
3. `CheckDeviceFormat` compatibility table.
4. Swapchain behavior tests: `SetDialogBoxMode`, frontbuffer, lockable backbuffer.
5. Keep this checklist updated as parity tests land.

## Existing Fast Sanity Coverage

| App | Current coverage | Gap |
| --- | --- | --- |
| `conf-d3d9-clear` | device create/reset/present/backbuffer copy path | no DXVK-specific D3D9 quirks |
| `conf-d3d9-buffer` | `D3DPOOL_DEFAULT` vertex-buffer lock permutations | no texture/surface locks, no non-default pool matrix, no lost-device cases |
| `conf-d3d9-ffp-quirks` | API-level texture-stage-state clamp behavior, opt-in with `--app` | no texcoord transform, DOTPRODUCT3, or Dref pixel oracle yet |
| `conf-d3d9-lock-matrix` | buffer, index-buffer, texture, and surface lock matrix for common flag/pool combinations | no lost-device cases, no multisample non-lockable assertion, no multiplane pitch rows |
| `conf-d3d9-triangle` | shader compile/create, texture upload, render target, depth stencil, draw, stretch/present path | no fixed-function quirk oracle |

## 1. Lock Matrix

DXVK behavior to mirror:

- Buffer lock normalization lives in `~/workspaces/dxvk/src/d3d9/d3d9_device.cpp:5500`.
- `DISCARD` is only honored when the buffer can actually discard; conflicting
  `NOOVERWRITE` or `READONLY` suppress it at `d3d9_device.cpp:5516`.
- `DISCARD` and `NOOVERWRITE` are ignored outside `D3DPOOL_DEFAULT` at
  `d3d9_device.cpp:5520`.
- `DONOTWAIT` is ignored for dynamic buffers at `d3d9_device.cpp:5526`.
- Device-lost lock behavior ignores discard at `d3d9_device.cpp:5531`.
- `READONLY` is ignored for non-managed pools at `d3d9_device.cpp:5541`.
- Texture allocation has a 1-byte overrun padding quirk for games that write
  past locked texture memory at
  `~/workspaces/dxvk/src/d3d9/d3d9_common_texture.cpp:92`.
- Multisample render-target/depth surfaces are explicitly non-lockable at
  `~/workspaces/dxvk/src/d3d9/d3d9_common_texture.cpp:292`.
- Multiplane lock pitch behavior is special-cased at
  `~/workspaces/dxvk/src/d3d9/d3d9_common_texture.cpp:364`.
- Texture unlock calls `AddDirtyRect` after unlock because some games keep using
  the locked pointer at `~/workspaces/dxvk/src/d3d9/d3d9_texture.cpp:101`.

Current `dxmt9` references:

- Legacy app coverage is limited to
  `experiments/apps/conf-d3d9-buffer/conf_d3d9_buffer.cpp:40`.
- Current lock-matrix app is
  `experiments/apps/conf-d3d9-lock-matrix/conf_d3d9_lock_matrix.cpp`.
- Texture lock entry is `src/d3d9/core.cpp:1925`.
- Surface lock entry is `src/d3d9/core.cpp:2096`.
- C bridge forwards lock flags without much normalization in
  `src/d3d9/device_c_resources.cpp:185` and
  `src/d3d9/device_c_resources.cpp:404`.

Required test shape:

| Resource | Matrix | Oracle |
| --- | --- | --- |
| vertex buffer | pool x usage x `DISCARD`/`NOOVERWRITE`/`READONLY`/`DONOTWAIT` | lock result plus data persistence |
| index buffer | same as vertex buffer | same as vertex buffer |
| 2D texture | `SYSTEMMEM`, dynamic `DEFAULT`, rect/full lock, readonly/discard | pitch, pointer, write/readback, dirty propagation |
| offscreen surface | `SYSTEMMEM`, `DEFAULT`, rect/full lock | pitch, pointer, write/readback |
| multisample RT/DS surface | lock attempt | must fail consistently |

Current test placement:

- Implemented as `conf-d3d9-lock-matrix`.
- Built by `scripts/build_apps/build_dx9_fast_sanity_apps.sh`.
- Run by default in `scripts/run_suites/run_dx9_fast_sanity_suite.sh` across
  `dxmt9-x64`, `builtin-x64`, and `builtin-x86`.
- Catalogue entry is `experiments/CATALOGUE.toml`.

Status:

- `partial` coverage, now with a dedicated exploratory app.
- `implementation-risk` remains around texture/surface flag normalization and
  dirty propagation.
- Remaining rows to add: device-lost locks, multisample non-lockable surfaces,
  multiplane pitch behavior, and texture dirty propagation after unlock.

## 2. Fixed-Function Micro-Tests

DXVK behavior to mirror:

- `SetTextureStageState` clamps stage/type rather than returning invalid call;
  see `~/workspaces/dxvk/src/d3d9/d3d9_device.cpp:4722`.
- `D3DTTFF == 0xffffffff` has special texcoord behavior: clamp to texcoord
  dimensions, treat as projected, but do not apply the transform matrix; see
  `~/workspaces/dxvk/src/d3d9/d3d9_fixed_function.cpp:1197`.
- Dref is clamped for D32F emulating UNORM depth textures at
  `~/workspaces/dxvk/src/d3d9/d3d9_fixed_function.cpp:1970`.
- `D3DTOP_DOTPRODUCT3` has special color/alpha lowering behavior at
  `~/workspaces/dxvk/src/d3d9/d3d9_fixed_function.cpp:2334`.

Current `dxmt9` references:

- D3D9 C entry for texture-stage state is
  `src/d3d9/device_c_device_state_draw.cpp:219`.
- Core virtual entry is `include/dxmt9/com.hpp:120`.
- Existing sample-style FFP coverage exists only incidentally in apps such as
  `experiments/apps/sample-d3d9-irrlicht-lights/sample_d3d9_irrlicht_lights.cpp:334`.
- API-level TSS clamp coverage is
  `experiments/apps/conf-d3d9-ffp-quirks/conf_d3d9_ffp_quirks.cpp`.

Required test shape:

| Micro-test | Minimum oracle |
| --- | --- |
| TSS clamp | `SetTextureStageState(largeStage, validType, value)` and `SetTextureStageState(validStage, largeType, value)` return `D3D_OK` and do not poison following draws |
| texcoord transform `0xffffffff` | render a 2x2 diagnostic texture with a transform matrix that would visibly move UVs; expected result is the untransformed/projected DXVK behavior |
| DOTPRODUCT3 | render fixed-function texture/color cases where color and alpha ops differ; verify RGB/alpha result against builtin lane |
| Dref clamp | sample depth texture with out-of-range Dref values; verify clamp against builtin lane |

Recommendation:

- Keep extending `conf-d3d9-ffp-quirks`.
- TSS clamp is already covered as an API-only section because it is cheap and
  catches the Dawn of Magic 2 compatibility behavior.
- Keep texcoord/DOTPRODUCT3/Dref as pixel checks over a small render target so
  they remain deterministic.

Status:

- `partial` coverage.
- TSS clamp support is implemented in `src/d3d9/core.cpp`.
- The app is intentionally opt-in until current PE artifacts can be rebuilt from
  source again.
- `implementation-risk` remains until texcoord/DOTPRODUCT3/Dref shader lowering
  is proven by pixel tests.

## 3. CheckDeviceFormat Compatibility Table

DXVK behavior to mirror:

- NULL render-target format is accepted for two-dimensional RT checks at
  `~/workspaces/dxvk/src/d3d9/d3d9_adapter.cpp:155`.
- `RESZ`, `ATOC`, `SSAA`, `NVDB`, `R2VB`, `INST`, and `CENT` implement
  vendor-shaped compatibility responses at
  `~/workspaces/dxvk/src/d3d9/d3d9_adapter.cpp:160`,
  `~/workspaces/dxvk/src/d3d9/d3d9_adapter.cpp:168`,
  `~/workspaces/dxvk/src/d3d9/d3d9_adapter.cpp:174`,
  `~/workspaces/dxvk/src/d3d9/d3d9_adapter.cpp:182`,
  `~/workspaces/dxvk/src/d3d9/d3d9_adapter.cpp:189`,
  `~/workspaces/dxvk/src/d3d9/d3d9_adapter.cpp:197`, and
  `~/workspaces/dxvk/src/d3d9/d3d9_adapter.cpp:204`.
- Depth-stencil and offscreen depth format restrictions are checked before
  generic format mapping at `~/workspaces/dxvk/src/d3d9/d3d9_adapter.cpp:130`.

Current `dxmt9` references:

- Public C entry is `src/d3d9/device_c_factory.cpp:139`.
- Core virtual entry is `include/dxmt9/com.hpp:61`.
- Current implementation hook is `src/d3d9/com.cpp:425`.

Required test shape:

| Format group | Query matrix |
| --- | --- |
| regular color | adapter format x usage x resource type |
| depth/stencil | `D3DUSAGE_DEPTHSTENCIL`, lockable DS formats, offscreen surface |
| NULL RT | `D3DUSAGE_RENDERTARGET`, surface/texture |
| vendor fourcc hacks | `RESZ`, `ATOC`, `SSAA`, `NVDB`, `R2VB`, `INST`, `CENT` |
| SRGB query bits | `D3DUSAGE_QUERY_SRGBREAD`, `D3DUSAGE_QUERY_SRGBWRITE` |

Recommendation:

- Add a table-driven `d9vk-d3d9-format-compat` app that prints every row as
  `format, usage, rtype, hr`.
- Treat builtin/Wine as the oracle for lane comparison and keep DXVK behavior as
  the compatibility target when builtin is absent or vendor behavior differs.

Status:

- `gap` coverage.

## 4. Swapchain Behavior

DXVK behavior to mirror:

- `SetDialogBoxMode` returns `D3D_OK` despite stricter documentation; see
  `~/workspaces/dxvk/src/d3d9/d3d9_swapchain.cpp:795`.
- DXVK can allocate an extra frontbuffer for `GetFrontBufferData` behavior; see
  `~/workspaces/dxvk/src/d3d9/d3d9_swapchain.cpp:941`.
- Backbuffers are lockable internally for GDI fallback even when the present flag
  does not request a lockable backbuffer; see
  `~/workspaces/dxvk/src/d3d9/d3d9_swapchain.cpp:1041`.

Current `dxmt9` references:

- Backbuffer usage is exercised indirectly by
  `experiments/apps/common/dx9_fast_sanity.hpp:230`.
- Current command-queue tracking of the active backbuffer is in
  `src/dxmt9/dxmt9_command_queue.cpp:236`,
  `src/dxmt9/dxmt9_command_queue.cpp:245`, and
  `src/dxmt9/dxmt9_command_queue.cpp:275`.

Required test shape:

| Behavior | Minimum oracle |
| --- | --- |
| `SetDialogBoxMode` | call in windowed mode and verify `D3D_OK` |
| frontbuffer | clear/present, call `GetFrontBufferData`, inspect expected pixel |
| lockable backbuffer | create both regular and `D3DPRESENTFLAG_LOCKABLE_BACKBUFFER` swapchains and verify lock/readback behavior expected by builtin/DXVK |

Recommendation:

- Add a `d9vk-d3d9-swapchain-quirks` app.
- Run this after lock-matrix because frontbuffer/backbuffer readback failures are
  much easier to debug once lock semantics are already isolated.

Status:

- `gap` coverage.

## Carry-Forward Notes

- Do not port DXVK code verbatim. Convert observable behavior into small
  oracle tests.
- Prefer fast sanity apps over large sample apps for these quirks because the
  failures need to identify a single D3D9 compatibility rule.
- Add new apps to `scripts/build_apps/build_dx9_fast_sanity_apps.sh` and
  `scripts/run_suites/run_dx9_fast_sanity_suite.sh` only when they are deterministic enough
  to run in all three lanes.
- If a test intentionally documents a current `dxmt9` gap, add it as an
  opt-in app first, then move it into the default fast sanity suite after the
  implementation lands.
