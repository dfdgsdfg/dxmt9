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
- `cube-volume-texture-update` - cube and volume `UpdateTexture` upload
  followed by shader sampling.
- `autogen-mipmap` - managed autogen-mipmap filter/state and generated
  sublevel sampling.
- `npot-filter-lod` - non-power-of-two explicit mip chain sampled through
  `MAXMIPLEVEL`.
- `managed-reset-texture` - managed texture draw, device reset, and redraw
  persistence.
- `sample-mask` - MSAA sample mask draw resolved to a deterministic partial
  color.
- `alpha-to-coverage` - Wine ATOC pseudo-format path with MSAA resolve
  evidence, skipped when unsupported.
- `cube-wrap` - cube sampler draw remains face-directed across sampler address
  modes.
- `line-aa-blending` - Wine line-antialiasing-adjacent alpha blending matrix
  reduced to center-pixel render intent.
- `default-attribute-components` - missing declaration color input defaults
  observed through shader color output.
- `vshader-input-types` - non-`D3DCOLOR` declaration input conversion observed
  through shader color output.
- `pointsize` - `D3DFVF_PSIZE` point-list rasterization and uncovered-pixel
  readback.
- `depth-stencil-init` - depth-surface clear initialization and reject/accept
  ordering.

Each mode exits non-zero on failed D3D calls or mismatched readback. The app is
kept intentionally small; broader HRESULT conformance belongs in
`tests/conformance/d3d9`.

Wine `visual.c:test_mipmap_upload` is intentionally listed but not implemented
as a runtime mode: it writes every mip level through the level-0 lock pointer,
which can run past the API-visible locked rectangle. Track that as a compatibility
gap or conformance-only oracle, not as an unsafe experiment probe.

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
| `cube-volume-texture-update` | passing | Cube and volume `UpdateTexture` uploads sample through ps_2_0 cube/volume shaders with expected colors. |
| `autogen-mipmap` | passing | Current runtime reports no A8R8G8B8 autogen support and exits through the documented skip path. |
| `npot-filter-lod` | failing | NPOT mip chain creates and draws, but `MAXMIPLEVEL=1` still samples level 0. |
| `managed-reset-texture` | passing | Managed texture renders before and after `ResetEx` with the expected color. |
| `sample-mask` | failing | `D3DRS_MULTISAMPLEMASK=0x1` resolves to the red clear color instead of the expected partial covered color. |
| `alpha-to-coverage` | passing | Runtime reports unsupported ATOC pseudo-format and exits through the documented skip path. |
| `cube-wrap` | passing | Cube sample remains on the +X face across wrap, mirror, clamp, border, and mirror-once address modes. |
| `line-aa-blending` | failing | Alpha-blended XYZ diffuse draws leave the clear color unchanged, so the Wine blend matrix is not satisfied. |
| `default-attribute-components` | passing | Missing declaration color input defaults to zero and shader color output reads back black over a red clear. |
| `vshader-input-types` | passing | `UBYTE4N` declaration color converts through vs_2_0/ps_2_0 to the expected green output. |
| `pointsize` | failing | `D3DFVF_PSIZE` point draw succeeds but the center pixel remains black. |
| `depth-stencil-init` | failing | Depth-surface clear/draw calls succeed, but the accepted nearer draw still reads back black. |
