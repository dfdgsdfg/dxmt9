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

# R-BACK-13.* — Tile-shader FFP selector + MSL emit specs.
tile_ffp_selector_spec = executable(
  'dxmt9-tile-ffp-selector-spec',
  'tile_ffp_selector_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-tile-ffp-selector-spec', tile_ffp_selector_spec)

tile_ffp_msl_spec = executable(
  'dxmt9-tile-ffp-msl-spec',
  'tile_ffp_msl_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-tile-ffp-msl-spec', tile_ffp_msl_spec)

# Z1 audit item #2 — unix-side chunk-injection probe.
# Constructs a wire-format chunk in memory and feeds it directly into
# dxmt9c_device_commit_chunk(), bypassing PE D3D9 + the WINE_UNIX_CALL
# bridge. Isolates B3 (CommandQueue) + B4 (encode thread) from B1 (PE
# recorder) and B2 (PE→unix bridge). Soft-assertion only; real numbers
# scrape from the cumulative `[dxmt9-perf]` line emitted at exit when
# DXMT_PERF_COUNTERS=1.
#
# NOT registered as a meson test() yet — `commit_chunk` from the
# native context trips an ASan halt-on-error in the current build
# (probe loops succeed without ASan; meson runs default to
# halt_on_error=1, which surfaces a Metal-side issue that needs
# triage). The binary still builds; run manually with:
#   DYLD_LIBRARY_PATH=...  \
#   DXMT_PERF_COUNTERS=1 UNIX_CHUNK_INJECT_ITERATIONS=N \
#   ./build-x86_64-builtin/tests/native/backend/dxmt9-unix-chunk-injection-probe
# until the ASan finding is debugged. The probe is the sole consumer
# of dxmt9c_device_commit_chunk from a native (non-Wine) main(), so
# its failure pattern is itself a useful triage handle for the queue
# code that currently runs only via the bridge.
unix_chunk_injection_probe = executable(
  'dxmt9-unix-chunk-injection-probe',
  'dxmt9_unix_chunk_injection_probe.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep, dxmt9_winemetal_dep],
  link_args: dxmt9_test_link_args,
)
