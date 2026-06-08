# Frame Graph (modern-renderer) module specs. The framegraph DAG structures
# (src/dxmt9/framegraph/fg_dag.hpp) are header-only at L1, so these specs only
# need the dxmt9 include path; dxmt9_dep is linked for parity with the other
# native specs and for any future framegraph .cpp logic.

fg_dag_spec = executable(
  'dxmt9-framegraph-fg-dag-spec',
  'fg_dag_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-framegraph-fg-dag-spec', fg_dag_spec,
     env: dxmt9_native_test_env)

fg_builder_spec = executable(
  'dxmt9-framegraph-fg-builder-spec',
  'fg_builder_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-framegraph-fg-builder-spec', fg_builder_spec,
     env: dxmt9_native_test_env)

fg_debug_export_spec = executable(
  'dxmt9-framegraph-fg-debug-export-spec',
  'fg_debug_export_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-framegraph-fg-debug-export-spec', fg_debug_export_spec,
     env: dxmt9_native_test_env)
