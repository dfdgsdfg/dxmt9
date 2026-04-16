dxmt9_util_srcs = files(
  'config/config.cpp',
  'dynamic_symbol.cpp',
  'log/log.cpp',
  'thread.cpp',
  'util_bmp.cpp',
  'util_buffer.cpp',
  'util_string.cpp',
)

if host_machine.system() == 'windows'
  dxmt9_util_srcs += files(
    'com/com_guid.cpp',
    'com/com_private_data.cpp',
  )
endif

dxmt9_util_internal_inc = include_directories('.', '..')

dxmt9_util_lib = static_library(
  'dxmt9_util',
  dxmt9_util_srcs,
  include_directories: [dxmt9_inc, dxmt9_util_internal_inc],
  install: false,
)

dxmt9_util_dep = declare_dependency(
  link_with: dxmt9_util_lib,
  include_directories: [dxmt9_inc, dxmt9_util_internal_inc],
)
