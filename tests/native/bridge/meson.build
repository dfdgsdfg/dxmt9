# Wine/PE bridge chunk-record, ops-dispatch, and setBytes wiring specs.

render_tape_event_checkpoint_authority_spec = executable(
  'dxmt9-render-tape-event-checkpoint-authority-spec',
  'render_tape_event_checkpoint_authority_spec.cpp',
  include_directories: [dxmt9_inc, include_directories('../../../src/d3d9')],
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)
test('dxmt9-render-tape-event-checkpoint-authority-spec',
     render_tape_event_checkpoint_authority_spec, env: dxmt9_native_test_env)

pe_retainer_spec = executable(
  'dxmt9-pe-retainer-spec',
  'pe_retainer_spec.cpp',
  include_directories: [
    dxmt9_inc,
    include_directories('../../../src/d3d9'),
  ],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-pe-retainer-spec', pe_retainer_spec)

pe_semantic_projection_spec = executable(
  'dxmt9-pe-semantic-projection-spec',
  'pe_semantic_projection_spec.cpp',
  include_directories: [
    dxmt9_inc,
    include_directories('../../../src/d3d9'),
  ],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-pe-semantic-projection-spec', pe_semantic_projection_spec)

# Fixed-role SegmentedTransportV1 semantic-batch seam. This host-only target
# feeds every production producer family through the immutable owner and
# compares ExactFixed with canonical contiguous D9C V2 bytes.
pe_semantic_batch_spec = executable(
  'dxmt9-pe-semantic-batch-spec',
  [
    'pe_semantic_batch_spec.cpp',
    '../../../src/d3d9/d3d9_pe_chunk_builder.cpp',
    '../../../src/d3d9/d3d9_pe_batch.cpp',
    '../../../src/d3d9/d3d9_pe_chunk_draw.cpp',
    '../../../src/d3d9/d3d9_pe_chunk_nondraw.cpp',
  ],
  include_directories: [
    dxmt9_inc,
    include_directories('../../../src/d3d9'),
  ],
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  cpp_args: ['-DDXMT9_PE_SHADOW_TEST_SEAM=1'],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-pe-semantic-batch-spec', pe_semantic_batch_spec)

# P1 host-only semantic owner coverage. Admission copies typed fixed values,
# constants/UP bytes, rectangles, and sparse rows while typed physical pins
# stay owned until reset/settlement. The real sparse producer is wired here for
# the compact max-family differential; the generic builder remains its oracle.
pe_semantic_owner_spec = executable(
  'dxmt9-pe-semantic-owner-spec',
  [
    'pe_semantic_owner_spec.cpp',
    '../../../src/d3d9/d3d9_pe_producer.cpp',
    '../../../src/d3d9/d3d9_pe_chunk_builder.cpp',
    '../../../src/d3d9/d3d9_pe_chunk_draw.cpp',
    '../../../src/d3d9/d3d9_pe_chunk_nondraw.cpp',
  ],
  include_directories: [
    dxmt9_inc,
    include_directories('../../../src/d3d9'),
  ],
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  cpp_args: ['-DDXMT9_PE_SHADOW_TEST_SEAM=1'],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-pe-semantic-owner-spec', pe_semantic_owner_spec)
foreach ledger_gate : ['0', '1']
  test('dxmt9-pe-semantic-owner-ledger-' + ledger_gate + '-spec',
       pe_semantic_owner_spec,
       env: ['DXMT9_PERF_COPY_MATERIALIZATION_LEDGER=' + ledger_gate])
endforeach

# Closed constant-settlement effects: no owner mutation or callback authority.
pe_constant_settlement_spec = executable(
  'dxmt9-pe-constant-settlement-spec',
  'pe_constant_settlement_spec.cpp',
  include_directories: [dxmt9_inc, include_directories('../../../src/d3d9')],
  dependencies: [dxmt9_util_dep],
  link_args: dxmt9_test_link_args,
)
test('dxmt9-pe-constant-settlement-spec', pe_constant_settlement_spec)

pe_append_preparation_spec = executable(
  'dxmt9-pe-append-preparation-spec',
  ['pe_append_preparation_spec.cpp',
   '../../../src/d3d9/d3d9_pe_producer.cpp',
   '../../../src/d3d9/d3d9_pe_chunk_builder.cpp',
   '../../../src/d3d9/d3d9_pe_chunk_draw.cpp',
   '../../../src/d3d9/d3d9_pe_chunk_nondraw.cpp'],
  include_directories: [dxmt9_inc, include_directories('../../../src/d3d9')],
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  cpp_args: ['-DDXMT9_PE_SHADOW_TEST_SEAM=1'],
  link_args: dxmt9_test_link_args,
)
test('dxmt9-pe-append-preparation-spec', pe_append_preparation_spec)

pe_binding_reuse_spec = executable(
  'dxmt9-pe-binding-reuse-spec',
  'pe_binding_reuse_spec.cpp',
  include_directories: [dxmt9_inc, include_directories('../../../src/d3d9')],
  dependencies: [dxmt9_util_dep],
  link_args: dxmt9_test_link_args,
)
test('dxmt9-pe-binding-reuse-spec', pe_binding_reuse_spec)

pe_aggregate_retainer_spec = executable(
  'dxmt9-pe-aggregate-retainer-spec',
  ['pe_aggregate_retainer_spec.cpp', '../../../src/d3d9/d3d9_pe_producer.cpp'],
  include_directories: [dxmt9_inc, include_directories('../../../src/d3d9')],
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  cpp_args: ['-DDXMT9_PE_SHADOW_TEST_SEAM=1'],
  link_args: dxmt9_test_link_args,
)
test('dxmt9-pe-aggregate-retainer-spec', pe_aggregate_retainer_spec)

pe_stream_metadata_spec = executable(
  'dxmt9-pe-stream-metadata-spec',
  'pe_stream_metadata_spec.cpp',
  include_directories: include_directories('../../../src/d3d9'),
  link_args: dxmt9_test_link_args,
)
test('dxmt9-pe-stream-metadata-spec', pe_stream_metadata_spec)

pe_fvf_transaction_spec = executable(
  'dxmt9-pe-fvf-transaction-spec',
  'pe_fvf_transaction_spec.cpp',
  include_directories: include_directories('../../../src/d3d9'),
  link_args: dxmt9_test_link_args,
)

test('dxmt9-pe-fvf-transaction-spec', pe_fvf_transaction_spec)

pe_level_surface_cache_spec = executable(
  'dxmt9-pe-level-surface-cache-spec',
  'pe_level_surface_cache_spec.cpp',
  include_directories: [
    dxmt9_inc,
    include_directories('../../../src/d3d9'),
  ],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-pe-level-surface-cache-spec', pe_level_surface_cache_spec)

pe_buffer_lock_order_spec = executable(
  'dxmt9-pe-buffer-lock-order-spec',
  'pe_buffer_lock_order_spec.cpp',
  include_directories: [
    dxmt9_inc,
    include_directories('../../../src/d3d9'),
  ],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-pe-buffer-lock-order-spec',
     pe_buffer_lock_order_spec,
     env: dxmt9_native_test_env)

# Regression guard for DXMT9_PE_DRAW_FULL_SNAPSHOT (Phase 16 debug knob).
# Runs the real producer in both modes over identical inputs and proves the
# snapshot record is self-contained and the delta record is a subset of it. See
# the header comment in the spec source for what this covers, and for what the
# earlier mirror-based version covered that this does not.
pe_full_snapshot_equivalence_spec = executable(
  'dxmt9-pe-full-snapshot-equivalence-spec',
  [
    'pe_full_snapshot_equivalence_spec.cpp',
    '../../../src/d3d9/d3d9_pe_producer.cpp',
    '../../../src/d3d9/d3d9_pe_tape_bootstrap.cpp',
    '../../../src/d3d9/d3d9_pe_chunk_builder.cpp',
    '../../../src/d3d9/d3d9_pe_chunk_draw.cpp',
  ],
  include_directories: [
    dxmt9_inc,
    include_directories('../../../src/d3d9'),
  ],
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  cpp_args: ['-DDXMT9_PE_SHADOW_TEST_SEAM=1'],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-pe-full-snapshot-equivalence-spec',
     pe_full_snapshot_equivalence_spec,
     env: dxmt9_native_test_env)

# Host coverage for the pure core of DXMT9_PE_THREAD_SAMPLER: module-range
# normalization, PC -> module classification, the bounded self-PC histogram,
# and the deterministic top-N selections. The Win32 sampling thread in the same
# header is compile-checked only by the PE lanes.
pe_thread_sampler_spec = executable(
  'dxmt9-pe-thread-sampler-spec',
  'pe_thread_sampler_spec.cpp',
  include_directories: [
    dxmt9_inc,
    include_directories('../../../src/d3d9'),
  ],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-pe-thread-sampler-spec', pe_thread_sampler_spec)

# Bounded native refinement for the PE autogen-mipmap binding mask and draw
# settlement protocol. This target is host-only and has no Metal/GPU/Wine
# dependency; the source contains its independent dense state oracle.
pe_autogen_bound_slots_spec = executable(
  'dxmt9-pe-autogen-bound-slots-spec',
  'pe_autogen_bound_slots_spec.cpp',
  include_directories: [
    dxmt9_inc,
    include_directories('../../../src/d3d9'),
  ],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-pe-autogen-bound-slots-spec', pe_autogen_bound_slots_spec)

pe_diagnostics_spec = executable(
  'dxmt9-pe-diagnostics-spec',
  'pe_diagnostics_spec.cpp',
  include_directories: [
    dxmt9_inc,
    include_directories('../../../src'),
    include_directories('../../../src/d3d9'),
  ],
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-pe-diagnostics-spec',
     pe_diagnostics_spec,
     args: [meson.project_source_root()])
foreach ledger_gate : ['0', '1']
  test('dxmt9-pe-diagnostics-ledger-' + ledger_gate + '-spec',
       pe_diagnostics_spec,
       args: [meson.project_source_root()],
       env: ['DXMT9_PERF_COPY_MATERIALIZATION_LEDGER=' + ledger_gate])
endforeach

pe_recorder_fault_matrix_spec = executable(
  'dxmt9-pe-recorder-fault-matrix-spec',
  'pe_recorder_fault_matrix_spec.cpp',
  include_directories: [
    dxmt9_inc,
    include_directories('../../../src'),
    include_directories('../../../src/d3d9'),
  ],
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-pe-recorder-fault-matrix-spec', pe_recorder_fault_matrix_spec)

test('dxmt9-d3d9-fault-matrix-script-spec', python3,
     args: [join_paths(meson.project_source_root(),
                       'tests/scripts/test_d3d9_fault_matrix.py')],
     env: dxmt9_native_test_env)

wsi_surface_protocol_spec = executable(
  'dxmt9-wsi-surface-protocol-spec',
  'wsi_surface_protocol_spec.cpp',
  include_directories: dxmt9_inc,
  link_args: dxmt9_test_link_args,
)

test('dxmt9-wsi-surface-protocol-spec', wsi_surface_protocol_spec)

bridge_ops_spec = executable(
  'dxmt9-bridge-ops-spec',
  'bridge_ops_spec.cpp',
  bridge_codegen[0],
  include_directories: [
    dxmt9_inc,
    bridge_gen_inc,
    include_directories('../../../src'),
  ],
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-bridge-ops-spec', bridge_ops_spec)

# Codegen-owned ABI fingerprint coverage: declaration order and pointed POD
# record mutations must invalidate the handshake, while identical schemas stay
# deterministic. The test uses temporary headers and never edits generated
# build outputs.
test('dxmt9-bridge-codegen-abi-spec', python3,
     args: [join_paths(meson.project_source_root(),
                       'tests/scripts/test_bridge_codegen_abi.py')],
     env: dxmt9_native_test_env)

# PE ABI/codegen source audit is deliberately source-only here. Native tests
# must not require PE artifacts; the same script accepts a true builtin x64/x86
# build directory in the cross verification lane.
test('dxmt9-pe-device-abi-source-spec', python3,
     args: [join_paths(meson.project_source_root(),
                       'scripts/check/audit_d3d9_pe_abi_codegen.py'),
            '--source-only'],
     env: dxmt9_native_test_env)

test('dxmt9-pe-device-abi-audit-negative-spec', python3,
     args: [join_paths(meson.project_source_root(),
                       'tests/native/bridge/pe_abi_codegen_audit_spec.py')],
     env: dxmt9_native_test_env)

bridge_marshalling_value_spec = executable(
  'dxmt9-bridge-marshalling-value-spec',
  'bridge_marshalling_value_spec.cpp',
  bridge_codegen[0],
  include_directories: [
    dxmt9_inc,
    bridge_gen_inc,
    include_directories('../../../src'),
  ],
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-bridge-marshalling-value-spec', bridge_marshalling_value_spec)

# Stand-alone PE-side bridge test: links no provider, instead overriding
# the unix-call thunk so we can observe the wmtcmd struct that the
# RenderCommandEncoder wrappers in Metal.hpp produce. Keeps the
# setVertexBytes / setFragmentBytes discriminators honest without needing
# a live Metal device.
wmt_setbytes_dispatch_spec = executable(
  'dxmt9-wmt-setbytes-dispatch-spec',
  'wmt_setbytes_dispatch_spec.cpp',
  include_directories: [
    dxmt9_inc,
    include_directories('../../../src'),
  ],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-wmt-setbytes-dispatch-spec', wmt_setbytes_dispatch_spec)

wmt_store_action_dispatch_spec = executable(
  'dxmt9-wmt-store-action-dispatch-spec',
  'wmt_store_action_dispatch_spec.cpp',
  include_directories: [
    dxmt9_inc,
    include_directories('../../../src'),
  ],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-wmt-store-action-dispatch-spec', wmt_store_action_dispatch_spec)

# R-FORMAT-11 — winemetal depth-resolve ABI surface. Header-only: pins the
# WMTDepthAttachmentInfo resolve_texture / resolve_filter fields and the
# WMTMultisampleDepthResolveFilter enum added for the RESZ depth resolve.
wmt_depth_resolve_abi_spec = executable(
  'dxmt9-wmt-depth-resolve-abi-spec',
  'wmt_depth_resolve_abi_spec.cpp',
  include_directories: [
    dxmt9_inc,
    include_directories('../../../src'),
  ],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-wmt-depth-resolve-abi-spec', wmt_depth_resolve_abi_spec)

# Regression test for the wow64 texture-lock shadow allocation size.
# SFIV crashed at 0x1A4B1000 because a BC3 1x1 mip's shadow was sized
# to one 4KB page yet the game wrote past it. The helper that backs
# the fix is unit-tested here so the size formula stays honest.
texture_lock_shadow_spec = executable(
  'dxmt9-texture-lock-shadow-spec',
  'texture_lock_shadow_spec.cpp',
  include_directories: [
    dxmt9_inc,
    include_directories('../../../src'),
    include_directories('../../../src/d3d9'),
  ],
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-texture-lock-shadow-spec', texture_lock_shadow_spec)

# The provider spec creates and retires several native Metal devices in one
# process.  Metal's shader-binary archive can race teardown of those short-lived
# devices on macOS (including an objc_retain fault in Metal.DeviceArray), which
# otherwise presents as an unbounded native-test hang.  Provider replay is
# intentionally hermetic, so disable archive persistence for this test lane.
render_tape_provider_test_env = environment()
render_tape_provider_test_env.set('DXMT9_UPDATE_PE_PRODUCER_GOLDEN', '0')
render_tape_provider_test_env.set('DXMT_DEBUG_DISABLE_SHADER_ARCHIVE', '1')
if host_machine.system() == 'darwin'
  if dxmt9_wine_install_path != ''
    render_tape_provider_test_env.set(
      'DYLD_LIBRARY_PATH', dxmt9_wine_unix_runtime_path)
  endif
endif

# R-BACK-2.54 / R-BACK-2.55 command-chunk canonical ABI scaffolds. A1 pins the
# layout/schema; later plan waves replace the focused behavior-test bodies.
foreach spec : [
  ['chunk-record-layout', 'chunk_record_layout_spec.cpp'],
  ['chunk-record-registry', 'chunk_record_registry_spec.cpp'],
  ['chunk-record-validation', 'chunk_record_validation_spec.cpp'],
  ['render-tape', 'render_tape_spec.cpp'],
  ['render-tape-capture', 'render_tape_capture_spec.cpp'],
  ['render-tape-identity-v3', 'render_tape_identity_v3_spec.cpp'],
  ['render-tape-volume-layout', 'render_tape_volume_layout_spec.cpp'],
  ['render-tape-projection', 'render_tape_projection_spec.cpp'],
  ['render-tape-provider', 'render_tape_provider_spec.cpp'],
  ['render-tape-source-plan', 'render_tape_source_plan_spec.cpp'],
]
  sources = [spec[1]]
  if spec[0] == 'render-tape-capture'
    sources += [
      '../../../src/d3d9/d3d9_pe_chunk_builder.cpp',
      '../../../src/d3d9/d3d9_pe_chunk_nondraw.cpp',
    ]
  endif
  dependencies = [dxmt9_dep, dxmt9_frontend_dep]
  if spec[0] == 'render-tape-provider'
    dependencies += [dxmt9_winemetal_dep]
  endif
  target = executable(
    'dxmt9-' + spec[0] + '-spec',
    sources,
    include_directories: dxmt9_inc,
    dependencies: dependencies,
    link_args: dxmt9_test_link_args,
  )
  if spec[0] == 'render-tape-provider'
    test('dxmt9-' + spec[0] + '-spec', target,
         env: render_tape_provider_test_env,
         timeout: 60)
  else
    test('dxmt9-' + spec[0] + '-spec', target,
         env: dxmt9_native_test_env)
  endif
  if spec[0] == 'render-tape'
    render_tape_fixture_spec = target
  elif spec[0] == 'render-tape-capture'
    render_tape_capture_fixture_spec = target
  elif spec[0] == 'render-tape-projection'
    render_tape_projection_fixture_spec = target
  elif spec[0] == 'render-tape-provider'
    render_tape_provider_fixture_spec = target
  endif
endforeach

render_tape_query_test_env = environment()
render_tape_d24s8_test_env = environment()
render_tape_d24s8_test_env.set('DXMT9_UPDATE_PE_PRODUCER_GOLDEN', '0')
render_tape_d24s8_test_env.set('DXMT_DEBUG_DISABLE_SHADER_ARCHIVE', '1')
render_tape_d24s8_test_env.set('DXMT9_ASYNC_QUERY_FENCES', '1')
render_tape_d24s8_test_env.set('MTL_DEBUG_LAYER', '1')
if host_machine.system() == 'darwin' and dxmt9_wine_install_path != ''
  render_tape_d24s8_test_env.set('DYLD_LIBRARY_PATH', dxmt9_wine_unix_runtime_path)
endif
test('dxmt9-render-tape-d24s8-provider-metal-oracle',
     render_tape_provider_fixture_spec,
     args: ['--test-d24s8-oracle'],
     env: render_tape_d24s8_test_env,
     is_parallel: false,
     timeout: 60)
test('dxmt9-render-tape-rgba16f-metal-oracle',
     render_tape_provider_fixture_spec,
     args: ['--test-rgba16f-oracle'],
     env: render_tape_d24s8_test_env,
     is_parallel: false,
     timeout: 60)
test('dxmt9-render-tape-a8r8g8b8-metal-oracle',
     render_tape_provider_fixture_spec,
     args: ['--test-a8r8g8b8-oracle'],
     env: render_tape_d24s8_test_env,
     is_parallel: false,
     timeout: 60)
render_tape_query_test_env.set('DXMT9_UPDATE_PE_PRODUCER_GOLDEN', '0')
render_tape_query_test_env.set('DXMT_DEBUG_DISABLE_SHADER_ARCHIVE', '1')
render_tape_query_test_env.set('DXMT9_METAL_OCCLUSION', '1')
if host_machine.system() == 'darwin' and dxmt9_wine_install_path != ''
  render_tape_query_test_env.set('DYLD_LIBRARY_PATH', dxmt9_wine_unix_runtime_path)
endif
test('dxmt9-render-tape-query-metal-oracle',
     render_tape_provider_fixture_spec,
     args: ['--test-query-oracle'],
     env: render_tape_query_test_env,
     timeout: 120)
render_tape_event_checkpoint_test_env = environment()
render_tape_event_checkpoint_test_env.set('DXMT9_UPDATE_PE_PRODUCER_GOLDEN', '0')
render_tape_event_checkpoint_test_env.set('DXMT_DEBUG_DISABLE_SHADER_ARCHIVE', '1')
render_tape_event_checkpoint_test_env.set('DXMT9_ASYNC_QUERY_FENCES', '1')
render_tape_event_checkpoint_test_env.set('DXMT9_RENDER_TAPE_CAPTURE', '1')
render_tape_event_checkpoint_test_env.set('MTL_DEBUG_LAYER', '1')
if host_machine.system() == 'darwin' and dxmt9_wine_install_path != ''
  render_tape_event_checkpoint_test_env.set('DYLD_LIBRARY_PATH', dxmt9_wine_unix_runtime_path)
endif
test('dxmt9-render-tape-event-checkpoint-metal-oracle',
     render_tape_provider_fixture_spec,
     args: ['--test-event-checkpoint-oracle'],
     env: render_tape_event_checkpoint_test_env,
     is_parallel: false,
     timeout: 60)
test('dxmt9-render-tape-volume-metal-oracle',
     render_tape_provider_fixture_spec,
     args: ['--test-volume-oracle'],
     env: render_tape_query_test_env,
     timeout: 120)
test('dxmt9-render-tape-retained-query-budget',
     render_tape_provider_fixture_spec,
     args: ['--test-retained-query-budget'],
     env: render_tape_query_test_env,
     timeout: 120)

render_tape_seed_reference_env = environment()
render_tape_seed_reference_env.set('DXMT9_UPDATE_PE_PRODUCER_GOLDEN', '0')
render_tape_seed_reference_env.set('DXMT_DEBUG_DISABLE_SHADER_ARCHIVE', '1')
render_tape_seed_reference_env.set('DXMT9_ASYNC_QUERY_FENCES', '0')
if host_machine.system() == 'darwin' and dxmt9_wine_install_path != ''
  render_tape_seed_reference_env.set('DYLD_LIBRARY_PATH', dxmt9_wine_unix_runtime_path)
endif
test('dxmt9-render-tape-seed-reference-rejection',
     render_tape_provider_fixture_spec,
     args: ['--test-seed-reference-rejection'],
     env: render_tape_seed_reference_env,
     timeout: 120)

test('dxmt9-render-tape-query-receipts', python3,
     args: [join_paths(meson.project_source_root(),
                      'scripts/check/test_render_tape_query_receipts.py')],
     env: dxmt9_native_test_env)

render_tape_origin_locator_spec = executable(
  'dxmt9-render-tape-origin-locator-spec',
  'render_tape_origin_locator_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-render-tape-origin-locator-spec',
     render_tape_origin_locator_spec,
     env: dxmt9_native_test_env)

render_tape_first_access_locator_spec = executable(
  'dxmt9-render-tape-first-access-locator-spec',
  'render_tape_first_access_locator_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-render-tape-first-access-locator-spec',
     render_tape_first_access_locator_spec,
     env: dxmt9_native_test_env)

test('dxmt9-render-tape-cli-spec', python3,
     args: [
       join_paths(meson.project_source_root(),
                  'scripts/check/test_render_tape_cli.py'),
       render_tape_fixture_spec,
       dxmt9_render_tape_tool,
       join_paths(meson.project_source_root(),
                  'scripts/tools/run_dxmt9_render_tape.py'),
     ],
     env: dxmt9_native_test_env)

test('dxmt9-render-tape-capture-cli-spec', python3,
     args: [
       join_paths(meson.project_source_root(),
                  'scripts/check/test_render_tape_capture_cli.py'),
       render_tape_capture_fixture_spec,
       dxmt9_render_tape_tool,
       join_paths(meson.project_source_root(),
                  'scripts/tools/run_dxmt9_render_tape.py'),
       join_paths(meson.project_source_root(),
                  'scripts/tools/run_dxmt9_render_tape_capture_fixture.py'),
     ],
     env: dxmt9_native_test_env)

test('dxmt9-render-tape-projection-cli-spec', python3,
     args: [
       join_paths(meson.project_source_root(),
                  'scripts/check/test_render_tape_projection_cli.py'),
       render_tape_projection_fixture_spec,
       dxmt9_render_tape_tool,
       join_paths(meson.project_source_root(),
                  'scripts/tools/run_dxmt9_render_tape.py'),
       render_tape_provider_fixture_spec,
       dxmt9_render_tape_provider_tool,
     ],
     env: render_tape_provider_test_env,
     timeout: 120)

test('dxmt9-render-tape-policy-cli-spec', python3,
     args: [
       join_paths(meson.project_source_root(),
                  'scripts/check/test_render_tape_policy_cli.py'),
       render_tape_projection_fixture_spec,
       dxmt9_render_tape_tool,
       join_paths(meson.project_source_root(),
                  'scripts/tools/run_dxmt9_render_tape.py'),
     ],
     env: dxmt9_native_test_env)

test('dxmt9-render-tape-provider-cli-spec', python3,
     args: [
       join_paths(meson.project_source_root(),
                  'scripts/check/test_render_tape_provider_cli.py'),
       render_tape_provider_fixture_spec,
       dxmt9_render_tape_provider_tool,
       dxmt9_render_tape_tool,
       join_paths(meson.project_source_root(),
                  'scripts/tools/run_dxmt9_render_tape.py'),
     ],
     env: render_tape_provider_test_env,
     timeout: 120)

test('dxmt9-render-tape-cpu-source-projection-spec', python3,
     args: [join_paths(meson.project_source_root(),
                      'scripts/check/test_render_tape_cpu_source_projection.py')],
     env: render_tape_provider_test_env)
test('dxmt9-render-tape-cpu-source-projection-cli-spec', python3,
     args: [join_paths(meson.project_source_root(),
                      'scripts/check/test_render_tape_cpu_source_projection_cli.py'),
            cpu_ready_production_routing_spec, dxmt9_render_tape_tool,
            join_paths(meson.project_source_root(), 'scripts/tools/run_dxmt9_render_tape.py')],
     env: render_tape_provider_test_env,
     timeout: 90)

test('dxmt9-render-tape-cpu-policy-python-spec', python3,
     args: [join_paths(meson.project_source_root(),
                      'scripts/check/test_render_tape_cpu_policy.py')],
     env: dxmt9_native_test_env)

test('dxmt9-render-tape-cpu-policy-cli-spec', python3,
     args: [join_paths(meson.project_source_root(),
                      'scripts/check/test_render_tape_cpu_policy_cli.py'),
            cpu_ready_production_routing_spec, dxmt9_render_tape_tool,
            join_paths(meson.project_source_root(), 'scripts/tools/run_dxmt9_render_tape.py')],
     env: dxmt9_native_test_env,
     timeout: 120)

test('dxmt9-render-tape-gpu-policy-python-spec', python3,
     args: [join_paths(meson.project_source_root(),
                      'scripts/check/test_render_tape_gpu_policy.py')],
     env: dxmt9_native_test_env)

test('dxmt9-render-tape-gpu-policy-cli-spec', python3,
     args: [join_paths(meson.project_source_root(),
                      'scripts/check/test_render_tape_gpu_policy_cli.py'),
            render_tape_provider_fixture_spec,
            cpu_ready_production_routing_spec,
            dxmt9_render_tape_provider_tool,
            dxmt9_render_tape_tool,
            join_paths(meson.project_source_root(),
                       'scripts/tools/run_dxmt9_render_tape.py')],
     env: render_tape_provider_test_env,
     timeout: 300)

test('dxmt9-render-tape-compilation-lineage-spec', python3,
     args: [join_paths(meson.project_source_root(),
                      'scripts/check/test_render_tape_compilation_lineage.py'),
            render_tape_provider_fixture_spec, dxmt9_render_tape_provider_tool,
            dxmt9_render_tape_tool,
            join_paths(meson.project_source_root(),
                       'scripts/tools/run_dxmt9_render_tape.py')],
     env: render_tape_provider_test_env,
     is_parallel: false,
     timeout: 120)

test('dxmt9-render-tape-cpu-experiment-cli-spec', python3,
     args: [
       join_paths(meson.project_source_root(),
                  'scripts/check/test_render_tape_cpu_experiment_cli.py'),
       cpu_ready_production_routing_spec,
       dxmt9_render_tape_tool,
       join_paths(meson.project_source_root(),
                  'scripts/tools/run_dxmt9_render_tape.py'),
     ],
     env: dxmt9_native_test_env,
     timeout: 120)

render_tape_publisher_spec = executable(
  'dxmt9-render-tape-publisher-spec',
  [
    'render_tape_publisher_spec.cpp',
    '../../../src/d3d9/d3d9_pe_render_tape_publisher.cpp',
  ],
  include_directories: [
    dxmt9_inc,
    include_directories('../../../src/d3d9'),
  ],
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)
test('dxmt9-render-tape-publisher-spec',
     render_tape_publisher_spec,
     env: dxmt9_native_test_env)

pe_chunk_record_value_spec = executable(
  'dxmt9-pe-chunk-record-value-spec',
  [
    'pe_chunk_record_value_spec.cpp',
    '../../../src/d3d9/d3d9_pe_chunk_builder.cpp',
    '../../../src/d3d9/d3d9_pe_batch.cpp',
    '../../../src/d3d9/d3d9_pe_chunk_draw.cpp',
    '../../../src/d3d9/d3d9_pe_chunk_nondraw.cpp',
  ],
  include_directories: [
    dxmt9_inc,
    include_directories('../../../src/d3d9'),
  ],
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  cpp_args: ['-DDXMT9_PE_SHADOW_TEST_SEAM=1'],
  link_args: dxmt9_test_link_args,
)
test('dxmt9-pe-chunk-record-value-spec',
     pe_chunk_record_value_spec,
     env: dxmt9_native_test_env)

pe_shadow_native_spec = executable(
  'dxmt9-pe-shadow-native-spec',
  ['pe_shadow_native_spec.cpp'],
  include_directories: [
    dxmt9_inc,
    include_directories('../../../src/d3d9'),
  ],
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  cpp_args: ['-DDXMT9_PE_SHADOW_TEST_SEAM=1'],
  link_args: dxmt9_test_link_args,
)
test('dxmt9-pe-shadow-native-spec',
     pe_shadow_native_spec,
     env: dxmt9_native_test_env)

pe_transition_algebra_spec = executable(
  'dxmt9-pe-transition-algebra-spec',
  ['pe_transition_algebra_spec.cpp'],
  include_directories: [
    dxmt9_inc,
    include_directories('../../../src/d3d9'),
  ],
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)
test('dxmt9-pe-transition-algebra-spec',
     pe_transition_algebra_spec,
     env: dxmt9_native_test_env)

pe_commit_transition_spec = executable(
  'dxmt9-pe-commit-transition-spec',
  [
    'pe_commit_transition_spec.cpp',
    '../../../src/d3d9/d3d9_pe_chunk_builder.cpp',
    '../../../src/d3d9/d3d9_pe_chunk_nondraw.cpp',
  ],
  include_directories: [
    dxmt9_inc,
    include_directories('../../../src/d3d9'),
  ],
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)
test('dxmt9-pe-commit-transition-spec',
     pe_commit_transition_spec,
     env: dxmt9_native_test_env)

pe_typed_slot_spec = executable(
  'dxmt9-pe-typed-slot-spec',
  ['pe_typed_slot_spec.cpp'],
  include_directories: [
    dxmt9_inc,
    include_directories('../../../src/d3d9'),
  ],
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  cpp_args: ['-DDXMT9_PE_SHADOW_TEST_SEAM=1'],
  link_args: dxmt9_test_link_args,
)
test('dxmt9-pe-typed-slot-spec',
     pe_typed_slot_spec,
     env: dxmt9_native_test_env)

pe_stateblock_category_spec = executable(
  'dxmt9-pe-stateblock-category-spec',
  ['pe_stateblock_category_spec.cpp'],
  include_directories: [
    dxmt9_inc,
    include_directories('../../../src/d3d9'),
  ],
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  cpp_args: [
    '-DDXMT9_STATEBLOCK_TEST_SEAM=1',
    '-DDXMT9_PE_SHADOW_TEST_SEAM=1',
  ],
  link_args: dxmt9_test_link_args,
)
test('dxmt9-pe-stateblock-category-spec',
     pe_stateblock_category_spec,
     env: dxmt9_native_test_env)

pe_producer_views_spec = executable(
  'dxmt9-pe-producer-views-spec',
  [
    'pe_producer_views_spec.cpp',
    '../../../src/d3d9/d3d9_pe_producer.cpp',
  ],
  include_directories: [
    dxmt9_inc,
    include_directories('../../../src/d3d9'),
  ],
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  cpp_args: ['-DDXMT9_PE_SHADOW_TEST_SEAM=1'],
  link_args: dxmt9_test_link_args,
)
test('dxmt9-pe-producer-views-spec',
     pe_producer_views_spec,
     env: dxmt9_native_test_env)

pe_producer_differential_spec = executable(
  'dxmt9-pe-producer-differential-spec',
  [
    'pe_producer_differential_spec.cpp',
    '../../../src/d3d9/d3d9_pe_producer.cpp',
    '../../../src/d3d9/d3d9_pe_chunk_builder.cpp',
    '../../../src/d3d9/d3d9_pe_chunk_draw.cpp',
    '../../../src/d3d9/d3d9_pe_chunk_nondraw.cpp',
  ],
  include_directories: [
    dxmt9_inc,
    include_directories('../../../src/d3d9'),
  ],
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  cpp_args: [
    '-DDXMT9_PE_SHADOW_TEST_SEAM=1',
    '-DDXMT9_PE_GOLDEN_PATH="' + meson.current_source_dir() +
    '/pe_producer_golden.txt"',
  ],
  link_args: dxmt9_test_link_args,
)
test('dxmt9-pe-producer-differential-spec',
     pe_producer_differential_spec,
     env: dxmt9_native_test_env)

pe_com_membership_spec = executable(
  'dxmt9-pe-com-membership-spec',
  ['pe_com_membership_spec.cpp'],
  include_directories: [
    dxmt9_inc,
    include_directories('../../../src/d3d9'),
  ],
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)
test('dxmt9-pe-com-membership-spec',
     pe_com_membership_spec,
     env: dxmt9_native_test_env)

pe_shadow_capability_surface_spec = executable(
  'dxmt9-pe-shadow-capability-surface-spec',
  ['pe_shadow_capability_surface_spec.cpp'],
  include_directories: [
    dxmt9_inc,
    include_directories('../../../src/d3d9'),
  ],
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)
test('dxmt9-pe-shadow-capability-surface-spec',
     pe_shadow_capability_surface_spec,
     env: dxmt9_native_test_env)


pe_com_cache_spec = executable(
  'dxmt9-pe-com-cache-spec',
  ['pe_com_cache_spec.cpp'],
  include_directories: [include_directories('../../../src/d3d9')],
  dependencies: [dxmt9_dep],
  link_args: dxmt9_test_link_args,
)
test('dxmt9-pe-com-cache-spec', pe_com_cache_spec,
     env: dxmt9_native_test_env)

pe_public_allocation_spec = executable(
  'dxmt9-pe-public-allocation-spec',
  'pe_public_allocation_spec.cpp',
  include_directories: include_directories('../../../src/d3d9'),
  link_args: dxmt9_test_link_args,
)

test('dxmt9-pe-public-allocation-spec', pe_public_allocation_spec)

draw_parent_completion_spec = executable(
  'dxmt9-draw-parent-completion-spec',
  'draw_parent_completion_spec.cpp',
  link_args: dxmt9_test_link_args,
)

test('dxmt9-draw-parent-completion-spec', draw_parent_completion_spec)
