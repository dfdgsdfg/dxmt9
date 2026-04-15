dxmt9_winemetal_srcs = files(
  'winemetal_bridge.cpp',
)

dxmt9_winemetal_internal_inc = include_directories('.', '..')

dxmt9_winemetal_lib = static_library(
  'dxmt9_winemetal',
  dxmt9_winemetal_srcs,
  include_directories: [dxmt9_inc, dxmt9_winemetal_internal_inc],
  dependencies: [dxmt9_util_dep],
  install: false,
)

dxmt9_winemetal_dep = declare_dependency(
  link_with: dxmt9_winemetal_lib,
  include_directories: [dxmt9_inc, dxmt9_winemetal_internal_inc],
  dependencies: [dxmt9_util_dep],
)
