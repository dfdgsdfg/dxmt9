# Wine D3D9 Test Inventory

Inventory of every Wine `dlls/d3d9/tests/{visual,device,d3d9ex,stateblock}.c`
test reachable from a `START_TEST` body, with the dxmt9 porting status
and the dxmt9 PE conformance function(s) that carry the evidence.

This file is the long-lived companion to the gitignored
`specs/wine_test.plan.md`. The plan tracks per-round implementation
staging; this gap doc tracks **which Wine oracles exist** and
**where each one is mirrored** in the dxmt9 test surface.

Generated from Wine source + plan tables by
`scripts/tools/gen_wine_d3d9_test_inventory.py` (see commit history).

## Provenance

The inventory is reproducible from two pinned commits: the dxmt9
commit whose plan / manifest fed the status column, and the Wine
commit whose source provided the line numbers. Capture both before
relying on the table for any cross-reference.

### dxmt9 generation revision

Captured from `git -C <repo>` at the moment this file was rendered.
A dirty work tree means the porting-status column may include
changes not yet visible upstream.

| Field | Value |
|-------|-------|
| Commit | `9bfc70f27aad8fc8b87bf5dc4a30f301275e57c8` — **work tree dirty** |
| Short  | `9bfc70f` |
| Tag / describe | `9bfc70f` |
| Author date | `2026-05-23` |
| Subject | tests/conformance/d3d9: add test_create_texture_npot_policy scaffold; test_npot_textures partial → scaffolded |

### Wine reference revision

The `Wine line` column below points into the Wine commit captured at
generation time. To reproduce the line numbers verbatim, check out
the same commit in the Wine checkout before opening the source.

| Field | Value |
|-------|-------|
| Commit | `6e073d28dee3af7f4c965daec94644e0f9f92727` |
| Short  | `6e073d2` |
| Tag / describe | `wine-11.6` |
| Author date | `2026-04-03` |
| Subject | Release 11.6. |
| Upstream | https://gitlab.winehq.org/wine/wine/-/commit/6e073d28dee3af7f4c965daec94644e0f9f92727 |

```sh
# Reproduce the inventory verbatim from a clean tree:
git -C "$DXMT9_REPO" checkout 9bfc70f27aad8fc8b87bf5dc4a30f301275e57c8
git -C "$WINE_REPO"  checkout 6e073d28dee3af7f4c965daec94644e0f9f92727
python3 scripts/tools/gen_wine_d3d9_test_inventory.py
```

## Legend

| Icon | Status meaning | Where evidence lives |
|------|----------------|----------------------|
| ✅ | `covered` | Full evidence in at least one lane (PE conformance, shader-runner readback, EXP probe, or native unit test). |
| 📐 | `scaffolded` | PE conformance scaffold registered in `tests/conformance/d3d9/MANIFEST.toml` and executed by `dxmt9-d3d9-conformance.exe`. |
| 🟢 | `covered/partial` | Covered in one lane; another lane has only partial evidence. |
| 🟡 | `partial` | Some evidence exists; matrix / breadth coverage still pending. |
| 🟠 | `failing/partial` | Mixed evidence; some sub-cases pass, others record failing readbacks. |
| 🔴 | `failing` | Test exists in the dxmt9 surface but records a deliberate failing readback. |
| ⏸️ | `deferred` / `partial/deferred` | Acknowledged gap; not yet fixable from the dxmt9 PE side alone. |
| ❓ | UNTRACKED | Wine test discovered after the last plan refresh. None today — flag if it appears. |

## Summary

| Wine source | Tests | covered | scaffolded | partial | failing | other |
|-------------|-----:|--------:|----------:|--------:|--------:|------:|
| `visual.c` | 135 | 65 | 55 | 15 | 0 | 0 |
| `device.c` | 105 | 1 | 102 | 2 | 0 | 0 |
| `d3d9ex.c` | 27 | 0 | 25 | 2 | 0 | 0 |
| `stateblock.c` | 1 | 0 | 1 | 0 | 0 | 0 |
| **TOTAL** | **268** | **66** | **183** | **19** | **0** | **0** |

`other` rolls up `deferred`, `partial/deferred`, and any UNTRACKED rows.

## Visual / Rendering Tests (`visual.c`)

Source: [`dlls/d3d9/tests/visual.c`](https://gitlab.winehq.org/wine/wine/-/blob/master/dlls/d3d9/tests/visual.c)

| Wine entry | Wine line | Status | dxmt9 PE function(s) / evidence |
|------------|----------:|:------:|---------------------------------|
| `add_dirty_rect_test` | 18885 | 📐 | `test_visual_add_dirty_rect_policy` |
| `alphareplicate_test` | 13262 | ✅ | — |
| `alphatest_test` | 13507 | ✅ | — |
| `clear_test` | 1154 | 📐 | `test_visual_clear_color_only_policy` |
| `clip_planes_test` | 16182 | 🟡 | — |
| `cnd_test` | 6675 | ✅ | — |
| `color_fill_test` | 1663 | 📐 | `test_visual_colorfill_format_policy` |
| `conditional_np2_repeat_test` | 9841 | ✅ | — |
| `constant_clamp_ps_test` | 6379 | ✅ | — |
| `depth_blit_test` | 14713 | 🟡 | — |
| `depth_bounds_test` | 14307 | 📐 | `test_visual_depth_bounds_caps_policy` |
| `depth_buffer2_test` | 14597 | 📐 | `test_visual_depth_buffer_reset_policy` |
| `depth_buffer_test` | 14442 | 📐 | `test_visual_depth_buffer_clear_policy` |
| `depth_clamp_test` | 14124 | 📐 | `test_visual_depth_clamp_render_state_policy` |
| `dp2add_ps_test` | 6551 | ✅ | — |
| `dp3_alpha_test` | 13324 | ✅ | — |
| `ds_size_test` | 16645 | 📐 | `test_visual_ds_smaller_than_rt_policy` |
| `fixed_function_bumpmap_test` | 10140 | 📐 | `test_visual_bumpenvmap_tss_policy` |
| `fixed_function_decl_test` | 9042 | ✅ | — |
| `float_texture_test` | 5022 | 📐 | `test_visual_float_texture_format_policy` |
| `fog_special_test` | 18220 | ✅ | — |
| `fog_test` | 2118 | ✅ | — |
| `fog_with_shader_test` | 2972 | ✅ | — |
| `fp_special_test` | 16285 | ✅ | `test_visual_fp_special_caps_policy` |
| `g16r16_texture_test` | 5101 | 📐 | `test_visual_g16r16_texture_format_policy` |
| `intz_test` | 14871 | 📐 | `test_vendor_policy_intz_caps` |
| `lighting_test` | 490 | 📐 | `test_visual_lighting_render_state_policy` |
| `loop_index_test` | 13791 | ✅ | — |
| `maxmip_test` | 4671 | ✅ | — |
| `multiple_rendertargets_test` | 11617 | 🟢 | — |
| `multisample_get_rtdata_test` | 17106 | 🟡 | — |
| `multisampled_depth_buffer_test` | 17240 | 📐 | `test_visual_multisampled_depth_buffer_caps_policy` |
| `nested_loop_test` | 7219 | ✅ | — |
| `np2_stretch_rect_test` | 12487 | 🟡 | — |
| `offscreen_test` | 2850 | 📐 | `test_visual_offscreen_surface_creation_policy` |
| `pixelshader_blending_test` | 11844 | ✅ | — |
| `pretransformed_varying_test` | 7316 | ✅ | `test_visual_pretransformed_vertex_declaration_policy` |
| `release_buffer_test` | 4923 | 📐 | `test_visual_release_buffer_bound_policy` |
| `resz_test` | 17547 | 📐 | `test_vendor_policy_resz_caps` |
| `sgn_test` | 13917 | ✅ | — |
| `shadow_test` | 15880 | 📐 | `test_visual_shadow_depth_compare_caps_policy` |
| `sincos_test` | 13684 | ✅ | — |
| `srgbtexture_test` | 8362 | 📐 | `test_visual_srgb_texture_caps_policy` |
| `srgbwrite_format_test` | 16509 | 📐 | `test_visual_srgb_write_caps_policy` |
| `stencil_cull_test` | 10394 | 📐 | `test_visual_stencil_cull_caps_policy` |
| `stream_test` | 12133 | 📐 | `test_stream_source_frequency_state` |
| `stretchrect_test` | 4048 | 🟡 | — |
| `test_3dc_formats` | 19655 | 📐 | `test_visual_3dc_format_caps_policy` |
| `test_alpha_to_coverage` | 26521 | ✅ | — |
| `test_backbuffer_resize` | 24193 | 📐 | `test_backbuffer_resize_present_parameter_policy` |
| `test_blend` | 8876 | ✅ | — |
| `test_blit_format_conversion` | 29162 | 📐 | `test_visual_blit_format_conversion_policy` |
| `test_buffer_no_dirty_update` | 19429 | 📐 | `test_visual_buffer_no_dirty_update_policy` |
| `test_clear_different_size_surfaces` | 1561 | 📐 | `test_visual_clear_smaller_rt_policy` |
| `test_color_clamping` | 23463 | ✅ | — |
| `test_color_vertex` | 25212 | ✅ | — |
| `test_compare_instructions` | 7614 | ✅ | — |
| `test_constant_clamp_vs` | 6189 | ✅ | — |
| `test_cube_wrap` | 2664 | ✅ | — |
| `test_default_attribute_components` | 27807 | ✅ | — |
| `test_default_diffuse` | 27607 | ✅ | — |
| `test_depth_stencil_init` | 22556 | 📐 | `test_visual_depth_stencil_init_policy` |
| `test_depthbias` | 21851 | ✅ | — |
| `test_desktop_window` | 25880 | 🟡 | — |
| `test_draw_mapped_buffer` | 26213 | 🟡 | — |
| `test_drawindexedprimitiveup` | 24293 | ✅ | — |
| `test_dsy` | 23840 | ✅ | — |
| `test_dynamic_map_synchronization` | 26843 | ✅ | — |
| `test_evict_bound_resources` | 23967 | 📐 | `test_visual_evict_managed_resources_policy` |
| `test_fetch4` | 15189 | 📐 | `test_vendor_policy_fetch4_caps` |
| `test_ffp_w` | 28095 | ✅ | — |
| `test_filling_convention` | 26975 | 📐 | `test_visual_filling_convention_caps_policy` |
| `test_flip` | 22028 | 📐 | `test_visual_swapchain_flip_present_policy` |
| `test_fog` | 28198 | ✅ | — |
| `test_fog_interpolation` | 19817 | ✅ | — |
| `test_format_conversion` | 27960 | 🟡 | — |
| `test_fragment_coords` | 10666 | ✅ | — |
| `test_generate_mipmap` | 5644 | ✅ | — |
| `test_generated_texcoords` | 29067 | ✅ | — |
| `test_lighting_matrices` | 28945 | 📐 | `test_visual_lighting_world_view_matrix_policy` |
| `test_line_antialiasing_blending` | 23684 | 📐 | `test_visual_line_antialiasing_blending_state_policy` |
| `test_managed_generate_mipmap` | 27515 | 🟡 | — |
| `test_managed_reset` | 27484 | ✅ | — |
| `test_map_synchronisation` | 24996 | ✅ | — |
| `test_max_index16` | 24074 | 📐 | `test_visual_max_index16_draw_policy` |
| `test_mipmap_autogen` | 5773 | ✅ | — |
| `test_mipmap_upload` | 27550 | 📐 | `test_vendor_policy_mipmap_upload_policy` |
| `test_mismatched_sample_types` | 25977 | ✅ | — |
| `test_mova` | 1965 | ✅ | — |
| `test_multisample_get_front_buffer_data` | 17168 | 📐 | `test_visual_multisample_get_front_buffer_data_policy` |
| `test_multisample_init` | 22489 | 🟡 | — |
| `test_multisample_mismatch` | 20823 | 📐 | `test_visual_multisample_rt_ds_mismatch_policy` |
| `test_multisample_stretch_rect` | 4494 | 🟡 | — |
| `test_mvp_software_vertex_shaders` | 24522 | 📐 | `test_visual_mvp_software_vp_policy` |
| `test_negative_fixedfunction_fog` | 19957 | 🟢 | — |
| `test_nrm_instruction` | 25655 | ✅ | — |
| `test_null_format` | 24739 | 📐 | `test_visual_null_format_caps_policy` |
| `test_per_stage_constant` | 19516 | ✅ | — |
| `test_pointsize` | 10873 | 🟢 | — |
| `test_position_index` | 20115 | 📐 | `test_visual_position_index_decl_policy` |
| `test_sample_attached_rendertarget` | 26344 | ✅ | — |
| `test_sample_mask` | 26723 | 📐 | `test_visual_sample_mask_render_state_policy` |
| `test_sanity` | 449 | ✅ | — |
| `test_shademode` | 8611 | 📐 | `test_visual_shademode_render_state_policy` |
| `test_signed_formats` | 20444 | 📐 | `test_visual_signed_formats_caps_policy` |
| `test_specular_lighting` | 774 | 📐 | `test_visual_specular_lighting_render_state_policy` |
| `test_sysmem_draw` | 25372 | ✅ | — |
| `test_table_fog_zw` | 20324 | ✅ | — |
| `test_texcoordindex` | 21069 | ✅ | — |
| `test_texture_blending` | 22642 | ✅ | — |
| `test_texture_transform_flags` | 28549 | ✅ | — |
| `test_uninitialized_varyings` | 22176 | ✅ | — |
| `test_updatetexture` | 21506 | ✅ | — |
| `test_vertex_blending` | 21243 | ✅ | — |
| `test_vertex_texture` | 24394 | 🟢 | — |
| `test_viewport` | 13994 | ✅ | — |
| `test_vshader_float16` | 9676 | ✅ | — |
| `test_vshader_input` | 7787 | ✅ | — |
| `texbem_test` | 3440 | ✅ | `test_vendor_policy_texbem_unsupported` |
| `texdepth_test` | 5242 | ✅ | `test_vendor_policy_texdepth_unsupported` |
| `texkill_test` | 5465 | ✅ | — |
| `texop_range_test` | 13126 | ✅ | — |
| `texop_test` | 12604 | ✅ | — |
| `tssargtemp_test` | 12041 | ✅ | — |
| `unbound_sampler_test` | 16760 | 📐 | `test_sampler_state_edges` |
| `update_surface_test` | 16915 | 📐 | `test_visual_update_surface_policy` |
| `vface_register_test` | 9984 | ✅ | `test_visual_vface_pixel_shader_create_policy` |
| `volume_dxtn_test` | 18535 | 📐 | `test_visual_volume_dxtn_format_policy` |
| `volume_srgb_test` | 18411 | 📐 | `test_visual_volume_srgb_caps_policy` |
| `volume_v16u16_test` | 18697 | 📐 | `test_visual_volume_v16u16_format_policy` |
| `yuv_color_test` | 12751 | 📐 | `test_visual_yuv_color_caps_policy` |
| `yuv_layout_test` | 12919 | 📐 | `test_visual_yuv_layout_lock_policy` |
| `z_range_test` | 3749 | 📐 | `test_visual_z_range_render_state_policy` |
| `zenable_test` | 18033 | 📐 | `test_visual_zenable_render_state_policy` |
| `zwriteenable_test` | 13421 | 📐 | `test_visual_zwriteenable_render_state_policy` |

## Device / Resource Tests (`device.c`)

Source: [`dlls/d3d9/tests/device.c`](https://gitlab.winehq.org/wine/wine/-/blob/master/dlls/d3d9/tests/device.c)

| Wine entry | Wine line | Status | dxmt9 PE function(s) / evidence |
|------------|----------:|:------:|---------------------------------|
| `test_begin_end_state_block` | 11366 | 📐 | — |
| `test_check_device_format` | 12577 | 📐 | `test_check_device_format_conversion_matrix`, `test_factory_caps_edge_matrix`, `test_factory_validation_return_codes` |
| `test_checkdevicemultisampletype` | 1116 | 📐 | `test_factory_caps_edge_matrix`, `test_factory_validation_return_codes` |
| `test_clip_planes_limits` | 13133 | 📐 | `test_clip_plane_state_getters` |
| `test_create_rt_ds_fail` | 10571 | 📐 | `test_create_rt_ds_failure_policy` |
| `test_creation_parameters` | 14818 | 📐 | `test_device_creation_parameters_policy`, `test_multithreaded_device_creation_policy` |
| `test_cube_textures` | 7820 | 🟡 | — |
| `test_cursor` | 1852 | 📐 | `window_cursor_ownership` |
| `test_cursor_clipping` | 14865 | 📐 | `window_cursor_ownership` |
| `test_cursor_pos` | 5286 | 📐 | `window_cursor_ownership` |
| `test_d3d9on12` | 15138 | 📐 | `d3d9on12_loader_safe_failure` |
| `test_depthstenciltest` | 2869 | 📐 | `test_set_get_depth_stencil_surface_policy` |
| `test_destroyed_window` | 13008 | 📐 | `window_cursor_ownership` |
| `test_device_caps` | 13482 | 📐 | — |
| `test_device_window_reset` | 5899 | 📐 | `window_cursor_ownership` |
| `test_display_formats` | 3551 | 📐 | `test_check_device_type_display_format_policy` |
| `test_display_modes` | 2591 | 📐 | `test_factory_validation_return_codes` |
| `test_draw_primitive` | 3081 | 📐 | `test_draw_primitive_outside_scene_policy` |
| `test_filter` | 8082 | 📐 | `resource_getdc_lod_autogen_mipmap`, `test_sampler_state_edges` |
| `test_format_unknown` | 12946 | 📐 | `resource_getdc_lod_autogen_mipmap` |
| `test_fpu_setup` | 4998 | 📐 | `test_fpu_setup` |
| `test_fvf_decl_conversion` | 501 | 📐 | — |
| `test_fvf_decl_management` | 843 | 📐 | `test_fvf_decl_management` |
| `test_get_declaration` | 428 | 📐 | — |
| `test_get_display_mode` | 14312 | 📐 | — |
| `test_get_render_target_data` | 12841 | 📐 | `test_get_render_target_data_policy` |
| `test_get_rt` | 3036 | 📐 | — |
| `test_get_set_pixel_shader` | 7167 | 📐 | `test_get_set_pixel_shader` |
| `test_get_set_texture` | 8196 | 📐 | `test_get_set_texture` |
| `test_get_set_vertex_declaration` | 376 | 📐 | — |
| `test_get_set_vertex_shader` | 6966 | 📐 | `test_get_set_vertex_shader` |
| `test_getdc` | 8991 | 📐 | `resource_getdc_lod_autogen_mipmap` |
| `test_invalid_multisample` | 1198 | 📐 | `test_invalid_multisample_render_target_quality` |
| `test_lights` | 3414 | 📐 | `test_light_enable_state` |
| `test_limits` | 2816 | 📐 | `test_limits` |
| `test_lockable_backbuffer` | 13045 | 📐 | `test_lockable_backbuffer_lock_policy`, `test_nonlockable_backbuffer_getdc_policy`, `test_reset_lockable_backbuffer_policy` |
| `test_lockbox_invalid` | 10968 | 📐 | `test_volume_lockbox_bounds_offset_policy` |
| `test_lockrect_invalid` | 8529 | 📐 | `test_resource_lock_error_policy`, `test_surface_reentrant_lock_preserves_output`, `test_texture_level_surface_unlock_policy`, `test_texture_reentrant_lock_preserves_output` |
| `test_lockrect_offset` | 8440 | 📐 | `test_compressed_surface_lockrect_block_offset`, `test_resource_lock_error_policy`, `test_surface_lockrect_subrect_offset_policy` |
| `test_lod` | 8247 | 📐 | `resource_getdc_lod_autogen_mipmap`, `test_texture_lod_policy` |
| `test_lost_device` | 12107 | 📐 | — |
| `test_mipmap_gen` | 7878 | 📐 | `resource_getdc_lod_autogen_mipmap`, `test_texture_autogen_filter_level_policy` |
| `test_mipmap_levels` | 1087 | 📐 | `test_base_texture_metadata_iface_policy`, `test_texture_auto_mipmap_level_count` |
| `test_mipmap_lock` | 11963 | 📐 | `test_mipmap_surface_update_lock_policy`, `test_resource_lock_error_policy` |
| `test_miptree_layout` | 12695 | 📐 | `test_miptree_layout_lock_pitch_policy` |
| `test_mode_change` | 5414 | 📐 | `test_mode_change_focus_swap_policy` |
| `test_multi_adapter` | 14502 | 📐 | `test_multi_adapter` |
| `test_multi_device` | 3712 | 📐 | `test_multi_device_independent_state` |
| `test_multiply_transform` | 14100 | 📐 | `test_stateblock_multiply_transform_capture` |
| `test_npot_textures` | 10090 | 📐 | `test_create_texture_npot_policy` |
| `test_null_stream` | 3330 | 📐 | `test_null_stream_shader_draw_policy`, `test_null_stream_state` |
| `test_occlusion_query` | 6511 | 📐 | `occlusion_query_public_sizes`, `test_query_get_data_size_policy` |
| `test_pinned_buffers` | 9992 | 📐 | `test_pinned_buffers_d3dusage_policy` |
| `test_pixel_format` | 11187 | 📐 | `test_pixel_format_window_policy` |
| `test_pixel_shader_constant` | 7238 | 📐 | `test_pixel_shader_constant` |
| `test_private_data` | 8823 | 📐 | `test_private_data_bytes`, `test_private_data_iunknown`, `test_private_data_iunknown_ownership_smoke`, `test_private_data_replace_and_size_policy`, `test_private_data_resource_wrappers` |
| `test_query_support` | 6430 | 📐 | `query_support_probe`, `test_query_get_data_size_policy` |
| `test_refcount` | 1497 | 📐 | `test_device_parent_caps_getter_policy`, `test_get_direct3d_addref`, `test_resource_get_device_addref`, `test_resource_get_device_wrapper_policy` |
| `test_render_target_device_mismatch` | 12889 | 📐 | `test_render_target_device_mismatch` |
| `test_reset` | 2031 | 📐 | — |
| `test_reset_fullscreen` | 4807 | 📐 | `test_reset_fullscreen_focus_window_policy` |
| `test_reset_resources` | 5985 | 📐 | — |
| `test_resource_access` | 13659 | 📐 | — |
| `test_resource_priority` | 12209 | 📐 | `test_resource_priority_pool_policy`, `test_resource_priority_roundtrip` |
| `test_resource_type` | 11789 | 📐 | `test_base_texture_metadata_iface_policy`, `test_cube_texture_face_desc_parity`, `test_resource_type`, `test_texture_level_surface_desc_parity`, `test_volume_mipmap_level_desc_policy` |
| `test_scene` | 2678 | 📐 | — |
| `test_scissor_size` | 3631 | 📐 | `test_scissor_default_matches_backbuffer_policy` |
| `test_set_palette` | 9935 | 📐 | `test_palette_alpha_caps_policy`, `test_palette_current_entry_isolation`, `test_set_palette_roundtrip` |
| `test_set_rt_vp_scissor` | 6055 | 📐 | `test_viewport_scissor_state_getters` |
| `test_set_stream_source` | 3468 | 📐 | `test_set_stream_source_state`, `test_stream_source_null_layout_policy`, `test_stream_source_null_offset_alignment_policy`, `test_stream_source_vb_offset_alignment_policy`, `test_stream_source_zero_stride_policy` |
| `test_shader_constant_apply` | 11446 | 📐 | `test_shader_constant_stateblock_cross_stage` |
| `test_shader_validator` | 14660 | 🟡 | `shader_validator_stub_behavior` |
| `test_shared_handle` | 11088 | 📐 | `test_shared_handle_policy` |
| `test_stretch_rect` | 13277 | 📐 | `test_stretch_rect_null_and_degenerate_policy` |
| `test_surface_alignment` | 8360 | 📐 | `test_surface_alignment` |
| `test_surface_blocks` | 9529 | 📐 | `resource_container_level_desc_and_locks`, `test_index_buffer_desc_binding_policy`, `test_vertex_buffer_desc_binding_policy` |
| `test_surface_dimensions` | 9338 | 📐 | `test_surface_dimensions` |
| `test_surface_double_unlock` | 9471 | 📐 | `test_surface_double_unlock_pool_policy` |
| `test_surface_format_null` | 9371 | 📐 | `test_surface_format_null_policy`, `test_vendor_format_public_api_policy` |
| `test_surface_get_container` | 8291 | 📐 | `resource_container_level_desc_and_locks`, `test_texture_surface_container_policy` |
| `test_swapchain` | 1269 | 📐 | `test_device_get_swap_chain_bounds_policy`, `test_swapchain_backbuffer_getter_policy`, `test_additional_swapchain_backbuffer_bounds`, `test_present_parameter_normalization`, `test_present_parameter_validation` |
| `test_swapchain_multisample_reset` | 13219 | 📐 | `test_swapchain_multisample_reset` |
| `test_swapchain_parameters` | 12337 | 📐 | `test_additional_swapchain_backbuffer_bounds`, `test_present_parameter_normalization`, `test_present_parameter_validation`, `test_swapchain_backbuffer_getter_policy` |
| `test_texture_stage_states` | 7658 | 📐 | `test_texture_stage_states` |
| `test_timestamp_query` | 6790 | 📐 | `test_query_get_data_size_policy`, `timestamp_query_public_sizes` |
| `test_unsupported_shaders` | 7296 | 📐 | `test_shader_unsupported_stage_variants`, `test_unsupported_shaders` |
| `test_unused_declaration_type` | 1001 | 📐 | `test_unused_declaration_type` |
| `test_update_texture_pool` | 10339 | 📐 | `test_update_texture_pool_copy_2d` |
| `test_update_volumetexture` | 10483 | ✅ | — |
| `test_vb_lock_flags` | 6291 | 📐 | `test_vb_lock_flags` |
| `test_vdecl_apply` | 11603 | 📐 | `test_vdecl_apply` |
| `test_vertex_buffer_alignment` | 6375 | 📐 | `test_vertex_buffer_alignment` |
| `test_vertex_buffer_read_write` | 14211 | 📐 | — |
| `test_vertex_declaration_alignment` | 923 | 📐 | — |
| `test_vertex_shader_constant` | 7037 | 📐 | `test_vertex_shader_constant` |
| `test_vidmem_accounting` | 10208 | 📐 | `test_base_vidmem_accounting_policy` |
| `test_volume_blocks` | 10615 | 📐 | `test_vendor_format_public_api_policy`, `test_volume_block_lock_layout`, `test_volume_blocks_compressed_layout_policy` |
| `test_volume_get_container` | 6159 | 📐 | `resource_container_level_desc_and_locks`, `test_volume_container_interface_policy` |
| `test_volume_locking` | 10260 | 📐 | `resource_container_level_desc_and_locks`, `test_resource_lock_error_policy` |
| `test_volume_resource` | 6240 | 📐 | `test_volume_resource_container_desc` |
| `test_window_position` | 14942 | 📐 | `test_fullscreen_window_position_restore`, `test_window_position_present_parameter_policy` |
| `test_window_style` | 5114 | 📐 | `window_cursor_ownership` |
| `test_wndproc` | 3882 | 📐 | `window_cursor_ownership` |
| `test_wndproc_windowed` | 4596 | 📐 | `window_cursor_ownership` |
| `test_writeonly_resource` | 12034 | 📐 | `test_writeonly_vertex_buffer_readback_policy` |

## D3D9Ex-Specific Tests (`d3d9ex.c`)

Source: [`dlls/d3d9/tests/d3d9ex.c`](https://gitlab.winehq.org/wine/wine/-/blob/master/dlls/d3d9/tests/d3d9ex.c)

| Wine entry | Wine line | Status | dxmt9 PE function(s) / evidence |
|------------|----------:|:------:|---------------------------------|
| `test_backbuffer_resize` | 3848 | 📐 | `test_backbuffer_resize_present_parameter_policy` |
| `test_create_depth_stencil_surface_ex` | 640 | 📐 | `test_create_depth_stencil_surface_ex` |
| `test_desktop_window` | 4976 | 🟡 | — |
| `test_device_caps` | 4052 | 📐 | — |
| `test_format_unknown` | 3977 | 📐 | `test_creation_failure_out_pointers` |
| `test_frame_latency` | 4184 | 📐 | `test_ex_frame_latency_state` |
| `test_get_adapter_displaymode_ex` | 522 | 📐 | `test_ex_adapter_display_mode_null_rotation`, `test_ex_adapter_luid_display_mode`, `test_ex_get_adapter_display_mode_ex_policy` |
| `test_get_adapter_luid` | 400 | 📐 | `test_ex_adapter_luid_display_mode`, `test_ex_get_adapter_luid_policy` |
| `test_lost_device` | 2009 | 📐 | — |
| `test_pinned_buffers` | 4883 | 📐 | `test_pinned_buffers_d3dusage_policy` |
| `test_qi_base_to_ex` | 240 | 📐 | — |
| `test_qi_ex_to_base` | 303 | 📐 | — |
| `test_reset` | 888 | 📐 | — |
| `test_reset_ex` | 1420 | 📐 | `test_ex_adapter_mode_enum_bounds`, `test_ex_create_reset_mode_validation` |
| `test_reset_resources` | 1843 | 📐 | — |
| `test_resource_access` | 4229 | 📐 | — |
| `test_scene` | 5031 | 📐 | — |
| `test_swapchain_get_displaymode_ex` | 432 | 📐 | `test_ex_swapchain_display_mode`, `test_ex_swapchain_display_mode_null_rotation`, `test_swapchain_get_display_mode_ex_policy` |
| `test_swapchain_parameters` | 3655 | 📐 | `test_ex_create_reset_mode_validation` |
| `test_sysmem_draw` | 4693 | 🟡 | — |
| `test_unsupported_shaders` | 2173 | 📐 | `test_ex_shader_validation_policy` |
| `test_user_memory` | 715 | 📐 | `test_ex_shared_handle_policy`, `test_ex_user_memory_lock_identity` |
| `test_user_memory_getdc` | 1957 | 📐 | `test_ex_user_memory_getdc_dib_identity`, `test_ex_user_memory_getdc_format_policy` |
| `test_vidmem_accounting` | 1909 | 📐 | `test_ex_vidmem_accounting_policy` |
| `test_window_style` | 3458 | 📐 | `window_cursor_ownership` |
| `test_wndproc` | 2611 | 📐 | `window_cursor_ownership` |
| `test_wndproc_windowed` | 3256 | 📐 | `window_cursor_ownership` |

## StateBlock Tests (`stateblock.c`)

Source: [`dlls/d3d9/tests/stateblock.c`](https://gitlab.winehq.org/wine/wine/-/blob/master/dlls/d3d9/tests/stateblock.c)

| Wine entry | Wine line | Status | dxmt9 PE function(s) / evidence |
|------------|----------:|:------:|---------------------------------|
| `test_state_management` | 2033 | 📐 | `stateblock_state_management_matrix`, `test_state_management_all_capture_apply_matrix`, `test_state_management_pixel_capture_apply_slice`, `test_state_management_vertex_capture_apply_slice`, `test_stateblock_invalid_type_recording_invalid_calls`, `test_stateblock_transform_capture_apply` |

## Maintenance

Regenerate this file with:

```sh
python3 scripts/tools/gen_wine_d3d9_test_inventory.py
```

The generator reads Wine source from `$WINE_REPO` (default
`~/workspaces/wine`) and the plan from `specs/wine_test.plan.md`.
It re-counts `START_TEST` callees by walking the brace-depth of the
test entrypoint body, so newly added Wine tests appear with status
`UNTRACKED` until the plan picks them up.

Cross-reference: per-round implementation staging lives in
`specs/wine_test.plan.md` (gitignored) and
`specs/wine_test_failures.plan.md`; once a Wine row reaches a stable
`covered` / `scaffolded` state, update both this file and the plan.
