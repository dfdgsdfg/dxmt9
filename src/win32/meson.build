# src/win32/meson.build — builds the Win32 PE d3d9.dll and dxmt9.dll bridge.
#
# Invoked when cross-compiling for Windows:
#   meson setup build-win32 --cross-file cross/aarch64-windows.ini
#   meson compile -C build-win32
#
# Prerequisites: llvm-mingw on PATH (provides aarch64-w64-mingw32-* tools).

if host_machine.system() != 'windows'
  subdir_done()
endif

d3d9_bridge_srcs = files(
  'main.c',
  'bridge.cpp',
  'factory.cpp',
  'device.cpp',
)

c_compiler = meson.get_compiler('c')
cpp_compiler = meson.get_compiler('cpp')
dxguid_dep = cpp_compiler.find_library('dxguid', required: true)

d3d9_bridge_runtime_subdir = 'x86_64-windows'
if host_machine.cpu_family() == 'x86'
  d3d9_bridge_runtime_subdir = 'i386-windows'
elif host_machine.cpu_family() != 'x86_64'
  d3d9_bridge_runtime_subdir = host_machine.cpu_family() + '-windows'
endif
d3d9_bridge_install_dir = get_option('prefix') / 'wine' / d3d9_bridge_runtime_subdir

dxmt9_bridge_deps = [dxguid_dep]
dxmt9_bridge_c_args = []
dxmt9_bridge_cpp_args = []

wine_builtin_dll = get_option('wine_builtin_dll')
wine_build_path = get_option('wine_build_path')
wine_install_path = get_option('wine_install_path')

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
    lib_winecrt0 = c_compiler.find_library('winecrt0', dirs: [
      join_paths(wine_build_path, 'libs/winecrt0', d3d9_bridge_runtime_subdir),
      join_paths(wine_build_path, 'dlls/winecrt0', d3d9_bridge_runtime_subdir),
    ], required: true)
    lib_ntdll = c_compiler.find_library('ntdll', dirs: [
      join_paths(wine_build_path, 'dlls/ntdll', d3d9_bridge_runtime_subdir),
    ], static: true, required: true)
    lib_dbghelp = c_compiler.find_library('dbghelp', dirs: [
      join_paths(wine_build_path, 'dlls/dbghelp', d3d9_bridge_runtime_subdir),
    ], required: true)
    winebuild = join_paths(wine_build_path, 'tools/winebuild/winebuild')
  else
    lib_winecrt0 = c_compiler.find_library('winecrt0', dirs: [
      join_paths(wine_install_path, 'lib/wine', d3d9_bridge_runtime_subdir),
    ], required: true)
    lib_ntdll = c_compiler.find_library('ntdll', dirs: [
      join_paths(wine_install_path, 'lib/wine', d3d9_bridge_runtime_subdir),
    ], static: true, required: true)
    lib_dbghelp = c_compiler.find_library('dbghelp', dirs: [
      join_paths(wine_install_path, 'lib/wine', d3d9_bridge_runtime_subdir),
    ], required: true)
    winebuild = join_paths(wine_install_path, 'bin/winebuild')
  endif

  dxmt9_bridge_deps += [lib_winecrt0, lib_ntdll, lib_dbghelp]
  dxmt9_bridge_c_args += ['-DDXMT9_WINE_BUILTIN_DLL=1']
  dxmt9_bridge_cpp_args += ['-DDXMT9_WINE_BUILTIN_DLL=1']
endif

dxmt9_bridge_dll = shared_library(
  'dxmt9',
  d3d9_bridge_srcs + [bridge_codegen[1]],
  name_prefix: '',
  name_suffix: 'dll',
  vs_module_defs: files('dxmt9.def')[0],
  include_directories: [dxmt9_inc, bridge_gen_inc],
  dependencies: dxmt9_bridge_deps,
  c_args: dxmt9_bridge_c_args,
  cpp_args: dxmt9_bridge_cpp_args,
  install: true,
  install_dir: d3d9_bridge_install_dir,
)

if wine_builtin_dll
  custom_target(
    'dxmt9_builtin_postprocess',
    input: dxmt9_bridge_dll,
    output: 'dxmt9.dll.postproc',
    command: [winebuild, '--builtin', '@INPUT@'],
    depends: dxmt9_bridge_dll,
    build_by_default: true,
  )
endif

d3d9_srcs = files('d3d9_entry.cpp')

d3d9_dll = shared_library(
  'd3d9',
  d3d9_srcs,
  name_prefix:         '',
  name_suffix:         'dll',
  vs_module_defs:      files('d3d9.def')[0],
  include_directories: [dxmt9_inc, bridge_gen_inc],
  dependencies: [dxguid_dep],
  link_with: [dxmt9_bridge_dll],
  install:      true,
  install_dir:  get_option('prefix') / 'windows',
)
