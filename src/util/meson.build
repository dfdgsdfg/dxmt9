dxmt9_util_srcs = files(
  'runtime.cpp',
)

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
