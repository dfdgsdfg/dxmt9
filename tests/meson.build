dxmt9_test_link_args = []
if host_machine.system() == 'darwin'
  dxmt9_test_link_args += meson.get_compiler('cpp').get_supported_link_arguments([
    '-Wl,-no_warn_duplicate_libraries',
  ])
endif

smoke = executable(
  'dxmt9-smoke',
  'native/smoke/smoke.cpp',
  include_directories: dxmt9_inc,
  dependencies: dxmt9_dep,
  link_args: dxmt9_test_link_args,
)

test('dxmt9-smoke', smoke)

core_spec = executable(
  'dxmt9-core-spec',
  'native/core/core_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-core-spec', core_spec,
     env: ['DXMT_DEBUG_DISABLE_SHADER_ARCHIVE=1'])
test('dxmt9-core-spec-pixel-vflip-contract', core_spec,
     env: ['DXMT9_CORE_SPEC_SOURCE_CONTRACT_ONLY=1', 'DXMT_DEBUG_FORCE_PIXEL_V_FLIP=1'])
test('dxmt9-core-spec-vertex-yflip-contract', core_spec,
     env: ['DXMT9_CORE_SPEC_SOURCE_CONTRACT_ONLY=1', 'DXMT_DEBUG_FLIP_VERTEX_Y=1'])

state_draw_transform_spec = executable(
  'dxmt9-state-draw-transform-spec',
  'native/core/state_draw_transform_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-state-draw-transform-spec', state_draw_transform_spec)

dod_state_format_spec = executable(
  'dxmt9-dod-state-format-spec',
  'native/core/dod_state_format_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-dod-state-format-spec', dod_state_format_spec)

shader_transform_spec = executable(
  'dxmt9-shader-transform-spec',
  'native/shader/shader_transform_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-shader-transform-spec', shader_transform_spec)

backend_key_descriptor_spec = executable(
  'dxmt9-backend-key-descriptor-spec',
  'native/backend/backend_key_descriptor_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-backend-key-descriptor-spec', backend_key_descriptor_spec)

backend_pipeline_key_spec = executable(
  'dxmt9-backend-pipeline-key-spec',
  'native/backend/backend_pipeline_key_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-backend-pipeline-key-spec', backend_pipeline_key_spec)

dod_replay_observer_spec = executable(
  'dxmt9-dod-replay-observer-spec',
  'native/backend/dod_replay_observer_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-dod-replay-observer-spec', dod_replay_observer_spec)

chunk_record_spec = executable(
  'dxmt9-chunk-record-spec',
  'native/bridge/chunk_record_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-chunk-record-spec', chunk_record_spec)

chunk_record_import_spec = executable(
  'dxmt9-chunk-record-import-spec',
  'native/bridge/chunk_record_import_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-chunk-record-import-spec', chunk_record_import_spec)

bridge_ops_spec = executable(
  'dxmt9-bridge-ops-spec',
  'native/bridge/bridge_ops_spec.cpp',
  bridge_codegen[0],
  include_directories: [
    dxmt9_inc,
    bridge_gen_inc,
    include_directories('../src'),
  ],
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-bridge-ops-spec', bridge_ops_spec)

resource_hazard_spec = executable(
  'dxmt9-resource-hazard-spec',
  'native/backend/resource_hazard_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-resource-hazard-spec', resource_hazard_spec)

allocation_counter_spec = executable(
  'dxmt9-allocation-counter-spec',
  'native/backend/allocation_counter_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

assert_perf_counters = find_program(
  join_paths(meson.project_source_root(), 'scripts', 'assert_perf_counters.py'),
  required: true,
)

test('dxmt9-allocation-counter-spec', assert_perf_counters,
     args: [allocation_counter_spec])

shader_runner = executable(
  'shader_runner_dxmt9',
  'shader_runner/shader_runner_dxmt9.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

if host_machine.system() == 'darwin'
  shader_manifest = join_paths(meson.project_source_root(), 'tests/shader_runner/corpus/MANIFEST.toml')
  test('dxmt9-shader-corpus', shader_runner, args: ['--manifest', shader_manifest], is_parallel: false)
endif

bash = find_program('bash', required: true)
python3 = find_program('python3', required: true)
verify_tla_script = join_paths(meson.project_source_root(), 'scripts', 'verify_tla.sh')
test('dxmt9-verify-tla', bash, args: [verify_tla_script], is_parallel: false)
test('dxmt9-manifest-check', bash, args: [join_paths(meson.project_source_root(), 'scripts/check_manifest.sh')],
     is_parallel: false)
test('dxmt9-shader-corpus-gaps', python3,
     args: [
       join_paths(meson.project_source_root(), 'scripts/shader_corpus_tool.py'),
       'gaps',
       '--fail-on-metadata-gaps',
     ],
     is_parallel: false)
test('dxmt9-d3d9-conformance-manifest-check', bash,
     args: [join_paths(meson.project_source_root(), 'scripts/check_d3d9_conformance_manifest.sh')],
     is_parallel: false)
test('dxmt9-d3d9-conformance-status-report', python3,
     args: [join_paths(meson.project_source_root(), 'scripts/d3d9_conformance_status.py')],
     is_parallel: false)
test('dxmt9-drift-report', bash, args: [join_paths(meson.project_source_root(), 'scripts/check_drift.sh')],
     is_parallel: false)
test('dxmt9-corpus-sync-smoke', python3,
     args: [join_paths(meson.project_source_root(), 'tests/shader_runner/corpus_sync_smoke.py')],
     is_parallel: false)
