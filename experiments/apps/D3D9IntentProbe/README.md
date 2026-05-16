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
- `ffp-vertex-blend-extended` - multi-weight FVF beta fields plus indexed
  vertex-blending through a declaration `BLENDINDICES` input.
- `texture-transform` - texture matrix plus `D3DTTFF_COUNT2` sampling intent.
- `generated-texcoords` - generated camera-space normal texture coordinates.
- `color-material` - `COLORVERTEX` and material-source lighting intent.
- `sysmem-draw-processvertices` - system-memory stream drawing and
  `ProcessVertices` output sanity.
- `dynamic-map-sync` - dynamic VB `DISCARD` / `NOOVERWRITE` and mapped draw
  ordering.
- `attached-rt-sampling` - render-target-as-texture hazard with EVENT query
  synchronization.
- `blit-format-conversion` - `StretchRect` format conversion readback.
- `reset-resource-lifecycle` - Ex reset with default-pool and managed resources.
- `depth-stencil-viewport-scissor` - depth testing constrained by viewport and
  scissor state.
- `mipmap-update-texture` - system-memory mip tree upload through
  `UpdateTexture`.
- `multisample-resolve` - MSAA render-target clear resolved through
  `StretchRect`.
- `fog-depthbias` - FFP fog and depth-bias ordering.
- `draw-indexed-up-edges` - `DrawIndexedPrimitiveUP` base-vertex and implicit
  state reset checks.
- `shader-edge-visual` - pixel-shader validation plus shader output readback.
- `d3d9ex-wsi` - D3D9Ex device state, frame-latency, and reset checks.

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
| `ffp-vertex-blend-extended` | passing | `XYZB3` implicit matrix and declaration indexed vertex blend render at the expected location. |
| `texture-transform` | passing | `D3DTS_TEXTURE0` flip matrix plus `D3DTTFF_COUNT2` renders the expected quadrant colors. |
| `generated-texcoords` | failing | Camera-space normal generated texture coordinate path does not pass readback. |
| `color-material` | failing | FFP lighting/material-source draw does not pass readback. |
| `sysmem-draw-processvertices` | failing | System-memory draw / `ProcessVertices` path does not pass self-validation. |
| `dynamic-map-sync` | failing | Dynamic `DISCARD` / `NOOVERWRITE` or mapped draw path does not pass readback. |
| `attached-rt-sampling` | passing | Attached render-target sampling with EVENT query synchronization accumulates the expected color. |
| `blit-format-conversion` | failing | `StretchRect` format-conversion replay returns `D3DERR_INVALIDCALL`. |
| `reset-resource-lifecycle` | failing | Ex reset with default/managed resources does not pass the post-reset managed texture readback. |
| `depth-stencil-viewport-scissor` | failing | Depth/scissor/viewport intersection does not pass self-validation. |
| `mipmap-update-texture` | passing | Matching 2D A8R8G8B8 mip-tree `UpdateTexture` uploads level 0 and renders the expected color. |
| `multisample-resolve` | passing | 2x MSAA A8R8G8B8 clear resolves through `StretchRect` to the expected color. |
| `fog-depthbias` | failing | FFP fog/depth-bias ordering does not pass readback. |
| `draw-indexed-up-edges` | failing | `DrawIndexedPrimitiveUP` base-vertex or implicit state reset path does not pass self-validation. |
| `shader-edge-visual` | failing | Pixel-shader validation/output path does not pass self-validation. |
| `d3d9ex-wsi` | passing | `CheckDeviceState`, frame-latency roundtrip, `ResetEx`, and post-reset clear pass. |
