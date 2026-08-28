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

cpu_ready_production_routing_spec = executable(
  'dxmt9-cpu-ready-production-routing-spec',
  'cpu_ready_production_routing_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep, dxmt9_winemetal_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-cpu-ready-production-routing-spec',
     cpu_ready_production_routing_spec,
     env: dxmt9_native_test_env_vars + [
       'DXMT9_OFFLOAD_COMMIT_REPLAY=1',
     ])

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

resource_hazard_spec = executable(
  'dxmt9-resource-hazard-spec',
  'resource_hazard_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep, dxmt9_winemetal_dep],
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
