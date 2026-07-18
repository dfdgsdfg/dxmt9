# Wine/PE bridge chunk-record, ops-dispatch, and setBytes wiring specs.

chunk_record_spec = executable(
  'dxmt9-chunk-record-spec',
  'chunk_record_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-chunk-record-spec', chunk_record_spec)

chunk_record_validation_spec = executable(
  'dxmt9-chunk-record-validation-spec',
  'chunk_record_validation_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-chunk-record-validation-spec', chunk_record_validation_spec,
     env: dxmt9_native_test_env)

chunk_record_hazard_spec = executable(
  'dxmt9-chunk-record-hazard-spec',
  'chunk_record_hazard_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-chunk-record-hazard-spec', chunk_record_hazard_spec,
     env: dxmt9_native_test_env)

chunk_record_replay_spec = executable(
  'dxmt9-chunk-record-replay-spec',
  'chunk_record_replay_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-chunk-record-replay-spec', chunk_record_replay_spec,
     env: dxmt9_native_test_env)

chunk_record_import_spec = executable(
  'dxmt9-chunk-record-import-spec',
  'chunk_record_import_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-chunk-record-import-spec', chunk_record_import_spec,
     env: dxmt9_native_test_env)

pe_chunk_record_value_spec = executable(
  'dxmt9-pe-chunk-record-value-spec',
  'pe_chunk_record_value_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-pe-chunk-record-value-spec', pe_chunk_record_value_spec,
     env: dxmt9_native_test_env)

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

# Regression guard for DXMT9_PE_DRAW_FULL_SNAPSHOT (Phase 16 debug knob).
# Proves that the delta-mode packet stream and the full-snapshot-mode
# packet stream produce identical unix-side effective state for
# representative D3D9 draw scenarios. See header comment in the spec
# source for the equivalence claim and the modelling boundary.
pe_full_snapshot_equivalence_spec = executable(
  'dxmt9-pe-full-snapshot-equivalence-spec',
  'pe_full_snapshot_equivalence_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-pe-full-snapshot-equivalence-spec',
     pe_full_snapshot_equivalence_spec,
     env: dxmt9_native_test_env)

# R-BACK-2.52 T3: on/off replay-equivalence spec for the opt-in inline
# const-delta wire mode (DXMT9_PE_INLINE_CONST_DELTA). Follows the
# pe_full_snapshot_equivalence_spec pattern -- hand-constructed wire records,
# no PE recorder, no live D9CDevice/Metal backend.
pe_inline_const_delta_equivalence_spec = executable(
  'dxmt9-pe-inline-const-delta-equivalence-spec',
  'pe_inline_const_delta_equivalence_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-pe-inline-const-delta-equivalence-spec',
     pe_inline_const_delta_equivalence_spec,
     env: dxmt9_native_test_env)

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

# R-BACK-2.54 / R-BACK-2.55 command-chunk V2 ABI scaffolds. A1 pins the
# layout/schema; later plan waves replace the focused behavior-test bodies.
foreach spec : [
  ['chunk-record-v2-layout', 'chunk_record_v2_layout_spec.cpp'],
  ['chunk-record-v2-registry', 'chunk_record_v2_registry_spec.cpp'],
  ['chunk-record-v2-validation', 'chunk_record_v2_validation_spec.cpp'],
  ['chunk-record-v1-v2-equivalence', 'chunk_record_v1_v2_equivalence_spec.cpp'],
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

pe_chunk_record_v2_value_spec = executable(
  'dxmt9-pe-chunk-record-v2-value-spec',
  [
    'pe_chunk_record_v2_value_spec.cpp',
    '../../../src/d3d9/d3d9_pe_chunk_v2_builder.cpp',
    '../../../src/d3d9/d3d9_pe_chunk_v2_draw.cpp',
    '../../../src/d3d9/d3d9_pe_chunk_v2_nondraw.cpp',
  ],
  include_directories: [
    dxmt9_inc,
    include_directories('../../../src/d3d9'),
  ],
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)
test('dxmt9-pe-chunk-record-v2-value-spec',
     pe_chunk_record_v2_value_spec,
     env: dxmt9_native_test_env)
