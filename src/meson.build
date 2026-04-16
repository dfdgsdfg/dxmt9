subdir('util')
subdir('d3d9')
subdir('dxmt9')
subdir('winemetal')
dxmt9_anchor_srcs = files('dxmt9/dxmt9.cpp')

dxmt9_deps = []
dxmt9_unix_link_args = [
  '-Wl,-install_name,@rpath/dxmt9.so',
]
dxmt9_unix_override_options = {}
dxmt9_unix_runtime_subdir = host_machine.cpu_family() == 'x86_64' ? 'x86_64-unix' : host_machine.cpu_family() + '-unix'

wine_build_path = get_option('wine_build_path')
wine_install_path = get_option('wine_install_path')

if host_machine.system() == 'darwin'
  c_args = get_option('c_args')
  if c_args.length() >= 2
    foreach i : range(c_args.length() - 1)
      if c_args[i] == '-arch'
        arch_name = c_args[i + 1]
        if arch_name == 'x86_64'
          dxmt9_unix_runtime_subdir = 'x86_64-unix'
        elif arch_name == 'arm64'
          dxmt9_unix_runtime_subdir = 'aarch64-unix'
        endif
      endif
    endforeach
  endif
endif

if host_machine.system() == 'darwin'
  dxmt9_deps += dependency('appleframeworks', modules: [
    'AppKit',
    'Foundation',
    'Metal',
    'QuartzCore',
  ])

  if wine_build_path != ''
    if not wine_build_path.startswith('/')
      wine_build_path = join_paths(meson.project_source_root(), wine_build_path)
    endif
    dxmt9_unix_link_args += [
      join_paths(wine_build_path, 'dlls/winemac.drv/winemac.so'),
      join_paths(wine_build_path, 'dlls/ntdll/ntdll.so'),
    ]
    dxmt9_unix_override_options = { 'b_asneeded' : false }
  elif wine_install_path != ''
    if not wine_install_path.startswith('/')
      wine_install_path = join_paths(meson.project_source_root(), wine_install_path)
    endif
    dxmt9_unix_link_args += [
      join_paths(wine_install_path, 'lib/wine', dxmt9_unix_runtime_subdir, 'winemac.so'),
      join_paths(wine_install_path, 'lib/wine', dxmt9_unix_runtime_subdir, 'ntdll.so'),
    ]
    dxmt9_unix_override_options = { 'b_asneeded' : false }
  endif
endif

dxmt9_native_lib = shared_library(
  'dxmt9_native',
  dxmt9_anchor_srcs,
  include_directories: [dxmt9_inc, bridge_gen_inc],
  dependencies: dxmt9_deps,
  link_whole: [dxmt9_util_lib, dxmt9_frontend_lib, dxmt9_shader_service_lib, dxmt9_winemetal_lib, dxmt9_runtime_lib],
  install: false,
)

dxmt9_dep = declare_dependency(
  include_directories: dxmt9_inc,
  link_with: dxmt9_native_lib,
)

meson.override_dependency('dxmt9', dxmt9_dep)

dxmt9_unix_install_dir = get_option('libdir') / 'wine' / dxmt9_unix_runtime_subdir

dxmt9_unix_module = shared_library(
  'dxmt9',
  dxmt9_anchor_srcs + [bridge_codegen[2]],
  include_directories: [dxmt9_inc, bridge_gen_inc],
  dependencies: dxmt9_deps,
  link_whole: [dxmt9_util_lib, dxmt9_frontend_lib, dxmt9_shader_service_lib, dxmt9_winemetal_lib, dxmt9_runtime_lib],
  link_args: dxmt9_unix_link_args,
  name_prefix: '',
  name_suffix: 'so',
  build_rpath: '@loader_path/:@loader_path/../../',
  install_rpath: '',
  override_options: dxmt9_unix_override_options,
  install_dir: dxmt9_unix_install_dir,
  install: true,
)
