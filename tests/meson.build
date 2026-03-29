smoke = executable(
  'dxmt9-smoke',
  'smoke.cpp',
  include_directories: dxmt9_inc,
  dependencies: dxmt9_dep,
)

test('dxmt9-smoke', smoke)

core_spec = executable(
  'dxmt9-core-spec',
  'core_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: dxmt9_dep,
)

test('dxmt9-core-spec', core_spec)

shader_runner = executable(
  'shader_runner_dxmt9',
  'shader_runner_dxmt9.cpp',
  include_directories: dxmt9_inc,
  dependencies: dxmt9_dep,
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
test('dxmt9-drift-report', bash, args: [join_paths(meson.project_source_root(), 'scripts/check_drift.sh')],
     is_parallel: false)
test('dxmt9-corpus-sync-smoke', python3,
     args: [join_paths(meson.project_source_root(), 'tests/corpus_sync_smoke.py')],
     is_parallel: false)
