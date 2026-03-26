dxmt9_lib = static_library(
  'dxmt9',
  'dxmt9.cpp',
  include_directories: dxmt9_inc,
  install: true,
)

dxmt9_dep = declare_dependency(
  include_directories: dxmt9_inc,
  link_with: dxmt9_lib,
)

meson.override_dependency('dxmt9', dxmt9_dep)

