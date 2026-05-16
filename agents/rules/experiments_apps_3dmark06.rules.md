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

Current evidence reaches dxmt9 initialization and D3D9 resource traffic, but the
run is blocked or degraded by Wine/vkd3d-shader's D3D compiler path for one
3DMark06 SM1-era shader. Treat a harness `pass` as process-level only until the
shader dialog and visual capture are resolved.

The compiler failure is caused by the default DLL load path, not by missing
DirectX DLL files: native `d3dx9_28.dll` / `d3dcompiler_*.dll` are present in
the prefix, but the Wine override registry is empty and the launcher only
forces `d3d9,winemetal=n,b`.

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

Override knobs:

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
validates the default short-run automation:

- `status`: `pass`
- `returncode`: `0`
- `timed_out`: `false`
- elapsed process time: about `50.7s`
- window capture mode: `window_id`
- captured window title: `3DMark06 - Professional Edition`

The capture still showed the `gametest_proxycon.txt` `D3DXCompileShader`
failure dialog, so the automatic Enter/30s/terminate path is reliable for
harness control but does not bypass the Wine/vkd3d-shader SM1 compiler gap.

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

Follow-up override experiment from 2026-05-16:

```sh
DXMT_EXPERIMENT_WINE_DLLOVERRIDES='d3d9,winemetal=n,b;d3dx9_28,d3dcompiler_43,d3dcompiler_47=n,b' \
  python3 scripts/run_apps/run_experiment.py run 3dmark06 --timeout 180 --output-suffix native-d3dx
```

This run did not emit `D3DCompile2`, `E5017`, or `gametest_proxycon` errors.
It still did not produce reliable benchmark evidence: logs only reached
factory/capability queries, automatic capture still fell back to full-screen,
and no draw/present traffic was observed in `3dmark06-native-d3dx` output.

## Current Risks / Follow-up

- Automatic window capture is not trustworthy: macOS window lookup may fail to
  find `3DMark06`, causing the harness to capture the full desktop instead of
  the benchmark window.
- The SM1 shader compiler dialog must be accounted for before using this target
  as visual or performance evidence.
- Prefer native `d3dx9_28` as the next experimental lane if investigating past
  the compiler dialog. Do not treat it as a dxmt9 renderer fix; it changes the
  app's shader compiler dependency.
- Keep 3DMark06 separate from 3DMark05. Current local evidence has 3DMark05
  reaching final presents, while 3DMark06 still exposes the SM1 compiler gap.
