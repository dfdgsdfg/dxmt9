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

**R-WILD-1.2** Experiments must not require Wine. They must run the application
against dxmt9 directly via a native macOS launcher that injects dxmt9 as the
D3D9 implementation (no PE loader, no Wine DLL stack). If a Wine path is
unavoidable, it must be documented as a dependency.

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
binary      = "experiments/apps/BasicHLSL/BasicHLSL.exe"
launcher    = "experiments/launchers/basicherl.sh"
reference   = "experiments/references/basicherl.png"
features    = ["vs_2_0", "ps_2_0", "lighting", "texturing"]
status      = "untested"   # passing | failing | untested

[[app]]
name        = "dx-sdk-tutorial07"
source      = "https://github.com/walbourn/directx-sdk-samples"
license     = "ms-directx-sdk"
binary      = "experiments/apps/Tutorial07/Tutorial07.exe"
launcher    = "experiments/launchers/tutorial07.sh"
reference   = "experiments/references/tutorial07.png"
features    = ["vs_2_0", "ps_2_0", "texturing"]
status      = "untested"

[[app]]
name        = "dx-sdk-hdrformats"
source      = "https://github.com/walbourn/directx-sdk-samples"
license     = "ms-directx-sdk"
binary      = "experiments/apps/HDRFormats/HDRFormats.exe"
launcher    = "experiments/launchers/hdrformats.sh"
reference   = "experiments/references/hdrformats.png"
features    = ["ps_3_0", "fp16_rt", "fp32_rt", "hdr_tonemap"]
status      = "untested"

[[app]]
name        = "dxut-simplesample"
source      = "https://github.com/walbourn/DXUT"
license     = "mit"
binary      = "experiments/apps/DXUTSimpleSample/SimpleSample.exe"
launcher    = "experiments/launchers/dxut_simplesample.sh"
reference   = "experiments/references/dxut_simplesample.png"
features    = ["vs_2_0", "ps_2_0", "skinned_mesh", "state_management"]
status      = "untested"

[[app]]
name        = "irrlicht-managed-lights"
source      = "https://github.com/zaki/irrlicht"
license     = "zlib"
binary      = "experiments/apps/irrlicht/20.ManagedLights"
launcher    = "experiments/launchers/irrlicht_managed_lights.sh"
reference   = "experiments/references/irrlicht_managed_lights.png"
features    = ["ffp", "dynamic_lighting", "scene_graph", "d3d9_backend"]
status      = "untested"
```

**R-WILD-5.2** The `status` field reflects the last known run result. A
`failing` entry must have an open issue documenting the failure mode.
