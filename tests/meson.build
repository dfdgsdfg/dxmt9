dxmt9_test_link_args = []
dxmt9_native_test_env = environment()
dxmt9_native_test_env_vars = []
if host_machine.system() == 'darwin'
  dxmt9_test_link_args += meson.get_compiler('cpp').get_supported_link_arguments([
    '-Wl,-no_warn_duplicate_libraries',
  ])
  dxmt9_wine_unix_runtime_subdir = host_machine.cpu_family() == 'x86_64' ? 'x86_64-unix' : host_machine.cpu_family() + '-unix'
  dxmt9_c_args = get_option('c_args')
  if dxmt9_c_args.length() >= 2
    foreach i : range(dxmt9_c_args.length() - 1)
      if dxmt9_c_args[i] == '-arch'
        dxmt9_arch_name = dxmt9_c_args[i + 1]
        if dxmt9_arch_name == 'x86_64'
          dxmt9_wine_unix_runtime_subdir = 'x86_64-unix'
        elif dxmt9_arch_name == 'arm64'
          dxmt9_wine_unix_runtime_subdir = 'aarch64-unix'
        endif
      endif
    endforeach
  endif
  dxmt9_wine_install_path = get_option('wine_install_path')
  if dxmt9_wine_install_path != ''
    if not dxmt9_wine_install_path.startswith('/')
      dxmt9_wine_install_path = join_paths(meson.project_source_root(), dxmt9_wine_install_path)
    endif
    dxmt9_wine_unix_runtime_path = join_paths(dxmt9_wine_install_path, 'lib/wine', dxmt9_wine_unix_runtime_subdir)
    dxmt9_native_test_env.prepend(
      'DYLD_LIBRARY_PATH',
      dxmt9_wine_unix_runtime_path,
      separator: ':')
    dxmt9_native_test_env_vars += 'DYLD_LIBRARY_PATH=' + dxmt9_wine_unix_runtime_path
  endif
endif

smoke = executable(
  'dxmt9-smoke',
  'native/smoke/smoke.cpp',
  include_directories: dxmt9_inc,
  dependencies: dxmt9_dep,
  link_args: dxmt9_test_link_args,
)

test('dxmt9-smoke', smoke)

core_device_flow_spec = executable(
  'dxmt9-core-device-flow-spec',
  'native/core/core_device_flow_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-core-device-flow-spec', core_device_flow_spec,
     env: dxmt9_native_test_env_vars + ['DXMT_DEBUG_DISABLE_SHADER_ARCHIVE=1'])

core_ffp_state_key_spec = executable(
  'dxmt9-core-ffp-state-key-spec',
  'native/core/core_ffp_state_key_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-core-ffp-state-key-spec', core_ffp_state_key_spec,
     env: dxmt9_native_test_env_vars + ['DXMT_DEBUG_DISABLE_SHADER_ARCHIVE=1'])

core_format_caps_spec = executable(
  'dxmt9-core-format-caps-spec',
  'native/core/core_format_caps_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-core-format-caps-spec', core_format_caps_spec,
     env: dxmt9_native_test_env_vars + ['DXMT_DEBUG_DISABLE_SHADER_ARCHIVE=1'])

core_shader_translator_spec = executable(
  'dxmt9-core-shader-translator-spec',
  'native/core/core_shader_translator_spec.cpp',
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
  'native/core/state_draw_transform_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-state-draw-transform-spec', state_draw_transform_spec,
     env: dxmt9_native_test_env_vars)

dod_state_format_spec = executable(
  'dxmt9-dod-state-format-spec',
  'native/core/dod_state_format_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-dod-state-format-spec', dod_state_format_spec,
     env: dxmt9_native_test_env_vars)

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

test('dxmt9-backend-key-descriptor-spec', backend_key_descriptor_spec,
     env: dxmt9_native_test_env)

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

test('dxmt9-chunk-record-import-spec', chunk_record_import_spec,
     env: dxmt9_native_test_env)

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

metalcapture_spec = executable(
  'dxmt9-metalcapture-spec',
  'native/backend/metalcapture_spec.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

test('dxmt9-metalcapture-spec', metalcapture_spec)

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

python3 = find_program('python3', required: true)

shader_runner = executable(
  'shader_runner_dxmt9',
  'shader_runner/shader_runner_dxmt9.cpp',
  include_directories: dxmt9_inc,
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  link_args: dxmt9_test_link_args,
)

if host_machine.system() == 'darwin'
  shader_corpus_root = join_paths(meson.project_source_root(), 'tests/shader_runner/corpus')
  shader_manifest = join_paths(shader_corpus_root, 'MANIFEST.toml')
  # shader_runner_dxmt9 accepts individual .shader_test files; shard the
  # manifest so Meson applies timeout per case instead of to the full corpus.
  shader_corpus_tool = join_paths(meson.project_source_root(), 'scripts/shader_corpus_tool.py')
  shader_corpus_files = run_command(
    python3,
    shader_corpus_tool,
    'list-files',
    '--manifest',
    shader_manifest,
    '--status',
    'passing',
    check: true,
  ).stdout().strip().split('\n')

  foreach shader_corpus_file : shader_corpus_files
    if shader_corpus_file == ''
      continue
    endif
    shader_corpus_test_name = 'dxmt9-shader-corpus-' + shader_corpus_file.replace('/', '-').replace('.shader_test', '')
    test(shader_corpus_test_name, shader_runner,
         args: [join_paths(shader_corpus_root, shader_corpus_file)],
         env: dxmt9_native_test_env,
         is_parallel: false,
         suite: ['shader-corpus'],
         timeout: 60)
  endforeach
endif

bash = find_program('bash', required: true)
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
