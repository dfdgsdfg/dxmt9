subdir('util')
subdir('d3d9')
subdir('dxmt9')
subdir('winemetal')
dxmt9_anchor_srcs = files('dxmt9/dxmt9.cpp')
dxmt9_native_bridge_srcs = files(
  'dxmt9/dxmt9_provider_public_api.cpp',
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
    dxmt9_winemetal_internal_inc,
    include_directories('..'),
  ],
  # link_whole the runtime + frontend libs — anchor srcs (dxmt9.cpp +
  # provider_public_api.cpp) only reference a handful of provider exports,
  # so a normal link would prune the runtime objects (CommandQueue,
  # Presenter, CreateDXMT9Device, …). The test binaries link against the
  # dylib for those symbols, so we force-load the static libs to keep
  # them visible.
  link_whole: [
    dxmt9_frontend_bridge_lib,
    dxmt9_frontend_core_lib,
    dxmt9_frontend_provider_lib,
    dxmt9_runtime_lib,
    dxmt9_shader_service_core_lib,
    dxmt9_provider_service_abi_lib,
  ],
  dependencies: dxmt9_deps + [
    dxmt9_util_dep,
    dxmt9_winemetal_dep,
  ],
  install: false,
)

dxmt9_dep = declare_dependency(
  include_directories: dxmt9_inc,
  link_with: dxmt9_native_lib,
  dependencies: [dxmt9_util_dep],
)

meson.override_dependency('dxmt9', dxmt9_dep)
