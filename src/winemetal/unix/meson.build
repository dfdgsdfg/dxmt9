if host_machine.system() != 'windows'
  subdir_done()
endif

dxmt9_winemetal_unix_runtime_subdir = build_machine.cpu_family() == 'x86_64' ? 'x86_64-unix' : build_machine.cpu_family() + '-unix'
dxmt9_winemetal_unix_install_dir = get_option('libdir') / 'wine' / dxmt9_winemetal_unix_runtime_subdir

shared_library(
  'winemetal',
  'winemetal_unix.cpp',
  name_prefix: '',
  name_suffix: 'so',
  include_directories: [dxmt9_inc, include_directories('..', '../..')],
  cpp_args: ['-DWINE_UNIX_LIB=1'],
  native: true,
  build_rpath: '@loader_path/:@loader_path/../../',
  install_rpath: '',
  override_options: { 'b_asneeded' : false },
  install: true,
  install_dir: dxmt9_winemetal_unix_install_dir,
)
