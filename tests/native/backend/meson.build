# Backend descriptor / pipeline / replay / hazard / capture / counter / render-pass specs.
query_range_draw_source_spec = executable(
  'dxmt9-query-range-draw-source-spec', 'query_range_draw_source_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep, dxmt9_winemetal_dep],
  link_args: dxmt9_test_link_args,
)
test('dxmt9-query-range-draw-source-spec', query_range_draw_source_spec,
     env: dxmt9_native_test_env)
test('dxmt9-query-range-draw-source-no-dedup-spec', query_range_draw_source_spec,
     env: dxmt9_native_test_env_vars + ['DXMT9_DISABLE_DRAW_UNIFORM_PAYLOAD_DEDUP=1'])
query_range_clear_source_spec = executable(
  'dxmt9-query-range-clear-source-spec', 'query_range_clear_source_spec.cpp',
  include_directories: dxmt9_inc,
)
test('dxmt9-query-range-clear-source-spec', query_range_clear_source_spec,
     env: dxmt9_native_test_env)
query_scope_range_owner_spec = executable(
  'dxmt9-query-scope-range-owner-spec', 'query_scope_range_owner_spec.cpp',
  include_directories: dxmt9_inc,
)
test('dxmt9-query-scope-range-owner-spec', query_scope_range_owner_spec,
     env: dxmt9_native_test_env)
query_scope_range_spec = executable(
  'dxmt9-query-scope-range-spec', 'query_scope_range_spec.cpp',
  include_directories: dxmt9_inc,
)
test('dxmt9-query-scope-range-spec', query_scope_range_spec,
     env: dxmt9_native_test_env)
query_scope_cb_spec = executable(
  'dxmt9-query-scope-cb-spec', 'query_scope_cb_spec.cpp',
  include_directories: dxmt9_inc,
)
test('dxmt9-query-scope-cb-spec', query_scope_cb_spec,
     env: dxmt9_native_test_env)
publication_mark_cache_spec = executable(
  'dxmt9-publication-mark-cache-spec', 'publication_mark_cache_spec.cpp',
  include_directories: dxmt9_inc,
)
test('dxmt9-publication-mark-cache-spec', publication_mark_cache_spec,
     env: dxmt9_native_test_env)
query_covered_pending_spec = executable(
  'dxmt9-query-covered-pending-spec', 'query_covered_pending_spec.cpp',
  include_directories: dxmt9_inc,
)
test('dxmt9-query-covered-pending-spec', query_covered_pending_spec,
     env: dxmt9_native_test_env)
test('dxmt9-query-covered-pending-sync-disabled-spec', query_covered_pending_spec,
     args: ['--expect-disabled'], env: dxmt9_native_test_env_vars + [
       'DXMT9_QUERY_COVERED_PENDING=1', 'DXMT9_ASYNC_QUERY_FENCES=0'])
query_backend_observer_spec = executable(
  'dxmt9-query-backend-observer-spec', 'query_backend_observer_spec.cpp',
  include_directories: dxmt9_inc, dependencies: dxmt9_util_dep,
)
test('dxmt9-query-backend-observer-spec', query_backend_observer_spec,
     env: dxmt9_native_test_env)

query_receipt_observer_spec = executable(
  'dxmt9-query-receipt-observer-spec', 'query_receipt_observer_spec.cpp',
  include_directories: dxmt9_inc, dependencies: dxmt9_util_dep,
)
test('dxmt9-query-receipt-observer-spec', query_receipt_observer_spec,
     env: dxmt9_native_test_env)

query_poll_receipt_bridge_spec = executable(
  'dxmt9-query-poll-receipt-bridge-spec', 'query_poll_receipt_bridge_spec.cpp',
  '../../../src/d3d9/d3d9_pe_chunk_builder.cpp',
  '../../../src/d3d9/d3d9_pe_chunk_nondraw.cpp',
  include_directories: [dxmt9_inc, include_directories('../../../src'),
                        include_directories('../../../src/d3d9')],
  dependencies: [dxmt9_dep, dxmt9_frontend_dep, dxmt9_winemetal_dep],
  link_args: dxmt9_test_link_args,
)
foreach offload : ['0', '1']
  test('dxmt9-query-poll-receipt-bridge-' + offload + '-spec',
    query_poll_receipt_bridge_spec,
    env: dxmt9_native_test_env_vars + [
      'DXMT9_OFFLOAD_COMMIT_REPLAY=' + offload,
      'DXMT_DEBUG_DISABLE_SHADER_ARCHIVE=1'], timeout: 30)
endforeach

test('dxmt9-query-poll-receipt-timeline-spec', query_poll_receipt_bridge_spec,
     env: dxmt9_native_test_env_vars + ['DXMT9_OFFLOAD_COMMIT_REPLAY=1',
       'DXMT9_QUERY_OBSERVER=1', 'DXMT9_QUERY_OBSERVER_END_STRIDE=1',
       'DXMT_DEBUG_DISABLE_SHADER_ARCHIVE=1'], timeout: 30)

query_replay_receipt_spec = executable(
  'dxmt9-query-replay-receipt-spec', 'query_replay_receipt_spec.cpp',
  include_directories: dxmt9_inc,
)
test('dxmt9-query-replay-receipt-spec', query_replay_receipt_spec,
     env: dxmt9_native_test_env)

query_replay_drain_observer_spec = executable(
  'dxmt9-query-replay-drain-observer-spec', 'query_replay_drain_observer_spec.cpp',
  include_directories: dxmt9_inc, dependencies: dxmt9_util_dep,
)
test('dxmt9-query-replay-drain-observer-spec', query_replay_drain_observer_spec,
     env: dxmt9_native_test_env)

query_bridge_poll_observer_spec = executable(
  'dxmt9-query-bridge-poll-observer-spec', 'query_bridge_poll_observer_spec.cpp',
  include_directories: dxmt9_inc, dependencies: dxmt9_util_dep,
)
test('dxmt9-query-bridge-poll-observer-spec', query_bridge_poll_observer_spec,
     env: dxmt9_native_test_env)

query_timeline_observer_spec = executable(
  'dxmt9-query-timeline-observer-spec', 'query_timeline_observer_spec.cpp',
  include_directories: dxmt9_inc, dependencies: dxmt9_util_dep,
)
test('dxmt9-query-timeline-observer-spec', query_timeline_observer_spec,
     env: dxmt9_native_test_env)

query_serialized_range_metal_spec = executable(
  'dxmt9-query-serialized-range-metal-spec', 'query_serialized_range_metal_spec.cpp',
  '../../../src/d3d9/d3d9_pe_chunk_builder.cpp',
  '../../../src/d3d9/d3d9_pe_chunk_nondraw.cpp',
  '../../../src/d3d9/d3d9_pe_chunk_draw.cpp',
  include_directories: [dxmt9_inc, include_directories('../../../src/d3d9')],
  dependencies: [dxmt9_dep, dxmt9_frontend_dep, dxmt9_winemetal_dep],
  link_args: dxmt9_test_link_args,
)
foreach ranges : ['0', '1']
  foreach offload : ['0', '1']
    foreach transport : ['contiguous', 'segmented']
      oracle_args = ranges == '1' ? ['--require-retained-writer'] : []
      if transport == 'segmented'
        oracle_args += ['--segmented']
      endif
      test('dxmt9-query-serialized-range-' + ranges + '-offload-' + offload +
          (transport == 'segmented' ? '-segmented' : '') + '-metal-spec',
        query_serialized_range_metal_spec, args: oracle_args,
        env: dxmt9_native_test_env_vars + [
          'MTL_DEBUG_LAYER=1', 'DXMT9_METAL_OCCLUSION=1',
          'DXMT9_QUERY_SCOPE_RANGES=' + ranges, 'DXMT9_QUERY_SCOPE_CB_JOIN=0',
          'DXMT9_CPU_READY_TAPE=1', 'DXMT9_TILE_FFP=off',
          'DXMT9_ASYNC_QUERY_FENCES=1', 'DXMT9_OFFLOAD_COMMIT_REPLAY=' + offload],
        is_parallel: false, timeout: 30)
      test('dxmt9-query-release-burst-' + ranges + '-offload-' + offload +
          '-' + transport + '-metal-spec', python3,
        args: [join_paths(meson.project_source_root(), 'tests/scripts/test_query_release_burst.py'),
               query_serialized_range_metal_spec.full_path()] + oracle_args,
        env: dxmt9_native_test_env_vars + [
          'MTL_DEBUG_LAYER=1', 'DXMT9_METAL_OCCLUSION=1',
          'DXMT9_QUERY_SCOPE_RANGES=' + ranges, 'DXMT9_QUERY_SCOPE_CB_JOIN=0',
          'DXMT9_CPU_READY_TAPE=1', 'DXMT9_TILE_FFP=off',
          'DXMT9_ASYNC_QUERY_FENCES=1', 'DXMT9_OFFLOAD_COMMIT_REPLAY=' + offload],
        is_parallel: false, timeout: 30)
    endforeach
  endforeach
endforeach

query_occlusion_metal_spec = executable(
  'dxmt9-query-occlusion-metal-spec', 'query_occlusion_metal_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep, dxmt9_winemetal_dep],
  link_args: dxmt9_test_link_args,
)
foreach tile : ['off', 'force']
  test('dxmt9-query-ranges-' + tile + '-metal-spec', query_occlusion_metal_spec,
    env: dxmt9_native_test_env_vars + [
      'MTL_DEBUG_LAYER=1', 'DXMT9_METAL_OCCLUSION=1', 'DXMT_PERF_COUNTERS=1',
      'DXMT9_QUERY_SCOPE_RANGES=1', 'DXMT9_QUERY_SCOPE_CB_JOIN=0',
      'DXMT9_COALESCE_QUERY_ENDS=1',
      'DXMT9_CPU_READY_TAPE=1', 'DXMT9_TILE_FFP=' + tile,
      'DXMT9_ASYNC_QUERY_FENCES=1'], is_parallel: false, timeout: 60)
  test('dxmt9-query-ranges-receipt-' + tile + '-metal-spec', query_occlusion_metal_spec,
    args: ['--snapshot-oracle'],
    env: dxmt9_native_test_env_vars + [
      'MTL_DEBUG_LAYER=1', 'DXMT9_METAL_OCCLUSION=1', 'DXMT_PERF_COUNTERS=1',
      'DXMT9_QUERY_SCOPE_RANGES=1', 'DXMT9_QUERY_SCOPE_CB_JOIN=0',
      'DXMT9_CPU_READY_TAPE=1', 'DXMT9_TILE_FFP=' + tile,
      'DXMT9_ASYNC_QUERY_FENCES=1'], is_parallel: false, timeout: 60)
endforeach
foreach tile : ['off', 'force']
  test('dxmt9-query-scope-cb-reference-' + tile + '-metal-spec', query_occlusion_metal_spec,
    env: dxmt9_native_test_env_vars + [
      'MTL_DEBUG_LAYER=1', 'DXMT9_METAL_OCCLUSION=1', 'DXMT_PERF_COUNTERS=1',
      'DXMT9_QUERY_SCOPE_CB_JOIN=0', 'DXMT9_CPU_READY_TAPE=1',
      'DXMT9_TILE_FFP=' + tile, 'DXMT9_ASYNC_QUERY_FENCES=1'],
    is_parallel: false, timeout: 60)
  test('dxmt9-query-scope-cb-' + tile + '-metal-spec', query_occlusion_metal_spec,
    env: dxmt9_native_test_env_vars + [
      'MTL_DEBUG_LAYER=1', 'DXMT9_METAL_OCCLUSION=1', 'DXMT_PERF_COUNTERS=1',
      'DXMT9_QUERY_SCOPE_CB_JOIN=1', 'DXMT9_CPU_READY_TAPE=1',
      'DXMT9_TILE_FFP=' + tile, 'DXMT9_ASYNC_QUERY_FENCES=1'],
    is_parallel: false, timeout: 60)
  test('dxmt9-query-scope-cb-receipt-' + tile + '-metal-spec', query_occlusion_metal_spec,
    args: ['--snapshot-oracle'],
    env: dxmt9_native_test_env_vars + [
      'MTL_DEBUG_LAYER=1', 'DXMT9_METAL_OCCLUSION=1', 'DXMT_PERF_COUNTERS=1',
      'DXMT9_QUERY_SCOPE_CB_JOIN=1', 'DXMT9_CPU_READY_TAPE=1',
      'DXMT9_TILE_FFP=' + tile, 'DXMT9_ASYNC_QUERY_FENCES=1'],
    is_parallel: false, timeout: 60)
  test('dxmt9-query-occlusion-' + tile + '-metal-spec', query_occlusion_metal_spec,
    env: dxmt9_native_test_env_vars + [
      'MTL_DEBUG_LAYER=1', 'DXMT9_METAL_OCCLUSION=1', 'DXMT_PERF_COUNTERS=1',
      'DXMT9_TILE_FFP=' + tile, 'DXMT9_ASYNC_QUERY_FENCES=1'],
    is_parallel: false, timeout: 60)
  test('dxmt9-query-poll-receipt-' + tile + '-metal-spec', query_occlusion_metal_spec,
    args: ['--snapshot-oracle'],
    env: dxmt9_native_test_env_vars + [
      'MTL_DEBUG_LAYER=1', 'DXMT9_METAL_OCCLUSION=1', 'DXMT_PERF_COUNTERS=1',
      'DXMT9_TILE_FFP=' + tile, 'DXMT9_ASYNC_QUERY_FENCES=1'],
    is_parallel: false, timeout: 60)
endforeach

test('dxmt9-runtime-policy-retirement-audit', python3,
     args: [join_paths(meson.project_source_root(),
                      'tests/scripts/test_runtime_policy_retirement.py')])
query_end_marker_spec = executable(
  'dxmt9-query-end-marker-spec', 'query_end_marker_spec.cpp',
  include_directories: dxmt9_inc,
)
test('dxmt9-query-end-marker-spec', query_end_marker_spec,
     env: dxmt9_native_test_env)

query_upload_tracker_spec = executable(
  'dxmt9-query-upload-tracker-spec', 'query_upload_tracker_spec.cpp',
  include_directories: dxmt9_inc,
)
test('dxmt9-query-upload-tracker-spec', query_upload_tracker_spec,
     env: dxmt9_native_test_env)

query_initializer_spec = executable(
  'dxmt9-query-initializer-spec', 'query_initializer_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep, dxmt9_winemetal_dep],
  link_args: dxmt9_test_link_args,
)
test('dxmt9-query-initializer-spec', query_initializer_spec,
     env: dxmt9_native_test_env_vars + ['MTL_DEBUG_LAYER=1', 'DXMT9_ASYNC_QUERY_FENCES=1'],
     is_parallel: false, timeout: 30)
test('dxmt9-query-covered-pending-gpu-spec', query_initializer_spec,
     args: ['--covered-query'],
     env: dxmt9_native_test_env_vars + ['MTL_DEBUG_LAYER=1',
       'DXMT9_ASYNC_QUERY_FENCES=1', 'DXMT9_QUERY_COVERED_PENDING=1',
       'DXMT9_COALESCE_QUERY_ENDS=0'],
     is_parallel: false, timeout: 30)
test('dxmt9-publication-mark-cache-gpu-spec', query_initializer_spec,
     args: ['--covered-query'],
     env: dxmt9_native_test_env_vars + ['MTL_DEBUG_LAYER=1',
       'DXMT9_ASYNC_QUERY_FENCES=1', 'DXMT9_QUERY_COVERED_PENDING=1',
       'DXMT9_COALESCE_QUERY_ENDS=0', 'DXMT9_PUBLISH_RESOURCE_DEDUP=1'],
     is_parallel: false, timeout: 30)
test('dxmt9-query-end-coalescing-gpu-spec', query_initializer_spec,
     args: ['--coalesced-query'],
     env: dxmt9_native_test_env_vars + ['MTL_DEBUG_LAYER=1', 'DXMT9_ASYNC_QUERY_FENCES=1', 'DXMT9_COALESCE_QUERY_ENDS=1'],
     is_parallel: false, timeout: 30)

metal_shader_compile_profile_spec = executable(
  'dxmt9-metal-shader-compile-profile-spec',
  'metal_shader_compile_profile_spec.mm',
  objcpp_args: ['-fobjc-arc'],
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_winemetal_dep,
    dependency('appleframeworks', modules: ['Foundation', 'Metal'])],
  link_args: dxmt9_test_link_args,
)
test('dxmt9-metal-shader-compile-profile-spec', metal_shader_compile_profile_spec,
  env: dxmt9_native_test_env_vars + ['MTL_DEBUG_LAYER=1', 'MTL_SHADER_VALIDATION=1'],
  is_parallel: false,
  timeout: 30)

vertex_position_probe_policy_spec = executable(
  'dxmt9-vertex-position-probe-policy-spec',
  'vertex_position_probe_policy_spec.cpp',
  include_directories: [dxmt9_inc, include_directories('../../../src')],
)
test('dxmt9-vertex-position-probe-policy-spec', vertex_position_probe_policy_spec,
     env: dxmt9_native_test_env)

vertex_position_probe_metal_spec = executable(
  'dxmt9-vertex-position-probe-metal-spec',
  'vertex_position_probe_metal_spec.mm',
  objcpp_args: ['-fobjc-arc'],
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_winemetal_dep,
    dependency('appleframeworks', modules: ['Foundation', 'Metal'])],
  link_args: dxmt9_test_link_args,
)
foreach mode : ['enabled', 'disabled', 'invalid', 'skinning', 'no-contract']
  test('dxmt9-vertex-position-probe-' + mode + '-metal-spec', vertex_position_probe_metal_spec,
    args: [mode],
    env: dxmt9_native_test_env_vars + ['MTL_DEBUG_LAYER=1', 'MTL_SHADER_VALIDATION=1'],
    is_parallel: false,
    timeout: 60)
endforeach

metal_sampling_lineage_spec = executable(
  'dxmt9-metal-sampling-lineage-spec',
  'metal_sampling_lineage_spec.mm',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_winemetal_dep,
    dependency('appleframeworks', modules: ['Foundation', 'Metal'])],
  link_args: dxmt9_test_link_args,
)
foreach enabled : ['0', '1']
  test('dxmt9-metal-sampling-lineage-' + enabled + '-spec', metal_sampling_lineage_spec,
    env: dxmt9_native_test_env_vars + [
      'DXMT9_EFFECT_DRAW_TRACE=' + enabled,
      'DXMT9_EFFECT_DRAW_TRACE_BINDINGS=1',
      'DXMT9_EFFECT_DRAW_TRACE_GEOMETRY=1',
      'MTL_DEBUG_LAYER=1'],
    is_parallel: false,
    timeout: 30)
endforeach

metal_depth_stencil_lineage_spec = executable(
  'dxmt9-metal-depth-stencil-lineage-spec',
  'metal_depth_stencil_lineage_spec.mm',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_winemetal_dep,
    dependency('appleframeworks', modules: ['Foundation', 'Metal'])],
  link_args: dxmt9_test_link_args,
)
foreach enabled : ['0', '1']
  test('dxmt9-metal-depth-stencil-lineage-' + enabled + '-spec', metal_depth_stencil_lineage_spec,
    env: dxmt9_native_test_env_vars + [
      'DXMT9_EFFECT_DRAW_TRACE=' + enabled,
      'DXMT9_EFFECT_DRAW_TRACE_BINDINGS=1',
      'DXMT9_EFFECT_DRAW_TRACE_GEOMETRY=1',
      'MTL_DEBUG_LAYER=1'],
    is_parallel: false,
    timeout: 30)
endforeach

metal_direct_buffer_lineage_spec = executable(
  'dxmt9-metal-direct-buffer-lineage-spec',
  'metal_direct_buffer_lineage_spec.mm',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_winemetal_dep,
    dependency('appleframeworks', modules: ['Foundation', 'Metal'])],
  link_args: dxmt9_test_link_args,
)
foreach enabled : ['0', '1']
  test('dxmt9-metal-direct-buffer-lineage-' + enabled + '-spec', metal_direct_buffer_lineage_spec,
    env: dxmt9_native_test_env_vars + [
      'DXMT9_EFFECT_DRAW_TRACE=' + enabled,
      'DXMT9_EFFECT_DRAW_TRACE_BINDINGS=1',
      'DXMT9_EFFECT_DRAW_TRACE_GEOMETRY=1',
      'MTL_DEBUG_LAYER=1'],
    is_parallel: false,
    timeout: 30)
endforeach

partial_clear_spec = executable(
  'dxmt9-partial-clear-spec',
  ['partial_clear_spec.cpp', 'partial_clear_metal_helper.mm'],
  objcpp_args: ['-fobjc-arc'],
  include_directories: dxmt9_inc,
  dependencies: [
    dxmt9_dep,
    dxmt9_frontend_dep,
    dxmt9_winemetal_dep,
    dependency('appleframeworks', modules: ['Foundation', 'Metal']),
  ],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-partial-clear-spec', partial_clear_spec,
     env: dxmt9_native_test_env_vars + ['MTL_DEBUG_LAYER=1'],
     is_parallel: false,
     timeout: 30)

backend_key_descriptor_spec = executable(
  'dxmt9-backend-key-descriptor-spec',
  'backend_key_descriptor_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-backend-key-descriptor-spec', backend_key_descriptor_spec,
     env: dxmt9_native_test_env)

# Regression guard for the FFP variant-key determinism + sensitivity
# contract. See the header comment in
# `tests/native/backend/ffp_key_determinism_spec.cpp` and the lifecycle
# comments on `core::FfpVertexKey` / `core::FfpPixelKey`.
ffp_key_determinism_spec = executable(
  'dxmt9-ffp-key-determinism-spec',
  'ffp_key_determinism_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-ffp-key-determinism-spec', ffp_key_determinism_spec,
     env: dxmt9_native_test_env)

# FFP triadic texture-stage args (D3DTSS_COLORARG0 / ALPHAARG0 consumed by
# D3DTOP_MULTIPLYADD / D3DTOP_LERP). Asserts the arg0 fields are captured
# into the FfpPixelKey (key + hash sensitivity) and that makeFfpPixelSource
# emits the triadic op bodies reading arg0. gap_d3d9 §B.10#10 / §B.2.
ffp_triadic_msl_spec = executable(
  'dxmt9-ffp-triadic-msl-spec',
  'ffp_triadic_msl_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-ffp-triadic-msl-spec', ffp_triadic_msl_spec,
     env: dxmt9_native_test_env)

# Present drawable acquire policy resolver matrix — locks the priority
# `Async > SyncOnSubmit > PreAcquire > Sync` and the "0" / empty-string
# treatment of the three DXMT9_PRESENT_* env strings. See the header
# comment in `tests/native/backend/present_acquire_policy_spec.cpp`.
present_acquire_policy_spec = executable(
  'dxmt9-present-acquire-policy-spec',
  'present_acquire_policy_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-present-acquire-policy-spec', present_acquire_policy_spec,
     env: dxmt9_native_test_env)

drawable_token_spec = executable(
  'dxmt9-drawable-token-spec',
  'drawable_token_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep, dxmt9_winemetal_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-drawable-token-spec', drawable_token_spec,
     env: dxmt9_native_test_env)

# Present boundary policy resolver matrix — locks the priority
# `Disabled > PresentCompletion > Completion > AfterAcquire > Default`
# and the "0" / empty-string / null treatment of the four
# DXMT9_PRESENT_BOUNDARY_* / DXMT9_DISABLE_PRESENT_BOUNDARY env strings.
# PresentCompletion defaults on (null / empty env counts as set) — see
# the header comment in
# `tests/native/backend/present_boundary_policy_spec.cpp`.
present_boundary_policy_spec = executable(
  'dxmt9-present-boundary-policy-spec',
  'present_boundary_policy_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-present-boundary-policy-spec', present_boundary_policy_spec,
     env: dxmt9_native_test_env)

# Vsync-disable resolver — DXMT9_DISABLE_VSYNC env-string handling.
# Locks the "0" / empty / null = off and non-zero-non-empty = on
# semantics. See the header comment in
# `tests/native/backend/present_disable_vsync_spec.cpp`.
present_disable_vsync_spec = executable(
  'dxmt9-present-disable-vsync-spec',
  'present_disable_vsync_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-present-disable-vsync-spec', present_disable_vsync_spec,
     env: dxmt9_native_test_env)

# R-BACK-3.9 / R-BACK-3.10 archive-prewarm-hardening value transforms —
# DXMT9_ARCHIVE_MAX_PREWARM_MB size-guard parsing/decision and the
# bounded mid-session shouldMilestoneSave() decision (including the
# R-BACK-3.11 savePoisoned short-circuit). Pure CPU spec; no Metal
# device or filesystem access.
archive_prewarm_spec = executable(
  'dxmt9-archive-prewarm-spec',
  'archive_prewarm_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-archive-prewarm-spec', archive_prewarm_spec,
     env: dxmt9_native_test_env)

backend_pipeline_key_spec = executable(
  'dxmt9-backend-pipeline-key-spec',
  'backend_pipeline_key_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep, dxmt9_winemetal_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-backend-pipeline-key-spec', backend_pipeline_key_spec)

# Append-only segmented PSO-handle publication: fixed ABI capacity, block
# boundaries, generation fail-closed lookup, and release/acquire readers.
pso_slot_publication_spec = executable(
  'dxmt9-pso-slot-publication-spec',
  'pso_slot_publication_spec.cpp',
  include_directories: dxmt9_inc,
)

test('dxmt9-pso-slot-publication-spec', pso_slot_publication_spec)

pso_cache_diagnostics_spec = executable(
  'dxmt9-pso-cache-diagnostics-spec',
  'pso_cache_diagnostics_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-pso-cache-diagnostics-spec', pso_cache_diagnostics_spec,
     env: dxmt9_native_test_env)

# P1-1 regression guard for D3DRS_BLENDOP / D3DRS_BLENDOPALPHA /
# D3DRS_SEPARATEALPHABLENDENABLE / D3DRS_SRCBLENDALPHA /
# D3DRS_DESTBLENDALPHA → MTLBlendOperation + separate-alpha lane wiring.
# Pure CPU spec on `makeBlendAttachmentKeys` + `toBlendOperation`; no
# Metal device required.
blend_op_family_spec = executable(
  'dxmt9-blend-op-family-spec',
  'blend_op_family_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep, dxmt9_winemetal_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-blend-op-family-spec', blend_op_family_spec)

dod_replay_observer_spec = executable(
  'dxmt9-dod-replay-observer-spec',
  'dod_replay_observer_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-dod-replay-observer-spec', dod_replay_observer_spec)

resource_mark_snapshot_observer_spec = executable(
  'dxmt9-resource-mark-snapshot-observer-spec',
  'resource_mark_snapshot_observer_spec.cpp',
  include_directories: dxmt9_inc,
)

test('dxmt9-resource-mark-snapshot-observer-spec',
     resource_mark_snapshot_observer_spec)

stateblock_snapshot_effect_observer_spec = executable(
  'dxmt9-stateblock-snapshot-effect-observer-spec',
  'stateblock_snapshot_effect_observer_spec.cpp',
  include_directories: dxmt9_inc,
)

test('dxmt9-stateblock-snapshot-effect-observer-spec',
     stateblock_snapshot_effect_observer_spec)

# R-BACK-2.88 / R-VERIF-2.23 bounded source-to-reclaim lifecycle observer.
# Uses production pure predicates plus a deterministic condition-variable fake
# backend; no Metal device, sleep, or polling.
pipeline_lifecycle_observer_spec = executable(
  'dxmt9-pipeline-lifecycle-observer-spec',
  'pipeline_lifecycle_observer_spec.cpp',
  include_directories: dxmt9_inc,
)

test('dxmt9-pipeline-lifecycle-observer-spec',
     pipeline_lifecycle_observer_spec)

resource_lifetime_spec = executable(
  'dxmt9-resource-lifetime-spec',
  'resource_lifetime_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep, dxmt9_winemetal_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-resource-lifetime-spec', resource_lifetime_spec,
     env: dxmt9_native_test_env)

queue_completion_sources_spec = executable(
  'dxmt9-queue-completion-sources-spec',
  'queue_completion_sources_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep, dxmt9_winemetal_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-queue-completion-sources-spec',
     queue_completion_sources_spec)

post_encode_payload_retirement_spec = executable(
  'dxmt9-post-encode-payload-retirement-spec',
  'post_encode_payload_retirement_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep, dxmt9_winemetal_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-post-encode-payload-retirement-spec',
     post_encode_payload_retirement_spec,
     args: [meson.project_source_root()])

cpu_ready_tape_spec = executable(
  'dxmt9-cpu-ready-tape-spec',
  'cpu_ready_tape_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep, dxmt9_winemetal_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-cpu-ready-tape-spec',
     cpu_ready_tape_spec,
     env: dxmt9_native_test_env)

encode_session_admission_spec = executable(
  'dxmt9-encode-session-admission-spec',
  'encode_session_admission_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep, dxmt9_winemetal_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-encode-session-admission-spec',
     encode_session_admission_spec)

encode_scheduling_progress_spec = executable(
  'dxmt9-encode-scheduling-progress-spec',
  'encode_scheduling_progress_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_winemetal_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-encode-scheduling-progress-spec',
     encode_scheduling_progress_spec)

session_release_event_spec = executable(
  'dxmt9-session-release-event-spec',
  'session_release_event_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep, dxmt9_winemetal_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-session-release-event-spec',
     session_release_event_spec)

replay_emission_plan_spec = executable(
  'dxmt9-replay-emission-plan-spec',
  'replay_emission_plan_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

prepared_encode_plan_spec = executable(
  'dxmt9-prepared-encode-plan-spec',
  'prepared_encode_plan_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep, dxmt9_winemetal_dep],
  link_args: dxmt9_test_link_args,
)
prepared_linear_reference = custom_target('prepared-linear-reference',
  input: '../../../src/dxmt9/dxmt9_prepared_encode_plan.hpp',
  output: 'prepared_linear_reference.generated.hpp',
  command: [python3, files('../../../scripts/check/gen_prepared_linear_reference.py'), '@INPUT@', '@OUTPUT@'],
)
prepared_encode_benchmark = executable('dxmt9-prepared-encode-benchmark',
  ['prepared_encode_benchmark.cpp', prepared_linear_reference],
  include_directories: [dxmt9_inc, include_directories('../../../src/dxmt9')],
  dependencies: [dxmt9_dep, dxmt9_frontend_dep, dxmt9_winemetal_dep],
  link_args: dxmt9_test_link_args,
)
test('dxmt9-prepared-encode-benchmark-spec', prepared_encode_benchmark,
     env: dxmt9_native_test_env)
test('dxmt9-prepared-encode-plan-spec', prepared_encode_plan_spec,
     env: dxmt9_native_test_env)
foreach dedup : ['0', '1']
  test('dxmt9-prepared-serial-publication-dedup-' + dedup, prepared_encode_plan_spec,
       args: ['--serial-publication-probe'],
       env: dxmt9_native_test_env_vars + [
         'DXMT9_PUBLISH_RESOURCE_DEDUP=' + dedup])
endforeach
test('dxmt9-prepared-serial-retired-off-ignored', prepared_encode_plan_spec,
     args: ['--serial-publication-probe'],
     env: dxmt9_native_test_env_vars + ['DXMT9_PREPARED_SERIAL_READS=0'])
test('dxmt9-prepared-encode-shadow-analysis', python3,
     args: [files('../../../scripts/check/test_prepared_encode_shadow.py'),
            prepared_encode_plan_spec.full_path()],
     depends: prepared_encode_plan_spec,
     env: dxmt9_native_test_env)

test('dxmt9-replay-emission-plan-spec',
     replay_emission_plan_spec,
     env: dxmt9_native_test_env)

# Binds `specs/verification/tla/ReplayEmissionPlanIslands.tla` to the C++
# predicate in `src/dxmt9/dxmt9_cpu_ready_tape.hpp`
# (`compatibilitySpanAdmission`), which the queue calls from
# `beginDirectChunkSlotReplay` and the Tape calls again from
# `extendCompatibilityProducerIdentity`. Truth table plus trace conformance:
# the production interleavings, and one translated trace per
# `.counterexample.cfg` so the negative TLA configs and the native negative
# cases stay paired.
replay_emission_plan_islands_spec = executable(
  'dxmt9-replay-emission-plan-islands-spec',
  'replay_emission_plan_islands_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-replay-emission-plan-islands-spec',
     replay_emission_plan_islands_spec,
     env: dxmt9_native_test_env)

cpu_ready_plan_spec = executable(
  'dxmt9-cpu-ready-plan-spec',
  'cpu_ready_plan_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-cpu-ready-plan-spec',
     cpu_ready_plan_spec,
     env: dxmt9_native_test_env)

direct_slot_provisioning_spec = executable(
  'dxmt9-direct-slot-provisioning-spec',
  'direct_slot_provisioning_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-direct-slot-provisioning-spec',
     direct_slot_provisioning_spec,
     env: dxmt9_native_test_env)

direct_source_lifecycle_spec = executable(
  'dxmt9-direct-source-lifecycle-spec',
  'direct_source_lifecycle_spec.cpp',
  include_directories: dxmt9_inc,
)

test('dxmt9-direct-source-lifecycle-spec',
     direct_source_lifecycle_spec,
     env: dxmt9_native_test_env)

compat_draw_batch_spec = executable(
  'dxmt9-compat-draw-batch-spec',
  'compat_draw_batch_spec.cpp',
  include_directories: dxmt9_inc,
)

test('dxmt9-compat-draw-batch-spec',
     compat_draw_batch_spec,
     env: dxmt9_native_test_env)

compat_draw_batch_failure_spec = executable(
  'dxmt9-compat-draw-batch-failure-spec',
  'compat_draw_batch_failure_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-compat-draw-batch-failure-spec',
     compat_draw_batch_failure_spec,
     env: dxmt9_native_test_env)

draw_run_batch_fold_spec = executable(
  'dxmt9-draw-run-batch-fold-spec',
  'draw_run_batch_fold_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-draw-run-batch-fold-spec',
     draw_run_batch_fold_spec,
     env: dxmt9_native_test_env)

process_memory_observer_spec = executable(
  'dxmt9-process-memory-observer-spec',
  'process_memory_observer_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-process-memory-observer-spec',
     process_memory_observer_spec,
     env: dxmt9_native_test_env)

source_payload_spec = executable(
  'dxmt9-source-payload-spec',
  'source_payload_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-source-payload-spec',
     source_payload_spec,
     env: dxmt9_native_test_env)

replay_projection_transaction_spec = executable(
  'dxmt9-replay-projection-transaction-spec',
  'replay_projection_transaction_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-replay-projection-transaction-spec',
     replay_projection_transaction_spec,
     env: dxmt9_native_test_env)

cpu_ready_arena_lease_spec = executable(
  'dxmt9-cpu-ready-arena-lease-spec',
  'cpu_ready_arena_lease_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep, dxmt9_winemetal_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-cpu-ready-arena-lease-spec',
     cpu_ready_arena_lease_spec,
     env: dxmt9_native_test_env)
test('dxmt9-query-end-coalescing-queue-spec', cpu_ready_arena_lease_spec,
     args: ['--coalesced-query'],
     env: dxmt9_native_test_env_vars + ['DXMT9_COALESCE_QUERY_ENDS=1', 'DXMT9_ASYNC_QUERY_FENCES=1'])

capture_color_observer_test_env = dxmt9_native_test_env_vars + [
  'DXMT9_DUMP_COLOR_ATTACHMENT_DIR=/unused-inert-queue-observer',
  'DXMT9_DUMP_COLOR_ATTACHMENT_INDEX=0',
  'DXMT9_DUMP_COLOR_ATTACHMENT_HASH_ONLY=1',
  'DXMT9_DUMP_COLOR_ATTACHMENT_CAPTURE_SOURCES_ONLY=1',
  'DXMT9_DUMP_COLOR_ATTACHMENT_MAX_DUMPS=16',
]
test('dxmt9-cpu-ready-arena-capture-observer-spec',
     cpu_ready_arena_lease_spec,
     env: capture_color_observer_test_env)

cpu_ready_production_routing_spec = executable(
  'dxmt9-cpu-ready-production-routing-spec',
  [
    'cpu_ready_production_routing_spec.cpp',
    'render_tape_test_metal_device.mm',
  ],
  include_directories: dxmt9_inc,
  dependencies: [
    dxmt9_dep,
    dxmt9_frontend_dep,
    dxmt9_winemetal_dep,
    dependency('appleframeworks', modules: ['Foundation', 'Metal']),
  ],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-cpu-ready-production-routing-spec',
     cpu_ready_production_routing_spec,
     env: dxmt9_native_test_env_vars + [
       'DXMT9_OFFLOAD_COMMIT_REPLAY=1',
     ])
test('dxmt9-deferred-ingress-routing-spec',
     cpu_ready_production_routing_spec,
     env: dxmt9_native_test_env_vars + [
       'DXMT9_OFFLOAD_COMMIT_REPLAY=1',
       # A retired OFF setting cannot restore producer-side marking.
       'DXMT9_DEFER_INGRESS_RESOURCE_MARKS=0',
     ])
test('dxmt9-direct-slot-headroom-ownership-spec',
     cpu_ready_production_routing_spec,
     args: ['--headroom-ownership-spec'],
     env: dxmt9_native_test_env)
test('dxmt9-qualified-ordinary-growth-spec',
     cpu_ready_production_routing_spec,
     args: ['--qualified-ordinary-growth-spec'],
     env: dxmt9_native_test_env)
test('dxmt9-render-tape-cpu-destroy-window-spec',
     cpu_ready_production_routing_spec,
     args: ['--general-cpu-destroy-spec'],
     env: dxmt9_native_test_env)
test('dxmt9-render-tape-cpu-mipmap-window-spec',
     cpu_ready_production_routing_spec,
     args: ['--general-cpu-mipmap-spec'],
     env: dxmt9_native_test_env)
test('dxmt9-render-tape-cpu-mutation-window-spec',
     cpu_ready_production_routing_spec,
     args: ['--general-cpu-mutation-spec'],
     env: dxmt9_native_test_env,
     timeout: 30)
test('dxmt9-render-tape-cpu-reset-qualification-spec',
     cpu_ready_production_routing_spec,
     args: ['--general-cpu-reset-spec'],
     env: dxmt9_native_test_env,
     timeout: 30)
test('dxmt9-render-tape-cpu-alias-after-parent-destroy-spec',
     cpu_ready_production_routing_spec,
     args: ['--general-cpu-alias-spec'],
     env: dxmt9_native_test_env,
     timeout: 30)
test('dxmt9-render-tape-cpu-two-pending-production-sources-spec',
     cpu_ready_production_routing_spec,
     args: ['--two-pending-production-sources-spec'],
     env: dxmt9_native_test_env,
     timeout: 30)
test('dxmt9-render-tape-cpu-phase-ledger-spec',
     cpu_ready_production_routing_spec,
     args: ['--cpu-tape-phase-ledger-spec'],
     env: dxmt9_native_test_env)
test('dxmt9-render-tape-capture-source-policy-spec',
     cpu_ready_production_routing_spec,
     args: ['--capture-source-policy-spec'],
     env: dxmt9_native_test_env,
     timeout: 30)

test('dxmt9-render-tape-cpu-measurement-spec',
     cpu_ready_production_routing_spec,
     args: ['--general-cpu-measurement-spec'],
     env: dxmt9_native_test_env)

test('dxmt9-render-tape-cpu-attachment-spec',
     cpu_ready_production_routing_spec,
     args: ['--general-cpu-attachment-spec'],
     env: dxmt9_native_test_env)

cpu_policy_spec = executable(
  'dxmt9-render-tape-cpu-policy-spec',
  'render_tape_cpu_policy_spec.cpp',
  include_directories: dxmt9_inc,
)
test('dxmt9-render-tape-cpu-policy-spec', cpu_policy_spec)

cpu_source_admission_spec = executable(
  'dxmt9-render-tape-cpu-source-admission-spec',
  'render_tape_cpu_source_admission_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-render-tape-cpu-source-admission-spec',
     cpu_source_admission_spec,
     env: dxmt9_native_test_env)

# R-BACK-2.52 inline-constant replay equivalence.  The fixture drives the
# production PE producer/semantic owner into ExactFixed, then uses the real
# offload preparation/replay and SourcePayloadView to compare six constant
# families at each logical draw under both direct-slot and compatibility paths.
inline_constant_replay_spec = executable(
  'dxmt9-inline-constant-replay-spec',
  [
    'inline_constant_replay_spec.cpp',
    '../../../src/d3d9/d3d9_pe_producer.cpp',
  ],
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep, dxmt9_winemetal_dep],
  cpp_args: ['-DDXMT9_PE_SHADOW_TEST_SEAM=1'],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-inline-constant-replay-spec', inline_constant_replay_spec,
     env: dxmt9_native_test_env_vars + [
       'DXMT9_OFFLOAD_COMMIT_REPLAY=1',
     ])

# Retired selector must not restore the legacy invalidation path.
test('dxmt9-precise-uniform-invalidation-spec', inline_constant_replay_spec,
     env: dxmt9_native_test_env_vars + [
       'DXMT9_OFFLOAD_COMMIT_REPLAY=1',
       'DXMT9_REPLAY_PRECISE_UNIFORM_INVALIDATION=0',
     ])

uniform_invalidation_metal_spec = executable(
  'dxmt9-uniform-invalidation-metal-spec',
  'uniform_invalidation_metal_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep, dxmt9_winemetal_dep],
  link_args: dxmt9_test_link_args,
)
test('dxmt9-uniform-invalidation-metal-spec', python3,
     args: [join_paths(meson.project_source_root(), 'tests/scripts/test_uniform_invalidation_metal.py'),
            uniform_invalidation_metal_spec.full_path()],
     env: dxmt9_native_test_env_vars + ['MTL_DEBUG_LAYER=1'],
     depends: uniform_invalidation_metal_spec,
     is_parallel: false,
     timeout: 150)

cpu_ready_session_join_spec = executable(
  'dxmt9-cpu-ready-session-join-spec',
  'cpu_ready_session_join_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep, dxmt9_winemetal_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-cpu-ready-session-join-spec',
     cpu_ready_session_join_spec,
     env: dxmt9_native_test_env_vars + [
       'DXMT9_OFFLOAD_COMMIT_REPLAY=1',
     ])

test('dxmt9-cpu-ready-session-physical-cap-spec', cpu_ready_session_join_spec,
     env: dxmt9_native_test_env_vars + [
       'DXMT9_OFFLOAD_COMMIT_REPLAY=1', 'DXMT9_SESSION_JOIN_CAPACITY_CASE=1'])

test('dxmt9-cpu-ready-closed-prefix-spec', cpu_ready_session_join_spec,
     env: dxmt9_native_test_env_vars + [
       'DXMT9_OFFLOAD_COMMIT_REPLAY=1', 'DXMT9_QUERY_SCOPE_RANGES=1',
       'DXMT9_METAL_OCCLUSION=1',
       'DXMT9_SESSION_JOIN_CLOSED_PREFIX_CASE=1'])

test('dxmt9-cpu-ready-session-join-p0-composition-spec',
     cpu_ready_session_join_spec,
     env: dxmt9_native_test_env_vars + [
       'DXMT9_OFFLOAD_COMMIT_REPLAY=1',
       'DXMT9_SESSION_JOIN_P0_COMPOSITION_CASE=1',
     ])

test('dxmt9-cpu-ready-session-join-perf-off-spec',
     cpu_ready_session_join_spec,
     env: dxmt9_native_test_env_vars + [
       'DXMT9_OFFLOAD_COMMIT_REPLAY=1',
       'DXMT9_SESSION_JOIN_PERF_OFF_CASE=1',
     ])

# The split policy is read once per process. Re-run the focused repeated-source
# transaction in a separate process so PerNRecords must carry its threshold
# across fragment calls (three one-record fragments, threshold=3).
test('dxmt9-cpu-ready-session-join-per-n-records-spec',
     cpu_ready_session_join_spec,
     env: dxmt9_native_test_env_vars + [
       'DXMT9_OFFLOAD_COMMIT_REPLAY=1',
       'DXMT9_SESSION_JOIN_SPLIT_POLICY_CASE=per-n-records',
       'DXMT9_MID_CHUNK_COMMIT_POLICY=per-n-records',
       'DXMT9_MID_CHUNK_COMMIT_RECORDS=3',
     ])

encode_partition_snapshot_spec = executable(
  'dxmt9-encode-partition-snapshot-spec',
  'encode_partition_snapshot_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep, dxmt9_winemetal_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-encode-partition-snapshot-spec',
     encode_partition_snapshot_spec,
     env: dxmt9_native_test_env)

encode_partition_serial_spec = executable(
  'dxmt9-encode-partition-serial-spec',
  'encode_partition_serial_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep, dxmt9_winemetal_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-encode-partition-serial-spec',
     encode_partition_serial_spec,
     env: dxmt9_native_test_env)

parallel_render_pass_spec = executable(
  'dxmt9-parallel-render-pass-spec',
  'parallel_render_pass_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep, dxmt9_winemetal_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-parallel-render-pass-spec',
     parallel_render_pass_spec,
     env: dxmt9_native_test_env)

# Concrete R-VERIF-2.15 oracle: two real Metal child encoders execute
# A -> B -> A direct-uniform generations and must byte-match the serial pass.
# The fixture also proves it can distinguish the omitted-dirty transition.
parallel_draw_binding_metal_spec = executable(
  'dxmt9-parallel-draw-binding-metal-spec',
  'parallel_draw_binding_metal_spec.mm',
  include_directories: dxmt9_inc,
  dependencies: [
    dxmt9_dep,
    dependency('appleframeworks', modules: ['Foundation', 'Metal']),
  ],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-parallel-draw-binding-metal-spec',
     parallel_draw_binding_metal_spec,
     env: dxmt9_native_test_env_vars + ['MTL_DEBUG_LAYER=1'],
     is_parallel: false,
     timeout: 30)

# Concrete Metal oracle for analytically controlled implicit texture gradients.
# This pins the native LOD convention separately from D3D9 sampler translation.
sampler_implicit_lod_metal_spec = executable(
  'dxmt9-sampler-implicit-lod-metal-spec',
  'sampler_implicit_lod_metal_spec.mm',
  include_directories: dxmt9_inc,
  dependencies: [
    dxmt9_dep,
    dependency('appleframeworks', modules: ['Foundation', 'Metal']),
  ],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-sampler-implicit-lod-metal-spec',
     sampler_implicit_lod_metal_spec,
     env: dxmt9_native_test_env_vars + ['MTL_DEBUG_LAYER=1'],
     is_parallel: false,
     timeout: 30)

# Concrete WMT/Metal oracle for complete RGBA16Float automatic mip generation.
# The fixture fills a non-uniform 1024x1024 level 0, issues the production WMT
# blit generateMipmaps command, and checks every adjacent level against a 2x2
# box reduction (including the HDR1 mip-2 crop failure shape).
wmt_generate_mipmaps_metal_spec = executable(
  'dxmt9-wmt-generate-mipmaps-metal-spec',
  'wmt_generate_mipmaps_metal_spec.mm',
  include_directories: dxmt9_inc,
  dependencies: [
    dxmt9_dep,
    dxmt9_winemetal_dep,
    dependency('appleframeworks', modules: ['Foundation', 'Metal']),
  ],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-wmt-generate-mipmaps-metal-spec',
     wmt_generate_mipmaps_metal_spec,
     env: dxmt9_native_test_env_vars + ['MTL_DEBUG_LAYER=1'],
     is_parallel: false,
     timeout: 30)

# Concrete Render Tape D24X8 capability fixture. The roundtrip deliberately
# converts through shaders so no physical depth/stencil byte layout is assumed.
render_tape_d24x8_snapshot_metal_spec = executable(
  'dxmt9-render-tape-d24x8-snapshot-metal-spec',
  'render_tape_d24x8_snapshot_metal_spec.mm',
  include_directories: dxmt9_inc,
  dependencies: [
    dxmt9_dep,
    dependency('appleframeworks', modules: ['Foundation', 'Metal']),
  ],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-render-tape-d24x8-snapshot-metal-spec',
     render_tape_d24x8_snapshot_metal_spec,
     env: dxmt9_native_test_env_vars + ['MTL_DEBUG_LAYER=1'],
     is_parallel: false,
     timeout: 30)

# Production ordinary Direct ChunkSlot replay oracle. Each child selects the
# cached resolver with an explicit env value, proves the Direct commit seam,
# then compares the same offscreen Metal readback against Legacy.
direct_chunk_slot_metal_oracle_spec = executable(
  'dxmt9-direct-chunk-slot-metal-oracle-spec',
  'direct_chunk_slot_metal_oracle_spec.mm',
  include_directories: dxmt9_inc,
  dependencies: [
    dxmt9_dep,
    dxmt9_frontend_dep,
    dxmt9_winemetal_dep,
    dependency('appleframeworks', modules: ['Foundation', 'Metal']),
  ],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-direct-chunk-slot-metal-oracle-spec',
     direct_chunk_slot_metal_oracle_spec,
     env: dxmt9_native_test_env_vars + ['MTL_DEBUG_LAYER=1'],
     is_parallel: false,
     timeout: 30)

test('dxmt9-capture-color-observer-metal-spec', python3,
     args: [join_paths(meson.project_source_root(),
                      'scripts/check/test_capture_color_observer_metal.py'),
            direct_chunk_slot_metal_oracle_spec],
     env: dxmt9_native_test_env_vars,
     is_parallel: false,
     timeout: 90)

encode_session_lifecycle_spec = executable(
  'dxmt9-encode-session-lifecycle-spec',
  'encode_session_lifecycle_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep, dxmt9_winemetal_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-encode-session-lifecycle-spec',
     encode_session_lifecycle_spec,
     env: dxmt9_native_test_env_vars + [
       # This fixture asserts diagnostic activation counters as well as commands.
       'DXMT9_FRAMEGRAPH_RETURN_DIAGNOSTICS=1',
       'DXMT9_PERF_ENCODER_GPU_TIME=0',
       'DXMT_METAL_CAPTURE_FRAME=0',
     ])

draw_seq_filter_spec = executable(
  'dxmt9-draw-seq-filter-spec',
  [
    'draw_seq_filter_spec.cpp',
    '../../../src/dxmt9/dxmt9_debug_trace.cpp',
  ],
  include_directories: dxmt9_inc,
  dependencies: dxmt9_util_dep,
  link_args: dxmt9_test_link_args,
)

test('dxmt9-draw-seq-filter-spec', draw_seq_filter_spec)

upload_arena_lock_diag_spec = executable(
  'dxmt9-upload-arena-lock-diag-spec',
  ['upload_arena_lock_diag_spec.cpp',
   '../../../src/dxmt9/dxmt9_upload_arena_lock_diag.cpp'],
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_util_dep, dxmt9_winemetal_dep],
  link_args: dxmt9_test_link_args,
  cpp_args: ['-UNDEBUG'],
)
test('dxmt9-upload-arena-lock-diag-spec', upload_arena_lock_diag_spec,
     env: ['DXMT9_PERF_UPLOAD_ARENA_LOCK_SPLIT=1'])
test('dxmt9-upload-arena-lock-diag-off-spec', upload_arena_lock_diag_spec,
     env: ['DXMT9_PERF_UPLOAD_ARENA_LOCK_SPLIT=0'])

resource_hazard_spec = executable(
  'dxmt9-resource-hazard-spec',
  'resource_hazard_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep, dxmt9_winemetal_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-resource-hazard-spec', resource_hazard_spec)

buffer_use_batch_spec = executable(
  'dxmt9-buffer-use-batch-spec',
  'buffer_use_batch_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep, dxmt9_winemetal_dep],
  link_args: dxmt9_test_link_args,
)
test('dxmt9-buffer-use-batch-spec', buffer_use_batch_spec,
     env: dxmt9_native_test_env_vars + ['DXMT9_PERF_UPLOAD_ARENA_LOCK_SPLIT=1'])

range_upload_lock_scope_spec = executable(
  'dxmt9-range-upload-lock-scope-spec',
  'range_upload_lock_scope_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep, dxmt9_winemetal_dep],
  link_args: dxmt9_test_link_args,
)
test('dxmt9-range-upload-lock-scope-spec', range_upload_lock_scope_spec,
     env: dxmt9_native_test_env, timeout: 30)
test('dxmt9-range-upload-retired-selector-spec', range_upload_lock_scope_spec,
     env: dxmt9_native_test_env_vars + ['DXMT9_LOCK_SCOPE_REFINEMENT=0'],
     timeout: 30)

queue_lock_scope_refinement_spec = executable(
  'dxmt9-queue-lock-scope-refinement-spec',
  'queue_lock_scope_refinement_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep, dxmt9_winemetal_dep],
  link_args: dxmt9_test_link_args,
)
test('dxmt9-queue-lock-scope-refinement-spec', queue_lock_scope_refinement_spec,
     env: dxmt9_native_test_env_vars + ['DXMT9_PERF_UPLOAD_ARENA_LOCK_SPLIT=1'])
test('dxmt9-queue-lock-retired-selector-spec', queue_lock_scope_refinement_spec,
     env: dxmt9_native_test_env_vars + [
       'DXMT9_LOCK_SCOPE_REFINEMENT=0',
       'DXMT9_PERF_UPLOAD_ARENA_LOCK_SPLIT=1'])

render_pass_actions_spec = executable(
  'dxmt9-render-pass-actions-spec',
  'render_pass_actions_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-render-pass-actions-spec', render_pass_actions_spec,
     env: dxmt9_native_test_env_vars)

# R-FORMAT-12 — D3DFMT_NULL depth-only (colorless) render-pass attachment
# decisions. Pure value transforms (encoders::renderPassAdmitsRt0 /
# colorAttachmentIncluded) extracted from beginRenderPass so the
# color-omitted / depth-retained policy is unit-testable without a Metal
# device. Modeled on render_pass_actions_spec.cpp.
null_rt_attachment_spec = executable(
  'dxmt9-null-rt-attachment-spec',
  'null_rt_attachment_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-null-rt-attachment-spec', null_rt_attachment_spec)

# R-FORMAT-11 — RESZ MSAA->INTZ depth resolve. Covers the POINTSIZE-sentinel
# trigger detection (core::isReszDepthResolveSentinel) and the
# Device::reszDepthResolve DepthResolveDesc construction (MSAA source +
# INTZ level-0 destination), the latter driven through a RecordingBackend
# observer. Uses the tests/native/core fixture; no Metal device.
resz_depth_resolve_spec = executable(
  'dxmt9-resz-depth-resolve-spec',
  'resz_depth_resolve_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-resz-depth-resolve-spec', resz_depth_resolve_spec)

metal_encoder_recorder_spec = executable(
  'dxmt9-metal-encoder-recorder-spec',
  'metal_encoder_recorder_spec.cpp',
  include_directories: [
    dxmt9_inc,
    include_directories('../../../src'),
  ],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-metal-encoder-recorder-spec', metal_encoder_recorder_spec)

encode_draw_recorder_spec = executable(
  'dxmt9-encode-draw-recorder-spec',
  'encode_draw_recorder_spec.cpp',
  include_directories: [
    dxmt9_inc,
    include_directories('../../../src'),
  ],
  dependencies: [dxmt9_dep, dxmt9_frontend_dep, dxmt9_winemetal_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-encode-draw-recorder-spec', encode_draw_recorder_spec,
     env: dxmt9_native_test_env)

# P0-3 (specs/d3d9.plan.md §2) — D3DRS_STENCILREF pass-through.
# Before the fix, the encoder caller now in dxmt9_draw_encoder_draw.mm
# invoked recordedSetDepthStencilState without a stencilRef and the
# default-argument zero silently applied. This spec locks both the
# pure-value transform (state::computeStencilRef) and the
# encoder-driven depth bind so a future regression would break the
# test before it ships.
stencil_ref_spec = executable(
  'dxmt9-stencil-ref-spec',
  'stencil_ref_spec.cpp',
  include_directories: [
    dxmt9_inc,
    include_directories('../../../src'),
  ],
  dependencies: [dxmt9_dep, dxmt9_frontend_dep, dxmt9_winemetal_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-stencil-ref-spec', stencil_ref_spec,
     env: dxmt9_native_test_env)

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
     args: [allocation_counter_spec],
     env: dxmt9_native_test_env,
     depends: dxmt9_winemetal_unix_install_name_fixup)

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

# R-BACK-5.8 / 5.11 — DYNAMIC DISCARD and MANAGED writable-upload backing
# versioning. CPU-only spec: drives Pool create/map/upload on a synthetic NULL
# device so ring shape and sequence watermarks are exercised without a real
# Metal allocation. It also pins MANAGED no-wait maps, upload-time rotation,
# full-shadow copy, sequence-gated reuse, and aggregate destroy lifetime.
# Live shared-mode behavior is covered by the runtime integration suite.
# dxmt9_winemetal_dep is needed because BufferRecord
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

# R-BACK-5.8 / R-BACK-43.4 — real Metal-resource lifetime oracle. This
# Objective-C++ fixture allocates Shared MTLBuffers and exercises Pool's
# capture/rotation/reuse path without submitting a command buffer; it skips
# on hosts without a Metal device and makes no GPU/pixel claim.
render_tape_buffer_lifetime_metal_spec = executable(
  'dxmt9-render-tape-buffer-lifetime-metal-spec',
  'render_tape_buffer_lifetime_metal_spec.mm',
  include_directories: dxmt9_inc,
  dependencies: [
    dxmt9_dep,
    dxmt9_winemetal_dep,
    dependency('appleframeworks', modules: ['Foundation', 'Metal']),
  ],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-render-tape-buffer-lifetime-metal-spec',
     render_tape_buffer_lifetime_metal_spec,
     env: dxmt9_native_test_env_vars + ['MTL_DEBUG_LAYER=1'],
     is_parallel: false,
     timeout: 30)

# R-BACK-13.* — Tile-shader FFP selector + MSL emit specs.
tile_ffp_selector_spec = executable(
  'dxmt9-tile-ffp-selector-spec',
  'tile_ffp_selector_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-tile-ffp-selector-spec', tile_ffp_selector_spec)
test('dxmt9-tile-ffp-auto-fallback-spec', tile_ffp_selector_spec,
     args: ['auto-fallback'])

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
  # dxmt9_winemetal_dep: the resource-array populator tests construct an
  # ArgbufEncoderResource on the stack, whose WMT::Reference<ArgumentEncoder>
  # dtor drags NSObject_release into the link (same reason as
  # dxmt9-argbuf-populator-spec). The spec never calls into Metal
  # (suppressMetalCalls=true).
  dependencies: [dxmt9_dep, dxmt9_frontend_dep, dxmt9_winemetal_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-argbuf-hybrid-spec', argbuf_hybrid_spec,
     env: dxmt9_native_test_env)

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

test('dxmt9-argbuf-hybrid-msl-spec', argbuf_hybrid_msl_spec,
     env: dxmt9_native_test_env)

shader_argbuf_binding_value_spec = executable(
  'dxmt9-shader-argbuf-binding-value-spec',
  'shader_argbuf_binding_value_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-shader-argbuf-binding-value-spec',
     shader_argbuf_binding_value_spec,
     env: dxmt9_native_test_env)

# R-BACK-12.22..12.26 — Stage 2 argbuf-hybrid populator spec.
# CPU-only spec for the per-encoder argbuf populator: encoder-resource
# init shape, dirtyBytesEstimate value-transform across categories,
# PopulatedArgbuf default. Hardware exercise of openArgbuf /
# populateConstantBuffers / shader-runner equality is covered by the
# runtime-integration suite.
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

test('dxmt9-argbuf-populator-spec', argbuf_populator_spec,
     env: dxmt9_native_test_env)

replay_offload_queue_spec = executable(
  'dxmt9-replay-offload-queue-spec',
  'replay_offload_queue_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep, dxmt9_winemetal_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-replay-offload-queue-spec', replay_offload_queue_spec,
     env: dxmt9_native_test_env)

wait_trace_spec = executable(
  'dxmt9-wait-trace-spec', 'wait_trace_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep, dxmt9_winemetal_dep],
  link_args: dxmt9_test_link_args,
)
test('dxmt9-wait-trace-spec', python3,
  args: [join_paths(meson.project_source_root(), 'tests/scripts/test_wait_trace.py'),
         wait_trace_spec.full_path(),
         join_paths(meson.project_source_root(), 'scripts/tools/summarize_wait_trace.py')],
  env: dxmt9_native_test_env)

cpu_supply_trace_spec = executable(
  'dxmt9-cpu-supply-trace-spec', 'cpu_supply_trace_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep, dxmt9_winemetal_dep],
  link_args: dxmt9_test_link_args,
)
test('dxmt9-cpu-supply-trace-spec', python3,
  args: [join_paths(meson.project_source_root(), 'tests/scripts/test_cpu_supply_trace.py'),
         cpu_supply_trace_spec.full_path()], env: dxmt9_native_test_env)

cpu_ready_early_prefix_spec = executable(
  'dxmt9-cpu-ready-early-prefix-spec',
  'cpu_ready_early_prefix_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-cpu-ready-early-prefix-spec', cpu_ready_early_prefix_spec,
     env: dxmt9_native_test_env)

presence_table_spec = executable(
  'dxmt9-presence-table-spec',
  'presence_table_spec.cpp',
  include_directories: include_directories('../../../src/d3d9'),
  dependencies: [dxmt9_dep],
  link_args: dxmt9_test_link_args,
)
test('dxmt9-presence-table-spec', presence_table_spec,
     env: dxmt9_native_test_env)

replay_byte_identity_spec = executable(
  'dxmt9-replay-byte-identity-spec',
  'replay_byte_identity_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep, dxmt9_winemetal_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-replay-byte-identity-off-spec', replay_byte_identity_spec,
     env: dxmt9_native_test_env_vars + [
       'DXMT9_OFFLOAD_COMMIT_REPLAY=0',
       'DXMT9_CPU_READY_TAPE=0',
     ])
test('dxmt9-replay-byte-identity-on-spec', replay_byte_identity_spec,
     env: dxmt9_native_test_env_vars + [
       'DXMT9_OFFLOAD_COMMIT_REPLAY=1',
       'DXMT9_CPU_READY_TAPE=0',
     ])
test('dxmt9-replay-byte-identity-tape-oversize-spec',
     replay_byte_identity_spec,
     args: ['--oversize'],
     env: dxmt9_native_test_env_vars + [
       'DXMT9_OFFLOAD_COMMIT_REPLAY=1',
       'DXMT9_CPU_READY_TAPE=1',
     ])

present_ordinal_boundary_spec = executable(
  'dxmt9-present-ordinal-boundary-spec',
  'present_ordinal_boundary_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep, dxmt9_winemetal_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-present-ordinal-boundary-spec', present_ordinal_boundary_spec,
     env: dxmt9_native_test_env)

# Producer mark / reclaim shared predicates + TLC trace conformance. Binds
# `specs/verification/tla/ProducerMarkReclaim.tla` to the C++ predicates in
# `src/dxmt9/dxmt9_mark_reclaim_predicates.hpp` that `Pool::mark*Use` and
# `HandleArena::reclaimCompleted` call. Header-only: no Metal, no queue.
producer_mark_reclaim_spec = executable(
  'dxmt9-producer-mark-reclaim-spec',
  'producer_mark_reclaim_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-producer-mark-reclaim-spec', producer_mark_reclaim_spec,
     env: dxmt9_native_test_env)

# Managed buffer mutation offload shared predicates + TLC trace conformance.
# Binds `specs/verification/tla/BufferMutationOffload.tla` to the C++
# predicates in `src/dxmt9/dxmt9_mutation_offload_predicates.hpp` that the
# R-BACK-44.x offload mode will call from `Buffer::unlock` and the
# `ReplayOffloadQueue` drain loop. Header-only: no Metal, no queue.
buffer_mutation_offload_spec = executable(
  'dxmt9-buffer-mutation-offload-spec',
  'buffer_mutation_offload_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-buffer-mutation-offload-spec', buffer_mutation_offload_spec,
     env: dxmt9_native_test_env)

# R-BACK-44.9..44.11 observation-only composition ledger. This test exercises
# source-qualified A->B->A generations, first-use/CPU-observer barriers,
# conservative NOOVERWRITE range accounting, dead DISCARD chains, and failure
# conservation. The observer is header-only and remains default-disabled.
mutation_composition_observer_spec = executable(
  'dxmt9-mutation-composition-observer-spec',
  'mutation_composition_observer_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-mutation-composition-observer-spec',
     mutation_composition_observer_spec,
     env: dxmt9_native_test_env)

# R-BACK-44.x production half: the pool rotate/apply split, the offload queue's
# reserve/commit/release transaction, the worker's one-FIFO dispatch across
# chunks and mutations, and the end-to-end unlock transaction over real
# core::Buffer / D9CBuffer / Pool objects. Registered twice because
# `DXMT9_MANAGED_MUTATION_OFFLOAD` is read once per process. The engine
# default is ON (2026-08-25 promotion), so the rollback registration pins
# the explicit `0` lane (must resolve to the synchronous path) and the
# `-mode-on` one covers the mode itself.
managed_mutation_offload_transaction_spec = executable(
  'dxmt9-managed-mutation-offload-transaction-spec',
  'managed_mutation_offload_transaction_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep, dxmt9_winemetal_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-managed-mutation-offload-transaction-spec',
     managed_mutation_offload_transaction_spec,
     env: dxmt9_native_test_env_vars + ['DXMT9_MANAGED_MUTATION_OFFLOAD=0'])

test('dxmt9-managed-mutation-offload-transaction-spec-mode-on',
     managed_mutation_offload_transaction_spec,
     env: dxmt9_native_test_env_vars + ['DXMT9_MANAGED_MUTATION_OFFLOAD=1'])

# R-HARN-REPLAY-8.4 cold schedule binding: fixed Tape-event rows are
# authenticated and bounded before effects, then bind real qualified queue
# source identities only at production publication time. This is deliberately
# separate from the CPU provider runner so its negative controls cannot pass
# through a requested route or fabricated source identity.
render_tape_completion_plan_spec = executable(
  'dxmt9-render-tape-completion-plan-spec',
  'render_tape_completion_plan_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-render-tape-completion-plan-spec',
     render_tape_completion_plan_spec,
     env: dxmt9_native_test_env)

render_tape_event_completion_binding_spec = executable(
  'dxmt9-render-tape-event-completion-binding-spec',
  'render_tape_event_completion_binding_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)
test('dxmt9-render-tape-event-completion-binding-spec',
     render_tape_event_completion_binding_spec,
     env: dxmt9_native_test_env)

render_tape_publication_spec = executable(
  'dxmt9-render-tape-publication-spec',
  'render_tape_publication_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)
test('dxmt9-render-tape-publication-spec',
     render_tape_publication_spec,
     env: dxmt9_native_test_env)

# Second, bounded CPU-general admission over an already-built general-provider
# plan. This is a pure narrowing gate; command/tape validation remains owned by
# the provider and no Metal device is linked by the spec.
render_tape_cpu_general_grammar_spec = executable(
  'dxmt9-render-tape-cpu-general-grammar-spec',
  'render_tape_cpu_general_grammar_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-render-tape-cpu-general-grammar-spec',
     render_tape_cpu_general_grammar_spec,
     env: dxmt9_native_test_env)

render_tape_cpu_snapshot_proof_spec = executable(
  'dxmt9-render-tape-cpu-snapshot-proof-spec',
  'render_tape_cpu_snapshot_proof_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)
test('dxmt9-render-tape-cpu-snapshot-proof-spec',
     render_tape_cpu_snapshot_proof_spec,
     env: dxmt9_native_test_env)

render_tape_cpu_work_boundary_spec = executable(
  'dxmt9-render-tape-cpu-work-boundary-spec',
  'render_tape_cpu_work_boundary_spec.cpp',
  include_directories: dxmt9_inc,
)
test('dxmt9-render-tape-cpu-work-boundary-spec',
     render_tape_cpu_work_boundary_spec,
     env: dxmt9_native_test_env)

render_tape_cpu_mutation_boundary_spec = executable(
  'dxmt9-render-tape-cpu-mutation-boundary-spec',
  'render_tape_cpu_mutation_boundary_spec.cpp',
  include_directories: dxmt9_inc,
)
test('dxmt9-render-tape-cpu-mutation-boundary-spec',
     render_tape_cpu_mutation_boundary_spec,
     env: dxmt9_native_test_env)

# Layer 3 of the semantic-isomorphism plan (design brief §5): the deterministic
# interleaving harness that discharges the C++ memory-order obligation the
# model cannot see. Real threads driving the REAL `CommandQueue` (arena-lease
# fixture), the REAL `nextSeqId_` atomic + `mutex_`, and the REAL `Pool` /
# `HandleArena` mark and reclaim paths. `meson.project_source_root()` is passed
# as argv[1] for the production memory-order source contract.
producer_interleaving_spec = executable(
  'dxmt9-producer-interleaving-spec',
  'producer_interleaving_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep, dxmt9_winemetal_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-producer-interleaving-spec', producer_interleaving_spec,
     args: [meson.project_source_root()],
     env: dxmt9_native_test_env,
     timeout: 300)

# R-BACK-2.54 / R-BACK-2.56 command-chunk canonical replay/lifetime scaffolds.
foreach spec : [
  ['chunk-record-replay', 'chunk_record_replay_spec.cpp'],
]
  target = executable(
    'dxmt9-' + spec[0] + '-spec',
    spec[1],
    include_directories: dxmt9_inc,
    dependencies: [dxmt9_dep, dxmt9_frontend_dep],
    link_args: dxmt9_test_link_args,
  )
  test('dxmt9-' + spec[0] + '-spec', target,
       env: dxmt9_native_test_env)
endforeach

chunk_record_allocation_spec = executable(
  'dxmt9-chunk-record-allocation-spec',
  [
    'chunk_record_allocation_spec.cpp',
    '../../../src/d3d9/d3d9_pe_chunk_builder.cpp',
    '../../../src/d3d9/d3d9_pe_chunk_draw.cpp',
    '../../../src/d3d9/d3d9_pe_chunk_nondraw.cpp',
    '../../../src/d3d9/d3d9_pe_producer.cpp',
  ],
  include_directories: [
    dxmt9_inc,
    include_directories('../../../src/d3d9'),
  ],
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)
test('dxmt9-chunk-record-allocation-spec',
     chunk_record_allocation_spec,
     env: dxmt9_native_test_env)

# Pure parsing, admission and exact-identity checks shared with the opt-in
# draw-texture diagnostic observer.
draw_texture_dump_policy_spec = executable(
  'dxmt9-draw-texture-dump-policy-spec',
  'draw_texture_dump_policy_spec.cpp',
  include_directories: [dxmt9_inc, include_directories('../../../src')],
)
test('dxmt9-draw-texture-dump-policy-spec', draw_texture_dump_policy_spec,
     env: dxmt9_native_test_env)
