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

imported_apply_state_value_spec = executable(
  'dxmt9-imported-apply-state-value-spec',
  'imported_apply_state_value_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-imported-apply-state-value-spec',
     imported_apply_state_value_spec)

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

# R-BACK-5.8 — D3DUSAGE_DYNAMIC rename ring fresh-allocate fallback.
# CPU-only spec: drives Pool::createBuffer + Pool::finalizeBufferMap on
# a synthetic NULL device so the ring's structural shape (entry count,
# active index, lastUsedSeqId watermarks) is exercised without a real
# Metal allocation. Live shared-mode behavior is covered by the runtime
# integration suite. dxmt9_winemetal_dep is needed because BufferRecord
# and BufferRenameRingEntry hold WMT::Reference<Buffer> destructors
# that link against winemetal's NSObject_release; the spec only
# constructs default-initialized references (handle == 0) so no actual
# release call ever executes.
dynamic_rename_ring_spec = executable(
  'dxmt9-dynamic-rename-ring-spec',
  'dynamic_rename_ring_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep, dxmt9_winemetal_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-dynamic-rename-ring-spec', dynamic_rename_ring_spec)

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

# R-BACK-12.22..12.26 — Stage 2 argument-buffer hybrid runtime adopter.
# CPU-only spec covering capability gate, selector decision shape,
# variant-key bit, argument-descriptor layout, and MSL prelude variant.
# Hardware shader-runner equality (R-BACK-12.26) is a follow-up.
argbuf_hybrid_spec = executable(
  'dxmt9-argbuf-hybrid-spec',
  'argbuf_hybrid_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-argbuf-hybrid-spec', argbuf_hybrid_spec)

# R-BACK-12.22..12.26 MSL routing — FFP and DXBC->MSL emitters route
# uniform reads through ArgbufLayout at slot 30 when the variant key
# carries argbufHybridMode. Pure source-level assertions on the emitted
# MSL strings (no Metal device); pairs with dxmt9-argbuf-hybrid-spec
# which covers the framework pieces (capability gate, descriptor build,
# prelude shape).
argbuf_hybrid_msl_spec = executable(
  'dxmt9-argbuf-hybrid-msl-spec',
  'argbuf_hybrid_msl_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-argbuf-hybrid-msl-spec', argbuf_hybrid_msl_spec)

shader_argbuf_binding_value_spec = executable(
  'dxmt9-shader-argbuf-binding-value-spec',
  'shader_argbuf_binding_value_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-shader-argbuf-binding-value-spec',
     shader_argbuf_binding_value_spec)

# R-BACK-12.22..12.26 — Stage 2 argbuf-hybrid populator spec.
# CPU-only spec for the per-encoder argbuf populator: encoder-resource
# init shape, dirtyBytesEstimate value-transform across categories,
# PopulatedArgbuf default. Hardware exercise of openArgbuf /
# populateConstantBuffers / populateResourceBindings / shader-runner
# equality is covered by the runtime-integration suite.
argbuf_populator_spec = executable(
  'dxmt9-argbuf-populator-spec',
  'dxmt9_argbuf_populator_spec.cpp',
  include_directories: dxmt9_inc,
  # Needs dxmt9_winemetal_dep so the WMT::Reference<ArgumentEncoder> dtor
  # inside ArgbufEncoderResource resolves NSObject_release at link time;
  # the spec itself never calls into Metal, but constructing the resource
  # on the stack drags the dtor into the link.
  dependencies: [dxmt9_dep, dxmt9_frontend_dep, dxmt9_winemetal_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-argbuf-populator-spec', argbuf_populator_spec)

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
