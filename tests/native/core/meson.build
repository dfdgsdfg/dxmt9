# Core device, FFP, format, shader translator, draw-state, and DOD specs.

core_device_lifecycle_spec = executable(
  'dxmt9-core-device-lifecycle-spec',
  'core_device_lifecycle_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-core-device-lifecycle-spec', core_device_lifecycle_spec,
     env: dxmt9_native_test_env_vars + ['DXMT_DEBUG_DISABLE_SHADER_ARCHIVE=1'])

core_device_com_spec = executable(
  'dxmt9-core-device-com-spec',
  'core_device_com_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-core-device-com-spec', core_device_com_spec,
     env: dxmt9_native_test_env_vars + ['DXMT_DEBUG_DISABLE_SHADER_ARCHIVE=1'])

core_device_coverage_spec = executable(
  'dxmt9-core-device-coverage-spec',
  'core_device_coverage_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-core-device-coverage-spec', core_device_coverage_spec,
     env: dxmt9_native_test_env_vars + ['DXMT_DEBUG_DISABLE_SHADER_ARCHIVE=1'])

core_ffp_state_key_spec = executable(
  'dxmt9-core-ffp-state-key-spec',
  'core_ffp_state_key_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-core-ffp-state-key-spec', core_ffp_state_key_spec,
     env: dxmt9_native_test_env_vars + ['DXMT_DEBUG_DISABLE_SHADER_ARCHIVE=1'])

core_format_caps_spec = executable(
  'dxmt9-core-format-caps-spec',
  'core_format_caps_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-core-format-caps-spec', core_format_caps_spec,
     env: dxmt9_native_test_env_vars + ['DXMT_DEBUG_DISABLE_SHADER_ARCHIVE=1'])

core_shader_translator_spec = executable(
  'dxmt9-core-shader-translator-spec',
  'core_shader_translator_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-core-shader-translator-spec', core_shader_translator_spec,
     env: dxmt9_native_test_env_vars + ['DXMT_DEBUG_DISABLE_SHADER_ARCHIVE=1'])
test('dxmt9-core-shader-translator-spec-pixel-vflip-contract', core_shader_translator_spec,
     env: dxmt9_native_test_env_vars + ['DXMT9_CORE_SPEC_SOURCE_CONTRACT_ONLY=1', 'DXMT_DEBUG_FORCE_PIXEL_V_FLIP=1'])
test('dxmt9-core-shader-translator-spec-vertex-yflip-contract', core_shader_translator_spec,
     env: dxmt9_native_test_env_vars + ['DXMT9_CORE_SPEC_SOURCE_CONTRACT_ONLY=1', 'DXMT_DEBUG_FLIP_VERTEX_Y=1'])

state_draw_transform_spec = executable(
  'dxmt9-state-draw-transform-spec',
  'state_draw_transform_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-state-draw-transform-spec', state_draw_transform_spec,
     env: dxmt9_native_test_env_vars)

draw_uniforms_layout_spec = executable(
  'dxmt9-draw-uniforms-layout-spec',
  'draw_uniforms_layout_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-draw-uniforms-layout-spec', draw_uniforms_layout_spec,
     env: dxmt9_native_test_env_vars)

draw_uniforms_dirty_spec = executable(
  'dxmt9-draw-uniforms-dirty-spec',
  'draw_uniforms_dirty_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-draw-uniforms-dirty-spec', draw_uniforms_dirty_spec,
     env: dxmt9_native_test_env_vars)

dod_state_format_spec = executable(
  'dxmt9-dod-state-format-spec',
  'dod_state_format_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-dod-state-format-spec', dod_state_format_spec,
     env: dxmt9_native_test_env_vars)

resource_format_boundary_spec = executable(
  'dxmt9-resource-format-boundary-spec',
  'resource_format_boundary_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-resource-format-boundary-spec', resource_format_boundary_spec,
     env: dxmt9_native_test_env_vars + ['DXMT_DEBUG_DISABLE_SHADER_ARCHIVE=1'])

# CPU-only chunk-build microbenchmark (V1 audit item (a), boundary B1).
# Times the cost of building POD chunk records from a typical D3D9
# hot-path mix isolated from any bridge / Metal / GPU work. Output
# scrape line: `[chunk_record_micro] iterations=... mean_ns=... ...`.
chunk_record_micro_spec = executable(
  'dxmt9-chunk-record-micro-spec',
  'chunk_record_micro_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-chunk-record-micro-spec', chunk_record_micro_spec,
     env: dxmt9_native_test_env_vars)
