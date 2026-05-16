---
description: Linux host, ELF loader, headless CI, Wine/Linux, WSI-unavailable, and non-Metal debugging rules for dxmt9
paths:
  - "scripts/**"
  - "tests/**"
  - "experiments/**"
  - "meson.build"
  - "cross/*.ini"
globs: "{scripts,tests,experiments,cross}/**/*"
alwaysApply: false
---

# Linux Debugging Rules

Use this for Linux-host or CI failures: ELF loader paths, `LD_LIBRARY_PATH`,
rpath/runpath, symbol visibility, Wine on Linux, headless execution, X11/Wayland
availability, and result schemas that must not claim macOS/Metal evidence.

## Do Not Claim macOS Evidence On Linux

Linux can run build, schema, native data, and some Wine/PE evidence, but it
cannot prove Cocoa, `macdrv`, `CAMetalLayer`, Xcode `.gputrace`, or macOS
`screencapture` behaviour.

**Rules:**
- On Linux/headless hosts, mark WSI/window evidence as `unavailable` unless an
  actual X11/Wayland/window-capture path is implemented and recorded.
- Do not set `diagnostics.wsi.layer_acquisition` to `macdrv_functions` or
  `legacy_macdrv_get_cocoa_view` from Linux evidence.
- Do not use full-screen or manual observation evidence to prove HWND-to-layer
  success.
- If a test is Linux-only or headless-only, include `diagnostics.headless` or an
  explicit unavailable reason in `dxmt9.debug.result.v1`.

## ELF Loader And Unix Provider Evidence

Linux failures around `.so` loading often look like bridge bugs. Prove the ELF
loader state before changing PE/unix code.

**Rules:**
- Record the resolved `.so` path, `LD_LIBRARY_PATH`, rpath/runpath expectation,
  and missing-symbol diagnostics for unix provider failures.
- Distinguish "library not found", "symbol not found", "wrong architecture",
  and "ABI hash mismatch" in sidecars or logs.
- Do not fix Linux loader issues by weakening app-local/builtin separation.
- Use `ldd`, `readelf -d`, `nm -D`, or `objdump -T` as evidence when diagnosing
  Linux `.so` resolution.

## Wine/Linux Is Not Wine/macOS

Wine on Linux has different windowing, driver, and unixlib behaviour from Wine
on macOS. A pass/fail on one host is not automatically evidence for the other.

**Rules:**
- Record host OS, Wine root, prefix, architecture, and display backend
  (`DISPLAY`, `WAYLAND_DISPLAY`, or headless) for Linux Wine runs.
- Keep Linux Wine evidence separate from macOS Wine evidence in manifests unless
  the acceptance criterion explicitly allows either host.
- If a Linux run uses a different D3D9 implementation, Vulkan path, or shim, do
  not compare it as a dxmt9 Metal-present path.
- For provider/ABI failures, still apply `debug_wine.rules.md`; for window/layer
  failures, apply Linux/headless classification instead of macOS `macdrv` rules.

## CI And Headless Gates

Linux CI is useful for deterministic contracts, not for macOS presentation.

**Rules:**
- Prefer schema, manifest, packet, transform, bridge layout, shader source, and
  pure data tests on Linux CI.
- Any test requiring a real compositor, GPU, Wine GUI, or Metal capture must be
  opt-in and clearly skipped or classified when unavailable.
- A headless skip must be machine-readable; avoid silent absence of capture or
  WSI artifacts.
- Do not make release-default runtime paths pay for Linux-only diagnostics.

## Related

- `agents/rules/debug_wine.rules.md` - Wine runtime, provider, prefix, and ABI
  debugging.
- `agents/rules/debug_windows.rules.md` - PE D3D9 ABI and Win32-visible
  behaviour.
- `agents/rules/debug_objcpp.rules.md` - macOS/Cocoa/macdrv evidence that Linux
  must not claim.
- `agents/rules/environment_variables.rules.md` - cross-platform debug
  environment variables.
