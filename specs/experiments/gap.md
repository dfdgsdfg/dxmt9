---
type: "Spec Gap"
title: "Experiments Gap"
description: "Implementation and evidence gaps for wild integration experiments."
tags: [specs, gap, experiments]
---

# Experiments Gap

Domain-owned implementation and evidence gap tracker. Use the [root gap index](../gap.md) for cross-domain rollup.

## Experiments Layer

⚠️ Partial. The runner, launcher harness, output layout, one verified local
bootstrap entry, and the full initial real-application catalogue from
R-WILD-3.1 exist. The layer remains partial because the current implementation
depends on the Wine builtin path rather than the native macOS injection path
described by R-WILD-1.2.

| Area | Status | Spec |
|---|---|---|
| `experiments/CATALOGUE.toml` + launcher tree scaffolded | ✅ | R-WILD-5.1 |
| Wine launcher injection harness (`run_experiment.py`, launcher scripts, Heroic staging) | ⚠️ | R-WILD-1.2 |
| Internal backbuffer frame dump + SSIM comparison + `result.json` output | ✅ | R-WILD-2.3, R-WILD-4.1 |
| Bootstrap verified entry: `conf-d3d9-wsi-present` on Heroic Wine 11.5 | ✅ | local workflow validation |
| Verified real application entries: `sample-d3d9-basic-hlsl`, `sample-d3d9-tutorial07`, `sample-d3d9-hdr-formats`, `sample-d3d9-dxut-simple`, `sample-d3d9-irrlicht-lights` | ✅ | Heroic Wine 11.5, direct capture, SSIM 1.0000 |
| Exploratory commercial entry: `app-d3d9-anno-1404` | ⚠️ | supported on Heroic `Wine-11.6-DXMT`; plain `Wine-11.6` is research-only due to Wine `d3dx10_43` aborts |
| Initial catalogue from R-WILD-3.1 staged and verified | ✅ | All five required feature groups covered |
| Reference screenshots for initial catalogue entries | ✅ | R-WILD-4.1 |

---
