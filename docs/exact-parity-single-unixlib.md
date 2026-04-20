# Exact-Parity Single Unixlib Rewrite

## Goal

Match upstream `dxmt` ownership direction, not just artifact count.

Final target:

- `winemetal.so` is the only unixlib.
- `winemetal.so` owns only native Metal/AppKit/Foundation service code.
- `src/dxmt9` is the upper-layer runtime/provider and consumes private `WMT::*` wrappers.
- `include/dxmt9/device_c.h` remains public, but becomes a PE/provider ABI only.
- `src/d3d9` stays the PE/frontend bridge/provider ABI layer.

Public names and shipped artifact names must stay unchanged:

- `d3d9.dll`
- `winemetal.dll`
- `winemetal.so`
- `include/dxmt9/device_c.h`
- `include/dxmt9/winemetal.h`
- `dxmt9_winemetal_*`

## Current Baseline

Branch:

- `phase/exact-parity-single-unixlib`

Base commit:

- `9cd4d8a Add winemetal wrapper foundation and temp prefix cleanup`

Known-good baseline at branch start:

- `meson test -C build --print-errorlogs`: `7/7 OK`
- oracle: `pass_count = 6`
- oracle: `fail_count = 0`

## Why This Needs a Phase Branch

This is not a narrow refactor.

The rewrite requires these three moves together:

1. Remove direct Metal/AppKit ownership from `src/dxmt9`.
2. Remove generated unix bridge use of `device_c.h`.
3. Thin `winemetal.so` so it stops linking frontend/provider/runtime/shader-service deps directly.

Trying to land only one of those pieces creates middle states that still compile but break runtime behavior. The first presenter/layer wrapper attempt already showed this with oracle size mismatches.

## Constraints

- Keep every landing green on:
  - `build`
  - `build-x86_64-builtin`
  - `build-win32-x64-builtin`
  - `build-win32-x86-builtin`
- Do not rename public exports or public headers.
- Do not add a second unixlib again.
- Keep the `dxmt9c_*` / `dxmt9p_*` split already established in `src/d3d9`.
- Do not thin `winemetal.so` before `src/dxmt9` has been moved onto `WMT::*`.

## Structural Delta vs Upstream

Remaining gap relative to upstream `dxmt`:

1. `winemetal.so` still directly links provider/runtime/shader deps.
2. `device_c.h` is still tied into unix bridge generation through `src/winemetal/winemetal_unix_schema.h`.
3. `src/dxmt9` still directly owns Metal/AppKit/Foundation state.
4. queue/completion orchestration still exposes backend-facing controller surface.

This branch exists to remove those four gaps in order.

## Phase Plan

### Phase 1: Move `src/dxmt9` onto `WMT::*`

Status:

- started

Already landed on base:

- private wrapper foundation:
  - `src/winemetal/winemetal.h`
  - `src/winemetal/Metal.hpp`

Required work:

1. Port HUD and command-buffer diagnostics to `WMT::*`.
2. Port presenter/layer ownership to `WMT::MetalLayer` / `WMT::MetalDrawable` without changing presentation sizing semantics.
3. Port queue/completion ownership from `id<MTLCommandBuffer>` + completion handlers to `WMT::CommandBuffer` + queue-owned completion wait.
4. Remove long-lived `id<MTL*>` / `CAMetalLayer*` ownership from `src/dxmt9` headers and runtime state.

Phase 1 acceptance:

- `src/dxmt9` headers contain no long-lived `id<MTL*>` or `CAMetalLayer*` state.
- queue completion path no longer relies on completion handlers.
- oracle remains `6/0`.

### Phase 2: Remove `device_c` unix bridge usage

Required work:

1. Make `src/winemetal/winemetal_unix_schema.h` native-service-only.
2. Stop feeding `include/dxmt9/device_c.h` into unix bridge generation.
3. Keep `dxmt9c_*` as PE/provider-local bridge wrappers only.
4. Stop linking generated `device_c` unix bridge client code into the D3D9 forwarder.

Phase 2 acceptance:

- generator no longer reads `include/dxmt9/device_c.h`
- `winemetal_unix.cpp` publishes native-service entries only
- `dxmt9c_*` resolve from `src/d3d9`, not generated unix bridge code

### Phase 3: Thin `winemetal.so`

Required work:

1. Remove frontend/provider/runtime/shader-service deps from `src/winemetal/unix/meson.build`.
2. Keep provider/service logic in `src/dxmt9`.
3. Keep public shader exports unchanged but make them provider-local logic using `WMT::*`, not unix table requests.

Phase 3 acceptance:

- `winemetal.so` links only native-service implementation, `dxmt9_util_dep`, and Apple/Wine unix deps
- `winemetal.so` no longer publishes provider-side shader/service entries
- shipped artifact set remains unchanged

## Immediate Next Slice

The next implementation slice must be small enough to stay green:

1. move only presenter registry ownership to wrapper-backed handles
2. keep backend present property writes and drawable acquisition behavior identical
3. re-run focused oracle first, then full oracle

The previous failed attempt changed presentation behavior too early. The next slice must preserve exact drawable sizing semantics while changing ownership only.

## Validation Gate

Run after each landing:

```sh
meson compile -C build
meson compile -C build-x86_64-builtin
meson compile -C build-win32-x64-builtin
meson compile -C build-win32-x86-builtin
meson test -C build --print-errorlogs
bash scripts/run_dx9_oracle_compare_suite.sh
```

Required result:

- tests: `7/7 OK`
- oracle: `pass_count = 6`
- oracle: `fail_count = 0`

