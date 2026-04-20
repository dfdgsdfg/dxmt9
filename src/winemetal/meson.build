dxmt9_winemetal_internal_inc = include_directories('.', '..', '../dxmt9')

dxmt9_provider_service_abi_lib = static_library(
  'dxmt9_provider_service_abi',
  dxmt9_provider_service_abi_srcs,
  include_directories: [
    dxmt9_inc,
    dxmt9_winemetal_internal_inc,
    dxmt9_runtime_internal_inc,
    include_directories('..'),
  ],
  dependencies: [
    dxmt9_util_dep,
  ],
  install: false,
)

dxmt9_provider_service_abi_dep = declare_dependency(
  link_with: dxmt9_provider_service_abi_lib,
  include_directories: [
    dxmt9_inc,
    dxmt9_winemetal_internal_inc,
  ],
  dependencies: [dxmt9_util_dep],
)

subdir('unix')

dxmt9_winemetal_dep = declare_dependency(
  link_with: dxmt9_winemetal_unix_module,
  include_directories: [dxmt9_inc, dxmt9_winemetal_internal_inc],
  dependencies: [
    dxmt9_util_dep,
  ],
)
