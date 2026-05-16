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

Last checked: 2026-05-15

## Summary

3DMark05 is already registered as an exploratory local-external D3D9 benchmark.
It currently has a launcher, installed Wine prefix, reference image, and a
passing experiment result under the dxmt9 experiment harness.

Current evidence does not show the Wine/vkd3d-shader SM1 HLSL compile failure
pattern being investigated for 3DMark06. The latest 3DMark05 dxmt9 output
reaches draw calls and a final present.

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
skinned UI still requires the operator to click `Run 3DMark` once. Earlier
attempts to automate the click were unreliable because Wine-on-macOS window
geometry and accessibility metadata did not line up with the visible UI.

## Latest Observed Result

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
- The launcher is not fully headless because the 3DMark UI requires one manual
  click.
- `dxmt9.log` contains common Wine macOS noise such as HID query fixmes,
  keyboard-layout fixmes, and experimental wow64 notice; these are not currently
  tied to a benchmark failure.
- If comparing against the 3DMark06 Wine 11.6/vkd3d-shader investigation, keep
  3DMark05 separate: current local evidence points to 3DMark05 running
  successfully under the configured patched Wine runtime.
