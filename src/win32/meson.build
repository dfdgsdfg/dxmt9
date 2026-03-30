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
  'bridge.cpp',
  'factory.cpp',
  'device.cpp',
)

cpp_compiler = meson.get_compiler('cpp')
dxguid_dep = cpp_compiler.find_library('dxguid', required: true)

d3d9_bridge_install_dir = get_option('prefix') / 'wine' / (host_machine.cpu_family() == 'x86_64' ? 'x86_64-windows' : host_machine.cpu_family() + '-windows')

dxmt9_bridge_dll = shared_library(
  'dxmt9',
  d3d9_bridge_srcs + [bridge_codegen[1]],
  name_prefix: '',
  name_suffix: 'dll',
  vs_module_defs: files('dxmt9.def')[0],
  include_directories: [dxmt9_inc, bridge_gen_inc],
  dependencies: [dxguid_dep],
  install: true,
  install_dir: d3d9_bridge_install_dir,
)

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
