dxmt9_frontend_core_srcs = files(
  'com.cpp',
  'core.cpp',
  'core_buffer.cpp',
  'core_draw.cpp',
  'core_factory.cpp',
  'core_format.cpp',
  'core_format_utils.cpp',
  'core_present.cpp',
  'core_resources.cpp',
  'core_state.cpp',
  'core_surface.cpp',
  'core_texture.cpp',
)

dxmt9_frontend_provider_srcs = files(
  'device_c_draw.cpp',
  'device_c_factory.cpp',
  'device_c_format_utils.cpp',
  'device_c_marshal.cpp',
  'device_c_record_hazard.cpp',
  'device_c_record_replay.cpp',
  'device_c_record_validate.cpp',
  'device_c_resources.cpp',
  'device_c_shader_dump.cpp',
  'device_c_shader_vdecl.cpp',
  'device_c_state.cpp',
  'device_c_swapchain_query_stateblock.cpp',
)

dxmt9_frontend_bridge_srcs = files(
  'device_c_bridge_factory.cpp',
  'device_c_bridge_device_state_draw.cpp',
  'device_c_bridge_resources.cpp',
  'device_c_bridge_shader_vdecl.cpp',
  'device_c_bridge_swapchain_query_stateblock.cpp',
)

dxmt9_pe_core_srcs = files(
  'd3d9_pe_factory.cpp',
  'd3d9_pe_device.cpp',
  'd3d9_pe_device_child_buffer.cpp',
  'd3d9_pe_device_child_misc.cpp',
  'd3d9_pe_device_child_shader.cpp',
  'd3d9_pe_device_child_surface.cpp',
)

dxmt9_frontend_export_srcs = files(
  'd3d9_pe_exports.cpp',
  'd3d9_pe_validator.cpp',
)

dxmt9_d3d9_internal_inc = include_directories('.', '..')

if host_machine.system() == 'windows'
  cpp = meson.get_compiler('cpp')
  dxguid_dep = cpp.find_library('dxguid', required: true)
  gdi32_dep = cpp.find_library('gdi32', required: true)

  dxmt9_pe_core_lib = static_library(
    'dxmt9_pe_core',
    dxmt9_pe_core_srcs,
    include_directories: [dxmt9_inc, dxmt9_d3d9_internal_inc],
    dependencies: [dxmt9_util_dep, gdi32_dep],
    install: false,
  )

  dxmt9_pe_core_dep = declare_dependency(
    link_with: dxmt9_pe_core_lib,
    include_directories: [dxmt9_inc, dxmt9_d3d9_internal_inc],
    dependencies: [dxmt9_util_dep, gdi32_dep],
  )
endif

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

if host_machine.system() == 'windows'
  dxmt9_d3d9_forwarder_deps = [
    dxguid_dep,
    dxmt9_util_dep,
    dxmt9_pe_core_dep,
  ]
  dxmt9_d3d9_forwarder_cpp_args = []

  if get_option('wine_builtin_dll')
    dxmt9_d3d9_forwarder_deps += [dxmt9_winemetal_import_dep]
  else
    dxmt9_d3d9_forwarder_cpp_args += ['-DDXMT9_DYNAMIC_WINEMETAL_BRIDGE=1']
  endif

  # PE-side d3d9 forwarder — IDirect3D9/Device9 vtables that wrap the
  # dxmt9c_* C bridge surface. The generated dxmt9_device_c_bridge_exports
  # supplies the unix-call client; dxmt9_pe_core_dep supplies the COM glue.
  dxmt9_d3d9_forwarder_lib = static_library(
    'dxmt9_d3d9_forwarder',
    dxmt9_frontend_export_srcs + [bridge_codegen[1]],
    include_directories: [dxmt9_inc, bridge_gen_inc, dxmt9_d3d9_internal_inc],
    dependencies: dxmt9_d3d9_forwarder_deps,
    cpp_args: dxmt9_d3d9_forwarder_cpp_args,
    install: false,
  )

  dxmt9_d3d9_forwarder_dep = declare_dependency(
    link_with: dxmt9_d3d9_forwarder_lib,
    include_directories: [dxmt9_inc, bridge_gen_inc, dxmt9_d3d9_internal_inc],
    dependencies: dxmt9_d3d9_forwarder_deps,
  )
endif
