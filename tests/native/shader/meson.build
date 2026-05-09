# Shader translator transform spec.

shader_transform_spec = executable(
  'dxmt9-shader-transform-spec',
  'shader_transform_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-shader-transform-spec', shader_transform_spec)
