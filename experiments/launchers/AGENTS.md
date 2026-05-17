# experiments/launchers

Per-app or synthetic-probe shell launchers invoked by
`scripts/run_apps/run_experiment.py` per `experiments/CATALOGUE.toml`'s
`launcher` field. Filenames match `CATALOGUE.name` (kebab-case) so
`rg <app>` finds every artifact.

## Shared Helpers

| File | Role |
|---|---|
| `common.sh` | Sourced by every launcher; provides `exp_stage_dxmt9`, `exp_run_wine_binary`, prefix discovery, capture hooks, and log hooks. |
| `dx9_fast_sanity.sh` | Bundle launcher for the d9vk fast-sanity apps (`d9vk-d3d9-clear`, `d9vk-d3d9-buffer`, etc.). |

## App-Bound Launchers

| Family | Launchers |
|---|---|
| D3D9 SDK / DXUT / third-party samples | `dx-sdk-basichlsl.sh`, `dx-sdk-tutorial07.sh`, `dx-sdk-hdrformats.sh`, `dxut-simple-sample.sh`, `irrlicht-managed-lights.sh` |
| Self-authored apps | `dxmt9-multitexture-terrain.sh`, `dxmt9-water-rt.sh`, `dxmt9-wsi-present-local.sh`, `dxmt9-d3d9-basic-ffp.sh`, `dxmt9-d3d9-render-state.sh`, `dxmt9-d3d9-blit-copy.sh`, `dxmt9-d3d9-stateblock.sh`, `dxmt9-d3d9-query.sh`, `dxmt9-d3d9-ffp-vertex-blend.sh`, `dxmt9-d3d9-ffp-vertex-blend-extended.sh`, `dxmt9-d3d9-texture-transform.sh`, `dxmt9-d3d9-generated-texcoords.sh`, `dxmt9-d3d9-color-material.sh`, `dxmt9-d3d9-sysmem-draw-processvertices.sh`, `dxmt9-d3d9-dynamic-map-sync.sh`, `dxmt9-d3d9-attached-rt-sampling.sh`, `dxmt9-d3d9-blit-format-conversion.sh`, `dxmt9-d3d9-reset-resource-lifecycle.sh`, `dxmt9-d3d9-depth-stencil-viewport-scissor.sh`, `dxmt9-d3d9-mipmap-update-texture.sh`, `dxmt9-d3d9-multisample-resolve.sh`, `dxmt9-d3d9-fog-depthbias.sh`, `dxmt9-d3d9-draw-indexed-up-edges.sh`, `dxmt9-d3d9-shader-edge-visual.sh`, `dxmt9-d3d9ex-wsi.sh`, `dxmt9-d3d9-cube-volume-texture-update.sh`, `dxmt9-d3d9-autogen-mipmap.sh`, `dxmt9-d3d9-npot-filter-lod.sh`, `dxmt9-d3d9-managed-reset-texture.sh`, `dxmt9-d3d9-sample-mask.sh`, `dxmt9-d3d9-alpha-to-coverage.sh`, `dxmt9-d3d9-cube-wrap.sh`, `dxmt9-d3d9-line-aa-blending.sh`, `dxmt9-d3d9-default-attribute-components.sh`, `dxmt9-d3d9-vshader-input-types.sh`, `dxmt9-d3d9-pointsize.sh`, `dxmt9-d3d9-depth-stencil-init.sh`, `dxmt9-d3d9-specular-lighting.sh`, `dxmt9-d3d9-shademode.sh`, `dxmt9-d3d9-filling-convention.sh`, `dxmt9-d3d9-mismatched-sample-types.sh`, `dxmt9-d3d9-max-index16.sh`, `dxmt9-d3d9-null-format.sh`, `dxmt9-d3d9-depth-clamp.sh`, `dxmt9-d3d9-clear-different-size-surfaces.sh`, `dxmt9-d3d9-color-fill.sh`, `dxmt9-d3d9-z-range.sh`, `dxmt9-d3d9-offscreen-surface.sh`, `dxmt9-d3d9-depth-stencil-size.sh` |
| D3D9IntentProbe SH-D shader probes | `dxmt9-d3d9-vshader-float16.sh`, `dxmt9-d3d9-shader-fog.sh`, `dxmt9-d3d9-vertex-texture.sh`, `dxmt9-d3d9-ffp-w.sh`, `dxmt9-d3d9-texture-transform-flags.sh`, `dxmt9-d3d9-texcoord-index-matrix.sh`, `dxmt9-d3d9-uninitialized-varyings.sh`, `dxmt9-d3d9-per-stage-constant.sh`, `dxmt9-d3d9-shader-fragment-coords.sh` |
| Commercial / 3rd-party titles | `3dmark05.sh`, `3dmark06.sh`, `street-fighter-iv-benchmark.sh` |
| Synthetic perf probes | `dxmt9-perf-bridge-empty.sh`, `dxmt9-perf-chain-parametric.sh`, `dxmt9-perf-depth-heavy.sh`, `dxmt9-perf-encode-replay.sh`, `dxmt9-perf-ffp-only.sh`, `dxmt9-perf-many-draw.sh`, `dxmt9-perf-multi-rt.sh`, `dxmt9-perf-offscreen-heavy.sh`, `dxmt9-perf-present-loop.sh`, `dxmt9-perf-present-only.sh`, `dxmt9-perf-skeletal.sh` |

## Conventions

- Each launcher sources `common.sh` first, then calls `exp_stage_dxmt9` and
  `exp_run_wine_binary`.
- Per-app variation comes from `CATALOGUE.toml` fields (`binary`,
  `window_title`, `capture_frame`, env vars).
- Launcher filename must exactly match `CATALOGUE.name` unless the launcher is
  an explicitly documented bundle such as `dx9_fast_sanity.sh`.
- Adding a new app requires a `[[app]]` entry, a launcher, and a `build_script`
  field when `--build` support is expected.
- Synthetic probes share probe binaries under `experiments/apps/`; the launcher
  injects different env vars or args.

Run through the consolidated runner:

```sh
python3 scripts/run_apps/run_experiment.py run <name>
python3 scripts/run_apps/run_experiment.py run <name> --build
```
