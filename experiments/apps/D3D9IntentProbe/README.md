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
- `specular-lighting` - FFP point-light specular contribution selected through
  the specular texture-stage argument.
- `shademode` - flat and Gouraud diffuse interpolation on a transformed
  triangle strip.
- `filling-convention` - 8x8 render-target shared-edge coverage for adjacent
  screen-space triangles.
- `mismatched-sample-types` - 2D/volume texture sampling through matching and
  mismatched sampler declarations.
- `max-index16` - `DrawIndexedPrimitive` with a 16-bit index referencing vertex
  `0xffff`.
- `null-format` - NULL render-target support path, skipped when unsupported,
  with depth-write intent when available.
- `depth-clamp` - transformed out-of-range depth handling follows
  `D3DPMISCCAPS_CLIPTLVERTS` clamp/clip intent.
- `clear-different-size-surfaces` - `Clear()` covers differently sized render
  targets and a larger depth surface.
- `color-fill` - `ColorFill()` accepted and rejected surface classes with
  target readback.
- `z-range` - depth compare behaviour for in-range `XYZRHW` z values on both
  sides of the depth clear value.
- `offscreen-surface` - render-target texture draw, restore, and texture
  sampling from the offscreen surface.
- `depth-stencil-size` - mismatched render-target/depth-stencil sizes validate
  correctly with depth disabled and report a valid depth-enabled state.
- `vshader-float16` - `FLOAT16_2` and `FLOAT16_4` vertex declaration inputs
  converted through a vertex/pixel shader pair.
- `shader-fog` - programmable vertex shader fog output blended by the fixed
  function pixel fog path.
- `vertex-texture` - `vs_3_0` vertex texture fetch from an `R32F` texture,
  skipped when the runtime does not advertise support.
- `ffp-w` - transformed `XYZRHW` draws with different reciprocal-W values.
- `texture-transform-flags` - `D3DTTFF_COUNT2` and projected `COUNT3`
  texture-transform modes.
- `texcoord-index-matrix` - texture stage coordinate index selection combined
  with a texture matrix.
- `uninitialized-varyings` - pixel shader readback from a varying not written
  by the active vertex shader.
- `per-stage-constant` - stage-local `D3DTSS_CONSTANT` routing across two FFP
  texture stages.
- `shader-fragment-coords` - `ps_3_0` `VPOS` fragment coordinate routing.

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
| `specular-lighting` | failing | FFP specular-lighting draw succeeds, but the center highlight reads back black. |
| `shademode` | failing | Flat/Gouraud diffuse draws succeed, but the sampled pixels remain black. |
| `filling-convention` | failing | 8x8 RT triangle draws succeed, but the render target remains at the clear color. |
| `mismatched-sample-types` | passing | Matching and mismatched 2D/volume sampler cases return the sampled texture colors. |
| `max-index16` | failing | `DrawIndexedPrimitive` with index `0xffff` succeeds, but the sampled quad remains black. |
| `null-format` | passing | Runtime reports NULL render-target format unsupported and exits through the documented skip path. |
| `depth-clamp` | passing | Transformed z above one is clipped on the current runtime and the probe exits cleanly. |
| `clear-different-size-surfaces` | failing | Render-target clears pass, but the larger lockable depth surface does not read back the expected half-depth clear. |
| `color-fill` | failing | `ColorFill` succeeds for target surfaces, but invalid surface classes are not rejected and target readback returns `D3DERR_INVALIDCALL`. |
| `z-range` | failing | In-range `LESS` / `GREATER` depth draws succeed but read back black. |
| `offscreen-surface` | failing | Offscreen render-target texture clears and draws, but sampling the drawn center returns the clear color. |
| `depth-stencil-size` | passing | Smaller depth-stencil with a larger render target validates with depth disabled and reports a valid depth-enabled state. |
| `vshader-float16` | passing | `FLOAT16_2` and `FLOAT16_4` declaration inputs convert through vs_2_0/ps_2_0 to the expected color. |
| `shader-fog` | failing | Shader fog draw calls succeed, but both fogged and unfogged halves read back black. |
| `vertex-texture` | failing | `R32F` vertex texture support is advertised, but vertex-sampler texture/sampler binding returns `D3DERR_INVALIDCALL`. |
| `ffp-w` | failing | `XYZRHW` draws with `rhw=1.0` and `rhw=0.5` succeed, but both sampled regions remain black. |
| `texture-transform-flags` | failing | `D3DTTFF_COUNT2` passes; projected `COUNT3` sampling reads the unprojected quadrant. |
| `texcoord-index-matrix` | failing | `TEXCOORDINDEX=1` draw succeeds, but the texture matrix flip is not reflected in the sampled quadrants. |
| `uninitialized-varyings` | failing | Pixel shader read from a VS-unwritten color varying returns white instead of the expected zero default. |
| `per-stage-constant` | failing | Per-stage constants and TSS setup succeed, but the draw reads back black instead of the combined constant color. |
| `shader-fragment-coords` | failing | `VPOS` ps_3_0 draw succeeds and the left side passes, but the right-side coordinate branch still reads red. |
