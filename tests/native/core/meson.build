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

# Factory HRESULT / enum validation parity. Locks the shared
# dxmt9::com / core::Factory validation contract behind the PE-side
# IDirect3D9 / IDirect3D9Ex wrapper (src/d3d9/d3d9_pe_factory.cpp):
# CheckDeviceType adapter/availability, CheckDeviceMultiSampleType NONE
# quality level + unsupported sample-count rejection, and Ex
# adapter-ordinal validation (EnumAdapterModesEx /
# GetAdapterDisplayModeEx / GetAdapterLUID / CreateDeviceEx mode
# relation). Wine oracle lives in the PE conformance functions in
# tests/conformance/d3d9 (read, not modified).
core_factory_validation_spec = executable(
  'dxmt9-core-factory-validation-spec',
  'core_factory_validation_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-core-factory-validation-spec', core_factory_validation_spec,
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

# Byte-equal determinism + sensitivity regression guard for the MSL
# source generators (translator + FFP + dispatch wrapper). Pairs with
# the FFP variant-key determinism spec
# (tests/native/backend/ffp_key_determinism_spec.cpp), which covers
# state -> key, by covering key -> source. Pure source-level value
# comparison; no Metal device, no env-var dependencies.
shader_source_determinism_spec = executable(
  'dxmt9-shader-source-determinism-spec',
  'shader_source_determinism_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-shader-source-determinism-spec', shader_source_determinism_spec,
     env: dxmt9_native_test_env_vars + ['DXMT_DEBUG_DISABLE_SHADER_ARCHIVE=1'])

# Decoder safe-rejection spec — defense-in-depth on malformed D3DBC
# bytecode input (truncated / unsupported version / OOB register /
# missing END / invalid opcode). DXMT_PERF_COUNTERS=1 is required so
# the count*() helpers are not no-ops during the snapshot deltas.
shader_bytecode_validation_spec = executable(
  'dxmt9-shader-bytecode-validation-spec',
  'shader_bytecode_validation_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-shader-bytecode-validation-spec', shader_bytecode_validation_spec,
     env: dxmt9_native_test_env_vars + ['DXMT_DEBUG_DISABLE_SHADER_ARCHIVE=1', 'DXMT_PERF_COUNTERS=1'])

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

# G2-D unknown-disposition audit follow-up: regression guard for the
# IDirect3DBaseTexture9::GenerateMipSubLevels promotion across Texture,
# CubeTexture, and VolumeTexture (PE wrappers in
# src/d3d9/d3d9_pe_device_child_surface.cpp). Exercises the C ABI
# (dxmt9c_texture_generate_mip_sublevels) and the AUTOGENMIPMAP usage
# round-trip the PE-side gate depends on.
core_d3d9_generate_mipmaps_spec = executable(
  'dxmt9-core-d3d9-generate-mipmaps-spec',
  'core_d3d9_generate_mipmaps_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-core-d3d9-generate-mipmaps-spec', core_d3d9_generate_mipmaps_spec,
     env: dxmt9_native_test_env_vars + ['DXMT_DEBUG_DISABLE_SHADER_ARCHIVE=1'])

core_stateblock_restore_spec = executable(
  'dxmt9-core-stateblock-restore-spec',
  'core_stateblock_restore_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-core-stateblock-restore-spec', core_stateblock_restore_spec,
     env: dxmt9_native_test_env_vars + ['DXMT_DEBUG_DISABLE_SHADER_ARCHIVE=1'])

# Wine D3D9 behavioral oracle: test_miptree_layout. Pure value-level
# pin on the NPOT mip-chain dimension / pitch / byte-size contract for
# A8R8G8B8 and DXT1 textures; mirrors the observable lock-rect layout
# without touching a device.
core_d3d9_miptree_layout_spec = executable(
  'dxmt9-core-d3d9-miptree-layout-spec',
  'core_d3d9_miptree_layout_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

# IDirect3DDevice9::Set/GetGammaRamp PE-side shadow round-trip (G2-B
# track, Option B). Mirrors the D3DGAMMARAMP-shaped
# { WORD red[256]; WORD green[256]; WORD blue[256]; } byte layout at
# value level so we do not need to instantiate D3D9DeviceImpl
# (Windows-only build).
core_d3d9_gamma_ramp_spec = executable(
  'dxmt9-core-d3d9-gamma-ramp-spec',
  'core_d3d9_gamma_ramp_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-core-d3d9-miptree-layout-spec', core_d3d9_miptree_layout_spec,
     env: dxmt9_native_test_env_vars + ['DXMT_DEBUG_DISABLE_SHADER_ARCHIVE=1'])

test('dxmt9-core-d3d9-gamma-ramp-spec', core_d3d9_gamma_ramp_spec,
     env: dxmt9_native_test_env_vars + ['DXMT_DEBUG_DISABLE_SHADER_ARCHIVE=1'])

# Wine D3D9 behavioral oracle: test_multiply_transform. Pure value-level
# pin on the row-major 4x4 multiply convention used by
# IDirect3DDevice9::MultiplyTransform (M_current * M_new) — companion
# to the stateblock conformance test
# tests/conformance/d3d9/d3d9_conformance_query_stateblock.c::
# test_stateblock_multiply_transform_capture.
core_d3d9_multiply_transform_spec = executable(
  'dxmt9-core-d3d9-multiply-transform-spec',
  'core_d3d9_multiply_transform_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-core-d3d9-multiply-transform-spec', core_d3d9_multiply_transform_spec,
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
