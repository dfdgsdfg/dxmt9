dxmt9_winemetal_internal_inc = include_directories('.', '..', '../dxmt9')

dxmt9_winemetal_api_srcs = files(
  'winemetal_dispatch.cpp',
)

if host_machine.system() == 'windows'
  dxmt9_winemetal_api_srcs += files('winemetal_thunks.cpp')
else
  dxmt9_winemetal_api_srcs += files('unix/winemetal_unix.cpp')
endif

dxmt9_winemetal_lib = static_library(
  'dxmt9_winemetal',
  dxmt9_winemetal_api_srcs,
  include_directories: [dxmt9_inc, dxmt9_winemetal_internal_inc],
  dependencies: [dxmt9_util_dep],
  install: false,
)

dxmt9_winemetal_dep = declare_dependency(
  link_with: dxmt9_winemetal_lib,
  include_directories: [dxmt9_inc, dxmt9_winemetal_internal_inc],
  dependencies: [dxmt9_util_dep],
)

if host_machine.system() != 'windows'
  subdir_done()
endif

cc = meson.get_compiler('c')
wine_builtin_dll = get_option('wine_builtin_dll')
wine_build_path = get_option('wine_build_path')
wine_install_path = get_option('wine_install_path')

winemetal_runtime_subdir = 'x86_64-windows'
if host_machine.cpu_family() == 'x86'
  winemetal_runtime_subdir = 'i386-windows'
elif host_machine.cpu_family() != 'x86_64'
  winemetal_runtime_subdir = host_machine.cpu_family() + '-windows'
endif

winemetal_install_dir = get_option('prefix') / 'wine' / winemetal_runtime_subdir

winemetal_bridge_deps = []
winemetal_bridge_c_args = []
winemetal_bridge_cpp_args = []

if not wine_builtin_dll
  subdir_done()
endif

if wine_builtin_dll
  if wine_build_path == '' and wine_install_path == ''
    error('wine_builtin_dll=true requires -Dwine_build_path or -Dwine_install_path')
  endif

  if wine_build_path != '' and not wine_build_path.startswith('/')
    wine_build_path = join_paths(meson.project_source_root(), wine_build_path)
  endif
  if wine_install_path != '' and not wine_install_path.startswith('/')
    wine_install_path = join_paths(meson.project_source_root(), wine_install_path)
  endif

  if wine_build_path != ''
    lib_winecrt0 = cc.find_library('winecrt0', dirs: [
      join_paths(wine_build_path, 'libs/winecrt0', winemetal_runtime_subdir),
      join_paths(wine_build_path, 'dlls/winecrt0', winemetal_runtime_subdir),
    ], required: true)
    lib_ntdll = cc.find_library('ntdll', dirs: [
      join_paths(wine_build_path, 'dlls/ntdll', winemetal_runtime_subdir),
    ], static: true, required: true)
    lib_dbghelp = cc.find_library('dbghelp', dirs: [
      join_paths(wine_build_path, 'dlls/dbghelp', winemetal_runtime_subdir),
    ], required: true)
    winebuild = join_paths(wine_build_path, 'tools/winebuild/winebuild')
  else
    lib_winecrt0 = cc.find_library('winecrt0', dirs: [
      join_paths(wine_install_path, 'lib/wine', winemetal_runtime_subdir),
    ], required: true)
    lib_ntdll = cc.find_library('ntdll', dirs: [
      join_paths(wine_install_path, 'lib/wine', winemetal_runtime_subdir),
    ], static: true, required: true)
    lib_dbghelp = cc.find_library('dbghelp', dirs: [
      join_paths(wine_install_path, 'lib/wine', winemetal_runtime_subdir),
    ], required: true)
    winebuild = join_paths(wine_install_path, 'bin/winebuild')
  endif

  winemetal_bridge_deps += [lib_winecrt0, lib_ntdll, lib_dbghelp]
  winemetal_bridge_c_args += ['-DDXMT9_WINE_BUILTIN_DLL=1']
  winemetal_bridge_cpp_args += ['-DDXMT9_WINE_BUILTIN_DLL=1']
endif

winemetal_dll = shared_library(
  'winemetal',
  files('main.c', 'winemetal_dispatch.cpp', 'winemetal_thunks.cpp'),
  name_prefix: '',
  name_suffix: 'dll',
  vs_module_defs: files('winemetal.def')[0],
  include_directories: [dxmt9_inc, dxmt9_winemetal_internal_inc],
  dependencies: winemetal_bridge_deps + [dxmt9_util_dep],
  c_args: winemetal_bridge_c_args,
  cpp_args: winemetal_bridge_cpp_args,
  install: true,
  install_dir: winemetal_install_dir,
)

custom_target(
  'winemetal_builtin_postprocess',
  input: winemetal_dll,
  output: 'winemetal.dll.postproc',
  command: [winebuild, '--builtin', '@INPUT@'],
  depends: winemetal_dll,
  build_by_default: true,
)

subdir('unix')
