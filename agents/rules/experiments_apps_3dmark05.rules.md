---
description: 3DMark05 experiment status, launcher constraints, and evidence notes for dxmt9 wild runs
paths:
  - "experiments/apps/3dmark05/**"
  - "experiments/apps_3rd/3dmark05/**"
  - "experiments/launchers/3dmark05.sh"
  - "experiments/output/3dmark05/**"
  - "experiments/prefixs/3dmark05/**"
  - "experiments/CATALOGUE.toml"
globs: "experiments/**/3dmark05/**"
alwaysApply: false
---

# 3DMark05 Experiment Status

Last checked: 2026-05-16

## Summary

3DMark05 is already registered as an exploratory local-external D3D9 benchmark.
It currently has a launcher, installed Wine prefix, reference image, and
repeatable dxmt9 execution that reaches 3D rendering.

Current evidence does not show the Wine/vkd3d-shader SM1 HLSL compile failure
pattern being investigated for 3DMark06. The latest 3DMark05 dxmt9 output
reaches draw calls and a final present.

A restored Wine builtin D3D9 oracle renders the same delayed GT1 scene normally.
This separates app/timing/Wine viability from the current dxmt9 visual
corruption.

## Repository State

- Catalogue entry: `experiments/CATALOGUE.toml`, app name `3dmark05`.
- Launcher: `experiments/launchers/3dmark05.sh`.
- Installed app prefix: `experiments/prefixs/3dmark05`.
- External app payload: `experiments/apps_3rd/3dmark05`.
- Output directory: `experiments/output/3dmark05`.
- Reference image: `experiments/references/3dmark05.png`.

Catalogue notes:

- Source kind is external commercial application, not vendored.
- Features include `commercial`, `benchmark`, `d3d9`, `stress`, `scene`.
- Status is `exploratory`.
- Configured Wine runtime is `sikarugir-cx-24.0.7`.
- Benchmark is documented near 3DMark06 as the SM2-era member of the pair.

## Run / Automation Notes

`experiments/launchers/3dmark05.sh` stages dxmt9 into the prefix and runs:

```sh
3DMark05.exe -gtall -batchall -featureall -cpuall -nosplash -nosysteminfo -noscreens
```

The flags select test groups and suppress splash/system-info/screens, but the
skinned UI still requires the operator to activate `Run 3DMark`. Current
practical automation:

- `experiments/launchers/3dmark05.sh` sends `Enter` about 20 seconds after
  launching the app by default. Set `DXMT_3DMARK05_AUTO_ENTER=0` to disable it,
  or `DXMT_3DMARK05_ENTER_DELAY_SEC=N` to retime it.
- After sending Enter, the launcher keeps the 3DMark05 process frontmost for
  about 40 seconds by default. This avoids the benchmark's `Display window lost
  focus` modal while the render window is opening. Set
  `DXMT_3DMARK05_FOCUS_KEEPALIVE_SEC=0` to disable or retime it.
- Do not capture immediately after the benchmark starts. The first rendered
  window can be black; the catalogue waits until `capture_delay_sec=32.0`.
  In the 2026-05-16 run, visible 3D output appeared about 5 seconds after the
  initial black frame.
- The catalogue keeps `run_timeout_sec=10`, which is applied after screenshot
  capture by `run_experiment.py`. This prevents long manual cleanup runs.

Earlier attempts to automate a mouse click were unreliable because Wine-on-macOS
window geometry and accessibility metadata did not line up with the visible UI.

Wine-builtin oracle note:

- `d3d9=b` alone is not enough after dxmt9 has been staged into the Wine root:
  `<wine-root>/lib/wine/*-windows/d3d9.dll` is itself dxmt9.
- To run a real Wine builtin oracle, temporarily copy
  `d3d9.dll.dxmt9-backup` over the staged `d3d9.dll` in both
  `x86_64-windows` and `i386-windows`, run with `--skip-stage`, then restore the
  staged dxmt9 DLLs. Confirm by checking the oracle log has `wined3d` noise and
  no `[dxmt9-*]` backend lines.

## Latest Observed Result

2026-05-16 restored Wine builtin oracle:

- `experiments/output/3dmark05-wine-builtin-oracle-restored/result.json`
  reports harness `status=pass`, `returncode=-15`, `timed_out=true`, and a
  1024x768 delayed capture.
- The captured GT1 scene is visually normal at the same timing: red-lit landing
  bay, readable center monitor, and intact HUD. This proves the app, Wine
  runtime, focus keepalive, Enter automation, capture delay, and timeout policy
  are not the source of the dxmt9 corruption.
- The oracle log contains `wined3d`/GLSL fixmes and no dxmt9 backend present
  trace, confirming this was the restored Wine builtin path rather than staged
  dxmt9.

2026-05-16 programmable-VS multi-stream follow-up:

- `experiments/output/3dmark05-post-vs-multistream-stream-bind-trace/result.json`
  reports harness `status=pass`, `returncode=-15`, `timed_out=true`, and a
  1024x768 delayed capture with shader archive disabled.
- `DXMT_TRACE_SHADER_INPUTS=1` shows real 3DMark05 shaders mapping inputs from
  stream 1, e.g. tangent/binormal/normal/texcoord declarations mapped as
  `v1->s1`, `v2->s1`, and related stream-1 inputs.
- `DXMT_TRACE_ENCODE_SEQ=5` with the focused stream bind trace shows stream 1 is
  actually bound to Metal slot 6 with non-null live/bound Metal handles and
  non-zero shadow byte counts. The earlier "stream1 decoded but not bound" class
  is therefore unlikely to be the remaining primary cause.
- The dxmt9 capture remains corrupted: scene structure is visible, but large
  black regions, over-bright/white geometry, and magenta diagonal/area artifacts
  remain.

2026-05-16 post-boundary-coverage run after the R-TEST-0.10 / texture readback
work:

- `experiments/output/3dmark05-post-boundary-coverage/result.json` reports
  harness `status=pass`, `returncode=-15`, `timed_out=true`, and captures the
  1024x768 `3DMark05` render window after the delayed 32 s capture.
- The render is still badly corrupted: large black/white triangular regions,
  over-bright geometry, and stippled/noisy patches remain. This confirms the
  new DXT/L8/A8L8/FFP readback coverage did not by itself fix the real app.
- Logs show a clean `winemetal` ABI handshake and repeated successful
  `device_present hr=0x00000000`, with no Metal validation failure, page-fault,
  or vkd3d-shader failure marker in the captured run.
- `DXMT_DISABLE_CULL=1`
  (`experiments/output/3dmark05-disable-cull-post-boundary`) produced the same
  corruption shape; cull/winding state is therefore unlikely to be the primary
  cause.
- `DXMT_DEBUG_FORCE_FULLSCREEN_VERTEX=1` with shader archive disabled captured
  a black frame. Treat this as an aggressive diagnostic override, not as proof
  of a normal app failure; it does show this override is not a useful oracle for
  the current 3DMark05 scene without additional fragment/depth controls.

2026-05-16 L8 shader-read swizzle follow-up:

- `experiments/output/3dmark05-l8-swizzle-refocus/result.json` reports
  harness `status=pass`, `returncode=-15`, `timed_out=true`, and captures the
  1024x768 `3DMark05` render window.
- The first post-fix attempt without keepalive reached many successful presents
  but captured the main UI with a `Display window lost focus` modal. Treat that
  as automation/focus loss, not a renderer crash.
- The prior Metal validation crash for a 1x1 `L8` cube texture shader-read view
  is fixed by using a six-slice cube view. Logs now show `fmt=50` cube texture
  creation and repeated successful `device_present hr=0x00000000`.
- The L8 swizzle fix changes the captured GT1 character from red/posterized to
  gray/white, but severe corruption remains: large black/tan triangular regions
  and stippled/noisy texture-like regions are still present. L8 luminance
  semantics was a real issue, but not the primary remaining visual-corruption
  cause.

2026-05-16 delayed-capture investigation:

- `experiments/launchers/3dmark05.sh` auto-pressed `Enter` after about 20
  seconds, and `capture_delay_sec=32.0` captured the active GT1 scene instead
  of the initial black frame.
- `experiments/output/3dmark05-backbuffer-capture/actual.png` shows repeatable
  severe visual corruption: large black/tan triangular regions, posterized red
  character rendering, and noisy stippled texture-like regions.
- The process is intentionally killed by the harness after capture
  (`run_timeout_sec=10`, `returncode=-15`, `timed_out=true`,
  `allow_timeout=true`), so this pass result only means the app reached a
  capturable frame.
- `DXMT_FORCE_EXPAND_INDEXED=1` did not materially change the corruption, so
  direct indexed draw / index-buffer fetch is unlikely to be the primary cause.
- `DXMT_DISABLE_ALPHA_TEST=1` did not materially change the corruption, so
  alpha-test discard is unlikely to be the primary cause.
- `DXMT_DEBUG_FORCE_FRAGMENT_COLOR=1` produced a solid magenta render with HUD
  still visible, confirming the present path is alive and that shader/pipeline
  execution reaches fragment output.
- Logs show VS/PS 2.x shader creation and many compressed texture resources
  (`DXT1`, `DXT5`, some `DXT3`), with repeated successful
  `device_present hr=0x00000000` and no D3DX/vkd3d shader compile failure
  signatures.

Earlier 2026-05-16 timing run:

- Operator/automation sent `Enter` about 20 seconds after launch.
- The benchmark entered the 3D scene and produced visibly non-black output after
  a short black-screen period.
- Harness `actual.png` was captured too early and reported `black_screen`
  (`mean_luma=0.0`, `variance=0.0`), so treat that result as a capture-timing
  failure, not proof that the app failed to render.
- `returncode` was `0`; `timed_out` was `false`.
- Logs showed repeated successful `device_present hr=0x00000000`, shader
  creation, and no D3DX/vkd3d shader compile failure signatures.

Earlier 2026-05-15 run:

`experiments/output/3dmark05/result.json` reports:

- `status`: `pass`
- `returncode`: `0`
- `timed_out`: `false`
- `capture_error`: `null`
- `failures`: `[]`
- elapsed process time: about `125.6s`
- captured window title: `3DMark05 - Business Edition`
- captured window size: `678x352`
- Wine runtime: `experiments/wine/sikarugir-cx-24.0.7`

`experiments/output/3dmark05/3DMark05_dxmt9.log` shows shader creation and
active rendering, including:

- vertex shader bytecode versions such as `0xfffe0200` and `0xfffe0201`
- pixel shader bytecode versions such as `0xffff0200`
- render draws reaching `draw seq=42878`
- final `present seq=42879` at `1024x768`

This indicates that the app gets through D3D9 device/shader setup and presents
frames in the current dxmt9 harness.

## Current Risks / Follow-up

- The experiment is still marked exploratory; pass status is harness-level, not
  a full visual/performance validation.
- The launcher uses AppleScript to press Enter; this is sufficient locally but
  still depends on macOS accessibility/focus behavior.
- The current dxmt9 render is visually corrupted. Treat successful process
  execution separately from visual correctness.
- `dxmt9.log` contains common Wine macOS noise such as HID query fixmes,
  keyboard-layout fixmes, and experimental wow64 notice; these are not currently
  tied to a benchmark failure.
- If comparing against the 3DMark06 Wine 11.6/vkd3d-shader investigation, keep
  3DMark05 separate: current local evidence points to 3DMark05 running
  successfully under the configured patched Wine runtime.
