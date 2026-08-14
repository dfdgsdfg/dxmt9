# Wine/PE bridge chunk-record, ops-dispatch, and setBytes wiring specs.

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

pe_buffer_lock_order_spec = executable(
  'dxmt9-pe-buffer-lock-order-spec',
  'pe_buffer_lock_order_spec.cpp',
  include_directories: [
    dxmt9_inc,
    include_directories('../../../src/d3d9'),
  ],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-pe-buffer-lock-order-spec',
     pe_buffer_lock_order_spec,
     env: dxmt9_native_test_env)

# Regression guard for DXMT9_PE_DRAW_FULL_SNAPSHOT (Phase 16 debug knob).
# Runs the real producer in both modes over identical inputs and proves the
# snapshot record is self-contained and the delta record is a subset of it. See
# the header comment in the spec source for what this covers, and for what the
# earlier mirror-based version covered that this does not.
pe_full_snapshot_equivalence_spec = executable(
  'dxmt9-pe-full-snapshot-equivalence-spec',
  [
    'pe_full_snapshot_equivalence_spec.cpp',
    '../../../src/d3d9/d3d9_pe_producer.cpp',
  ],
  include_directories: [
    dxmt9_inc,
    include_directories('../../../src/d3d9'),
  ],
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-pe-full-snapshot-equivalence-spec',
     pe_full_snapshot_equivalence_spec,
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

wmt_store_action_dispatch_spec = executable(
  'dxmt9-wmt-store-action-dispatch-spec',
  'wmt_store_action_dispatch_spec.cpp',
  include_directories: [
    dxmt9_inc,
    include_directories('../../../src'),
  ],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-wmt-store-action-dispatch-spec', wmt_store_action_dispatch_spec)

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

# R-BACK-2.54 / R-BACK-2.55 command-chunk canonical ABI scaffolds. A1 pins the
# layout/schema; later plan waves replace the focused behavior-test bodies.
foreach spec : [
  ['chunk-record-layout', 'chunk_record_layout_spec.cpp'],
  ['chunk-record-registry', 'chunk_record_registry_spec.cpp'],
  ['chunk-record-validation', 'chunk_record_validation_spec.cpp'],
  ['render-tape', 'render_tape_spec.cpp'],
  ['render-tape-capture', 'render_tape_capture_spec.cpp'],
  ['render-tape-projection', 'render_tape_projection_spec.cpp'],
  ['render-tape-provider', 'render_tape_provider_spec.cpp'],
]
  sources = [spec[1]]
  if spec[0] == 'render-tape-capture'
    sources += [
      '../../../src/d3d9/d3d9_pe_chunk_builder.cpp',
      '../../../src/d3d9/d3d9_pe_chunk_nondraw.cpp',
    ]
  endif
  dependencies = [dxmt9_dep, dxmt9_frontend_dep]
  if spec[0] == 'render-tape-provider'
    dependencies += [dxmt9_winemetal_dep]
  endif
  target = executable(
    'dxmt9-' + spec[0] + '-spec',
    sources,
    include_directories: dxmt9_inc,
    dependencies: dependencies,
    link_args: dxmt9_test_link_args,
  )
  if spec[0] == 'render-tape-provider'
    test('dxmt9-' + spec[0] + '-spec', target,
         env: dxmt9_native_test_env,
         timeout: 60)
  else
    test('dxmt9-' + spec[0] + '-spec', target,
         env: dxmt9_native_test_env)
  endif
  if spec[0] == 'render-tape'
    render_tape_fixture_spec = target
  elif spec[0] == 'render-tape-capture'
    render_tape_capture_fixture_spec = target
  elif spec[0] == 'render-tape-projection'
    render_tape_projection_fixture_spec = target
  elif spec[0] == 'render-tape-provider'
    render_tape_provider_fixture_spec = target
  endif
endforeach

test('dxmt9-render-tape-cli-spec', python3,
     args: [
       join_paths(meson.project_source_root(),
                  'scripts/check/test_render_tape_cli.py'),
       render_tape_fixture_spec,
       dxmt9_render_tape_tool,
       join_paths(meson.project_source_root(),
                  'scripts/tools/run_dxmt9_render_tape.py'),
     ],
     env: dxmt9_native_test_env)

test('dxmt9-render-tape-capture-cli-spec', python3,
     args: [
       join_paths(meson.project_source_root(),
                  'scripts/check/test_render_tape_capture_cli.py'),
       render_tape_capture_fixture_spec,
       dxmt9_render_tape_tool,
       join_paths(meson.project_source_root(),
                  'scripts/tools/run_dxmt9_render_tape.py'),
       join_paths(meson.project_source_root(),
                  'scripts/tools/run_dxmt9_render_tape_capture_fixture.py'),
     ],
     env: dxmt9_native_test_env)

test('dxmt9-render-tape-projection-cli-spec', python3,
     args: [
       join_paths(meson.project_source_root(),
                  'scripts/check/test_render_tape_projection_cli.py'),
       render_tape_projection_fixture_spec,
       dxmt9_render_tape_tool,
     ],
     env: dxmt9_native_test_env)

test('dxmt9-render-tape-provider-cli-spec', python3,
     args: [
       join_paths(meson.project_source_root(),
                  'scripts/check/test_render_tape_provider_cli.py'),
       render_tape_provider_fixture_spec,
       dxmt9_render_tape_provider_tool,
       dxmt9_render_tape_tool,
       join_paths(meson.project_source_root(),
                  'scripts/tools/run_dxmt9_render_tape.py'),
     ],
     env: dxmt9_native_test_env,
     timeout: 120)

render_tape_publisher_spec = executable(
  'dxmt9-render-tape-publisher-spec',
  [
    'render_tape_publisher_spec.cpp',
    '../../../src/d3d9/d3d9_pe_render_tape_publisher.cpp',
  ],
  include_directories: [
    dxmt9_inc,
    include_directories('../../../src/d3d9'),
  ],
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)
test('dxmt9-render-tape-publisher-spec',
     render_tape_publisher_spec,
     env: dxmt9_native_test_env)

pe_chunk_record_value_spec = executable(
  'dxmt9-pe-chunk-record-value-spec',
  [
    'pe_chunk_record_value_spec.cpp',
    '../../../src/d3d9/d3d9_pe_chunk_builder.cpp',
    '../../../src/d3d9/d3d9_pe_chunk_draw.cpp',
    '../../../src/d3d9/d3d9_pe_chunk_nondraw.cpp',
  ],
  include_directories: [
    dxmt9_inc,
    include_directories('../../../src/d3d9'),
  ],
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)
test('dxmt9-pe-chunk-record-value-spec',
     pe_chunk_record_value_spec,
     env: dxmt9_native_test_env)

pe_shadow_native_spec = executable(
  'dxmt9-pe-shadow-native-spec',
  ['pe_shadow_native_spec.cpp'],
  include_directories: [
    dxmt9_inc,
    include_directories('../../../src/d3d9'),
  ],
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)
test('dxmt9-pe-shadow-native-spec',
     pe_shadow_native_spec,
     env: dxmt9_native_test_env)

pe_producer_views_spec = executable(
  'dxmt9-pe-producer-views-spec',
  [
    'pe_producer_views_spec.cpp',
    '../../../src/d3d9/d3d9_pe_producer.cpp',
  ],
  include_directories: [
    dxmt9_inc,
    include_directories('../../../src/d3d9'),
  ],
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)
test('dxmt9-pe-producer-views-spec',
     pe_producer_views_spec,
     env: dxmt9_native_test_env)

pe_producer_differential_spec = executable(
  'dxmt9-pe-producer-differential-spec',
  [
    'pe_producer_differential_spec.cpp',
    '../../../src/d3d9/d3d9_pe_producer.cpp',
    '../../../src/d3d9/d3d9_pe_chunk_builder.cpp',
    '../../../src/d3d9/d3d9_pe_chunk_draw.cpp',
    '../../../src/d3d9/d3d9_pe_chunk_nondraw.cpp',
  ],
  include_directories: [
    dxmt9_inc,
    include_directories('../../../src/d3d9'),
  ],
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  cpp_args: [
    '-DDXMT9_PE_GOLDEN_PATH="' + meson.current_source_dir() +
    '/pe_producer_golden.txt"',
  ],
  link_args: dxmt9_test_link_args,
)
test('dxmt9-pe-producer-differential-spec',
     pe_producer_differential_spec,
     env: dxmt9_native_test_env)
