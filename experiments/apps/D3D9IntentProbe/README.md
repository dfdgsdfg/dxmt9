# D3D9IntentProbe

Repo-local D3D9 probe app for small self-validating runtime checks derived from
Wine D3D9 visual/device/stateblock coverage themes.

Modes:

- `basic-ffp` - shader-free transformed quad draw and readback.
- `render-state` - color-write mask and alpha-test intent.
- `blit-copy` - `ColorFill`, `UpdateSurface`, `StretchRect`, and
  `GetRenderTargetData` copy intent.
- `stateblock` - compact stateblock capture/apply for render state, TSS, and
  transform state.
- `query` - public query support and EVENT completion behaviour.
- `ffp-vertex-blend` - one-weight fixed-function vertex blending through
  `D3DVBF_1WEIGHTS`.

Each mode exits non-zero on failed D3D calls or mismatched readback. The app is
kept intentionally small; broader HRESULT conformance belongs in
`tests/conformance/d3d9`.

Current dxmt9 runtime status from local Wine 11.7 runs:

| Mode | Status | Observed gap |
|---|---|---|
| `basic-ffp` | failing | FFP diffuse draw returns black on backbuffer readback. |
| `render-state` | failing | Draw-dependent color-write / alpha-test accepts read back black. |
| `blit-copy` | failing | `StretchRect` path passes; `UpdateSurface` to render target reads back black. |
| `stateblock` | failing | `D3DSBT_ALL` cull restore passes; recorded TSS/transform restore does not. |
| `query` | passing | EVENT query completes; OCCLUSION/TIMESTAMP support probes return valid support status. |
| `ffp-vertex-blend` | passing | One-weight `D3DVBF_1WEIGHTS` FVF path renders at the translated location. |
