# Backend descriptor / pipeline / replay / hazard / capture / counter / render-pass specs.

backend_key_descriptor_spec = executable(
  'dxmt9-backend-key-descriptor-spec',
  'backend_key_descriptor_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-backend-key-descriptor-spec', backend_key_descriptor_spec,
     env: dxmt9_native_test_env)

backend_pipeline_key_spec = executable(
  'dxmt9-backend-pipeline-key-spec',
  'backend_pipeline_key_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-backend-pipeline-key-spec', backend_pipeline_key_spec)

dod_replay_observer_spec = executable(
  'dxmt9-dod-replay-observer-spec',
  'dod_replay_observer_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-dod-replay-observer-spec', dod_replay_observer_spec)

resource_hazard_spec = executable(
  'dxmt9-resource-hazard-spec',
  'resource_hazard_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-resource-hazard-spec', resource_hazard_spec)

render_pass_actions_spec = executable(
  'dxmt9-render-pass-actions-spec',
  'render_pass_actions_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-render-pass-actions-spec', render_pass_actions_spec,
     env: dxmt9_native_test_env_vars)

metalcapture_spec = executable(
  'dxmt9-metalcapture-spec',
  'metalcapture_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-metalcapture-spec', metalcapture_spec)

allocation_counter_spec = executable(
  'dxmt9-allocation-counter-spec',
  'allocation_counter_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-allocation-counter-spec', assert_perf_counters,
     args: [allocation_counter_spec])

# R-BACK-5.9 / R-BACK-5.10 / R-BACK-14.* — MTLHeap small-resource pooling.
# Pure-CPU classify + lifetime test (no Metal device required); deeper
# allocation behavior is covered by the runtime integration suite.
# Includes dxmt9_winemetal_dep so the WMT::Reference dtor in
# HeapManager::Family resolves against winemetal's NSObject_release —
# the test only constructs default-initialized references (handle ==
# 0) so no actual release call ever executes.
heap_pooling_spec = executable(
  'dxmt9-heap-pooling-spec',
  'heap_pooling_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep, dxmt9_winemetal_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-heap-pooling-spec', heap_pooling_spec)
