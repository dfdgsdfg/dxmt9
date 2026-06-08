# Modern-renderer module specs — backend interface contract, traditional
# (chunk-replay) backend, and framegraph backend. The render module .cpp files
# are compiled into the dxmt9 runtime (src/dxmt9/meson.build) so these specs
# resolve render::createBackend / TraditionalBackend / FrameGraphBackend
# transitively through dxmt9_dep.

backend_interface_spec = executable(
  'dxmt9-render-backend-interface-spec',
  'backend_interface_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-render-backend-interface-spec', backend_interface_spec,
     env: dxmt9_native_test_env)

traditional_backend_spec = executable(
  'dxmt9-render-traditional-backend-spec',
  'traditional_backend_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-render-traditional-backend-spec', traditional_backend_spec,
     env: dxmt9_native_test_env)

framegraph_backend_spec = executable(
  'dxmt9-render-framegraph-backend-spec',
  'framegraph_backend_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-render-framegraph-backend-spec', framegraph_backend_spec,
     env: dxmt9_native_test_env)

parity_harness_spec = executable(
  'dxmt9-render-parity-harness-spec',
  'parity_harness_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-render-parity-harness-spec', parity_harness_spec,
     env: dxmt9_native_test_env)

divergence_spec = executable(
  'dxmt9-render-divergence-spec',
  'divergence_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-render-divergence-spec', divergence_spec,
     env: dxmt9_native_test_env)
