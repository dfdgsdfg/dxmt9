---
type: "Spec Requirements"
title: "Experiments Requirements"
description: "Experiments requirements and compatibility contracts."
tags: [specs, experiments, requirements]
---

# Experiments Requirements

Experiments are uncontrolled, end-to-end integration runs against real D3D9
software. Unlike tests (which have pixel-exact oracles), experiments ask
"does this real application run correctly through dxmt9?" — pass criteria are
coarser and human-reviewable.

---

## 1. Scope

**R-WILD-1.1** Each experiment must target a specific real D3D9 application:
a known-working open-source game, tech demo, or SDK sample that exercises a
meaningful subset of the D3D9 surface area.

**R-WILD-1.2** Every experiment runs through the documented launcher harness
(`run_experiment.py` + catalogue launchers), in one of two lanes:

- **Native lane** — native macOS sample binaries link or inject dxmt9
  directly (no PE loader, no Wine DLL stack). This is the preferred lane
  when the application source is available to build natively.
- **Wine lane** — real Windows PE applications (the wild catalogue: 3DMark05,
  SFIV, and peers) run under a managed Wine runtime. The Wine runtime is a
  first-class, manifest-selected dependency governed by
  `specs/experiments/runtime/` (`R-RT-*`) and the operational rules in
  `agents/rules/test_wild.rules.md`; it is not an ad-hoc exception. The
  catalogue entry's `wine_id` names the runtime, and runs record the resolved
  `wine_root` in `result.json` so every result states the runtime it ran
  under.

An experiment must not depend on a Wine build outside the manifest, and the
native lane must not silently acquire a Wine dependency — moving an entry
between lanes is a catalogue change, not a launcher default.

> *Revised 2026-08-06.* The original clause required all experiments to be
> Wine-free with native injection, treating Wine as an "unavoidable
> documented dependency". That predates the wild catalogue: real PE
> applications cannot run without a PE loader, the winemetal bridge exists
> precisely because Wine is the production environment, and the runtime
> manifest (`R-RT-*`) has since made the Wine dependency explicit, versioned,
> and audited. The native lane remains for buildable samples.

**R-WILD-1.3** Each experiment must be reproducible: given the same application
binary and the same dxmt9 build, the outcome must be deterministic across runs
on the same machine.

---

## 2. Pass Criteria

**R-WILD-2.1** An experiment passes if all of the following hold:
- The application launches without crashing (exit code 0 or normal shutdown)
- At least one frame is rendered (pixel variance above a minimum threshold)
- No Metal validation errors or `DXMT_ASSERT` failures are triggered
- The rendered output is visually plausible (fuzzy screenshot comparison or
  human sign-off)

**R-WILD-2.2** An experiment fails if any of the following occur:
- Crash or GPU device lost during the run
- Black screen (all pixels below luminance threshold) for more than 2 seconds
- Metal API validation layer reports an error
- `DXMT_ASSERT` fires

**R-WILD-2.3** Pixel-exact comparison is not required. Fuzzy comparison uses
structural similarity (SSIM ≥ 0.90 against a stored reference screenshot) or
human sign-off for the initial reference.

---

## 3. Application Catalogue

Each experiment entry specifies the application and what D3D9 features it
exercises. The catalogue grows as compatibility improves.

**R-WILD-3.1** The initial catalogue must include at least one application per
major feature group:

| Application | Source | License | Key features exercised |
|---|---|---|---|
| Microsoft DirectX SDK `BasicHLSL` | https://github.com/walbourn/directx-sdk-samples | MS DirectX SDK (redistributable) | vs_2_0/ps_2_0, constant buffers, diffuse lighting |
| Microsoft DirectX SDK `Tutorial07` | https://github.com/walbourn/directx-sdk-samples | MS DirectX SDK (redistributable) | Texture mapping, vs_2_0/ps_2_0 |
| Microsoft DirectX SDK `HDRFormats` | https://github.com/walbourn/directx-sdk-samples | MS DirectX SDK (redistributable) | FP16/FP32 render targets, HDR tone-mapping, ps_3_0 |
| DXUT `SimpleSample` / `BasicHLSL11` | https://github.com/walbourn/DXUT | MIT | Skinned meshes, state management, more complex draw loop |
| Irrlicht engine demo (`20.ManagedLights`) | https://github.com/zaki/irrlicht | zlib | D3D9 backend, FFP lighting with 8 dynamic lights, scene graph |

**R-WILD-3.2** When a new opcode group or backend feature is implemented, at
least one catalogue entry must be identified that exercises it in a real
rendering context.

**R-WILD-3.3** Catalogue entries must distinguish project-owned fixtures from
third-party fixtures and external local applications. An experiment artifact is
not covered by the dxmt9 MIT grant unless its entry explicitly uses
`license_scope = "project-mit"`. Third-party sources, SDK samples, permissive
sample code, commercial applications, and reference screenshots must carry
`license`, `source_kind`, and `license_scope` metadata.

---

## 4. Reference Screenshots

**R-WILD-4.1** Each catalogue entry must have a reference screenshot committed
to `experiments/references/<app-name>.png`. The reference is captured from a
known-good run (either on Windows D3D9 or a previously passing dxmt9 build)
and committed after human review.

**R-WILD-4.2** Reference screenshots must be captured at a fixed resolution
(1280×720) with a fixed scene state (deterministic camera position / game tick).

**R-WILD-4.3** Updating a reference screenshot requires an explicit commit with
a comment explaining why the visual output changed. Automated reference updates
are forbidden.

---

## 5. Catalogue Manifest

**R-WILD-5.1** `experiments/CATALOGUE.toml` must list every catalogue entry:

```toml
[[app]]
name        = "dx-sdk-basicherl"
source      = "https://github.com/walbourn/directx-sdk-samples"
license     = "ms-directx-sdk"
source_kind = "third-party-fixture"
license_scope = "third-party-fixture"
binary      = "experiments/apps/sample-d3d9-basic-hlsl/sample-d3d9-basic-hlsl.exe"
launcher    = "experiments/launchers/basicherl.sh"
reference   = "experiments/references/basicherl.png"
features    = ["vs_2_0", "ps_2_0", "lighting", "texturing"]
status      = "untested"   # passing | failing | untested

[[app]]
name        = "sample-d3d9-tutorial07"
source      = "https://github.com/walbourn/directx-sdk-samples"
license     = "ms-directx-sdk"
source_kind = "third-party-fixture"
license_scope = "third-party-fixture"
binary      = "experiments/apps/sample-d3d9-tutorial07/sample-d3d9-tutorial07.exe"
launcher    = "experiments/launchers/tutorial07.sh"
reference   = "experiments/references/tutorial07.png"
features    = ["vs_2_0", "ps_2_0", "texturing"]
status      = "untested"

[[app]]
name        = "sample-d3d9-hdr-formats"
source      = "https://github.com/walbourn/directx-sdk-samples"
license     = "ms-directx-sdk"
source_kind = "third-party-fixture"
license_scope = "third-party-fixture"
binary      = "experiments/apps/sample-d3d9-hdr-formats/sample-d3d9-hdr-formats.exe"
launcher    = "experiments/launchers/hdrformats.sh"
reference   = "experiments/references/hdrformats.png"
features    = ["ps_3_0", "fp16_rt", "fp32_rt", "hdr_tonemap"]
status      = "untested"

[[app]]
name        = "dxut-simplesample"
source      = "https://github.com/walbourn/DXUT"
license     = "mit"
source_kind = "third-party-fixture"
license_scope = "third-party-fixture"
binary      = "experiments/apps/sample-d3d9-dxut-simple/sample-d3d9-dxut-simple.exe"
launcher    = "experiments/launchers/dxut_simplesample.sh"
reference   = "experiments/references/dxut_simplesample.png"
features    = ["vs_2_0", "ps_2_0", "skinned_mesh", "state_management"]
status      = "untested"

[[app]]
name        = "sample-d3d9-irrlicht-lights"
source      = "https://github.com/zaki/irrlicht"
license     = "zlib"
source_kind = "third-party-fixture"
license_scope = "third-party-fixture"
binary      = "experiments/apps/sample-d3d9-irrlicht-lights/20.ManagedLights"
launcher    = "experiments/launchers/irrlicht_managed_lights.sh"
reference   = "experiments/references/irrlicht_managed_lights.png"
features    = ["ffp", "dynamic_lighting", "scene_graph", "d3d9_backend"]
status      = "untested"
```

**R-WILD-5.2** The `status` field reflects the last known run result. A
`failing` entry must have an open issue documenting the failure mode.

**R-WILD-5.3** The manifest must include `source_kind`, `license`, and
`license_scope` for every entry. Accepted `source_kind` values are
`project-authored`, `third-party-fixture`, `structure-reference`, and
`external-application`. Accepted `license_scope` values are `project-mit`,
`third-party-fixture`, and `external-not-vendored`.
