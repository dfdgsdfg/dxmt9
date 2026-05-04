dxmt9_test_link_args = []
if host_machine.system() == 'darwin'
  dxmt9_test_link_args += meson.get_compiler('cpp').get_supported_link_arguments([
    '-Wl,-no_warn_duplicate_libraries',
  ])
endif

smoke = executable(
  'dxmt9-smoke',
  'smoke.cpp',
  include_directories: dxmt9_inc,
  dependencies: dxmt9_dep,
  link_args: dxmt9_test_link_args,
)

test('dxmt9-smoke', smoke)

core_spec = executable(
  'dxmt9-core-spec',
  'core_spec.cpp',
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
  'state_draw_transform_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-state-draw-transform-spec', state_draw_transform_spec)

shader_transform_spec = executable(
  'dxmt9-shader-transform-spec',
  'shader_transform_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-shader-transform-spec', shader_transform_spec)

backend_key_descriptor_spec = executable(
  'dxmt9-backend-key-descriptor-spec',
  'backend_key_descriptor_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-backend-key-descriptor-spec', backend_key_descriptor_spec)

backend_pipeline_key_spec = executable(
  'dxmt9-backend-pipeline-key-spec',
  'backend_pipeline_key_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-backend-pipeline-key-spec', backend_pipeline_key_spec)

chunk_record_spec = executable(
  'dxmt9-chunk-record-spec',
  'chunk_record_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-chunk-record-spec', chunk_record_spec)

chunk_record_import_spec = executable(
  'dxmt9-chunk-record-import-spec',
  'chunk_record_import_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-chunk-record-import-spec', chunk_record_import_spec)

resource_hazard_spec = executable(
  'dxmt9-resource-hazard-spec',
  'resource_hazard_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-resource-hazard-spec', resource_hazard_spec)

shader_runner = executable(
  'shader_runner_dxmt9',
  'shader_runner_dxmt9.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

if host_machine.system() == 'darwin'
  shader_manifest = join_paths(meson.project_source_root(), 'tests/shader_tests/MANIFEST.toml')
  test('dxmt9-shader-corpus', shader_runner, args: ['--manifest', shader_manifest], is_parallel: false)
endif

bash = find_program('bash', required: true)
python3 = find_program('python3', required: true)
verify_tla_script = join_paths(meson.project_source_root(), 'scripts', 'verify_tla.sh')
test('dxmt9-verify-tla', bash, args: [verify_tla_script], is_parallel: false)
test('dxmt9-manifest-check', bash, args: [join_paths(meson.project_source_root(), 'scripts/check_manifest.sh')],
     is_parallel: false)
test('dxmt9-d3d9-conformance-manifest-check', bash,
     args: [join_paths(meson.project_source_root(), 'scripts/check_d3d9_conformance_manifest.sh')],
     is_parallel: false)
test('dxmt9-drift-report', bash, args: [join_paths(meson.project_source_root(), 'scripts/check_drift.sh')],
     is_parallel: false)
test('dxmt9-corpus-sync-smoke', python3,
     args: [join_paths(meson.project_source_root(), 'tests/corpus_sync_smoke.py')],
     is_parallel: false)
