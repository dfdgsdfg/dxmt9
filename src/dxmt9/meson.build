dxmt9_shader_service_core_srcs = files('dxmt9_shader_service.cpp')
dxmt9_provider_service_abi_srcs = files('dxmt9_shader_service_exports.cpp')

# Shader-source-generation pipeline — used by both the GPU runtime (encoders
# + pipeline cache) AND the ELF winemetal.so unix module (for the shader
# __wine_unix_call handlers). Listed once here, compiled into runtime_lib
# below; the unix module compiles the same .cpp files into its own TU so
# winemetal.so resolves shader_service::compile without dragging the GPU
# runtime in.
dxmt9_shader_pipeline_srcs = files(
  'dxmt9_draw_shader.cpp',
  'dxmt9_ffp_shaders.cpp',
  'dxmt9_shader_decoder.cpp',
  'dxmt9_shader_metal_ir.cpp',
  'dxmt9_shader_request.cpp',
  'dxmt9_shader_sources.cpp',
)

dxmt9_runtime_srcs = files(
  'dxmt9_archive_prewarm.cpp',
  'dxmt9_argbuf_hybrid.cpp',
  'dxmt9_command_queue.cpp',
  'dxmt9_compat.cpp',
  'dxmt9_capture.cpp',
  'dxmt9_blit_encoders.cpp',
  'dxmt9_debug_alloc_guard.cpp',
  'dxmt9_debug_trace.cpp',
  'dxmt9_device.cpp',
  'dxmt9_draw_encoder.mm',
  'dxmt9_draw_encoder_diagnostics.mm',
  'dxmt9_draw_encoder_hazard.mm',
  'dxmt9_draw_state.cpp',
  'dxmt9_format_convert.cpp',
  'dxmt9_heap_manager.cpp',
  'dxmt9_pipeline_cache.cpp',
  'dxmt9_resource_initializer.mm',
  'dxmt9_resource_pool.cpp',
  'dxmt9_shader_archive.cpp',
  'dxmt9_hud.mm',
  'dxmt9_perf_counters.cpp',
  'dxmt9_presenter.mm',
  'dxmt9_presenter_macdrv.cpp',
  'dxmt9_signposts.cpp',
  'dxmt9_transient_resource_arena.cpp',
  'dxmt9_queue.cpp',
  'dxmt9_queue_trace.cpp',
  'dxmt9_uniform_dirty.cpp',
  'framegraph/fg_builder.cpp',
  'framegraph/fg_debug_export.cpp',
  'framegraph/fg_optimizer.cpp',
  'framegraph/fg_optimizer/dce.cpp',
  'framegraph/fg_optimizer/lifetime.cpp',
  'framegraph/fg_optimizer/loadstore.cpp',
  'framegraph/fg_optimizer/memoryless.cpp',
  'framegraph/fg_optimizer/passcoalesce.cpp',
  'framegraph/fg_optimizer/reorder.cpp',
  'render/backend_factory.cpp',
  'render/decision_recorder.cpp',
  'render/framegraph_backend.cpp',
  'render/traditional_backend.cpp',
) + dxmt9_shader_pipeline_srcs

dxmt9_runtime_internal_inc = include_directories('.', '..')

# Per-draw heap-allocation guard. The `auto` feature value derives from
# buildtype: enabled only for explicit `buildtype=debug`. Repo default
# (`debugoptimized`) leaves the guard compiled out so CI behaves as before.
# Manual override via `-Ddxmt9_per_draw_alloc_guard=enabled|disabled` always
# wins. Runtime behavior (when compiled in): guard is ON by default; set
# DXMT_DEBUG_NO_PER_DRAW_ALLOC=0 in the env to opt out.
dxmt9_per_draw_alloc_guard_opt = get_option('dxmt9_per_draw_alloc_guard')
if dxmt9_per_draw_alloc_guard_opt.enabled() or (
  dxmt9_per_draw_alloc_guard_opt.auto() and get_option('buildtype') == 'debug'
)
  dxmt9_runtime_cpp_args = ['-DDXMT_DEBUG_NO_PER_DRAW_ALLOC=1']
else
  dxmt9_runtime_cpp_args = []
endif

dxmt9_shader_service_core_lib = static_library(
  'dxmt9_shader_service_core',
  dxmt9_shader_service_core_srcs,
  include_directories: [dxmt9_inc, dxmt9_runtime_internal_inc],
  dependencies: [dxmt9_util_dep],
  install: false,
)

dxmt9_shader_service_core_dep = declare_dependency(
  link_with: dxmt9_shader_service_core_lib,
  include_directories: [dxmt9_inc, dxmt9_runtime_internal_inc],
  dependencies: [dxmt9_util_dep],
)

dxmt9_runtime_lib = static_library(
  'dxmt9_runtime',
  dxmt9_runtime_srcs,
  include_directories: [dxmt9_inc, dxmt9_runtime_internal_inc],
  cpp_args: dxmt9_runtime_cpp_args,
  objcpp_args: dxmt9_runtime_cpp_args,
  dependencies: [dxmt9_util_dep],
  install: false,
)

dxmt9_runtime_dep = declare_dependency(
  link_with: dxmt9_runtime_lib,
  include_directories: [dxmt9_inc, dxmt9_runtime_internal_inc],
  dependencies: [dxmt9_util_dep],
)
