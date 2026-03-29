dxmt9_srcs = [
  'backend.cpp',
  'com.cpp',
  'core.cpp',
  'dxmt9.cpp',
  'winemetal_bridge.cpp',
]

dxmt9_deps = []

if host_machine.system() == 'darwin'
  dxmt9_srcs += ['backend_metal.mm']
  dxmt9_deps += dependency('appleframeworks', modules: [
    'AppKit',
    'Foundation',
    'Metal',
    'QuartzCore',
  ])
endif

dxmt9_lib = static_library(
  'dxmt9',
  dxmt9_srcs,
  include_directories: dxmt9_inc,
  dependencies: dxmt9_deps,
  install: true,
)

dxmt9_dep = declare_dependency(
  include_directories: dxmt9_inc,
  link_with: dxmt9_lib,
)

meson.override_dependency('dxmt9', dxmt9_dep)
