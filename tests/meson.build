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

bash = find_program('bash', required: true)
verify_tla_script = join_paths(meson.project_source_root(), 'scripts', 'verify_tla.sh')
test('dxmt9-verify-tla', bash, args: [verify_tla_script], is_parallel: false)
