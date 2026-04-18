# src/win32/meson.build — builds the thin Win32 PE d3d9.dll forwarding layer.

if host_machine.system() != 'windows'
  subdir_done()
endif

d3d9_srcs = files('d3d9_entry.cpp')

d3d9_dll = shared_library(
  'd3d9',
  d3d9_srcs,
  name_prefix:         '',
  name_suffix:         'dll',
  vs_module_defs:      files('d3d9.def')[0],
  include_directories: [dxmt9_inc, bridge_gen_inc],
  dependencies: [dxmt9_d3d9_entry_shim_dep, dxmt9_winemetal_import_dep],
  install:      true,
  install_dir:  get_option('prefix') / 'windows',
)
