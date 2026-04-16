dxmt9_runtime_srcs = files(
  'backend.cpp',
  'backend_metal.mm',
  'dxmt9_capture.cpp',
  'dxmt9_hud.mm',
  'dxmt9_presenter_support.mm',
  'dxmt9_queue.mm',
  'dxmt9_shader_service.cpp',
  'dxmt9_shader_service_exports.cpp',
)

dxmt9_runtime_internal_inc = include_directories('.', '..')

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
