dxmt9_util_com_core_srcs = files(
  'com/com_guid.cpp',
  'com/com_private_data_core.cpp',
  'com/com_private_data_iface.cpp',
)

dxmt9_util_com_leaf_srcs = files()

dxmt9_util_common_srcs = files(
  'config/config.cpp',
  'dynamic_symbol.cpp',
  'log/log.cpp',
  'sha1/sha1.c',
  'sha1/sha1_util.cpp',
  'thread.cpp',
  'util_bmp.cpp',
  'util_buffer.cpp',
  'util_hash.cpp',
  'util_string.cpp',
  'wsi_monitor_headless.cpp',
  'wsi_window_headless.cpp',
)

dxmt9_util_wsi_leaf_srcs = files()

if host_machine.system() == 'darwin'
  dxmt9_util_wsi_leaf_srcs += files('wsi_platform_darwin.cpp')
elif host_machine.system() == 'windows'
  dxmt9_util_wsi_leaf_srcs += files('wsi_platform_win32.cpp')
else
  dxmt9_util_wsi_leaf_srcs += files('wsi_platform_headless.cpp')
endif

dxmt9_util_srcs = dxmt9_util_com_core_srcs + dxmt9_util_com_leaf_srcs + dxmt9_util_common_srcs + dxmt9_util_wsi_leaf_srcs

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
