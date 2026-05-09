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
