# tests/shader_runner/corpus

Project-authored shader, fixed-function, texture, viewport, and render-state
readback corpus. Wine D3D9 tests are behavioral references only; do not copy
LGPL test code into this tree.

## Wine D3D9 Candidate Inventory

| Priority | Candidate | Target area | Notes |
|---|---|---|---|
| P0 | `ps_1_4_texdepth_depth_write` | `discard` / `render_state` | Add when ps_1_4 runtime bytecode is accepted. Covers `texdepth` depth writes. |
| P0 | `ps_3_0_vface_front_back_rt` | `render_state` | Front/back face sign across backbuffer and offscreen RT. |
| P0 | `ps_3_0_vpos_fragment_coords` | `viewport` | `vPos.xy` and pixel-center semantics. |
| P0 | `texbem_ps11_bumpenvmat` | `texture` | ps_1.x `texbem` plus bump env matrix. Requires ps_1.x texture opcode coverage. |
| P0 | `ffp_bumpenvmap_lscale_loffset` | `texture` | FFP bump-env signed decode, luminance scale, and offset. |
| P0 | `ps_3_0_unbound_sampler` | `texture` | Descriptor fallback for unbound 2D/cube/volume samplers. |
| P0 | `tss_texop_argtemp` | `texture` | FFP `D3DTA_TEMP` propagation across texture stages. |
| P0 | `tss_texcoordindex_generated` | `texture` | Generated camera-space position/normal/reflection/spheremap coordinates. |
| P1 | `ps_1_x_cnd_coissue` | `comparison` | ps_1.x `cnd`, co-issue, and writemask/swizzle limits. |
| P1 | `ps_3_0_nested_loop` | `flow_control` | Runtime nested loop and loop counter arithmetic. |
| P1 | `vs_3_0_loop_index` | `flow_control` | Vertex loop index/addressing readback. |
| P1 | `sincos_runtime` | `transcendental` | Runtime `sincos` precision instead of source-only coverage. |
| P1 | `sgn_runtime` | `transcendental` | Negative/zero/positive `sgn` readback. |
| P1 | `default_attribute_components` | `vs_specific` | Missing declaration component defaults. |
| P1 | `vshader_input_semantic_mapping` | `vs_specific` | Declaration usage/order/index mapping and color normalization. |
| P1 | `pretransformed_varying_ps3` | `vs_specific` | `POSITIONT` plus ps_3_0 varying semantics. |
| P1 | `texture_transform_flags_projected` | `texture` | `D3DTTFF_PROJECTED` count2/3/4 combinations. |
| P1 | `ps_dsy_derivative` | `arithmetic` | `dsy`/gradient direction on pixel quads. |
| P2 | `srgb_texture_formats_mips` | `texture` | Extend existing sRGB texture read to more formats and mips. |
| P2 | `srgb_write_formats` | `render_state` | Extend existing sRGB write to RT format matrix. |
| P2 | `mrt_independent_outputs` | `render_state` | MRT masks, clears, and independent output combinations. |
| P2 | `pixelshader_blending` | `render_state` | Broader shader alpha output and fixed blend matrix. |
| P2 | `depthbias_slope_constant` | `render_state` | Constant and slope-scaled depth bias. Needs runner hook. |
| P2 | `depth_clamp` | `render_state` | Viewport/depth clamp with out-of-range z. |
| P2 | `clip_planes_multi` | `render_state` | Multiple user clip planes and coordinate spaces. |
| P2 | `texture_blending_ops` | `texture` | FFP texture op matrix beyond the reduced cases. |
| P2 | `texop_range` | `texture` | `D3DTOP_*` clamp/range behavior. |
| P2 | `alphareplicate_dp3alpha` | `texture` | `D3DTA_ALPHAREPLICATE` and DP3 alpha routing. |
| P2 | `pointsize_psize` | `vs_specific` | FFP/VS point size, clamp, and attenuation. |
| P2 | `vertex_texture` | `vs_specific` | Vertex texture fetch, gated on support policy. |

Current runner-friendly Wine-derived additions include blend subtract, separate
alpha blend, FFP `addsigned`, and FFP `modulate2x` readback cases.
