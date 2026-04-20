if host_machine.system() == 'windows'
  subdir_done()
endif

dxmt9_winemetal_unix_runtime_subdir = host_machine.cpu_family() == 'x86_64' ? 'x86_64-unix' : host_machine.cpu_family() + '-unix'
dxmt9_winemetal_unix_common_link_args = []
dxmt9_winemetal_unix_override_options = {}

wine_build_path = get_option('wine_build_path')
wine_install_path = get_option('wine_install_path')

if host_machine.system() == 'darwin'
  c_args = get_option('c_args')
  if c_args.length() >= 2
    foreach i : range(c_args.length() - 1)
      if c_args[i] == '-arch'
        arch_name = c_args[i + 1]
        if arch_name == 'x86_64'
          dxmt9_winemetal_unix_runtime_subdir = 'x86_64-unix'
        elif arch_name == 'arm64'
          dxmt9_winemetal_unix_runtime_subdir = 'aarch64-unix'
        endif
      endif
    endforeach
  endif

  if wine_build_path != ''
    if not wine_build_path.startswith('/')
      wine_build_path = join_paths(meson.project_source_root(), wine_build_path)
    endif
    dxmt9_winemetal_unix_common_link_args += [
      join_paths(wine_build_path, 'dlls/winemac.drv/winemac.so'),
      join_paths(wine_build_path, 'dlls/ntdll/ntdll.so'),
    ]
    dxmt9_winemetal_unix_override_options = { 'b_asneeded' : false }
  elif wine_install_path != ''
    if not wine_install_path.startswith('/')
      wine_install_path = join_paths(meson.project_source_root(), wine_install_path)
    endif
    dxmt9_winemetal_unix_common_link_args += [
      join_paths(wine_install_path, 'lib/wine', dxmt9_winemetal_unix_runtime_subdir, 'winemac.so'),
      join_paths(wine_install_path, 'lib/wine', dxmt9_winemetal_unix_runtime_subdir, 'ntdll.so'),
    ]
    dxmt9_winemetal_unix_override_options = { 'b_asneeded' : false }
  endif
endif

dxmt9_winemetal_unix_install_dir = get_option('libdir') / 'wine' / dxmt9_winemetal_unix_runtime_subdir
dxmt9_winemetal_unix_framework_dep = dependency('appleframeworks', modules: [
  'AppKit',
  'Foundation',
  'Metal',
  'QuartzCore',
])

dxmt9_winemetal_unix_module = shared_library(
  'winemetal',
  files(
    'winemetal_unix.cpp',
    'winemetal_private_api.mm',
  ),
  name_prefix: '',
  name_suffix: 'so',
  include_directories: [
    dxmt9_inc,
    dxmt9_winemetal_internal_inc,
    include_directories('../..'),
  ],
  dependencies: [
    dxmt9_util_dep,
    dxmt9_winemetal_unix_framework_dep,
  ],
  cpp_args: ['-DWINE_UNIX_LIB=1'],
  link_args: ['-Wl,-install_name,@rpath/winemetal.so'] + dxmt9_winemetal_unix_common_link_args,
  build_rpath: '@loader_path/:@loader_path/../../',
  install_rpath: '',
  override_options: dxmt9_winemetal_unix_override_options,
  install: true,
  install_dir: dxmt9_winemetal_unix_install_dir,
)
