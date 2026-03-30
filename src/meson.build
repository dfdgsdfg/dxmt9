dxmt9_native_srcs = [
  'backend.cpp',
  'com.cpp',
  'core.cpp',
  'device_c.cpp',
  'dxmt9.cpp',
  'winemetal_bridge.cpp',
]

dxmt9_deps = []

if host_machine.system() == 'darwin'
  dxmt9_native_srcs += ['backend_metal.mm']
  dxmt9_deps += dependency('appleframeworks', modules: [
    'AppKit',
    'Foundation',
    'Metal',
    'QuartzCore',
  ])
endif

dxmt9_native_lib = shared_library(
  'dxmt9_native',
  dxmt9_native_srcs,
  include_directories: [dxmt9_inc, bridge_gen_inc],
  dependencies: dxmt9_deps,
  install: false,
)

dxmt9_dep = declare_dependency(
  include_directories: dxmt9_inc,
  link_with: dxmt9_native_lib,
)

meson.override_dependency('dxmt9', dxmt9_dep)

dxmt9_unix_install_dir = get_option('libdir') / 'wine' / (host_machine.cpu_family() == 'x86_64' ? 'x86_64-unix' : host_machine.cpu_family() + '-unix')

dxmt9_unix_module = shared_library(
  'dxmt9',
  dxmt9_native_srcs + [bridge_codegen[2]],
  include_directories: [dxmt9_inc, bridge_gen_inc],
  dependencies: dxmt9_deps,
  name_prefix: '',
  name_suffix: 'so',
  build_rpath: '@loader_path/:@loader_path/../../',
  install_rpath: '',
  install_dir: dxmt9_unix_install_dir,
  install: true,
)
