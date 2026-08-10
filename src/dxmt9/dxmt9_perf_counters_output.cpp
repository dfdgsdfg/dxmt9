#include "dxmt9_perf_counters.hpp"

#include "dxmt9_perf_counters_internal.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>

namespace dxmt9::perf {

using detail::Counters;
using detail::counters;
using detail::load;

namespace {

const char* splitReasonName(EncoderSplitReason reason) {
  switch (reason) {
    case EncoderSplitReason::Final:
      return "final";
    case EncoderSplitReason::RenderTargetChange:
      return "rt_change";
    case EncoderSplitReason::Hazard:
      return "hazard";
    case EncoderSplitReason::ClearBarrier:
      return "clear";
    case EncoderSplitReason::SurfaceCopy:
      return "surface_copy";
    case EncoderSplitReason::StretchRect:
      return "stretch";
    case EncoderSplitReason::Readback:
      return "readback";
    case EncoderSplitReason::ColorFill:
      return "color_fill";
    case EncoderSplitReason::Present:
      return "present";
    case EncoderSplitReason::PresentAcquire:
      return "present_acquire";
    case EncoderSplitReason::TileMidPassIneligible:
      return "tile_midpass";
    case EncoderSplitReason::OrderedControl:
      return "ordered_control";
  }
  return "unknown";
}

}  // namespace

// --- Per-frame snapshot mode (opt-in) ---------------------------------------
// The cumulative `[dxmt9-perf]` line at process exit hides which frame caused
// a spike. When DXMT9_PERF_FRAME_SAMPLING=1 the encode-thread Present handler
// records a snapshot once per Present and prints a `[dxmt9-perf-frame]` line
// whose values are deltas vs the prior snapshot.
//
// Subset emitted per frame (~30 keys): all timing nanoseconds we already
// expose plus the major volume counters (submit_draw, submit_present,
// render_pass_begin/end, draw_calls/indexed/primitives/triangles/vertices,
// command_buffers, bind_pipeline, present_encoded). Keeps line length
// bounded; the cumulative report at exit still covers all 200+ keys.

bool frameSamplingEnabled() {
  static const bool value = []() {
    if (const char* v = std::getenv("DXMT9_PERF_FRAME_SAMPLING")) {
      return v[0] != '\0' && v[0] != '0';
    }
    return false;
  }();
  return value;
}

bool encoderBreakdownEnabled() {
  static const bool value = []() {
    if (const char* v = std::getenv("DXMT9_PERF_ENCODER_BREAKDOWN")) {
      return v[0] != '\0' && v[0] != '0';
    }
    return false;
  }();
  return enabled() && value;
}

std::uint64_t encoderBreakdownSeqFilter() {
  static const std::uint64_t value = []() -> std::uint64_t {
    const char* env = std::getenv("DXMT9_PERF_ENCODER_BREAKDOWN_SEQ");
    if (!env || env[0] == '\0') {
      return 0;
    }
    char* end = nullptr;
    const auto parsed = std::strtoull(env, &end, 10);
    return end != env ? static_cast<std::uint64_t>(parsed) : 0;
  }();
  return value;
}

namespace {

std::uint64_t parseEnvU64(const char* name) {
  const char* env = std::getenv(name);
  if (!env || env[0] == '\0') {
    return 0;
  }
  char* end = nullptr;
  const auto parsed = std::strtoull(env, &end, 10);
  return end != env ? static_cast<std::uint64_t>(parsed) : 0;
}

}  // namespace

bool encoderBreakdownSeqAllowed(std::uint64_t seq) {
  const auto exact = encoderBreakdownSeqFilter();
  if (exact != 0) {
    return seq == exact;
  }
  static const std::uint64_t minSeq =
      parseEnvU64("DXMT9_PERF_ENCODER_BREAKDOWN_SEQ_MIN");
  static const std::uint64_t maxSeq =
      parseEnvU64("DXMT9_PERF_ENCODER_BREAKDOWN_SEQ_MAX");
  if (minSeq != 0 && seq < minSeq) {
    return false;
  }
  if (maxSeq != 0 && seq > maxSeq) {
    return false;
  }
  return minSeq == 0 || maxSeq == 0 || minSeq <= maxSeq;
}

bool encoderBreakdownSeqFilterActive() {
  if (encoderBreakdownSeqFilter() != 0) {
    return true;
  }
  static const bool value =
      parseEnvU64("DXMT9_PERF_ENCODER_BREAKDOWN_SEQ_MIN") != 0 ||
      parseEnvU64("DXMT9_PERF_ENCODER_BREAKDOWN_SEQ_MAX") != 0;
  return value;
}

void emitEncoderBreakdown(const EncoderBreakdown& b) {
  if (!encoderBreakdownEnabled()) {
    return;
  }
  std::fprintf(
      stderr,
      "[dxmt9-perf-encoder seq=%llu encoder=%llu rt=0x%llx depth=0x%llx "
      "rt_format=%llu rt_width=%llu rt_height=%llu rt_bpp=%llu "
      "rt_alias_texture=0x%llx rt_texture_usage=0x%llx "
      "rt_format_swizzle=%llu rt_texture_needs_shader_read_view=%llu "
      "depth_format=%llu depth_width=%llu depth_height=%llu depth_bpp=%llu "
      "depth_alias_texture=0x%llx depth_texture_usage=0x%llx "
      "depth_format_swizzle=%llu depth_texture_needs_shader_read_view=%llu "
      "color_attachment_count=%llu color0_included=%llu "
      "color0_load_action=%llu color0_store_action=%llu "
      "color0_clear=%llu color_load_bytes=%llu color_store_bytes=%llu "
      "depth_included=%llu depth_load_action=%llu depth_store_action=%llu "
      "depth_clear=%llu depth_load_bytes=%llu depth_store_bytes=%llu "
      "stencil_included=%llu stencil_load_action=%llu stencil_store_action=%llu "
      "stencil_clear=%llu stencil_load_bytes=%llu stencil_store_bytes=%llu "
      "end_reason=%s draw_calls=%llu indexed_draws=%llu "
      "expanded_indexed_draws=%llu ffp_draws=%llu programmable_draws=%llu "
      "pretransformed_draws=%llu textured_draws=%llu "
      "cull_none_draws=%llu cull_front_draws=%llu cull_back_draws=%llu "
      "fill_solid_draws=%llu fill_wireframe_draws=%llu "
      "depth_enabled_draws=%llu depth_write_draws=%llu "
      "depth_func_less_draws=%llu depth_func_lessequal_draws=%llu "
      "depth_func_always_draws=%llu depth_func_other_draws=%llu "
      "scissor_enabled_draws=%llu alpha_blend_enabled_draws=%llu "
      "blend_state_samples=%llu blend_state_changes=%llu "
      "blend_state_unique=%llu blend_state_unique_overflows=%llu "
      "blend_state_last=0x%llx blend_enabled_noop_draws=%llu "
      "blend_constant_factor_draws=%llu "
      "blend_screen_draws=%llu blend_additive_draws=%llu "
      "blend_alpha_composite_draws=%llu "
      "alpha_blend_textured_draws=%llu "
      "alpha_blend_textured_primitives=%llu "
      "alpha_blend_textured_vertices=%llu "
      "alpha_blend_small_draws=%llu "
      "alpha_blend_small_primitives=%llu "
      "alpha_blend_small_vertices=%llu "
      "alpha_test_enabled_draws=%llu alpha_test_effective_draws=%llu "
      "clip_plane_enabled_draws=%llu "
      "point_draws=%llu line_draws=%llu triangle_draws=%llu primitive_count=%llu "
      "triangle_estimate=%llu vertex_count=%llu "
      "route_depth_only_draws=%llu "
      "route_depth_only_primitives=%llu "
      "route_depth_only_vertices=%llu "
      "route_programmable_textured_draws=%llu "
      "route_programmable_textured_primitives=%llu "
      "route_programmable_textured_vertices=%llu "
      "route_programmable_color_draws=%llu "
      "route_programmable_color_primitives=%llu "
      "route_programmable_color_vertices=%llu "
      "route_alpha_blend_primitives=%llu "
      "route_alpha_test_primitives=%llu "
      "tile_ffp_routed_tile_draws=%llu "
      "tile_ffp_routed_tile_primitives=%llu "
      "tile_ffp_routed_tile_vertices=%llu "
      "tile_ffp_routed_portable_draws=%llu "
      "tile_ffp_routed_portable_primitives=%llu "
      "tile_ffp_routed_portable_vertices=%llu "
      "tile_ffp_eligible_draws=%llu "
      "tile_ffp_eligible_primitives=%llu "
      "tile_ffp_eligible_vertices=%llu "
      "tile_ffp_fallback_gpu_family_draws=%llu "
      "tile_ffp_fallback_gpu_family_primitives=%llu "
      "tile_ffp_fallback_not_ffp_draws=%llu "
      "tile_ffp_fallback_not_ffp_primitives=%llu "
      "tile_ffp_fallback_precision_draws=%llu "
      "tile_ffp_fallback_precision_primitives=%llu "
      "tile_ffp_fallback_unsupported_state_draws=%llu "
      "tile_ffp_fallback_unsupported_state_primitives=%llu "
      "indexed_triangle_opaque_depth_write_draws=%llu "
      "indexed_triangle_opaque_depth_write_primitives=%llu "
      "indexed_triangle_opaque_depth_write_vertices=%llu "
      "indexed_triangle_depth_read_draws=%llu "
      "indexed_triangle_depth_read_primitives=%llu "
      "indexed_triangle_depth_read_vertices=%llu "
      "indexed_triangle_alpha_blend_draws=%llu "
      "indexed_triangle_alpha_blend_primitives=%llu "
      "indexed_triangle_alpha_blend_vertices=%llu "
      "indexed_triangle_scissor_draws=%llu "
      "indexed_triangle_scissor_primitives=%llu "
      "indexed_triangle_scissor_vertices=%llu "
      "indexed_triangle_textured_draws=%llu "
      "indexed_triangle_textured_primitives=%llu "
      "indexed_triangle_textured_vertices=%llu "
      "indexed_triangle_large_4096_draws=%llu "
      "indexed_triangle_large_4096_primitives=%llu "
      "indexed_triangle_large_4096_vertices=%llu "
      "indexed_triangle_large_4096_opaque_depth_write_draws=%llu "
      "indexed_triangle_large_4096_opaque_depth_write_primitives=%llu "
      "indexed_triangle_large_4096_opaque_depth_write_vertices=%llu "
      "indexed_triangle_large_4096_depth_read_draws=%llu "
      "indexed_triangle_large_4096_depth_read_primitives=%llu "
      "indexed_triangle_large_4096_depth_read_vertices=%llu "
      "indexed_triangle_large_4096_alpha_blend_draws=%llu "
      "indexed_triangle_large_4096_alpha_blend_primitives=%llu "
      "indexed_triangle_large_4096_alpha_blend_vertices=%llu "
      "indexed_triangle_large_4096_scissor_draws=%llu "
      "indexed_triangle_large_4096_scissor_primitives=%llu "
      "indexed_triangle_large_4096_scissor_vertices=%llu "
      "indexed_triangle_large_4096_textured_draws=%llu "
      "indexed_triangle_large_4096_textured_primitives=%llu "
      "indexed_triangle_large_4096_textured_vertices=%llu "
      "texture_mask_or=0x%llx "
      "fragment_texture_binding_samples=%llu "
      "fragment_texture_binding_mask_or=0x%llx "
      "x8_rt_texture_binding_samples=%llu "
      "x8_rt_texture_binding_mask_or=0x%llx "
      "x8_rt_texture_binding_unique_handles=%llu "
      "x8_rt_texture_binding_unique_handle_overflows=%llu "
      "x8_rt_texture_binding_shader_read_view_samples=%llu "
      "x8_rt_texture_binding_active_rt_alias_samples=%llu "
      "x8_shader_alpha_fill_samples=%llu "
      "x8_shader_alpha_fill_mask_or=0x%llx "
      "x8_rt_texture_binding_last_stage=%llu "
      "x8_rt_texture_binding_last_handle=0x%llx "
      "draw_primitive_min=%llu draw_primitive_max=%llu "
      "draw_vertex_min=%llu draw_vertex_max=%llu "
      "draw_primitive_bucket_1_63=%llu "
      "draw_primitive_bucket_64_255=%llu "
      "draw_primitive_bucket_256_1023=%llu "
      "draw_primitive_bucket_1024_4095=%llu "
      "draw_primitive_bucket_4096_plus=%llu "
      "draw_vertex_bucket_1_255=%llu "
      "draw_vertex_bucket_256_1023=%llu "
      "draw_vertex_bucket_1024_4095=%llu "
      "draw_vertex_bucket_4096_16383=%llu "
      "draw_vertex_bucket_16384_plus=%llu "
      "draw_geometry_signature_samples=%llu "
      "draw_geometry_signature_unique=%llu "
      "draw_geometry_signature_unique_overflows=%llu "
      "draw_geometry_signature_duplicates=%llu "
      "draw_geometry_signature_consecutive_duplicates=%llu "
      "draw_geometry_signature_last=0x%llx "
      "indexed_base_vertex_samples=%llu "
      "indexed_base_vertex_nonzero_draws=%llu "
      "indexed_base_vertex_negative_draws=%llu "
      "indexed_base_vertex_positive_draws=%llu "
      "indexed_base_vertex_min=%lld indexed_base_vertex_max=%lld "
      "native_base_vertex_requested_draws=%llu "
      "native_base_vertex_used_draws=%llu "
      "native_base_vertex_skipped_negative_draws=%llu "
      "split_large_indexed_source_draws=%llu "
      "split_large_indexed_metal_draws=%llu "
      "split_large_indexed_extra_draws=%llu "
      "split_large_indexed_primitive_limit=%llu "
      "split_large_indexed_stream0_span_limit=%llu "
      "split_large_indexed_chunk_stream0_span_max=%llu "
      "split_large_indexed_primitive_count=%llu "
      "indexed_order_probe_draws=%llu "
      "indexed_order_probe_skipped=%llu "
      "indexed_order_probe_bytes=%llu "
      "indexed_order_optimized_draws=%llu "
      "indexed_order_optimized_skipped=%llu "
      "indexed_order_optimized_bytes=%llu "
      "probe_scissor_rect_draws=%llu "
      "probe_scissor_rect_skipped=%llu "
      "probe_scissor_rect_area_delta_pixels=%llu "
      "probe_disable_alpha_blend_draws=%llu "
      "probe_disable_depth_write_draws=%llu "
      "probe_depth_func_always_draws=%llu "
      "probe_force_texture_white_draws=%llu "
      "probe_fragmentless_depth_only_draws=%llu "
      "probe_fragmentless_depth_only_primitives=%llu "
      "probe_fragmentless_depth_only_vertices=%llu "
      "indexed_vertex_reuse_samples=%llu "
      "indexed_vertex_reuse_skipped=%llu "
      "indexed_vertex_reference_count=%llu "
      "indexed_unique_vertex_estimate=%llu "
      "indexed_vertex_cache_miss_estimate_16=%llu "
      "indexed_vertex_cache_miss_estimate_32=%llu "
      "indexed_vertex_cache_miss_estimate_64=%llu "
      "indexed_cache_opt_candidate_draws=%llu "
      "indexed_cache_opt_candidate_skipped=%llu "
      "indexed_cache_opt_candidate_bytes=%llu "
      "indexed_cache_opt_candidate_original_miss16=%llu "
      "indexed_cache_opt_candidate_original_miss32=%llu "
      "indexed_cache_opt_candidate_original_miss64=%llu "
      "indexed_cache_opt_candidate_miss16=%llu "
      "indexed_cache_opt_candidate_miss32=%llu "
      "indexed_cache_opt_candidate_miss64=%llu "
      "indexed_cache_opt_candidate_gate_pass=%llu "
      "indexed_cache_opt_candidate_gate_fail=%llu "
      "indexed_cache_opt_candidate_opaque_depth_draws=%llu "
      "indexed_cache_opt_candidate_screen_blend_draws=%llu "
      "indexed_cache_opt_candidate_primitive_bucket_1_63=%llu "
      "indexed_cache_opt_candidate_primitive_bucket_64_255=%llu "
      "indexed_cache_opt_candidate_primitive_bucket_256_1023=%llu "
      "indexed_cache_opt_candidate_primitive_bucket_1024_4095=%llu "
      "indexed_cache_opt_candidate_primitive_bucket_4096_plus=%llu "
      "reordered_index_cache_lookups=%llu "
      "reordered_index_cache_hits=%llu "
      "reordered_index_cache_rejected_hits=%llu "
      "reordered_index_cache_misses=%llu "
      "reordered_index_cache_created=%llu "
      "reordered_index_cache_created_bytes=%llu "
      "stream0_stride_min=%llu stream0_stride_max=%llu "
      "stream_state_samples=%llu stream_metal_binds=%llu "
      "stream_metal_bind_firsts=%llu "
      "stream_metal_bind_handle_changes=%llu "
      "stream_metal_bind_offset_changes=%llu "
      "stream_unique_handles=%llu stream_unique_handle_overflows=%llu "
      "stream_unique_bytes=%llu stream_unique_dynamic_handles=%llu "
      "stream_unique_writeonly_handles=%llu "
      "stream_unique_default_pool_handles=%llu "
      "stream_unique_managed_pool_handles=%llu "
      "stream_unique_systemmem_pool_handles=%llu "
      "stream_unique_scratch_pool_handles=%llu "
      "stream_handle_changes=%llu stream_offset_changes=%llu "
      "stream_stride_changes=%llu stream0_last_handle=0x%llx "
      "stream0_last_offset=%llu stream0_last_stride=%llu "
      "ib_state_samples=%llu ib_metal_binds=%llu ib_handle_changes=%llu "
      "ib_unique_handles=%llu ib_unique_handle_overflows=%llu "
      "ib_unique_bytes=%llu ib_unique_dynamic_handles=%llu "
      "ib_unique_writeonly_handles=%llu "
      "ib_unique_default_pool_handles=%llu "
      "ib_unique_managed_pool_handles=%llu "
      "ib_unique_systemmem_pool_handles=%llu "
      "ib_unique_scratch_pool_handles=%llu "
      "ib_last_handle=0x%llx pso_state_samples=%llu "
      "pso_handle_changes=%llu pso_unique_handles=%llu "
      "pso_unique_handle_overflows=%llu pso_last_handle=0x%llx "
      "shader_variant_changes=%llu shader_variant_unique=%llu "
      "shader_variant_unique_overflows=%llu shader_variant_last=0x%llx "
      "vertex_shader_last=0x%llx pixel_shader_last=0x%llx "
      "vertex_shader_source_last=%llu pixel_shader_source_last=%llu "
      "vsout_layout_changes=%llu vsout_layout_unique=%llu "
      "vsout_layout_unique_overflows=%llu vsout_layout_last=0x%x "
      "vsout_layout_cache_hits=%llu vsout_layout_cache_misses=%llu "
      "argbuf_table_bytes=%llu argbuf_cbuf_bytes=%llu "
      "argbuf_cbuf_vs_bytes=%llu argbuf_cbuf_ffp_vs_bytes=%llu "
      "argbuf_cbuf_ps_bytes=%llu argbuf_cbuf_ffp_ps_bytes=%llu "
      "argbuf_cbuf_vs_first_bytes=%llu "
      "argbuf_cbuf_vs_rewrite_changed_bytes=%llu "
      "argbuf_cbuf_vs_rewrite_unchanged_bytes=%llu "
      "argbuf_cbuf_vs_float_changed_bytes=%llu "
      "argbuf_cbuf_vs_int_changed_bytes=%llu "
      "argbuf_cbuf_vs_bool_changed_bytes=%llu "
      "argbuf_cbuf_vs_uploads=%llu "
      "argbuf_cbuf_vs_full_struct_uploads=%llu "
      "argbuf_cbuf_vs_usage_unknown_uploads=%llu "
      "argbuf_cbuf_vs_usage_indexed_float_uploads=%llu "
      "argbuf_cbuf_vs_plan_float_regs_sum=%llu "
      "argbuf_cbuf_vs_plan_float_regs_max=%llu "
      "argbuf_cbuf_vs_dirty_float_regs_sum=%llu "
      "argbuf_cbuf_vs_dirty_float_regs_max=%llu "
      "argbuf_cbuf_vs_usage_float_regs_sum=%llu "
      "argbuf_cbuf_vs_usage_float_regs_max=%llu "
      "argbuf_cbuf_ffp_vs_first_bytes=%llu "
      "argbuf_cbuf_ffp_vs_rewrite_changed_bytes=%llu "
      "argbuf_cbuf_ffp_vs_rewrite_unchanged_bytes=%llu "
      "argbuf_cbuf_ffp_vs_matrix_changed_bytes=%llu "
      "argbuf_cbuf_ffp_vs_material_changed_bytes=%llu "
      "argbuf_cbuf_ffp_vs_light_changed_bytes=%llu "
      "argbuf_cbuf_ffp_vs_blend_changed_bytes=%llu "
      "argbuf_cbuf_ffp_vs_tex_transform_changed_bytes=%llu "
      "argbuf_cbuf_ffp_vs_clip_changed_bytes=%llu "
      "argbuf_cbuf_ffp_vs_viewport_changed_bytes=%llu "
      "argbuf_cbuf_ffp_vs_fog_point_changed_bytes=%llu "
      "set_vertex_bytes_calls=%llu set_vertex_bytes_bytes=%llu "
      "set_vertex_bytes_slot5_calls=%llu set_vertex_bytes_slot5_bytes=%llu "
      "set_vertex_bytes_other_calls=%llu set_vertex_bytes_other_bytes=%llu "
      "transient_vertex_bytes=%llu "
      "transient_vertex_user_bytes=%llu "
      "transient_vertex_preupload_bytes=%llu "
      "transient_vertex_decl_fallback_bytes=%llu "
      "transient_vertex_expanded_main_bytes=%llu "
      "transient_vertex_expanded_extra_bytes=%llu "
      "transient_vertex_staged_stream_bytes=%llu "
      "transient_index_bytes=%llu "
      "transient_index_user_bytes=%llu "
      "transient_index_preupload_bytes=%llu "
      "transient_index_shadow_fallback_bytes=%llu "
      "transient_index_probe_reorder_bytes=%llu "
      "transient_index_optimized_order_bytes=%llu "
      "transient_index_staged_ib_bytes=%llu]\n",
      static_cast<unsigned long long>(b.seqId),
      static_cast<unsigned long long>(b.encoderIndex),
      static_cast<unsigned long long>(b.rtHandle),
      static_cast<unsigned long long>(b.depthHandle),
      static_cast<unsigned long long>(b.rtFormat),
      static_cast<unsigned long long>(b.rtWidth),
      static_cast<unsigned long long>(b.rtHeight),
      static_cast<unsigned long long>(b.rtBytesPerPixel),
      static_cast<unsigned long long>(b.rtAliasTexture),
      static_cast<unsigned long long>(b.rtTextureUsage),
      static_cast<unsigned long long>(b.rtFormatNeedsShaderReadSwizzle),
      static_cast<unsigned long long>(b.rtTextureNeedsShaderReadView),
      static_cast<unsigned long long>(b.depthFormat),
      static_cast<unsigned long long>(b.depthWidth),
      static_cast<unsigned long long>(b.depthHeight),
      static_cast<unsigned long long>(b.depthBytesPerPixel),
      static_cast<unsigned long long>(b.depthAliasTexture),
      static_cast<unsigned long long>(b.depthTextureUsage),
      static_cast<unsigned long long>(b.depthFormatNeedsShaderReadSwizzle),
      static_cast<unsigned long long>(b.depthTextureNeedsShaderReadView),
      static_cast<unsigned long long>(b.colorAttachmentCount),
      static_cast<unsigned long long>(b.color0Included),
      static_cast<unsigned long long>(b.color0LoadAction),
      static_cast<unsigned long long>(b.color0StoreAction),
      static_cast<unsigned long long>(b.color0Clear),
      static_cast<unsigned long long>(b.colorLoadBytes),
      static_cast<unsigned long long>(b.colorStoreBytes),
      static_cast<unsigned long long>(b.depthIncluded),
      static_cast<unsigned long long>(b.depthLoadAction),
      static_cast<unsigned long long>(b.depthStoreAction),
      static_cast<unsigned long long>(b.depthClear),
      static_cast<unsigned long long>(b.depthLoadBytes),
      static_cast<unsigned long long>(b.depthStoreBytes),
      static_cast<unsigned long long>(b.stencilIncluded),
      static_cast<unsigned long long>(b.stencilLoadAction),
      static_cast<unsigned long long>(b.stencilStoreAction),
      static_cast<unsigned long long>(b.stencilClear),
      static_cast<unsigned long long>(b.stencilLoadBytes),
      static_cast<unsigned long long>(b.stencilStoreBytes),
      splitReasonName(b.endReason),
      static_cast<unsigned long long>(b.drawCalls),
      static_cast<unsigned long long>(b.indexedDraws),
      static_cast<unsigned long long>(b.expandedIndexedDraws),
      static_cast<unsigned long long>(b.ffpDraws),
      static_cast<unsigned long long>(b.programmableDraws),
      static_cast<unsigned long long>(b.preTransformedDraws),
      static_cast<unsigned long long>(b.texturedDraws),
      static_cast<unsigned long long>(b.cullNoneDraws),
      static_cast<unsigned long long>(b.cullFrontDraws),
      static_cast<unsigned long long>(b.cullBackDraws),
      static_cast<unsigned long long>(b.fillSolidDraws),
      static_cast<unsigned long long>(b.fillWireframeDraws),
      static_cast<unsigned long long>(b.depthEnabledDraws),
      static_cast<unsigned long long>(b.depthWriteDraws),
      static_cast<unsigned long long>(b.depthFuncLessDraws),
      static_cast<unsigned long long>(b.depthFuncLessEqualDraws),
      static_cast<unsigned long long>(b.depthFuncAlwaysDraws),
      static_cast<unsigned long long>(b.depthFuncOtherDraws),
      static_cast<unsigned long long>(b.scissorEnabledDraws),
      static_cast<unsigned long long>(b.alphaBlendEnabledDraws),
      static_cast<unsigned long long>(b.blendStateSamples),
      static_cast<unsigned long long>(b.blendStateChanges),
      static_cast<unsigned long long>(b.blendStateUnique),
      static_cast<unsigned long long>(b.blendStateUniqueOverflows),
      static_cast<unsigned long long>(b.blendStateLast),
      static_cast<unsigned long long>(b.blendEnabledNoopDraws),
      static_cast<unsigned long long>(b.blendConstantFactorDraws),
      static_cast<unsigned long long>(b.blendScreenDraws),
      static_cast<unsigned long long>(b.blendAdditiveDraws),
      static_cast<unsigned long long>(b.blendAlphaCompositeDraws),
      static_cast<unsigned long long>(b.alphaBlendTexturedDraws),
      static_cast<unsigned long long>(b.alphaBlendTexturedPrimitives),
      static_cast<unsigned long long>(b.alphaBlendTexturedVertices),
      static_cast<unsigned long long>(b.alphaBlendSmallDraws),
      static_cast<unsigned long long>(b.alphaBlendSmallPrimitives),
      static_cast<unsigned long long>(b.alphaBlendSmallVertices),
      static_cast<unsigned long long>(b.alphaTestEnabledDraws),
      static_cast<unsigned long long>(b.alphaTestEffectiveDraws),
      static_cast<unsigned long long>(b.clipPlaneEnabledDraws),
      static_cast<unsigned long long>(b.pointDraws),
      static_cast<unsigned long long>(b.lineDraws),
      static_cast<unsigned long long>(b.triangleDraws),
      static_cast<unsigned long long>(b.primitiveCount),
      static_cast<unsigned long long>(b.triangleEstimate),
      static_cast<unsigned long long>(b.vertexCount),
      static_cast<unsigned long long>(b.routeDepthOnlyDraws),
      static_cast<unsigned long long>(b.routeDepthOnlyPrimitives),
      static_cast<unsigned long long>(b.routeDepthOnlyVertices),
      static_cast<unsigned long long>(b.routeProgrammableTexturedDraws),
      static_cast<unsigned long long>(b.routeProgrammableTexturedPrimitives),
      static_cast<unsigned long long>(b.routeProgrammableTexturedVertices),
      static_cast<unsigned long long>(b.routeProgrammableColorDraws),
      static_cast<unsigned long long>(b.routeProgrammableColorPrimitives),
      static_cast<unsigned long long>(b.routeProgrammableColorVertices),
      static_cast<unsigned long long>(b.routeAlphaBlendPrimitives),
      static_cast<unsigned long long>(b.routeAlphaTestPrimitives),
      static_cast<unsigned long long>(b.tileFfpRoutedTileDraws),
      static_cast<unsigned long long>(b.tileFfpRoutedTilePrimitives),
      static_cast<unsigned long long>(b.tileFfpRoutedTileVertices),
      static_cast<unsigned long long>(b.tileFfpRoutedPortableDraws),
      static_cast<unsigned long long>(b.tileFfpRoutedPortablePrimitives),
      static_cast<unsigned long long>(b.tileFfpRoutedPortableVertices),
      static_cast<unsigned long long>(b.tileFfpEligibleDraws),
      static_cast<unsigned long long>(b.tileFfpEligiblePrimitives),
      static_cast<unsigned long long>(b.tileFfpEligibleVertices),
      static_cast<unsigned long long>(b.tileFfpFallbackGpuFamilyDraws),
      static_cast<unsigned long long>(b.tileFfpFallbackGpuFamilyPrimitives),
      static_cast<unsigned long long>(b.tileFfpFallbackNotFfpDraws),
      static_cast<unsigned long long>(b.tileFfpFallbackNotFfpPrimitives),
      static_cast<unsigned long long>(b.tileFfpFallbackPrecisionDraws),
      static_cast<unsigned long long>(b.tileFfpFallbackPrecisionPrimitives),
      static_cast<unsigned long long>(b.tileFfpFallbackUnsupportedStateDraws),
      static_cast<unsigned long long>(b.tileFfpFallbackUnsupportedStatePrimitives),
      static_cast<unsigned long long>(b.indexedTriangleOpaqueDepthWriteDraws),
      static_cast<unsigned long long>(b.indexedTriangleOpaqueDepthWritePrimitives),
      static_cast<unsigned long long>(b.indexedTriangleOpaqueDepthWriteVertices),
      static_cast<unsigned long long>(b.indexedTriangleDepthReadDraws),
      static_cast<unsigned long long>(b.indexedTriangleDepthReadPrimitives),
      static_cast<unsigned long long>(b.indexedTriangleDepthReadVertices),
      static_cast<unsigned long long>(b.indexedTriangleAlphaBlendDraws),
      static_cast<unsigned long long>(b.indexedTriangleAlphaBlendPrimitives),
      static_cast<unsigned long long>(b.indexedTriangleAlphaBlendVertices),
      static_cast<unsigned long long>(b.indexedTriangleScissorDraws),
      static_cast<unsigned long long>(b.indexedTriangleScissorPrimitives),
      static_cast<unsigned long long>(b.indexedTriangleScissorVertices),
      static_cast<unsigned long long>(b.indexedTriangleTexturedDraws),
      static_cast<unsigned long long>(b.indexedTriangleTexturedPrimitives),
      static_cast<unsigned long long>(b.indexedTriangleTexturedVertices),
      static_cast<unsigned long long>(b.indexedTriangleLarge4096Draws),
      static_cast<unsigned long long>(b.indexedTriangleLarge4096Primitives),
      static_cast<unsigned long long>(b.indexedTriangleLarge4096Vertices),
      static_cast<unsigned long long>(b.indexedTriangleLarge4096OpaqueDepthWriteDraws),
      static_cast<unsigned long long>(b.indexedTriangleLarge4096OpaqueDepthWritePrimitives),
      static_cast<unsigned long long>(b.indexedTriangleLarge4096OpaqueDepthWriteVertices),
      static_cast<unsigned long long>(b.indexedTriangleLarge4096DepthReadDraws),
      static_cast<unsigned long long>(b.indexedTriangleLarge4096DepthReadPrimitives),
      static_cast<unsigned long long>(b.indexedTriangleLarge4096DepthReadVertices),
      static_cast<unsigned long long>(b.indexedTriangleLarge4096AlphaBlendDraws),
      static_cast<unsigned long long>(b.indexedTriangleLarge4096AlphaBlendPrimitives),
      static_cast<unsigned long long>(b.indexedTriangleLarge4096AlphaBlendVertices),
      static_cast<unsigned long long>(b.indexedTriangleLarge4096ScissorDraws),
      static_cast<unsigned long long>(b.indexedTriangleLarge4096ScissorPrimitives),
      static_cast<unsigned long long>(b.indexedTriangleLarge4096ScissorVertices),
      static_cast<unsigned long long>(b.indexedTriangleLarge4096TexturedDraws),
      static_cast<unsigned long long>(b.indexedTriangleLarge4096TexturedPrimitives),
      static_cast<unsigned long long>(b.indexedTriangleLarge4096TexturedVertices),
      static_cast<unsigned long long>(b.textureMaskOr),
      static_cast<unsigned long long>(b.fragmentTextureBindingSamples),
      static_cast<unsigned long long>(b.fragmentTextureBindingMaskOr),
      static_cast<unsigned long long>(b.x8RtTextureBindingSamples),
      static_cast<unsigned long long>(b.x8RtTextureBindingMaskOr),
      static_cast<unsigned long long>(b.x8RtTextureBindingUniqueHandles),
      static_cast<unsigned long long>(b.x8RtTextureBindingUniqueHandleOverflows),
      static_cast<unsigned long long>(b.x8RtTextureBindingShaderReadViewSamples),
      static_cast<unsigned long long>(b.x8RtTextureBindingActiveRtAliasSamples),
      static_cast<unsigned long long>(b.x8ShaderAlphaFillSamples),
      static_cast<unsigned long long>(b.x8ShaderAlphaFillMaskOr),
      static_cast<unsigned long long>(b.x8RtTextureBindingLastStage),
      static_cast<unsigned long long>(b.x8RtTextureBindingLastHandle),
      static_cast<unsigned long long>(b.drawPrimitiveCountMin),
      static_cast<unsigned long long>(b.drawPrimitiveCountMax),
      static_cast<unsigned long long>(b.drawVertexCountMin),
      static_cast<unsigned long long>(b.drawVertexCountMax),
      static_cast<unsigned long long>(b.drawPrimitiveBucket1_63),
      static_cast<unsigned long long>(b.drawPrimitiveBucket64_255),
      static_cast<unsigned long long>(b.drawPrimitiveBucket256_1023),
      static_cast<unsigned long long>(b.drawPrimitiveBucket1024_4095),
      static_cast<unsigned long long>(b.drawPrimitiveBucket4096Plus),
      static_cast<unsigned long long>(b.drawVertexBucket1_255),
      static_cast<unsigned long long>(b.drawVertexBucket256_1023),
      static_cast<unsigned long long>(b.drawVertexBucket1024_4095),
      static_cast<unsigned long long>(b.drawVertexBucket4096_16383),
      static_cast<unsigned long long>(b.drawVertexBucket16384Plus),
      static_cast<unsigned long long>(b.drawGeometrySignatureSamples),
      static_cast<unsigned long long>(b.drawGeometrySignatureUnique),
      static_cast<unsigned long long>(b.drawGeometrySignatureUniqueOverflows),
      static_cast<unsigned long long>(b.drawGeometrySignatureDuplicates),
      static_cast<unsigned long long>(b.drawGeometrySignatureConsecutiveDuplicates),
      static_cast<unsigned long long>(b.drawGeometrySignatureLast),
      static_cast<unsigned long long>(b.indexedBaseVertexSamples),
      static_cast<unsigned long long>(b.indexedBaseVertexNonZeroDraws),
      static_cast<unsigned long long>(b.indexedBaseVertexNegativeDraws),
      static_cast<unsigned long long>(b.indexedBaseVertexPositiveDraws),
      static_cast<long long>(b.indexedBaseVertexMin),
      static_cast<long long>(b.indexedBaseVertexMax),
      static_cast<unsigned long long>(b.nativeBaseVertexRequestedDraws),
      static_cast<unsigned long long>(b.nativeBaseVertexUsedDraws),
      static_cast<unsigned long long>(b.nativeBaseVertexSkippedNegativeDraws),
      static_cast<unsigned long long>(b.splitLargeIndexedSourceDraws),
      static_cast<unsigned long long>(b.splitLargeIndexedMetalDraws),
      static_cast<unsigned long long>(b.splitLargeIndexedExtraDraws),
      static_cast<unsigned long long>(b.splitLargeIndexedPrimitiveLimit),
      static_cast<unsigned long long>(b.splitLargeIndexedStream0SpanLimit),
      static_cast<unsigned long long>(b.splitLargeIndexedChunkStream0SpanMax),
      static_cast<unsigned long long>(b.splitLargeIndexedPrimitiveCount),
      static_cast<unsigned long long>(b.indexedOrderProbeDraws),
      static_cast<unsigned long long>(b.indexedOrderProbeSkipped),
      static_cast<unsigned long long>(b.indexedOrderProbeBytes),
      static_cast<unsigned long long>(b.indexedOrderOptimizedDraws),
      static_cast<unsigned long long>(b.indexedOrderOptimizedSkipped),
      static_cast<unsigned long long>(b.indexedOrderOptimizedBytes),
      static_cast<unsigned long long>(b.probeScissorRectDraws),
      static_cast<unsigned long long>(b.probeScissorRectSkipped),
      static_cast<unsigned long long>(b.probeScissorRectAreaDeltaPixels),
      static_cast<unsigned long long>(b.probeDisableAlphaBlendDraws),
      static_cast<unsigned long long>(b.probeDisableDepthWriteDraws),
      static_cast<unsigned long long>(b.probeDepthFuncAlwaysDraws),
      static_cast<unsigned long long>(b.probeForceTextureWhiteDraws),
      static_cast<unsigned long long>(b.probeFragmentlessDepthOnlyDraws),
      static_cast<unsigned long long>(b.probeFragmentlessDepthOnlyPrimitives),
      static_cast<unsigned long long>(b.probeFragmentlessDepthOnlyVertices),
      static_cast<unsigned long long>(b.indexedVertexReuseSamples),
      static_cast<unsigned long long>(b.indexedVertexReuseSkipped),
      static_cast<unsigned long long>(b.indexedVertexReferenceCount),
      static_cast<unsigned long long>(b.indexedUniqueVertexEstimate),
      static_cast<unsigned long long>(b.indexedVertexCacheMissEstimate16),
      static_cast<unsigned long long>(b.indexedVertexCacheMissEstimate32),
      static_cast<unsigned long long>(b.indexedVertexCacheMissEstimate64),
      static_cast<unsigned long long>(b.indexedCacheOptCandidateDraws),
      static_cast<unsigned long long>(b.indexedCacheOptCandidateSkipped),
      static_cast<unsigned long long>(b.indexedCacheOptCandidateBytes),
      static_cast<unsigned long long>(b.indexedCacheOptCandidateOriginalMiss16),
      static_cast<unsigned long long>(b.indexedCacheOptCandidateOriginalMiss32),
      static_cast<unsigned long long>(b.indexedCacheOptCandidateOriginalMiss64),
      static_cast<unsigned long long>(b.indexedCacheOptCandidateMiss16),
      static_cast<unsigned long long>(b.indexedCacheOptCandidateMiss32),
      static_cast<unsigned long long>(b.indexedCacheOptCandidateMiss64),
      static_cast<unsigned long long>(b.indexedCacheOptCandidateGatePass),
      static_cast<unsigned long long>(b.indexedCacheOptCandidateGateFail),
      static_cast<unsigned long long>(b.indexedCacheOptCandidateOpaqueDepthDraws),
      static_cast<unsigned long long>(b.indexedCacheOptCandidateScreenBlendDraws),
      static_cast<unsigned long long>(b.indexedCacheOptCandidatePrimitiveBucket1_63),
      static_cast<unsigned long long>(b.indexedCacheOptCandidatePrimitiveBucket64_255),
      static_cast<unsigned long long>(b.indexedCacheOptCandidatePrimitiveBucket256_1023),
      static_cast<unsigned long long>(b.indexedCacheOptCandidatePrimitiveBucket1024_4095),
      static_cast<unsigned long long>(b.indexedCacheOptCandidatePrimitiveBucket4096Plus),
      static_cast<unsigned long long>(b.reorderedIndexCacheLookups),
      static_cast<unsigned long long>(b.reorderedIndexCacheHits),
      static_cast<unsigned long long>(b.reorderedIndexCacheRejectedHits),
      static_cast<unsigned long long>(b.reorderedIndexCacheMisses),
      static_cast<unsigned long long>(b.reorderedIndexCacheCreated),
      static_cast<unsigned long long>(b.reorderedIndexCacheCreatedBytes),
      static_cast<unsigned long long>(b.stream0StrideMin),
      static_cast<unsigned long long>(b.stream0StrideMax),
      static_cast<unsigned long long>(b.streamStateSamples),
      static_cast<unsigned long long>(b.streamMetalBinds),
      static_cast<unsigned long long>(b.streamMetalBindFirsts),
      static_cast<unsigned long long>(b.streamMetalBindHandleChanges),
      static_cast<unsigned long long>(b.streamMetalBindOffsetChanges),
      static_cast<unsigned long long>(b.streamUniqueHandles),
      static_cast<unsigned long long>(b.streamUniqueHandleOverflows),
      static_cast<unsigned long long>(b.streamUniqueBytes),
      static_cast<unsigned long long>(b.streamUniqueDynamicHandles),
      static_cast<unsigned long long>(b.streamUniqueWriteOnlyHandles),
      static_cast<unsigned long long>(b.streamUniqueDefaultPoolHandles),
      static_cast<unsigned long long>(b.streamUniqueManagedPoolHandles),
      static_cast<unsigned long long>(b.streamUniqueSystemMemPoolHandles),
      static_cast<unsigned long long>(b.streamUniqueScratchPoolHandles),
      static_cast<unsigned long long>(b.streamHandleChanges),
      static_cast<unsigned long long>(b.streamOffsetChanges),
      static_cast<unsigned long long>(b.streamStrideChanges),
      static_cast<unsigned long long>(b.stream0LastHandle),
      static_cast<unsigned long long>(b.stream0LastOffset),
      static_cast<unsigned long long>(b.stream0LastStride),
      static_cast<unsigned long long>(b.ibStateSamples),
      static_cast<unsigned long long>(b.ibMetalBinds),
      static_cast<unsigned long long>(b.ibHandleChanges),
      static_cast<unsigned long long>(b.ibUniqueHandles),
      static_cast<unsigned long long>(b.ibUniqueHandleOverflows),
      static_cast<unsigned long long>(b.ibUniqueBytes),
      static_cast<unsigned long long>(b.ibUniqueDynamicHandles),
      static_cast<unsigned long long>(b.ibUniqueWriteOnlyHandles),
      static_cast<unsigned long long>(b.ibUniqueDefaultPoolHandles),
      static_cast<unsigned long long>(b.ibUniqueManagedPoolHandles),
      static_cast<unsigned long long>(b.ibUniqueSystemMemPoolHandles),
      static_cast<unsigned long long>(b.ibUniqueScratchPoolHandles),
      static_cast<unsigned long long>(b.ibLastHandle),
      static_cast<unsigned long long>(b.psoStateSamples),
      static_cast<unsigned long long>(b.psoHandleChanges),
      static_cast<unsigned long long>(b.psoUniqueHandles),
      static_cast<unsigned long long>(b.psoUniqueHandleOverflows),
      static_cast<unsigned long long>(b.psoLastHandle),
      static_cast<unsigned long long>(b.shaderVariantChanges),
      static_cast<unsigned long long>(b.shaderVariantUnique),
      static_cast<unsigned long long>(b.shaderVariantUniqueOverflows),
      static_cast<unsigned long long>(b.shaderVariantLast),
      static_cast<unsigned long long>(b.vertexShaderLast),
      static_cast<unsigned long long>(b.pixelShaderLast),
      static_cast<unsigned long long>(b.vertexShaderSourceLast),
      static_cast<unsigned long long>(b.pixelShaderSourceLast),
      static_cast<unsigned long long>(b.vsOutLayoutChanges),
      static_cast<unsigned long long>(b.vsOutLayoutUnique),
      static_cast<unsigned long long>(b.vsOutLayoutUniqueOverflows),
      static_cast<unsigned>(b.vsOutLayoutLast),
      static_cast<unsigned long long>(b.vsOutLayoutCacheHits),
      static_cast<unsigned long long>(b.vsOutLayoutCacheMisses),
      static_cast<unsigned long long>(b.argbufTableBytes),
      static_cast<unsigned long long>(b.argbufCbufBytes),
      static_cast<unsigned long long>(b.argbufCbufVsBytes),
      static_cast<unsigned long long>(b.argbufCbufFfpVsBytes),
      static_cast<unsigned long long>(b.argbufCbufPsBytes),
      static_cast<unsigned long long>(b.argbufCbufFfpPsBytes),
      static_cast<unsigned long long>(b.argbufCbufVsFirstBytes),
      static_cast<unsigned long long>(b.argbufCbufVsRewriteChangedBytes),
      static_cast<unsigned long long>(b.argbufCbufVsRewriteUnchangedBytes),
      static_cast<unsigned long long>(b.argbufCbufVsFloatChangedBytes),
      static_cast<unsigned long long>(b.argbufCbufVsIntChangedBytes),
      static_cast<unsigned long long>(b.argbufCbufVsBoolChangedBytes),
      static_cast<unsigned long long>(b.argbufCbufVsUploads),
      static_cast<unsigned long long>(b.argbufCbufVsFullStructUploads),
      static_cast<unsigned long long>(b.argbufCbufVsUsageUnknownUploads),
      static_cast<unsigned long long>(b.argbufCbufVsUsageIndexedFloatUploads),
      static_cast<unsigned long long>(b.argbufCbufVsPlanFloatRegsSum),
      static_cast<unsigned long long>(b.argbufCbufVsPlanFloatRegsMax),
      static_cast<unsigned long long>(b.argbufCbufVsDirtyFloatRegsSum),
      static_cast<unsigned long long>(b.argbufCbufVsDirtyFloatRegsMax),
      static_cast<unsigned long long>(b.argbufCbufVsUsageFloatRegsSum),
      static_cast<unsigned long long>(b.argbufCbufVsUsageFloatRegsMax),
      static_cast<unsigned long long>(b.argbufCbufFfpVsFirstBytes),
      static_cast<unsigned long long>(b.argbufCbufFfpVsRewriteChangedBytes),
      static_cast<unsigned long long>(b.argbufCbufFfpVsRewriteUnchangedBytes),
      static_cast<unsigned long long>(b.argbufCbufFfpVsMatrixChangedBytes),
      static_cast<unsigned long long>(b.argbufCbufFfpVsMaterialChangedBytes),
      static_cast<unsigned long long>(b.argbufCbufFfpVsLightChangedBytes),
      static_cast<unsigned long long>(b.argbufCbufFfpVsBlendChangedBytes),
      static_cast<unsigned long long>(b.argbufCbufFfpVsTexTransformChangedBytes),
      static_cast<unsigned long long>(b.argbufCbufFfpVsClipChangedBytes),
      static_cast<unsigned long long>(b.argbufCbufFfpVsViewportChangedBytes),
      static_cast<unsigned long long>(b.argbufCbufFfpVsFogPointChangedBytes),
      static_cast<unsigned long long>(b.setVertexBytesCalls),
      static_cast<unsigned long long>(b.setVertexBytesBytes),
      static_cast<unsigned long long>(b.setVertexBytesSlot5Calls),
      static_cast<unsigned long long>(b.setVertexBytesSlot5Bytes),
      static_cast<unsigned long long>(b.setVertexBytesOtherCalls),
      static_cast<unsigned long long>(b.setVertexBytesOtherBytes),
      static_cast<unsigned long long>(b.transientVertexBytes),
      static_cast<unsigned long long>(b.transientVertexUserBytes),
      static_cast<unsigned long long>(b.transientVertexPreuploadBytes),
      static_cast<unsigned long long>(b.transientVertexDeclFallbackBytes),
      static_cast<unsigned long long>(b.transientVertexExpandedMainBytes),
      static_cast<unsigned long long>(b.transientVertexExpandedExtraBytes),
      static_cast<unsigned long long>(b.transientVertexStagedStreamBytes),
      static_cast<unsigned long long>(b.transientIndexBytes),
      static_cast<unsigned long long>(b.transientIndexUserBytes),
      static_cast<unsigned long long>(b.transientIndexPreuploadBytes),
      static_cast<unsigned long long>(b.transientIndexShadowFallbackBytes),
      static_cast<unsigned long long>(b.transientIndexProbeReorderBytes),
      static_cast<unsigned long long>(b.transientIndexOptimizedOrderBytes),
      static_cast<unsigned long long>(b.transientIndexStagedIbBytes));
  for (std::size_t i = 0; i < b.streams.size(); ++i) {
    const auto& s = b.streams[i];
    if (!s.valid) {
      continue;
    }
    std::fprintf(
        stderr,
        "[dxmt9-perf-encoder-stream seq=%llu encoder=%llu stream=%zu "
        "samples=%llu metal_binds=%llu metal_bind_firsts=%llu "
        "metal_bind_handle_changes=%llu metal_bind_offset_changes=%llu "
        "unique_handles=%llu unique_handle_overflows=%llu unique_bytes=%llu "
        "unique_dynamic_handles=%llu unique_writeonly_handles=%llu "
        "unique_default_pool_handles=%llu unique_managed_pool_handles=%llu "
        "unique_systemmem_pool_handles=%llu unique_scratch_pool_handles=%llu "
        "handle_changes=%llu "
        "offset_changes=%llu stride_changes=%llu last_handle=0x%llx "
        "last_offset=%llu last_stride=%llu]\n",
        static_cast<unsigned long long>(b.seqId),
        static_cast<unsigned long long>(b.encoderIndex),
        i,
        static_cast<unsigned long long>(s.samples),
        static_cast<unsigned long long>(s.metalBinds),
        static_cast<unsigned long long>(s.metalBindFirsts),
        static_cast<unsigned long long>(s.metalBindHandleChanges),
        static_cast<unsigned long long>(s.metalBindOffsetChanges),
        static_cast<unsigned long long>(s.uniqueHandles),
        static_cast<unsigned long long>(s.uniqueHandleOverflows),
        static_cast<unsigned long long>(s.uniqueBytes),
        static_cast<unsigned long long>(s.uniqueDynamicHandles),
        static_cast<unsigned long long>(s.uniqueWriteOnlyHandles),
        static_cast<unsigned long long>(s.uniqueDefaultPoolHandles),
        static_cast<unsigned long long>(s.uniqueManagedPoolHandles),
        static_cast<unsigned long long>(s.uniqueSystemMemPoolHandles),
        static_cast<unsigned long long>(s.uniqueScratchPoolHandles),
        static_cast<unsigned long long>(s.handleChanges),
        static_cast<unsigned long long>(s.offsetChanges),
        static_cast<unsigned long long>(s.strideChanges),
        static_cast<unsigned long long>(s.lastHandle),
        static_cast<unsigned long long>(s.lastOffset),
        static_cast<unsigned long long>(s.lastStride));
  }
}

CounterSnapshot snapshot() {
  const Counters& c = counters();
  CounterSnapshot s;
  s.submitDraw = load(c.submitDraw);
  s.submitClear = load(c.submitClear);
  s.submitStretch = load(c.submitStretch);
  s.submitPresent = load(c.submitPresent);
  s.submitFlush = load(c.submitFlush);
  s.commandBuffers = load(c.commandBuffers);
  s.renderPassBegin = load(c.renderPassBegin);
  s.renderPassEnd = load(c.renderPassEnd);
  s.drawCalls = load(c.drawCalls);
  s.drawIndexedCalls = load(c.drawIndexedCalls);
  s.drawPrimitiveCount = load(c.drawPrimitiveCount);
  s.drawTriangleEstimate = load(c.drawTriangleEstimate);
  s.drawVertexCount = load(c.drawVertexCount);
  s.bindPipeline = load(c.bindPipeline);
  s.presentEncoded = load(c.presentEncoded);
  s.submitDrawCpuNs = load(c.submitDrawCpuNs);
  s.encodeChunkCalls = load(c.encodeChunkCalls);
  s.encodeChunkCpuNs = load(c.encodeChunkCpuNs);
  s.encodeDrawCpuNs = load(c.encodeDrawCpuNs);
  s.encodeDrawPipelineLookupCpuNs = load(c.encodeDrawPipelineLookupCpuNs);
  s.encodeDrawUniformBuildCpuNs = load(c.encodeDrawUniformBuildCpuNs);
  s.encodeDrawFvfDecodeCpuNs = load(c.encodeDrawFvfDecodeCpuNs);
  s.encodeDrawBindingPacketCpuNs = load(c.encodeDrawBindingPacketCpuNs);
  s.encodeDrawBindingPacketPlanCpuNs = load(c.encodeDrawBindingPacketPlanCpuNs);
  s.encodeDrawBindingPacketPlanFragmentCpuNs =
      load(c.encodeDrawBindingPacketPlanFragmentCpuNs);
  s.encodeDrawBindingPacketPlanVertexCpuNs =
      load(c.encodeDrawBindingPacketPlanVertexCpuNs);
  s.encodeDrawBindingPacketPlanExtraStreamCpuNs =
      load(c.encodeDrawBindingPacketPlanExtraStreamCpuNs);
  s.encodeDrawBindingPacketPlanRasterCpuNs =
      load(c.encodeDrawBindingPacketPlanRasterCpuNs);
  s.encodeDrawBindingPacketCacheCpuNs = load(c.encodeDrawBindingPacketCacheCpuNs);
  s.encodeDrawBindingPacketTextureRecordCpuNs =
      load(c.encodeDrawBindingPacketTextureRecordCpuNs);
  s.encodeDrawArgbufSetupCpuNs = load(c.encodeDrawArgbufSetupCpuNs);
  s.encodeDrawArgbufOpenCpuNs = load(c.encodeDrawArgbufOpenCpuNs);
  s.encodeDrawArgbufCbufUpdateCpuNs = load(c.encodeDrawArgbufCbufUpdateCpuNs);
  s.encodeDrawStreamBindCpuNs = load(c.encodeDrawStreamBindCpuNs);
  s.encodeDrawIssueCpuNs = load(c.encodeDrawIssueCpuNs);
  s.encodeDrawStreamBindViewportCpuNs = load(c.encodeDrawStreamBindViewportCpuNs);
  s.encodeDrawStreamBindFfpCpuNs = load(c.encodeDrawStreamBindFfpCpuNs);
  s.encodeDrawStreamBindVsCpuNs = load(c.encodeDrawStreamBindVsCpuNs);
  s.encodeDrawStreamBindTextureCpuNs = load(c.encodeDrawStreamBindTextureCpuNs);
  s.encodeDrawStreamBindIndexCpuNs = load(c.encodeDrawStreamBindIndexCpuNs);
  s.encodeDrawFvfDecodeDeclCpuNs = load(c.encodeDrawFvfDecodeDeclCpuNs);
  s.encodeDrawFvfDecodeBytesCpuNs = load(c.encodeDrawFvfDecodeBytesCpuNs);
  s.encodeDrawFvfDecodeExpandedCpuNs = load(c.encodeDrawFvfDecodeExpandedCpuNs);
  s.encodeDrawUniformBuildMainCpuNs = load(c.encodeDrawUniformBuildMainCpuNs);
  s.encodeDrawUniformBuildFfpCpuNs = load(c.encodeDrawUniformBuildFfpCpuNs);
  s.encodeDrawUniformBuildVsCpuNs = load(c.encodeDrawUniformBuildVsCpuNs);
  s.encodeDrawPhaseSetupCpuNs = load(c.encodeDrawPhaseSetupCpuNs);
  s.encodeDrawPhaseArgbufUniformCpuNs = load(c.encodeDrawPhaseArgbufUniformCpuNs);
  s.encodeDrawPhaseStreamPrepCpuNs = load(c.encodeDrawPhaseStreamPrepCpuNs);
  s.encodeDrawPhaseFfpVertexCpuNs = load(c.encodeDrawPhaseFfpVertexCpuNs);
  s.encodeDrawPhaseVertexBindCpuNs = load(c.encodeDrawPhaseVertexBindCpuNs);
  s.encodeDrawPhaseBaseStateCpuNs = load(c.encodeDrawPhaseBaseStateCpuNs);
  s.encodeDrawPhaseTileFfpFallthroughCpuNs = load(c.encodeDrawPhaseTileFfpFallthroughCpuNs);
  s.encodeDrawPhaseRemainderCpuNs = load(c.encodeDrawPhaseRemainderCpuNs);
  s.transientUploadCpuNs = load(c.transientUploadCpuNs);
  s.commandBufferCreateCpuNs = load(c.commandBufferCreateCpuNs);
  s.commandBufferCommitCpuNs = load(c.commandBufferCommitCpuNs);
  s.completionWaitNs = load(c.completionWaitNs);
  s.presentAcquireWaitNs = load(c.presentAcquireWaitNs);
  s.presentBoundaryWaitNs = load(c.presentBoundaryWaitNs);
  s.presentTokenWaitNs = load(c.presentTokenWaitNs);
  s.gpuCommandBufferTimeNs = load(c.gpuCommandBufferTimeNs);
  s.gpuCommandBufferTimeSamples = load(c.gpuCommandBufferTimeSamples);
  s.renderEncoderGpuTimeNs = load(c.renderEncoderGpuTimeNs);
  s.renderEncoderGpuTimeSamples = load(c.renderEncoderGpuTimeSamples);
  s.renderEncoderGpuLastPassType = load(c.renderEncoderGpuLastPassType);
  s.renderEncoderGpuLastRt = load(c.renderEncoderGpuLastRt);
  s.renderEncoderGpuLastDepth = load(c.renderEncoderGpuLastDepth);
  s.renderEncoderGpuLastPso = load(c.renderEncoderGpuLastPso);
  s.gpuCommandBufferErrors = load(c.gpuCommandBufferErrors);
  s.subCommandBufferCommits = load(c.subCommandBufferCommits);
  return s;
}

void emitFrameDelta(std::uint64_t frameId,
                    const CounterSnapshot& prev,
                    const CounterSnapshot& curr) {
  using Clock = std::chrono::steady_clock;
  static thread_local Clock::time_point prevEmitTime{};
  const auto now = Clock::now();
  double wallMs = 0.0;
  if (prevEmitTime != Clock::time_point{}) {
    wallMs =
        std::chrono::duration<double, std::milli>(now - prevEmitTime).count();
  }
  prevEmitTime = now;
  const double fps = wallMs > 0.0 ? 1000.0 / wallMs : 0.0;

  auto delta = [](std::uint64_t a, std::uint64_t b) -> std::uint64_t {
    // Atomic loads are relaxed and the snapshot covers many counters,
    // so a later snapshot field could lag a prior one if a writer is
    // mid-update — clamp to 0 instead of wrapping.
    return b >= a ? b - a : 0ull;
  };
  std::fprintf(
      stderr,
      "[dxmt9-perf-frame frame=%llu "
      "wall_ms=%.3f fps=%.3f "
      "submit_draw=%llu submit_clear=%llu submit_stretch=%llu "
      "submit_present=%llu submit_flush=%llu command_buffers=%llu "
      "render_pass_begin=%llu render_pass_end=%llu "
      "draw_calls=%llu draw_indexed=%llu draw_primitives=%llu "
      "draw_triangles=%llu draw_vertices=%llu bind_pipeline=%llu "
      "present_encoded=%llu "
      "submit_draw_cpu_ms=%.3f encode_chunk_calls=%llu "
      "encode_chunk_cpu_ms=%.3f encode_draw_cpu_ms=%.3f "
      "encode_draw_pipeline_lookup_cpu_ms=%.3f "
      "encode_draw_uniform_build_cpu_ms=%.3f "
      "encode_draw_fvf_decode_cpu_ms=%.3f "
      "encode_draw_binding_packet_cpu_ms=%.3f "
      "encode_draw_binding_packet_plan_cpu_ms=%.3f "
      "encode_draw_binding_packet_plan_fragment_cpu_ms=%.3f "
      "encode_draw_binding_packet_plan_vertex_cpu_ms=%.3f "
      "encode_draw_binding_packet_plan_extra_stream_cpu_ms=%.3f "
      "encode_draw_binding_packet_plan_raster_cpu_ms=%.3f "
      "encode_draw_binding_packet_cache_cpu_ms=%.3f "
      "encode_draw_binding_packet_texture_record_cpu_ms=%.3f "
      "encode_draw_argbuf_setup_cpu_ms=%.3f "
      "encode_draw_argbuf_open_cpu_ms=%.3f "
      "encode_draw_argbuf_cbuf_update_cpu_ms=%.3f "
      "encode_draw_stream_bind_cpu_ms=%.3f "
      "encode_draw_issue_cpu_ms=%.3f transient_upload_cpu_ms=%.3f "
      "command_buffer_create_cpu_ms=%.3f command_buffer_commit_cpu_ms=%.3f "
      "completion_wait_ms=%.3f present_acquire_wait_ms=%.3f "
      "present_boundary_wait_ms=%.3f present_token_wait_ms=%.3f "
      "gpu_command_buffer_time_ms=%.3f gpu_command_buffer_time_samples=%llu "
      "render_encoder_gpu_time_ms=%.3f render_encoder_gpu_time_samples=%llu "
      "gpu_command_buffer_errors=%llu sub_command_buffers=%llu]\n",
      static_cast<unsigned long long>(frameId),
      wallMs,
      fps,
      static_cast<unsigned long long>(delta(prev.submitDraw, curr.submitDraw)),
      static_cast<unsigned long long>(delta(prev.submitClear, curr.submitClear)),
      static_cast<unsigned long long>(delta(prev.submitStretch, curr.submitStretch)),
      static_cast<unsigned long long>(delta(prev.submitPresent, curr.submitPresent)),
      static_cast<unsigned long long>(delta(prev.submitFlush, curr.submitFlush)),
      static_cast<unsigned long long>(delta(prev.commandBuffers, curr.commandBuffers)),
      static_cast<unsigned long long>(delta(prev.renderPassBegin, curr.renderPassBegin)),
      static_cast<unsigned long long>(delta(prev.renderPassEnd, curr.renderPassEnd)),
      static_cast<unsigned long long>(delta(prev.drawCalls, curr.drawCalls)),
      static_cast<unsigned long long>(delta(prev.drawIndexedCalls, curr.drawIndexedCalls)),
      static_cast<unsigned long long>(delta(prev.drawPrimitiveCount, curr.drawPrimitiveCount)),
      static_cast<unsigned long long>(delta(prev.drawTriangleEstimate, curr.drawTriangleEstimate)),
      static_cast<unsigned long long>(delta(prev.drawVertexCount, curr.drawVertexCount)),
      static_cast<unsigned long long>(delta(prev.bindPipeline, curr.bindPipeline)),
      static_cast<unsigned long long>(delta(prev.presentEncoded, curr.presentEncoded)),
      static_cast<double>(delta(prev.submitDrawCpuNs, curr.submitDrawCpuNs)) / 1000000.0,
      static_cast<unsigned long long>(delta(prev.encodeChunkCalls, curr.encodeChunkCalls)),
      static_cast<double>(delta(prev.encodeChunkCpuNs, curr.encodeChunkCpuNs)) / 1000000.0,
      static_cast<double>(delta(prev.encodeDrawCpuNs, curr.encodeDrawCpuNs)) / 1000000.0,
      static_cast<double>(delta(prev.encodeDrawPipelineLookupCpuNs,
                                 curr.encodeDrawPipelineLookupCpuNs)) /
          1000000.0,
      static_cast<double>(delta(prev.encodeDrawUniformBuildCpuNs,
                                 curr.encodeDrawUniformBuildCpuNs)) /
          1000000.0,
      static_cast<double>(delta(prev.encodeDrawFvfDecodeCpuNs,
                                 curr.encodeDrawFvfDecodeCpuNs)) /
          1000000.0,
      static_cast<double>(delta(prev.encodeDrawBindingPacketCpuNs,
                                 curr.encodeDrawBindingPacketCpuNs)) /
          1000000.0,
      static_cast<double>(delta(prev.encodeDrawBindingPacketPlanCpuNs,
                                 curr.encodeDrawBindingPacketPlanCpuNs)) /
          1000000.0,
      static_cast<double>(
          delta(prev.encodeDrawBindingPacketPlanFragmentCpuNs,
                curr.encodeDrawBindingPacketPlanFragmentCpuNs)) /
          1000000.0,
      static_cast<double>(
          delta(prev.encodeDrawBindingPacketPlanVertexCpuNs,
                curr.encodeDrawBindingPacketPlanVertexCpuNs)) /
          1000000.0,
      static_cast<double>(
          delta(prev.encodeDrawBindingPacketPlanExtraStreamCpuNs,
                curr.encodeDrawBindingPacketPlanExtraStreamCpuNs)) /
          1000000.0,
      static_cast<double>(
          delta(prev.encodeDrawBindingPacketPlanRasterCpuNs,
                curr.encodeDrawBindingPacketPlanRasterCpuNs)) /
          1000000.0,
      static_cast<double>(delta(prev.encodeDrawBindingPacketCacheCpuNs,
                                 curr.encodeDrawBindingPacketCacheCpuNs)) /
          1000000.0,
      static_cast<double>(
          delta(prev.encodeDrawBindingPacketTextureRecordCpuNs,
                curr.encodeDrawBindingPacketTextureRecordCpuNs)) /
          1000000.0,
      static_cast<double>(delta(prev.encodeDrawArgbufSetupCpuNs,
                                 curr.encodeDrawArgbufSetupCpuNs)) /
          1000000.0,
      static_cast<double>(delta(prev.encodeDrawArgbufOpenCpuNs,
                                 curr.encodeDrawArgbufOpenCpuNs)) /
          1000000.0,
      static_cast<double>(delta(prev.encodeDrawArgbufCbufUpdateCpuNs,
                                 curr.encodeDrawArgbufCbufUpdateCpuNs)) /
          1000000.0,
      static_cast<double>(delta(prev.encodeDrawStreamBindCpuNs,
                                 curr.encodeDrawStreamBindCpuNs)) /
          1000000.0,
      static_cast<double>(delta(prev.encodeDrawIssueCpuNs, curr.encodeDrawIssueCpuNs)) /
          1000000.0,
      static_cast<double>(delta(prev.transientUploadCpuNs, curr.transientUploadCpuNs)) /
          1000000.0,
      static_cast<double>(delta(prev.commandBufferCreateCpuNs,
                                 curr.commandBufferCreateCpuNs)) /
          1000000.0,
      static_cast<double>(delta(prev.commandBufferCommitCpuNs,
                                 curr.commandBufferCommitCpuNs)) /
          1000000.0,
      static_cast<double>(delta(prev.completionWaitNs, curr.completionWaitNs)) / 1000000.0,
      static_cast<double>(delta(prev.presentAcquireWaitNs, curr.presentAcquireWaitNs)) /
          1000000.0,
      static_cast<double>(delta(prev.presentBoundaryWaitNs, curr.presentBoundaryWaitNs)) /
          1000000.0,
      static_cast<double>(delta(prev.presentTokenWaitNs, curr.presentTokenWaitNs)) /
          1000000.0,
      static_cast<double>(delta(prev.gpuCommandBufferTimeNs, curr.gpuCommandBufferTimeNs)) /
          1000000.0,
      static_cast<unsigned long long>(
          delta(prev.gpuCommandBufferTimeSamples, curr.gpuCommandBufferTimeSamples)),
      static_cast<double>(delta(prev.renderEncoderGpuTimeNs, curr.renderEncoderGpuTimeNs)) /
          1000000.0,
      static_cast<unsigned long long>(
          delta(prev.renderEncoderGpuTimeSamples, curr.renderEncoderGpuTimeSamples)),
      static_cast<unsigned long long>(
          delta(prev.gpuCommandBufferErrors, curr.gpuCommandBufferErrors)),
      static_cast<unsigned long long>(
          delta(prev.subCommandBufferCommits, curr.subCommandBufferCommits)));
}


}  // namespace dxmt9::perf
