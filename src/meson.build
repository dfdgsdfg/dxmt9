subdir('util')
subdir('d3d9')
subdir('dxmt9')
subdir('winemetal')
dxmt9_anchor_srcs = files('dxmt9/dxmt9.cpp')
dxmt9_native_bridge_srcs = files(
  'winemetal/winemetal_public_api.cpp',
  'winemetal/winemetal_bridge_service_abi.cpp',
)

dxmt9_deps = []

if host_machine.system() == 'darwin'
  dxmt9_deps += dependency('appleframeworks', modules: [
    'AppKit',
    'Foundation',
    'Metal',
    'QuartzCore',
  ])
endif

dxmt9_native_lib = shared_library(
  'dxmt9_native',
  dxmt9_anchor_srcs,
  dxmt9_native_bridge_srcs,
  include_directories: [
    dxmt9_inc,
    bridge_gen_inc,
    dxmt9_winemetal_internal_inc,
    include_directories('..'),
  ],
  dependencies: dxmt9_deps + [
    dxmt9_util_dep,
    dxmt9_frontend_bridge_dep,
    dxmt9_unix_runtime_root_dep,
  ],
  install: false,
)

dxmt9_dep = declare_dependency(
  include_directories: dxmt9_inc,
  link_with: dxmt9_native_lib,
  dependencies: [dxmt9_util_dep],
)

meson.override_dependency('dxmt9', dxmt9_dep)
