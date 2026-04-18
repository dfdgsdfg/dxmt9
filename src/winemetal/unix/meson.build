if host_machine.system() == 'windows'
  subdir_done()
endif

dxmt9_winemetal_unix_runtime_subdir = host_machine.cpu_family() == 'x86_64' ? 'x86_64-unix' : host_machine.cpu_family() + '-unix'
dxmt9_winemetal_unix_install_dir = get_option('libdir') / 'wine' / dxmt9_winemetal_unix_runtime_subdir

dxmt9_winemetal_unix_module = shared_library(
  'winemetal',
  files(
    '../winemetal_public_api.cpp',
    '../winemetal_unix_call_handlers.cpp',
    'winemetal_unix.cpp',
  ),
  name_prefix: '',
  name_suffix: 'so',
  include_directories: [dxmt9_inc, bridge_gen_inc, include_directories('..', '../..')],
  dependencies: [
    dxmt9_util_dep,
    dxmt9_winemetal_service_provider_dep,
    dependency('appleframeworks', modules: [
      'AppKit',
      'Foundation',
      'Metal',
      'QuartzCore',
    ]),
  ],
  cpp_args: ['-DWINE_UNIX_LIB=1'],
  build_rpath: '@loader_path/:@loader_path/../../',
  install_rpath: '',
  override_options: { 'b_asneeded' : false },
  install: true,
  install_dir: dxmt9_winemetal_unix_install_dir,
)
