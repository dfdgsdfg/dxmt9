---
description: 3DMark06 experiment status, launcher interaction, and Wine shader-compiler evidence for dxmt9 wild runs
paths:
  - "experiments/apps/3dmark06/**"
  - "experiments/apps_3rd/3dmark06/**"
  - "experiments/launchers/3dmark06.sh"
  - "experiments/output/3dmark06/**"
  - "experiments/prefixs/3dmark06/**"
  - "experiments/CATALOGUE.toml"
globs: "experiments/**/3dmark06/**"
alwaysApply: false
---

# 3DMark06 Experiment Status

Last checked: 2026-05-16

## Summary

3DMark06 is registered as an exploratory local-external D3D9 benchmark. It has
a launcher, installed Wine prefix, and external application install path under
the current dxmt9 experiment harness.

Builtin D3DX evidence reaches dxmt9 initialization and D3D9 resource traffic,
but it is blocked by Wine/vkd3d-shader's D3D compiler path for one 3DMark06
SM1-era shader. The 3DMark06 launcher now prefers Microsoft native D3DX/
D3DCompiler DLLs and verifies that the files are not Wine PE builtin copies.

Native D3DX avoids the compiler dialog and reaches draw traffic. Treat a
harness `pass` as process-level unless the capture is known to be the 3DMark06
window; macOS capture may still fall back to a full-screen desktop image.

## Repository State

- Catalogue entry: `experiments/CATALOGUE.toml`, app name `3dmark06`.
- Launcher: `experiments/launchers/3dmark06.sh`.
- Installed app prefix: `experiments/prefixs/3dmark06`.
- External app payload slot: `experiments/apps_3rd/3dmark06`.
- Output directory: `experiments/output/3dmark06`.
- Reference image: `experiments/references/3dmark06.png`.
- Configured Wine runtime: `sikarugir-cx-24.0.7`.

## Run / Automation Notes

`experiments/launchers/3dmark06.sh` stages dxmt9 into the prefix and runs:

```sh
3DMark06.exe -gtall -batchall -featureall -nosplash -nosysteminfo -noscreens
```

The launcher now automates the short exploratory path by default:

- wait about 20 seconds after launch
- send one `Enter` keypress to the active 3DMark06 window
- let the benchmark proceed for 30 seconds after Enter
- terminate matching `3DMark06.exe` processes

The launcher also defaults to native D3DX/D3DCompiler for this target:

```sh
d3d9,winemetal=n,b;d3dx9_28,d3dcompiler_43,d3dcompiler_47=n,b
```

This keeps dxmt9/`d3d9` on the native lane while avoiding Wine/vkd3d-shader's
current SM1 HLSL compiler gap.

For Sikarugir/CrossOver-style runners, the launcher exports both
`DXMT_EXPERIMENT_WINE_DLLOVERRIDES` and `WINEDLLOVERRIDES`. `--dll` alone was
observed loading `d3dx9_28.dll` and `d3dcompiler_47.dll` as builtin in
`+loaddll` traces.

The prefix can contain files named `d3dx9_28.dll` / `d3dcompiler_*.dll` that
are still Wine PE builtin DLLs. Verify provenance with `objdump -p`: builtin
copies import `wined3d` or `vkd3d`; Microsoft native copies do not. The launcher
does this check for the 32-bit `syswow64` DLLs and, when needed, runs:

```sh
WINE=<wine.real when present> WINESERVER=<wineserver.real when present> \
WINEPREFIX=<3dmark06 prefix> WINETRICKS_FORCE=1 \
  winetricks -q d3dx9_28 d3dcompiler_43 d3dcompiler_47
```

Override knobs:

- `DXMT_EXPERIMENT_WINE_DLLOVERRIDES=<overrides>` replaces the default DLL lane.
- `DXMT_3DMARK06_ENSURE_NATIVE_D3DX=0` skips native DLL provenance checks.
- `DXMT_3DMARK06_AUTO_CONTROL=0` disables the helper.
- `DXMT_3DMARK06_ENTER_DELAY_SEC=<seconds>` changes the Enter delay.
- `DXMT_3DMARK06_RUN_AFTER_ENTER_SEC=<seconds>` changes post-Enter run time.
- `DXMT_3DMARK06_AUTO_TERMINATE=0` keeps the process alive after Enter.

A first-run `Please Register` dialog may also need the same continue/enter
interaction.

Use a longer timeout while doing manual interaction:

```sh
python3 scripts/run_apps/run_experiment.py run 3dmark06 --timeout 180
```

## Latest Observed Results

`experiments/output/3dmark06-auto-terminate/result.json` from 2026-05-16
validates the short-run automation when using the old builtin-D3DX default:

- `status`: `pass`
- `returncode`: `0`
- `timed_out`: `false`
- elapsed process time: about `50.7s`
- window capture mode: `window_id`
- captured window title: `3DMark06 - Professional Edition`

The capture still showed the `gametest_proxycon.txt` `D3DXCompileShader`
failure dialog, so the automatic Enter/30s/terminate path is reliable for
harness control but builtin D3DX does not bypass the Wine/vkd3d-shader SM1
compiler gap.

`experiments/output/3dmark06/result.json` from 2026-05-16 reports:

- `status`: `pass`
- `returncode`: `0`
- `timed_out`: `false`
- elapsed process time: about `88.3s`
- Wine runtime: `experiments/wine/sikarugir-cx-24.0.7`
- capture fallback: `capture_mode = full_screen`
- capture error: `unable to find onscreen window matching title: 3DMark06`

The dxmt9 log reached repeated `abi-hash handshake OK` messages, factory/device
queries, resource locks, and texture traffic. The same run also showed a
3DMark06 error dialog for:

```text
C:\Program Files (x86)\Futuremark\3DMark06\data\timeline\gametest_proxycon.txt:
D3DXCompileShader failed: The function called is not supported at this time (E_NOTIMPL)
E5017: Aborting due to not yet implemented feature: SM1 non-float expression.
E5017: Aborting due to not yet implemented feature: SM1 "cast" expression.
```

This failure is in Wine/vkd3d-shader's `D3DCompile2` path, not direct evidence
of a dxmt9 D3D9 renderer failure.

The native-D3DX lane was promoted to the 3DMark06 launcher default after this
follow-up override experiment from 2026-05-16:

```sh
DXMT_EXPERIMENT_WINE_DLLOVERRIDES='d3d9,winemetal=n,b;d3dx9_28,d3dcompiler_43,d3dcompiler_47=n,b' \
  python3 scripts/run_apps/run_experiment.py run 3dmark06 --timeout 180 --output-suffix native-d3dx
```

This run did not emit `D3DCompile2`, `E5017`, or `gametest_proxycon` errors,
but it did not yet prove the benchmark path because it likely did not reach the
post-Enter game-test compiler path.

Subsequent default-lane validation from 2026-05-16 showed two separate
pitfalls:

- setting only `DXMT_EXPERIMENT_WINE_DLLOVERRIDES` for the launcher was
  insufficient under the Sikarugir/CrossOver runner; `WINEDLLOVERRIDES` must
  also be exported;
- DLL files copied from Wine's PE runtime can sit in the prefix with the same
  names as Microsoft native DLLs; install native DLLs with `winetricks` before
  trusting the `n,b` override.

After installing Microsoft native `d3dx9_28`, `d3dcompiler_43`, and
`d3dcompiler_47`, `experiments/output/3dmark06-native-ms-default` got past the
compiler dialog and created SM2 shaders, then hit an app-side null read:

```text
wine: Unhandled page fault on read access to 00000000 at address 004EE55F
```

The address is inside `3DMark06.exe`, but dxmt9 logs immediately before it
showed `SetDepthStencilSurface(user_surface)` followed by
`GetDepthStencilSurface()` returning `NULL`. The PE device wrapper must return
the currently-set user depth-stencil surface from its cache, not only the
swapchain auto depth-stencil surface.

`experiments/output/3dmark06-native-ms-ds-cache/result.json` from 2026-05-16
validated that cache fix at process/log level:

- `status`: `pass`
- `returncode`: `0`
- elapsed process time: about `55.7s`
- no `D3DCompile2`, `E5017`, `gametest_proxycon`, or unhandled page fault
- `GetDepthStencilSurface()` returned `cached surface=...`
- benchmark draw traffic reached `DrawIndexedPrimitive`
- visual capture fell back to full-screen desktop and is not benchmark evidence

`experiments/output/3dmark06-native-guard/result.json` from 2026-05-16
validated the launcher native-DLL guard path, but still failed visual capture:

- `status`: `fail` due `black_screen`
- `returncode`: `0`
- elapsed process time: about `55.5s`
- capture used a `3DMark06` window id, but `actual.png` was fully black
- no compiler dialog or page fault
- dxmt9 log showed 5 presents and thousands of indexed draws

## Current Risks / Follow-up

- Automatic window capture is not trustworthy: macOS window lookup may fail to
  find `3DMark06`, causing the harness to capture the full desktop instead of
  the benchmark window.
- A valid-looking 3DMark06 window capture can still be fully black while logs
  show draw/present traffic. Do not treat `black_screen` as the compiler
  blocker returning; inspect logs for `D3DCompile2`, `E5017`, page faults,
  presents, and draws.
- Re-validate benchmark visuals with a trustworthy 3DMark06 window capture.
- Do not treat native `d3dx9_28` as a dxmt9 renderer fix; it changes the app's
  shader compiler dependency to get past the Wine/vkd3d-shader gap.
- Keep 3DMark06 separate from 3DMark05. Current local evidence has 3DMark05
  reaching final presents, while 3DMark06 depends on native D3DX to get past
  the Wine/vkd3d-shader SM1 compiler gap.
