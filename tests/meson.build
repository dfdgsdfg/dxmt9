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
