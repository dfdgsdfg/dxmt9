dxmt9_frontend_core_srcs = files(
  'com.cpp',
  'core.cpp',
)

dxmt9_frontend_provider_srcs = files(
  'device_c_common.cpp',
  'device_c_factory.cpp',
  'device_c_device_state_draw.cpp',
  'device_c_resources.cpp',
  'device_c_shader_vdecl.cpp',
  'device_c_swapchain_query_stateblock.cpp',
)

dxmt9_frontend_bridge_srcs = files(
  'device_c_bridge_factory.cpp',
  'device_c_bridge_device_state_draw.cpp',
  'device_c_bridge_resources.cpp',
  'device_c_bridge_shader_vdecl.cpp',
  'device_c_bridge_swapchain_query_stateblock.cpp',
)

dxmt9_d3d9_internal_inc = include_directories('.', '..')

dxmt9_frontend_core_lib = static_library(
  'dxmt9_frontend_core',
  dxmt9_frontend_core_srcs,
  include_directories: [dxmt9_inc, dxmt9_d3d9_internal_inc],
  dependencies: [dxmt9_util_dep],
  install: false,
)

dxmt9_frontend_core_dep = declare_dependency(
  link_with: dxmt9_frontend_core_lib,
  include_directories: [dxmt9_inc, dxmt9_d3d9_internal_inc],
  dependencies: [dxmt9_util_dep],
)

dxmt9_frontend_provider_lib = static_library(
  'dxmt9_frontend_provider',
  dxmt9_frontend_provider_srcs,
  include_directories: [dxmt9_inc, dxmt9_d3d9_internal_inc],
  dependencies: [
    dxmt9_util_dep,
    dxmt9_frontend_core_dep,
  ],
  install: false,
)

dxmt9_frontend_provider_dep = declare_dependency(
  link_with: dxmt9_frontend_provider_lib,
  include_directories: [dxmt9_inc, dxmt9_d3d9_internal_inc],
  dependencies: [
    dxmt9_util_dep,
    dxmt9_frontend_core_dep,
  ],
)

dxmt9_frontend_bridge_lib = static_library(
  'dxmt9_frontend_bridge',
  dxmt9_frontend_bridge_srcs,
  include_directories: [dxmt9_inc, dxmt9_d3d9_internal_inc],
  install: false,
)

dxmt9_frontend_bridge_dep = declare_dependency(
  link_with: dxmt9_frontend_bridge_lib,
  include_directories: [dxmt9_inc, dxmt9_d3d9_internal_inc],
)

dxmt9_frontend_dep = declare_dependency(
  include_directories: [dxmt9_inc, dxmt9_d3d9_internal_inc],
  dependencies: [
    dxmt9_frontend_bridge_dep,
    dxmt9_frontend_core_dep,
    dxmt9_frontend_provider_dep,
  ],
)
