---
domain: present-pacing
workload: 3DMark05 GT2
title: "H400: Current GT2 baseline; D9VK comparison fails the visual gate"
type: evidence
status: dxmt9-baseline-d9vk-visual-gate-open
updated: 2026-09-27
source: traces/gt2-compare-h400/{run.py,analyze.py,summary.json}; experiments/output/app-d3d9-3dmark05-compare-h400-{dxmt9-warm,dxmt9-r1,dxmt9-r2,d9vk-warm,d9vk-r1}/result.json
related: present-pacing-stalker-renderer-comparison.399.md; ../baselines/baselines-d9vk-moltenvk.06.md
---

# H400: GT2 does not yet provide a valid current cross-renderer comparison

HEAD `dbc216d5`, same Sikarugir CX 24.0.7 root and 3DMark05 prefix.
All three canonical staging builds report no pending work before measurement.
Use the catalogue runner, named `gt2` lane, perf profile, counters and frame
sampling OFF. Inherited renderer/Metal/DYLD overrides are cleared. The
benchmark-owned `.3dr` `Result.xml` reports 1024x768, no AA, optimal filtering,
VS/PS 3_0, and only GT2 - Firefly Forest executed. Artifact hashes and settings
are checked by `analyze.py`; process-elapsed FPS is not used.

## Results and acceptance

| Route/run | `.3dr` FPS | Receipt | Interpretation |
|---|---:|---|---|
| dxmt9 warm | 31.815619 | pass, exit 0 | Excluded warm-up |
| D9VK warm | 18.829296 | black_screen, exit 0 | Excluded; visual gate failed |
| D9VK r1 | 19.020905 | black_screen, exit 0 | Excluded; visual gate failed |
| dxmt9 r1 | 31.800011 | pass, exit 0 | Accepted baseline |
| dxmt9 r2 | 31.552811 | pass, exit 0 | Accepted baseline |

dxmt9 mean is **31.676411 FPS**, observed range/mean **0.78%**. Two repeats
are a baseline check, not a statistical confidence interval. Warm-up and
scored screenshots show the forest; this is not a pixel-exact oracle. No GPU
error-counter claim is possible with observers OFF.

Planned warm-up plus reversed-order A/B repetitions are not completed: both
D9VK attempts produce completely black automatic captures (mean luminance and
variance zero). Independent Computer Use window captures are also black,
including after restoring focus on the second attempt. Both processes exit
normally and emit `.3dr` files, but that does not satisfy the visual gate.
Whether this is actual presentation/rendering failure or a fullscreen capture
limitation still needs confirmation. Do not compare these FPS values against
dxmt9 or use them to infer that the H399 STALKER gap is workload-specific.

## Provider identity and failure signature

D9VK uses the same x86 Sikarugir `v1.10.3-20250511-async (macOS)` release as
H399, DLL SHA-256
`22511d1fbb15cdbc5365dfcb231f028af427533bfb6957505720dac0aca98e3e`.
The DLL is temporarily installed beside 3DMark05.exe with `--skip-stage` and
removed by hash-checked cleanup. The runtime log identifies MoltenVK 1.4.1;
the vendor library hash is
`3e365bfdfb4e9292d9cd1d896330c6d639721015d0443c6391ef86c104c17568`.
An in-process loaded-image check during warm-up confirms the app-local DLL,
winevulkan and `experiments/wine/vendor/libMoltenVK.dylib`.

Both D9VK logs contain repeated unknown-format `1178816557` warnings and
`VK_ERROR_FEATURE_NOT_PRESENT: Metal does not support disabling primitive
restart`. These are observations, not established causes. The swapchain
changes from 1024x768 to 1440x900 during fullscreen transition; the original
benchmark settings remain 1024x768. The historical MoltenVK 1.2.10 run also
has a fullscreen size transition, so that alone does not establish a new bug.

Final dxmt9 runs restage HEAD. No app-local D9VK DLL remains. README and
runtime code are unchanged. Preserve the failed runs; next resolve the visual
gate before resuming matched A/B. The old 27.041 FPS MoltenVK 1.2.10 reference
must not substitute for a current 1.4.1 result.
