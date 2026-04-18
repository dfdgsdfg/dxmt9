dxmt9_frontend_core_srcs = files(
  'com.cpp',
  'core.cpp',
)

dxmt9_frontend_provider_srcs = files(
  'device_c.cpp',
)

dxmt9_pe_core_srcs = files(
  'd3d9_pe_factory.cpp',
  'd3d9_pe_device.cpp',
)

dxmt9_frontend_export_srcs = files(
  'd3d9_pe_exports.cpp',
  'd3d9_pe_validator.cpp',
)

dxmt9_d3d9_internal_inc = include_directories('.', '..')

if host_machine.system() == 'windows'
  cpp = meson.get_compiler('cpp')
  dxguid_dep = cpp.find_library('dxguid', required: true)

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

dxmt9_frontend_bridge_dep = declare_dependency(
  include_directories: [dxmt9_inc, dxmt9_d3d9_internal_inc],
  dependencies: [
    dxmt9_util_dep,
  ],
)

dxmt9_frontend_dep = declare_dependency(
  include_directories: [dxmt9_inc, dxmt9_d3d9_internal_inc],
  dependencies: [
    dxmt9_frontend_core_dep,
    dxmt9_frontend_provider_dep,
  ],
)

if host_machine.system() == 'windows'
  dxmt9_d3d9_forwarder_lib = static_library(
    'dxmt9_d3d9_forwarder',
    dxmt9_frontend_export_srcs + [bridge_codegen[1]],
    include_directories: [dxmt9_inc, dxmt9_d3d9_internal_inc],
    dependencies: [
      dxguid_dep,
      dxmt9_util_dep,
      dxmt9_pe_core_dep,
      dxmt9_winemetal_import_dep,
    ],
    install: false,
  )

  dxmt9_d3d9_forwarder_dep = declare_dependency(
    link_with: dxmt9_d3d9_forwarder_lib,
    include_directories: [dxmt9_inc, dxmt9_d3d9_internal_inc],
    dependencies: [
      dxguid_dep,
      dxmt9_util_dep,
      dxmt9_pe_core_dep,
      dxmt9_winemetal_import_dep,
    ],
  )
endif
