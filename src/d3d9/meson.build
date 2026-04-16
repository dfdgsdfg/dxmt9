dxmt9_frontend_srcs = files(
  'com.cpp',
  'core.cpp',
  'device_c.cpp',
)

dxmt9_pe_core_srcs = files(
  'd3d9_pe_factory.cpp',
  'd3d9_pe_device.cpp',
)

dxmt9_pe_bridge_srcs = files(
  'd3d9_pe_exports.cpp',
  'd3d9_pe_validator.cpp',
)

dxmt9_d3d9_internal_inc = include_directories('.', '..')

if host_machine.system() == 'windows'
  dxmt9_pe_core_lib = static_library(
    'dxmt9_pe_core',
    dxmt9_pe_core_srcs,
    include_directories: [dxmt9_inc, dxmt9_d3d9_internal_inc],
    dependencies: [dxmt9_util_dep],
    install: false,
  )

  dxmt9_pe_core_dep = declare_dependency(
    link_with: dxmt9_pe_core_lib,
    include_directories: [dxmt9_inc, dxmt9_d3d9_internal_inc],
    dependencies: [dxmt9_util_dep],
  )

  dxmt9_pe_bridge_lib = static_library(
    'dxmt9_pe_bridge',
    dxmt9_pe_bridge_srcs,
    include_directories: [dxmt9_inc, dxmt9_d3d9_internal_inc],
    dependencies: [dxmt9_util_dep, dxmt9_pe_core_dep],
    install: false,
  )

  dxmt9_pe_bridge_dep = declare_dependency(
    link_with: dxmt9_pe_bridge_lib,
    include_directories: [dxmt9_inc, dxmt9_d3d9_internal_inc],
    dependencies: [dxmt9_util_dep, dxmt9_pe_core_dep],
  )
endif

dxmt9_frontend_lib = static_library(
  'dxmt9_frontend',
  dxmt9_frontend_srcs,
  include_directories: [dxmt9_inc, dxmt9_d3d9_internal_inc],
  dependencies: [dxmt9_util_dep],
  install: false,
)

dxmt9_frontend_dep = declare_dependency(
  link_with: dxmt9_frontend_lib,
  include_directories: [dxmt9_inc, dxmt9_d3d9_internal_inc],
  dependencies: [dxmt9_util_dep],
)
