dxmt9_shader_service_core_srcs = files('dxmt9_shader_service.cpp')
dxmt9_provider_service_abi_srcs = files('dxmt9_shader_service_exports.cpp')

dxmt9_runtime_srcs = files(
  'backend.cpp',
  'backend_metal.mm',
  'dxmt9_compat.cpp',
  'dxmt9_capture.cpp',
  'dxmt9_hud.mm',
  'dxmt9_presenter_support.mm',
  'dxmt9_queue.mm',
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

# The runtime (backend_metal.mm, dxmt9_queue.mm, dxmt9_hud.mm,
# dxmt9_presenter_support.mm) still contains Objective-C++ code; the WMT
# conversion removed the direct ObjC Metal calls from backend_metal.mm, but
# the sibling files retain ObjC for Metal Layer management and Cocoa
# interaction. Keep the runtime library darwin-only; post-merge with dxmt,
# the upper runtime relocates to a dxmt-compatible unix lib.
if host_machine.system() == 'darwin'
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
else
  dxmt9_runtime_dep = declare_dependency(
    include_directories: [dxmt9_inc, dxmt9_runtime_internal_inc],
    dependencies: [dxmt9_util_dep],
  )
endif
