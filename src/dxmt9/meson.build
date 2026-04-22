dxmt9_shader_service_core_srcs = files('dxmt9_shader_service.cpp')
dxmt9_provider_service_abi_srcs = files('dxmt9_shader_service_exports.cpp')

dxmt9_runtime_srcs = files(
  'backend_metal.mm',
  'dxmt9_command_queue.cpp',
  'dxmt9_compat.cpp',
  'dxmt9_capture.cpp',
  'dxmt9_blit_encoders.cpp',
  'dxmt9_device.cpp',
  'dxmt9_ffp_shaders.cpp',
  'dxmt9_format_convert.cpp',
  'dxmt9_pipeline_cache.cpp',
  'dxmt9_resource_pool.cpp',
  'dxmt9_shader_translator.cpp',
  'dxmt9_hud.mm',
  'dxmt9_presenter.cpp',
  'dxmt9_presenter_macdrv.cpp',
  'dxmt9_queue.cpp',
  'dxmt9_shader_sources.cpp',
)

dxmt9_runtime_internal_inc = include_directories('.', '..')

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
  dependencies: [dxmt9_util_dep],
  install: false,
)

dxmt9_runtime_dep = declare_dependency(
  link_with: dxmt9_runtime_lib,
  include_directories: [dxmt9_inc, dxmt9_runtime_internal_inc],
  dependencies: [dxmt9_util_dep],
)
