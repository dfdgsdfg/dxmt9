# src/win32/meson.build — builds the thin Win32 PE d3d9.dll forwarding layer.

if host_machine.system() != 'windows'
  subdir_done()
endif

d3d9_srcs = files('d3d9_entry.cpp')
d3d9_install_dir = get_option('prefix') / 'windows'

if get_option('wine_builtin_dll')
  d3d9_runtime_subdir = 'x86_64-windows'
  if host_machine.cpu_family() == 'x86'
    d3d9_runtime_subdir = 'i386-windows'
  elif host_machine.cpu_family() != 'x86_64'
    d3d9_runtime_subdir = host_machine.cpu_family() + '-windows'
  endif
  d3d9_install_dir = get_option('prefix') / 'wine' / d3d9_runtime_subdir
endif

d3d9_dll = shared_library(
  'd3d9',
  d3d9_srcs,
  name_prefix:         '',
  name_suffix:         'dll',
  vs_module_defs:      files('d3d9.def')[0],
  include_directories: [dxmt9_inc, bridge_gen_inc],
  dependencies: [dxmt9_d3d9_forwarder_dep],
  install:      true,
  install_dir:  d3d9_install_dir,
)

if get_option('wine_builtin_dll')
  if not is_variable('dxmt9_winebuild')
    error('wine_builtin_dll=true requires src/winemetal to resolve winebuild first')
  endif

  custom_target(
    'd3d9_builtin_postprocess',
    input: d3d9_dll,
    output: 'd3d9.dll.postproc',
    command: [dxmt9_winebuild, '--builtin', '@INPUT@'],
    depends: d3d9_dll,
    build_by_default: true,
  )
endif
